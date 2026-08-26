#include <fcntl.h>
#include <gflags/gflags.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>
#include <cmath>
#include <immintrin.h>
#include "smart.h"
#include <infiniband/verbs.h>


#include "libhrd_cpp/hrd.h"
#define CPU_FREQUENCY_HZ 2900000000.0
static constexpr size_t kAppBufSize = MB(2);
static constexpr int kAppBaseSHMKey = 2;

static constexpr size_t kAppDefaultRunTime = 1000000;
static constexpr size_t kAppRunTimeSlack = 10;

// Number of outstanding requests kept by a server thread across all QPs.
// This is only used for READs where we can detect completion by polling on
// a READ's destination buffer.
//
// For WRITEs, this is hard to do unless we make every send() signaled. So,
// the number of per-thread outstanding operations per thread with WRITEs is
// O(NUM_CLIENTS * kAppUnsigBatch).
static constexpr size_t kAppWindowSize = 1;
static const char* SERVER_XRCD_FILE_PATH = "/tmp/server_xrcd";
static_assert(is_power_of_two(kAppWindowSize), "");

// Sweep paramaters
static constexpr size_t kAppNumServers = 128;
static constexpr size_t kAppNumClients = 8;  // Total client QPs in cluster
static constexpr size_t kAppNumClientMachines = 1;
static constexpr size_t kAppUnsigBatch = 1;//qp的总size需要是batch的两倍，原因是聚合。
static constexpr size_t kAppLatBatch = 1; 
static constexpr size_t kNativeRcSQDepth = 128;
static_assert(kAppUnsigBatch > 0, "Hollow RC completion window must be non-zero");
// static_assert(kHrdSQDepth == 128, "");  // Small queues => more scalaing
static_assert(kAppNumClients % kAppNumClientMachines == 0, "");

// We don't use postlist, so we don't need a postlist check
//static_assert(kHrdSQDepth >= 2 * kAppUnsigBatch, "");  // Queue capacity check

hrd_ctrl_blk_t* srm_cb;
ibv_pd* srm_pd;

struct thread_params_t {
  size_t id;
  double* tput;
  double* tput_Gbps;
  double* soft_peak_gbps;
};

DEFINE_uint64(machine_id, std::numeric_limits<size_t>::max(), "Machine ID");
DEFINE_uint64(is_client, 0, "Is this process a client?");
DEFINE_uint64(run_time, 0, "Running time");
DEFINE_uint64(dual_port, 0, "Use two ports?");
DEFINE_uint64(use_uc, 0, "Use unreliable connected transport?");
DEFINE_uint64(do_read, 0, "Do RDMA reads?");
DEFINE_uint64(size, 0, "RDMA size");
DEFINE_uint64(use_xrc, 0, "Use XRC");
DEFINE_uint64(test_lat, 0, "Test latency");
DEFINE_uint64(use_srm, 0, "Test SRM QPs");
DEFINE_uint64(test_lat_thread, 0, "Test latency thread");
DEFINE_uint64(use_smart_rc, 0, "use Smart in RC");
DEFINE_double(peak_bw_gbps, 0, "Peak software send bandwidth (Gbps), 0 = unlimited");
DEFINE_uint64(measure_soft_bw, 0, "Measure per-thread software peak bandwidth");
DEFINE_uint64(srm_active_qps, 0,
              "SRM QPs used per server thread, 0 = all created QPs");
DEFINE_uint64(srm_remote_targets, 0,
              "SRM remote targets used per server thread, 0 = active QPs");
DEFINE_uint64(srm_app_stats, 0, "Print SRM post/poll interval statistics");
DEFINE_uint64(xrc_remote_targets, 0,
              "XRC remote SRQ/MR targets per send QP, 0 = all clients");
DEFINE_uint64(xrc_qps_per_server, 1,
              "XRC send QPs per server thread");

//peak_bw_gbps代表峰值带宽，单位是Gbps。这个参数用于限制软件发送数据的速率，
static double measure_cycles_per_second() {
  static double cached = 0.0;
  if (cached > 0.0) return cached;

  constexpr double kCalibDurationSec = 0.01;  // 10 ms for stable reading
  struct timespec start_ts, now_ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &start_ts);
  uint64_t start_cycles = hrd_get_cycles();

  while (true) {
    clock_gettime(CLOCK_MONOTONIC_RAW, &now_ts);
    double elapsed =
        (now_ts.tv_sec - start_ts.tv_sec) +
        (now_ts.tv_nsec - start_ts.tv_nsec) / 1e9;
    if (elapsed >= kCalibDurationSec) {
      uint64_t end_cycles = hrd_get_cycles();
      cached = static_cast<double>(end_cycles - start_cycles) / elapsed;
      break;
    }
  }

  if (cached <= 0.0) cached = CPU_FREQUENCY_HZ;
  return cached;
}

struct BwLimiter {
  double cycles_per_sec;
  double bytes_per_sec;
  size_t next_tsc;
  BwLimiter()
      : cycles_per_sec(measure_cycles_per_second()),
        bytes_per_sec(FLAGS_peak_bw_gbps > 0
                          ? (FLAGS_peak_bw_gbps * 1e9 / 8.0) /
                                static_cast<double>(kAppNumServers)
                          : 0.0),
        next_tsc(0) {}
  void wait(size_t bytes) {
    if (bytes_per_sec <= 0.0) return;
    size_t now = hrd_get_cycles();
    if (next_tsc < now) next_tsc = now;
    double need = (bytes / bytes_per_sec) * cycles_per_sec;
    size_t target = next_tsc + static_cast<size_t>(need);
    while (hrd_get_cycles() < target) {
      _mm_pause();
    }
    next_tsc = target;
  }
};

std::vector<size_t> traffic_size;

// size_t traffic_size[]={
//     2, 2, 2, 2, 2, 3, 3, 5, 6, 10,
//     11, 12, 14, 16, 28, 44, 61, 85, 107, 119,
//     167, 186, 233, 260, 325, 406, 508, 710, 1109, 96093
//     };//Facebook_KVstorage

FILE* log_file;

uint64_t seed_array[kAppNumServers + 1];

// 全局变量
std::mutex barrier_mutex;            // 互斥锁保护
std::condition_variable barrier_cv;  // 条件变量
int barrier_count = 0;               // 到达线程墙的线程计数
std::mutex srm_create_mutex;
std::condition_variable srm_create_cv;
size_t srm_next_server_create = 0;
std::mutex shared_mutex;             // 共享cb和pd的互斥锁
std::condition_variable shared_cv;   // 共享cb和pd的条件变量
bool shared_ready = false;           // 共享cb和pd是否准备好
// 线程墙实现



#define MAX_USER_XRC_QP_PER_SRM SRM_MAX_USER_XRC_QP_PER_SRM

#define __cacheline_aligned __attribute__((__aligned__(64)))
#define NUM_LEVEL SRM_NUM_LEVEL
#define NUM_SCHED SRM_NUM_SCHED









#define KQP_NUM_PER_THREAD kAppNumClients








struct aligned_u32 *wqe_table, *level_table;

struct xrc_table_entry (*xrc_table)[NUM_LEVEL*NUM_SCHED][KQP_NUM_PER_THREAD];
uint64_t xrc_tot_bytes[kAppNumServers + 1][NUM_SCHED][KQP_NUM_PER_THREAD];

int kqp_idx_arr[kAppNumServers + 1][KQP_NUM_PER_THREAD];

int kqp_cnt[kAppNumServers + 1][NUM_SCHED*NUM_LEVEL][KQP_NUM_PER_THREAD]; 
int free_kqp_cnt[kAppNumServers + 1][NUM_SCHED*NUM_LEVEL];
int free_kqp_idx[kAppNumServers + 1][NUM_SCHED*NUM_LEVEL][KQP_NUM_PER_THREAD];
static inline uint64_t rdtsc() {
  unsigned int lo, hi;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
void thread_barrier() {
  std::unique_lock<std::mutex> lock(barrier_mutex);
  barrier_count++;
  // 通知下一个线程开始执行
  barrier_cv.notify_all();

  if (barrier_count == kAppNumServers + FLAGS_test_lat_thread) {
    // 如果所有线程都到达，通知所有线程继续
    barrier_cv.notify_all();
  } else {
    // 否则，当前线程等待
    barrier_cv.wait(lock, [] {
      return barrier_count == kAppNumServers + FLAGS_test_lat_thread;
    });
  }
}

void clt_thread_barrier() {
  std::unique_lock<std::mutex> lock(barrier_mutex);
  barrier_count++;
  // 通知下一个线程开始执行
  barrier_cv.notify_all();

  if (barrier_count == kAppNumClients + FLAGS_test_lat_thread) {
    // 如果所有线程都到达，通知所有线程继续
    barrier_cv.notify_all();
  } else {
    // 否则，当前线程等待
    barrier_cv.wait(lock, [] {
      return barrier_count == kAppNumClients + FLAGS_test_lat_thread;
    });
  }
}



void print_qp_info(struct hrd_qp_attr_t* info) {
  printf("QP Number: %u\n", info->qpn);
  printf("GID: ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x", info->gid.raw[i]);
  }
  printf("\n");
  printf("r_key: %u\n", info->rkey);
  printf("addr: %lu\n", info->buf_addr);
}

std::vector<uint64_t>cpu_cycles,ps_cycles;
void run_server(thread_params_t* params) {
  size_t srv_gid = params->id;  // Global ID of this server thread
  size_t ib_port_index = 1;
  int shm_key = kAppBaseSHMKey + static_cast<int>(srv_gid);
  const size_t xrc_qps_per_server =
      FLAGS_use_xrc ? static_cast<size_t>(FLAGS_xrc_qps_per_server) : 1;

  hrd_conn_config_t conn_config{};
  conn_config.num_qps =
      (FLAGS_use_xrc ? xrc_qps_per_server * kAppNumClientMachines
                     : kAppNumClients);
  if (srv_gid == kAppNumServers && FLAGS_test_lat_thread) {
    // lat thread
    conn_config.num_qps = 1;
  }
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = shm_key;
  conn_config.use_xrc = FLAGS_use_xrc != 0;
  conn_config.num_srqs = 0;
  conn_config.xrcd_fd = -1;
  conn_config.is_client = false;
  conn_config.fst_client_t = false;
  conn_config.isSmall = (srv_gid == kAppNumServers && FLAGS_test_lat_thread) ? 1 : 0;
  if (!conn_config.use_xrc)
    conn_config.sq_depth = kNativeRcSQDepth;
  // if(FLAGS_use_xrc)
  //   conn_config.sq_depth = kHrdSQDepth*10;

  // if (srv_gid == 0) {
  //   srm_cb = new hrd_ctrl_blk_t();
  //   memset(srm_cb, 0, sizeof(hrd_ctrl_blk_t));
  //   hrd_resolve_port_index(srm_cb, 0);
  //   srm_pd = ibv_alloc_pd(srm_cb->resolve.ib_ctx);
  //   {
  //       std::unique_lock<std::mutex> lock(shared_mutex);
  //       shared_ready = true;
  //       shared_cv.notify_all();
  //   }
  // } else {
  //   std::unique_lock<std::mutex> lock(shared_mutex);
  //   shared_cv.wait(lock, []{ return shared_ready; });
  // }

  hrd_ctrl_blk_t* cb;
  if (conn_config.use_xrc == 0)
    cb = hrd_ctrl_blk_init(srv_gid, ib_port_index, 1, &conn_config, nullptr,
                           nullptr, nullptr);
  else
    cb = hrd_ctrl_blk_init_xrc(srv_gid, ib_port_index, 1, &conn_config, nullptr,
                               0);
  // Set the buffer to 0 so that we can detect READ completion by polling.
  memset(const_cast<uint8_t*>(cb->conn_buf), 0, kAppBufSize);

  for (size_t i = 0; i < conn_config.num_qps; i++) {
    char srv_qp_name[kHrdQPNameSize];
    if (cb->conn_config.use_xrc) {
      sprintf(srv_qp_name, "server-xrc-%zu-%zu", srv_gid, i);
      hrd_publish_conn_qp(cb, i, srv_qp_name);
      continue;
    }
    size_t clt_id = i;
    if (srv_gid == kAppNumServers && FLAGS_test_lat_thread) {
      clt_id = kAppNumClients;
    }
    sprintf(srv_qp_name, "server-%zu-%zu", srv_gid, clt_id);
    hrd_publish_conn_qp(cb, i, srv_qp_name);
  }

  hrd_qp_attr_t* clt_qp[kAppNumClients];
  if (srv_gid == kAppNumServers) {
    char clt_qp_name[kHrdQPNameSize];
    sprintf(clt_qp_name, "client-%zu-%zu", kAppNumClients, srv_gid);

    clt_qp[0] = nullptr;
    while (clt_qp[0] == nullptr) {
      clt_qp[0] = hrd_get_published_qp(clt_qp_name);
      if (clt_qp[0] == nullptr) usleep(20000);
    }

    // printf("main: Server %zu found client %zu! Connecting..\n", srv_gid,
    //        kAppNumClients);
    hrd_connect_qp(cb, 0, clt_qp[0]);
    hrd_wait_till_ready(clt_qp_name);

    print_qp_info(clt_qp[0]);
  } else {
    if (cb->conn_config.use_xrc) {
      for (size_t lane = 0; lane < xrc_qps_per_server; lane++) {
        char clt_xrc_qp_name[kHrdQPNameSize];
        sprintf(clt_xrc_qp_name, "client-xrc-qp-%zu-%zu", srv_gid, lane);

        hrd_qp_attr_t* clt_xrc_qp = nullptr;
        while (clt_xrc_qp == nullptr) {
          clt_xrc_qp = hrd_get_published_qp(clt_xrc_qp_name);
          if (clt_xrc_qp == nullptr) usleep(20000);
        }

        printf("main: Server %zu connecting XRC lane %zu\n", srv_gid, lane);
        hrd_connect_qp(cb, lane, clt_xrc_qp);
        hrd_wait_till_ready(clt_xrc_qp_name);
      }
    }

    for (size_t i = 0; i < kAppNumClients; i++) {
      char clt_qp_name[kHrdQPNameSize];
      sprintf(clt_qp_name, "client-%zu-%zu", i, srv_gid);

      clt_qp[i] = nullptr;
      while (clt_qp[i] == nullptr) {
        clt_qp[i] = hrd_get_published_qp(clt_qp_name);
        if (clt_qp[i] == nullptr) usleep(20000);
      }

      if (!cb->conn_config.use_xrc) {
        printf("main: Server %zu found client %zu! Connecting..\n", srv_gid, i);
        hrd_connect_qp(cb, i, clt_qp[i]);
        hrd_wait_till_ready(clt_qp_name);
      }
      print_qp_info(clt_qp[i]);
    }
  }

  size_t xrc_remote_targets = kAppNumClients;
  if (cb->conn_config.use_xrc) {
    xrc_remote_targets =
        FLAGS_xrc_remote_targets == 0
            ? kAppNumClients
            : std::min(static_cast<size_t>(FLAGS_xrc_remote_targets),
                       kAppNumClients);
    rt_assert(xrc_remote_targets > 0,
              "XRC remote target count must be non-zero");
    printf("main: Server %zu ready (xrc_qps=%zu xrc_remote_targets=%zu)\n",
           srv_gid, xrc_qps_per_server, xrc_remote_targets);
  } else {
    printf("main: Server %zu ready (native_rc_qps=%zu sq_depth=%zu)\n",
           srv_gid, conn_config.num_qps, conn_config.sq_depth);
  }

// 注册本线程所有要轮询的CQ，供SMART在积分不足时轮询多个CQ
  std::vector<size_t> nxt_poll_num(cb->conn_config.num_qps, kAppUnsigBatch);
  if (!FLAGS_use_srm && FLAGS_use_smart_rc) {
    for (size_t i = 0; i < cb->conn_config.num_qps; ++i) {
      smartshim::register_cq(cb->conn_cq[i]);
      smartshim::register_next_poll_table(nxt_poll_num.data(), cb->conn_config.num_qps);
    }
  }

  struct ibv_send_wr wr, *bad_send_wr;
  struct ibv_sge sgl;
  // struct ibv_wc wc_lat;
  struct ibv_wc wc[kAppUnsigBatch];
  size_t rolling_iter = 0;             // For performance measurement
  size_t nb_tx[kAppNumClients+1] = {0};  // Per-QP signaling
  size_t nb_tx_tot = 0;                // For windowing (for READs only)

  std::vector<double> lats;

  auto opcode = FLAGS_do_read == 0 ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;
  uint64_t seed = 0xdeadbeef;
  size_t cn, qp_cn;
  cn = qp_cn = -1;
  int real_sz;
  double tot_sz = 0;

  uint64_t lat_st, lat_ed;
  double elapsed_cycles, elapsed_time_us;
  BwLimiter bw_limiter;
  
  double soft_peak_gbps = 0.0;
  size_t soft_burst_bytes = 0;
  size_t soft_burst_start_cycles = 0;
  bool soft_burst_active = false;
  
  
  thread_barrier();
  struct timespec run_start, run_end;
  struct timespec msr_start, msr_end;
  struct timespec lat_start, lat_end;
  clock_gettime(CLOCK_REALTIME, &run_start);
  clock_gettime(CLOCK_REALTIME, &msr_start);
  while (1) {
    if (srv_gid != kAppNumServers) {
      if (rolling_iter >= KB(512)) {
        clock_gettime(CLOCK_REALTIME, &msr_end);
        double msr_seconds =
            (msr_end.tv_sec - msr_start.tv_sec) +
            (msr_end.tv_nsec - msr_start.tv_nsec) / 1000000000.0;
        double tput = rolling_iter / msr_seconds;
        double tput_Gbps = tot_sz / msr_seconds / 1e9 * 8;
        if(FLAGS_measure_soft_bw){
          params->soft_peak_gbps[srv_gid] = soft_peak_gbps;
        }

        clock_gettime(CLOCK_REALTIME, &run_end);
        double run_seconds =
            (run_end.tv_sec - run_start.tv_sec) +
            (run_end.tv_nsec - run_start.tv_nsec) / 1000000000.0;
        if (run_seconds >= FLAGS_run_time) {
          printf("main: Server %zu exiting.\n", srv_gid);
          hrd_ctrl_blk_destroy(cb);
          return;
        }

        printf(
            "main: Server %zu: %.2f ops, %.2f Gbps. Total active QPs = %zu. "
            "Outstanding ops per thread (for READs) = %zu. "
            "Seconds = %.1f of %zu.\n",
            srv_gid, tput, tput_Gbps, kAppNumServers * kAppNumClients,
            kAppWindowSize, run_seconds, FLAGS_run_time);

        params->tput[srv_gid] = tput;
        params->tput_Gbps[srv_gid] = tput_Gbps;
        if (srv_gid == 0) {
          double tot = 0, tot_Gbps = 0;
          for (size_t i = 0; i < kAppNumServers; i++) {
            tot += params->tput[i];
            tot_Gbps += params->tput_Gbps[i];
          }
          hrd_red_printf("Total tput = %.2f ops,%.2f Gbps\n", tot, tot_Gbps);
          if(FLAGS_measure_soft_bw){
            double soft_tot = 0.0, soft_max = 0.0;
            for (size_t i = 0; i < kAppNumServers; i++) {
              soft_tot += params->soft_peak_gbps[i];
              if (params->soft_peak_gbps[i] > soft_max) {
                soft_max = params->soft_peak_gbps[i];
              }
            }
            hrd_red_printf("Total software peak = %.2f Gbps, Max per-thread = %.2f Gbps\n",
                           soft_tot, soft_max);
          }
        }

        rolling_iter = 0;
        tot_sz = 0;
        clock_gettime(CLOCK_REALTIME, &msr_start);
        if (FLAGS_test_lat) {
          double avg =
              std::accumulate(lats.begin(), lats.end(), 0.0) / lats.size();
          sort(lats.begin(), lats.end());
          printf(
              "Latency(us): min = %.2f, max = %.2f, avg = %.2f, median = %.2f, "
              "99th = %.2f\n",
              lats[0], lats[lats.size() - 1], avg, lats[lats.size() / 2],
              lats[lats.size() * 99 / 100]);
          lats.clear();
        }
      }
      size_t window_i =
          nb_tx_tot % kAppWindowSize;  // Current window slot to use

      // For READs, restrict outstanding ops per-thread to kAppWindowSize
      if (opcode == IBV_WR_RDMA_READ && nb_tx_tot >= kAppWindowSize) {
        while (cb->conn_buf[window_i * FLAGS_size] == 0) {
          // Wait for a window slow to open up
        }
        cb->conn_buf[window_i * FLAGS_size] = 0;
      }

      // Choose the next client to send a packet to
      const uint32_t target_rand = hrd_fastrand(&seed);
      size_t cn;
      if (cb->conn_config.use_xrc) {
        // Keep every physical XRC_SEND QP active at every target count.
        qp_cn = target_rand % xrc_qps_per_server;
        cn = (target_rand / xrc_qps_per_server) % xrc_remote_targets;
      } else {
        cn = target_rand % kAppNumClients;
        qp_cn = cn;
      }
      memset(&wr, 0, sizeof(wr));
      wr.opcode = opcode;
      wr.num_sge = 1;
      wr.next = nullptr;
      wr.sg_list = &sgl;

      // wr.send_flags = nb_tx[qp_cn] % kAppUnsigBatch == 0 ? IBV_SEND_SIGNALED : 0;
      // if (!FLAGS_use_srm && FLAGS_use_smart_rc) wr.send_flags = IBV_SEND_SIGNALED;
      // else if(!FLAGS_use_srm && !FLAGS_use_smart_rc)
        // wr.send_flags = (nb_tx[qp_cn] % kAppUnsigBatch == 0) ? IBV_SEND_SIGNALED : 0;
      wr.send_flags = IBV_SEND_SIGNALED;
      // if (nb_tx[qp_cn] % kAppUnsigBatch == 0 && nb_tx[qp_cn] > 0 &&
      //     !FLAGS_test_lat) {
      //   int ret=0;
      //   if (!FLAGS_use_srm && FLAGS_use_smart_rc) {
      //     // 若刚在 before_post_send 中从该CQ拿过CQE，则跳过busy-wait并复位标志
      //       // printf("before hrd_poll_cq_ret1 \n");
      //       // ret = hrd_poll_cq_ret(cb->conn_cq[qp_cn], kAppUnsigBatch, wc);
      //       ret = ibv_poll_cq(cb->conn_cq[qp_cn], kAppUnsigBatch, wc);
      //       smartshim::clear_cq_flag(cb->conn_cq[qp_cn]);
      //       if (ret > 0) smartshim::on_cq_completions(ret);
          
      //   } 
      //   // else {
      //   //   // This can happen if a client dies before the server
      //   //   // ret = hrd_poll_cq_ret(cb->conn_cq[qp_cn], 1, wc);
      //   //   ret = ibv_poll_cq(cb->conn_cq[qp_cn], kAppUnsigBatch, wc);
      //   // }
      //   if (ret == -1) {
      //     hrd_ctrl_blk_destroy(cb);
      //     return;
      //   }
      // }
      if (!FLAGS_use_srm && !FLAGS_use_smart_rc && !FLAGS_test_lat) {
        if (FLAGS_measure_soft_bw && soft_burst_active) {
          soft_burst_active = false;
          soft_burst_bytes = 0;
          soft_burst_start_cycles = 0;
        }
        while (nb_tx[qp_cn] >= nxt_poll_num[qp_cn] && nb_tx[qp_cn] > 0) {
          int got = ibv_poll_cq(cb->conn_cq[qp_cn], (int)kAppUnsigBatch, wc);
          if (got < 0) {
            // 处理错误：可以直接退出或打印
            fprintf(stderr, "ibv_poll_cq error on qp %zu: %d\n", qp_cn, got);
            hrd_ctrl_blk_destroy(cb);
            return;
          }
          else if (got > 0) {
            // 允许的发送阈值前移 got：下一次最多再发 got 个
            nxt_poll_num[qp_cn] += (size_t)got;

            // if(srv_gid == 0){
            //   uint64_t ed_lat = rdtsc();
            //   fprintf(log_file,"吞吐线程post_send cycles:%llu, poll completions cycles:%llu\n,", lat_ed - lat_st,ed_lat - lat_ed);
            //   fflush(log_file);
            // }


            break;  // 已获得预算，退出等待，继续 post send
          }
        }
       
    }else if (!FLAGS_use_srm && FLAGS_use_smart_rc && !FLAGS_test_lat) { 
        if (FLAGS_measure_soft_bw && soft_burst_active) {
          soft_burst_active = false;
          soft_burst_bytes = 0;
          soft_burst_start_cycles = 0;
        }
       while (nb_tx[qp_cn] >= nxt_poll_num[qp_cn] && nb_tx[qp_cn] > 0) {
          int got = ibv_poll_cq(cb->conn_cq[qp_cn], (int)kAppUnsigBatch, wc);
          if (got < 0) {
            // 处理错误：可以直接退出或打印
            fprintf(stderr, "ibv_poll_cq error on qp %zu: %d\n", qp_cn, got);
            hrd_ctrl_blk_destroy(cb);
            return;
          }
          else if (got > 0) {
            // 允许的发送阈值前移 got：下一次最多再发 got 个
            nxt_poll_num[qp_cn] += (size_t)got;
            smartshim::clear_cq_flag(cb->conn_cq[qp_cn]);
            smartshim::on_cq_completions(got);
            break;  // 已获得预算，退出等待，继续 post send
          }

       }


    }

      // wr.send_flags |= (FLAGS_do_read == 0) ? IBV_SEND_INLINE : 0;
      //real_sz = traffic_size[hrd_fastrand(&seed) % traffic_size.size()];
      real_sz = KB(2);
      // if(hrd_fastrand(&seed)%2==1) real_sz =304;
      // else real_sz = KB(4);
      // real_sz = 32;
      tot_sz += real_sz;

      sgl.addr =
          reinterpret_cast<uint64_t>(&cb->conn_buf[window_i * FLAGS_size]);
      sgl.length = real_sz;
      sgl.lkey = cb->conn_buf_mr->lkey;

      // size_t remote_offset = hrd_fastrand(&seed) % (kAppBufSize -
      // FLAGS_size);
      size_t remote_offset = 0;
      wr.wr.rdma.remote_addr = clt_qp[cn]->buf_addr + remote_offset;
      wr.wr.rdma.rkey = clt_qp[cn]->rkey;
      // wr.wr.rdma.remote_addr = 0;
      // wr.wr.rdma.rkey = 0;
      if (cb->conn_config.use_xrc)
        wr.qp_type.xrc.remote_srqn = clt_qp[cn]->srqn;
      nb_tx[qp_cn]++;
      if (FLAGS_test_lat) {
        clock_gettime(CLOCK_REALTIME, &lat_start);
      }
      
      // //文件
      // if(srv_gid == 0){
      //   lat_st = rdtsc();
      //   wr.wr_id = 114514;
      // }


      if(!FLAGS_use_srm && FLAGS_use_smart_rc){
        int ret_smart = smartshim::before_post_send(1, cb->conn_cq[qp_cn], wc, kAppUnsigBatch);
      }
      
      if (FLAGS_measure_soft_bw && !soft_burst_active) {
        soft_burst_active = true;
        soft_burst_start_cycles = hrd_get_cycles();
        soft_burst_bytes = 0;
      }

      bw_limiter.wait(sgl.length);

      if(srv_gid == 0)
        lat_st = rdtsc();
      int ret = ibv_post_send(cb->conn_qp[qp_cn], &wr, &bad_send_wr);

      if(srv_gid == 0)
        lat_ed = rdtsc();
      // //文件
      // if(srv_gid == 0){
      //   lat_ed = rdtsc();
      //   cpu_cycles.push_back(lat_ed - lat_st);
      //   if(cpu_cycles.size()>=5000000){
      //        uint64_t avg_cpu_cycles = std::accumulate(cpu_cycles.begin(),cpu_cycles.end(),0LL)/cpu_cycles.size(); 
      //        fprintf(log_file,"avg_cpu_cycles %llu\n", avg_cpu_cycles);
      //        fflush(log_file);
      //        cpu_cycles.clear();
      //   }
      // }

      rt_assert(ret == 0);
      nb_tx_tot++;
      if (FLAGS_measure_soft_bw && soft_burst_active) {
        soft_burst_bytes += sgl.length;
        size_t now_cycles = hrd_get_cycles();
        double seconds =
            (now_cycles - soft_burst_start_cycles) / measure_cycles_per_second();
        if (seconds > 0.0) {
          double gbps = (soft_burst_bytes * 8.0) / (seconds * 1e9);
          if (gbps > soft_peak_gbps) soft_peak_gbps = gbps;
        }
      }
      rolling_iter++;
      if (FLAGS_test_lat) {
        int ret = hrd_poll_cq_ret(cb->conn_cq[qp_cn], 1, wc);
        if (ret == -1) {
          hrd_ctrl_blk_destroy(cb);
          return;
        }
        clock_gettime(CLOCK_REALTIME, &lat_end);
        double lat_sec = (lat_end.tv_sec - lat_start.tv_sec) * 1e6 +
                         (lat_end.tv_nsec - lat_start.tv_nsec) / 1e3;
        lats.push_back(lat_sec);
      }
    } else {
      if (rolling_iter >= KB(32)) {
        double avg =
            std::accumulate(lats.begin(), lats.end(), 0.0) / lats.size();
        sort(lats.begin(), lats.end());
        printf(
            "Latency(us): min = %.2f, max = %.2f, avg = %.2f, median = %.2f, "
            "99th = %.2f\n",
            lats[0], lats[lats.size() - 1], avg, lats[lats.size() / 2],
            lats[lats.size() * 99 / 100]);
        lats.clear();

        rolling_iter = 0;
      }
      size_t window_i =
          nb_tx_tot % kAppWindowSize;  // Current window slot to use

      // For READs, restrict outstanding ops per-thread to kAppWindowSize
      if (opcode == IBV_WR_RDMA_READ && nb_tx_tot >= kAppWindowSize) {
        while (cb->conn_buf[window_i * FLAGS_size] == 0) {
          // Wait for a window slow to open up
        }
        cb->conn_buf[window_i * FLAGS_size] = 0;
      }

      // Choose the next client to send a packet to
      // size_t cn = (hrd_fastrand(&seed)) % kAppNumClients;
      cn = 0;
      qp_cn = cn;
      wr.opcode = opcode;
      wr.num_sge = 1;
      wr.next = nullptr;
      wr.sg_list = &sgl;

      wr.send_flags = IBV_SEND_SIGNALED;

      // wr.send_flags |= (FLAGS_do_read == 0) ? IBV_SEND_INLINE : 0;
      real_sz = KB(1);

      sgl.addr =
          reinterpret_cast<uint64_t>(&cb->conn_buf[window_i * FLAGS_size]);
      sgl.length = real_sz;
      sgl.lkey = cb->conn_buf_mr->lkey;

      // size_t remote_offset = hrd_fastrand(&seed) % (kAppBufSize -
      // FLAGS_size);
      size_t remote_offset = 0;
      wr.wr.rdma.remote_addr = clt_qp[cn]->buf_addr + remote_offset;
      wr.wr.rdma.rkey = clt_qp[cn]->rkey;
      // wr.wr.rdma.remote_addr = 0;
      // wr.wr.rdma.rkey = 0;
      // wr.qp_type.xrc.remote_srqn = clt_qp[cn]->srqn;
      nb_tx[qp_cn]++;

      uint64_t st_lat;
      lat_st = rdtsc();

      st_lat = lat_st;

      int ret = ibv_post_send(cb->conn_qp[qp_cn], &wr, &bad_send_wr);

      //ed_lat = rdtsc();



      rt_assert(ret == 0);
      rolling_iter++;
      ret = hrd_poll_cq_ret(cb->conn_cq[qp_cn], 1, wc);
      if (ret == -1) {
        hrd_ctrl_blk_destroy(cb);
        return;
      }

      lat_ed = rdtsc();
      if (sgl.length <= KB(10)) {
        elapsed_cycles = (double)(lat_ed - lat_st);
        elapsed_time_us = (elapsed_cycles / CPU_FREQUENCY_HZ) * 1000000.0;
        lats.push_back(elapsed_time_us);
      }

      // fprintf(log_file,"时延线程post_send cycles:%llu, poll completions cycles:%llu\n,", ed_lat - st_lat,lat_ed - ed_lat);
      // fflush(log_file);
    }
  }
}


void run_server_srm(thread_params_t* params) {
  size_t srv_gid = params->id;  // Global ID of this server thread
  size_t ib_port_index = 1;
  int shm_key = kAppBaseSHMKey + static_cast<int>(srv_gid);
  size_t remote_peer_count = (srv_gid == kAppNumServers && FLAGS_test_lat_thread)
                                 ? 1
                                 : kAppNumClients;

  hrd_conn_config_t conn_config{};
  conn_config.num_qps = remote_peer_count;
  conn_config.cq_depth =
      std::max(conn_config.sq_depth,
               remote_peer_count * static_cast<size_t>(kAppUnsigBatch));
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = shm_key;
  conn_config.use_xrc = false;
  conn_config.is_client = false;
  conn_config.fst_client_t = false;
  hrd_ctrl_blk_t* cb;

  // if (srv_gid == 0) {
  //   srm_cb = new hrd_ctrl_blk_t();
  //   memset(srm_cb, 0, sizeof(hrd_ctrl_blk_t));
  //   hrd_resolve_port_index(srm_cb, 0);
  //   srm_pd = ibv_alloc_pd(srm_cb->resolve.ib_ctx);
  //   {
  //       std::unique_lock<std::mutex> lock(shared_mutex);
  //       shared_ready = true;
  //       shared_cv.notify_all();
  //   }
  // } else {
  //   std::unique_lock<std::mutex> lock(shared_mutex);
  //   shared_cv.wait(lock, []{ return shared_ready; });
  // }

  {
    std::unique_lock<std::mutex> lock(srm_create_mutex);
    srm_create_cv.wait(lock, [&]() { return srm_next_server_create == srv_gid; });
  }

  cb = hrd_ctrl_blk_init_srm(srv_gid, ib_port_index, 1, &conn_config, nullptr,
                             conn_config.is_client, srm_cb, srm_pd);

  {
    std::lock_guard<std::mutex> lock(srm_create_mutex);
    srm_next_server_create++;
    srm_create_cv.notify_all();
  }
  //cb->ahs = new ibv_ah*[kAppNumClientMachines];
  // Set the buffer to 1 so that we can detect WRITE completion in client.
  memset(const_cast<uint8_t*>(cb->conn_buf), 1, kAppBufSize);


  std::vector<hrd_qp_attr_t*> clt_qp(conn_config.num_qps, nullptr);
  const size_t total_remote_qps = conn_config.num_qps;
  const size_t active_remote_qps =
      FLAGS_srm_active_qps == 0
          ? total_remote_qps
          : std::min(static_cast<size_t>(FLAGS_srm_active_qps),
                     total_remote_qps);
  rt_assert(active_remote_qps > 0, "SRM active QP count must be non-zero");
  const size_t active_remote_targets =
      FLAGS_srm_remote_targets == 0
          ? active_remote_qps
          : std::min(static_cast<size_t>(FLAGS_srm_remote_targets),
                     active_remote_qps);
  rt_assert(active_remote_targets > 0,
            "SRM remote target count must be non-zero");
  if (srv_gid != kAppNumServers) {
    for (size_t i = 0; i < kAppNumClients; i++) {
      char srv_qp_name[kHrdQPNameSize];
      sprintf(srv_qp_name, "server-%zu-%zu", srv_gid, i);
      hrd_publish_conn_qp_srm(cb, i, -1, srv_qp_name);
    }

    for (size_t i = 0; i < kAppNumClients; i++) {
      char clt_qp_name[kHrdQPNameSize];
      sprintf(clt_qp_name, "client-%zu-%zu", i, srv_gid);

      clt_qp[i] = nullptr;
      while (clt_qp[i] == nullptr) {
        clt_qp[i] = hrd_get_published_qp(clt_qp_name);
        if (clt_qp[i] == nullptr) usleep(20000);
      }

      printf("main: Server %zu found client %zu! Connecting..\n", srv_gid, i);
      int ret = hrd_connect_qp_srm(cb, static_cast<int>(i), clt_qp[i]);
      if (ret == -1) {
        hrd_ctrl_blk_destroy_srm(cb);
        printf("srm failed creating,删用户态资源\n");
        return;
      }
      hrd_wait_till_ready(clt_qp_name);

      printf("服务端lkey：%d，收到的rkey：%d，srqn：%u\n",
             cb->conn_buf_mr->lkey, clt_qp[i]->rkey, clt_qp[i]->srqn);
    }
  } else {
    char srv_qp_name[kHrdQPNameSize];
    sprintf(srv_qp_name, "server-%zu-%zu", srv_gid, kAppNumClients);
    hrd_publish_conn_qp_srm(cb, 0, -1, srv_qp_name);

    char clt_qp_name[kHrdQPNameSize];
    sprintf(clt_qp_name, "client-%zu-%zu", kAppNumClients, srv_gid);

    clt_qp[0] = nullptr;
    while (clt_qp[0] == nullptr) {
      clt_qp[0] = hrd_get_published_qp(clt_qp_name);
      if (clt_qp[0] == nullptr) usleep(20000);
    }

    printf("main: Server %zu found client %zu! Connecting..\n",
           srv_gid, kAppNumClients);
    int ret = hrd_connect_qp_srm(cb, 0, clt_qp[0]);
    if (ret == -1) {
      hrd_ctrl_blk_destroy_srm(cb);
      printf("srm failed creating,删用户态资源\n");
      return;
    }
    hrd_wait_till_ready(clt_qp_name);

    printf("服务端lkey：%d，收到的rkey：%d，srqn：%u\n",
           cb->conn_buf_mr->lkey, clt_qp[0]->rkey, clt_qp[0]->srqn);
  }


  printf("main: Server %zu ready (local_qps=%zu remote_targets=%zu)\n",
         srv_gid, active_remote_qps, active_remote_targets);

  struct ibv_send_wr wr, *bad_send_wr;
  struct ibv_sge sgl;
  struct ibv_wc wc[kAppUnsigBatch];
  size_t rolling_iter = 0;  // For performance measurement

  struct timespec run_start, run_end;
  struct timespec msr_start, msr_end;

  uint64_t lat_st, lat_ed;
  double elapsed_cycles, elapsed_time_us;

  clock_gettime(CLOCK_REALTIME, &run_start);
  clock_gettime(CLOCK_REALTIME, &msr_start);
  std::vector<double> lats;

  auto opcode = FLAGS_do_read == 0 ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;
  uint64_t seed = seed_array[srv_gid];
  size_t  cn;
  cn = -1;

  size_t real_sz;
  double tot_sz = 0;
  thread_barrier();


  std::vector<size_t> qp_outstanding(total_remote_qps, 0);
  std::vector<uint8_t> qp_ready_queued(active_remote_qps, 0);
  std::vector<size_t> qp_ready_queue(active_remote_qps + 1, 0);
  size_t qp_ready_head = 0;
  size_t qp_ready_tail = 0;
  auto qp_ready_empty = [&]() {
    return qp_ready_head == qp_ready_tail;
  };
  auto qp_ready_push = [&](size_t qp_index) {
    if (qp_index >= active_remote_qps || qp_ready_queued[qp_index] ||
        qp_outstanding[qp_index] >= kAppUnsigBatch)
      return;
    qp_ready_queue[qp_ready_tail] = qp_index;
    qp_ready_tail = (qp_ready_tail + 1) % qp_ready_queue.size();
    qp_ready_queued[qp_index] = 1;
  };
  auto qp_ready_pop = [&]() {
    size_t qp_index = qp_ready_queue[qp_ready_head];
    qp_ready_head = (qp_ready_head + 1) % qp_ready_queue.size();
    qp_ready_queued[qp_index] = 0;
    return qp_index;
  };
  for (size_t i = 0; i < active_remote_qps; i++)
    qp_ready_push(i);
  size_t total_outstanding = 0;
  uint64_t stat_post_calls = 0;
  uint64_t stat_post_cycles = 0;
  uint64_t stat_poll_calls = 0;
  uint64_t stat_poll_cycles = 0;
  uint64_t stat_poll_empty = 0;
  uint64_t stat_poll_cqes = 0;
  auto report_app_stats = [&](double seconds) {
    if (!FLAGS_srm_app_stats)
      return;

    printf("SRM_APP_STATS server=%zu active_qps=%zu post_calls=%llu "
           "post_avg_cycles=%.2f post_mops=%.3f poll_calls=%llu "
           "poll_avg_cycles=%.2f poll_empty_pct=%.2f poll_cqes=%llu\n",
           srv_gid, active_remote_qps,
           static_cast<unsigned long long>(stat_post_calls),
           stat_post_calls
               ? static_cast<double>(stat_post_cycles) / stat_post_calls
               : 0.0,
           seconds > 0.0 ? stat_post_calls / seconds / 1e6 : 0.0,
           static_cast<unsigned long long>(stat_poll_calls),
           stat_poll_calls
               ? static_cast<double>(stat_poll_cycles) / stat_poll_calls
               : 0.0,
           stat_poll_calls
               ? 100.0 * static_cast<double>(stat_poll_empty) /
                     stat_poll_calls
               : 0.0,
           static_cast<unsigned long long>(stat_poll_cqes));

    stat_post_calls = 0;
    stat_post_cycles = 0;
    stat_poll_calls = 0;
    stat_poll_cycles = 0;
    stat_poll_empty = 0;
    stat_poll_cqes = 0;
  };
  auto drain_qp_cqs = [&]() {
    while (total_outstanding > 0) {
      int ret = ibv_poll_cq(cb->conn_cq[0], kAppUnsigBatch, wc);
      rt_assert(ret >= 0, "Failed to drain shared CQ");
      for (int i = 0; i < ret; i++) {
        size_t qp_index = static_cast<size_t>(wc[i].wr_id >> 32);

        rt_assert(wc[i].status == IBV_WC_SUCCESS,
                  "CQ error while draining shared CQ");
        rt_assert(qp_index < active_remote_qps,
                  "Invalid QP index in shared CQ wr_id");
        rt_assert(qp_outstanding[qp_index] > 0,
                  "Shared CQ returned QP without outstanding credit");
        const size_t completed_wqes = qp_outstanding[qp_index];
        rt_assert(completed_wqes > 0 &&
                      completed_wqes <= kAppUnsigBatch,
                  "Invalid fixed-window Hollow RC completion span");
        rt_assert(total_outstanding >= completed_wqes,
                  "Invalid total Hollow RC outstanding credit");
        qp_outstanding[qp_index] = 0;
        total_outstanding -= completed_wqes;
        qp_ready_push(qp_index);
      }

      if (ret == 0)
        std::this_thread::yield();
    }
  };
  int nxt_post_wqe_nums =
      srv_gid == kAppNumServers ? kAppLatBatch : kAppUnsigBatch;
  while (1) {
    if (srv_gid != kAppNumServers) {
      if (rolling_iter >= KB(512)) {
        clock_gettime(CLOCK_REALTIME, &msr_end);
        double msr_seconds =
            (msr_end.tv_sec - msr_start.tv_sec) +
            (msr_end.tv_nsec - msr_start.tv_nsec) / 1000000000.0;
        double tput = rolling_iter / msr_seconds;
        double tput_Gbps = tot_sz / msr_seconds / 1e9 * 8;
        report_app_stats(msr_seconds);

        clock_gettime(CLOCK_REALTIME, &run_end);
        double run_seconds =
            (run_end.tv_sec - run_start.tv_sec) +
            (run_end.tv_nsec - run_start.tv_nsec) / 1000000000.0;
        if (run_seconds >= FLAGS_run_time) {
          printf("main: Server %zu exiting.\n", srv_gid);
          drain_qp_cqs();
          hrd_ctrl_blk_destroy_srm(cb);
          return;
        }

        printf(
            "main: Server %zu: %.2f ops, %.2f Gbps. Total active QPs = %zu. "
            "Outstanding ops per thread (for READs) = %zu. "
            "Seconds = %.1f of %zu.\n",
            srv_gid, tput, tput_Gbps, kAppNumServers * kAppNumClients,
            kAppWindowSize, run_seconds, FLAGS_run_time);

        params->tput[srv_gid] = tput;
        params->tput_Gbps[srv_gid] = tput_Gbps;
        if (srv_gid == 0) {
          double tot = 0, tot_Gbps = 0;
          for (size_t i = 0; i < kAppNumServers; i++) {
            tot += params->tput[i];
            tot_Gbps += params->tput_Gbps[i];
          }

          hrd_red_printf("Total tput = %.2f ops,%.2f Gbps\n", tot, tot_Gbps);
        }

        rolling_iter = 0;
        tot_sz = 0;
        clock_gettime(CLOCK_REALTIME, &msr_start);

        if (FLAGS_test_lat) {
          double avg =
              std::accumulate(lats.begin(), lats.end(), 0.0) / lats.size();
          sort(lats.begin(), lats.end());
          printf(
              "Latency(us): min = %.2f, max = %.2f, avg = %.2f , median = "
              "%.2f, 99th = %.2f\n",
              lats[0], lats[lats.size() - 1], avg, lats[lats.size() / 2],
              lats[lats.size() * 99 / 100]);
          lats.clear();
        }
        // hrd_ctrl_blk_destroy(cb);
        // return ;
      }

      

      

      if (total_outstanding > 0) {
        uint64_t poll_start = FLAGS_srm_app_stats ? rdtsc() : 0;
        int ret = ibv_poll_cq(cb->conn_cq[0], kAppUnsigBatch, wc);
        if (FLAGS_srm_app_stats) {
          stat_poll_calls++;
          stat_poll_cycles += rdtsc() - poll_start;
          stat_poll_empty += ret == 0;
          if (ret > 0)
            stat_poll_cqes += static_cast<uint64_t>(ret);
        }
        if (ret < 0) {
          hrd_ctrl_blk_destroy_srm(cb);
          return;
        }
        for (int i = 0; i < ret; i++) {
          size_t completed_qp =
              static_cast<size_t>(wc[i].wr_id >> 32);

          if (wc[i].status != IBV_WC_SUCCESS) {
            printf("poll cq error status:%d vendor_err:%u wr_id:%llu qp_num:%u opcode:%d\n",
                   wc[i].status, wc[i].vendor_err,
                   (unsigned long long)wc[i].wr_id, wc[i].qp_num,
                   wc[i].opcode);
            rt_assert(false);
          }
          rt_assert(completed_qp < active_remote_qps,
                    "Invalid QP index in shared CQ wr_id");
          rt_assert(qp_outstanding[completed_qp] > 0,
                    "Shared CQ returned QP without outstanding credit");
          const size_t completed_wqes = qp_outstanding[completed_qp];
          rt_assert(completed_wqes > 0 &&
                        completed_wqes <= kAppUnsigBatch,
                    "Invalid fixed-window Hollow RC completion span");
          rt_assert(total_outstanding >= completed_wqes,
                    "Invalid total Hollow RC outstanding credit");
          qp_outstanding[completed_qp] = 0;
          total_outstanding -= completed_wqes;
          qp_ready_push(completed_qp);
        }
      }

      if (qp_ready_empty())
        continue;
      cn = qp_ready_pop();

      rt_assert(clt_qp[cn] != nullptr, "SRM remote QP not connected");
      rt_assert(qp_outstanding[cn] == 0,
                "Hollow RC watermark CQ requires one pending completion "
                "window per logical QP");
      nxt_post_wqe_nums =
          static_cast<int>(kAppUnsigBatch - qp_outstanding[cn]);
      for(int post_wqe_i = 0;post_wqe_i < nxt_post_wqe_nums;post_wqe_i++){
        const size_t remote_cn = cn % active_remote_targets;

        rt_assert(clt_qp[remote_cn] != nullptr,
                  "SRM remote target QP not connected");
        /* Never terminate or reset accounting in the middle of an
         * unsignaled completion window: its closing signaled WQE has not
         * been posted yet, so it cannot be drained from the CQ. */
        if (rolling_iter >= KB(512) && post_wqe_i == 0) {
          clock_gettime(CLOCK_REALTIME, &msr_end);
          double msr_seconds =
              (msr_end.tv_sec - msr_start.tv_sec) +
              (msr_end.tv_nsec - msr_start.tv_nsec) / 1000000000.0;
          double tput = rolling_iter / msr_seconds;
          double tput_Gbps = tot_sz / msr_seconds / 1e9 * 8;
          report_app_stats(msr_seconds);

          clock_gettime(CLOCK_REALTIME, &run_end);
          double run_seconds =
              (run_end.tv_sec - run_start.tv_sec) +
              (run_end.tv_nsec - run_start.tv_nsec) / 1000000000.0;
          if (run_seconds >= FLAGS_run_time) {
            printf("main: Server %zu exiting.\n", srv_gid);
            drain_qp_cqs();
            hrd_ctrl_blk_destroy_srm(cb);
            return;
          }

          printf(
              "main: Server %zu: %.2f ops, %.2f Gbps. Total active QPs = %zu. "
              "Outstanding ops per thread (for READs) = %zu. "
              "Seconds = %.1f of %zu.\n",
              srv_gid, tput, tput_Gbps, kAppNumServers * kAppNumClients,
              kAppWindowSize, run_seconds, FLAGS_run_time);

          params->tput[srv_gid] = tput;
          params->tput_Gbps[srv_gid] = tput_Gbps;
          if (srv_gid == 0) {
            double tot = 0, tot_Gbps = 0;
            for (size_t i = 0; i < kAppNumServers; i++) {
              tot += params->tput[i];
              tot_Gbps += params->tput_Gbps[i];
            }

            hrd_red_printf("Total tput = %.2f ops,%.2f Gbps\n", tot, tot_Gbps);
          }

          rolling_iter = 0;
          tot_sz = 0;
          clock_gettime(CLOCK_REALTIME, &msr_start);

          if (FLAGS_test_lat) {
            double avg =
                std::accumulate(lats.begin(), lats.end(), 0.0) / lats.size();
            sort(lats.begin(), lats.end());
            printf(
                "Latency(us): min = %.2f, max = %.2f, avg = %.2f , median = "
                "%.2f, 99th = %.2f\n",
                lats[0], lats[lats.size() - 1], avg, lats[lats.size() / 2],
                lats[lats.size() * 99 / 100]);
            lats.clear();
          }
          // hrd_ctrl_blk_destroy(cb);f
          // return ;
        }
        //real_sz = traffic_size[hrd_fastrand(&seed) % traffic_size.size()];
        real_sz = KB(1);
        // if(hrd_fastrand(&seed)%2 ==1) real_sz = KB(4);
        // else real_sz = 304;


        
        tot_sz += real_sz;

        memset(&wr, 0, sizeof(wr));
        wr.opcode = opcode;
        wr.num_sge = 1;
        wr.next = nullptr;
        wr.sg_list = &sgl;
        wr.wr_id = (static_cast<uint64_t>(cn) << 32) |
                   (rolling_iter & 0xffffffffULL);

        /*
         * Fixed-window CQ moderation: only the WQE that closes this
         * logical QP's window requests a CQE.  Its completion cumulatively
         * acknowledges every earlier unsignaled WQE in the same window.
         */
        wr.send_flags =
            qp_outstanding[cn] + 1 == kAppUnsigBatch
                ? IBV_SEND_SIGNALED
                : 0;

        sgl.addr = reinterpret_cast<uint64_t>(&cb->conn_buf[0]);
        sgl.length = real_sz;
        sgl.lkey = cb->conn_buf_mr->lkey;

        size_t remote_offset = 0;
        // size_t remote_offset = rolling_iter;
        wr.wr.rdma.remote_addr =
            clt_qp[0]->buf_addr + remote_offset;
        // printf("发送端的数据缓存区地址: %p\n", sgl.addr);
        // printf("发送端要写入的缓存区地址: %p\n", wr.wr.rdma.remote_addr);
        wr.wr.rdma.rkey = clt_qp[0]->rkey;
        wr.qp_type.xrc.remote_srqn = clt_qp[remote_cn]->srqn;

        // wr.qp_type.srm.remote_gid.raw[15] = hrd_fastrand(&seed) % 2;//测试多核
        //  printf("interface_id:0x%llx,
        //  subnet_prefix:0x%llx\n",wr.qp_type.srm.remote_gid.global.interface_id,wr.qp_type.srm.remote_gid.global.subnet_prefix);

        // printf("ready to post send, rolling_iter%d\n",rolling_iter);

        // //文件
        // if(srv_gid == 0)
        //   lat_st = rdtsc();

        if(srv_gid == 0){
          lat_st = rdtsc();
        }
        uint64_t post_start = FLAGS_srm_app_stats ? rdtsc() : 0;
        int ret = ibv_post_send(cb->conn_qp[cn], &wr, &bad_send_wr);
        if (FLAGS_srm_app_stats) {
          stat_post_calls++;
          stat_post_cycles += rdtsc() - post_start;
        }

        if (unlikely(ret != 0)) {
          fprintf(stderr,
                  "SRM post_send failed: ret=%d (%s) errno=%d (%s) "
                  "srv=%zu cn=%zu qpn=%u rolling_iter=%zu "
                  "qp_outstanding=%zu total_outstanding=%zu bad_wr=%p wr=%p\n",
                  ret, strerror(ret), errno, strerror(errno), srv_gid, cn,
                  cb->conn_qp[cn]->qp_num, rolling_iter,
                  qp_outstanding[cn], total_outstanding,
                  static_cast<void*>(bad_send_wr), static_cast<void*>(&wr));
        }

        // //文件
        // uint64_t ps_ed ;
        // if(srv_gid == 0)
        //   ps_ed = rdtsc();


        

        rt_assert(ret == 0);
        qp_outstanding[cn]++;
        total_outstanding++;
        rolling_iter++;

        // printf("finish to post send, rolling_iter%d\n",rolling_iter);
      }
      nxt_post_wqe_nums = 0;
    } else {
      // test lat thread
      if (rolling_iter >= KB(128)) {//srm时延
        double avg =
            std::accumulate(lats.begin(), lats.end(), 0.0) / lats.size();
        sort(lats.begin(), lats.end());
        printf(
            "Latency(us): min = %.2f, max = %.2f, avg = %.2f, median = %.2f, "
            "99th = %.2f\n",
            lats[0], lats[lats.size() - 1], avg, lats[lats.size() / 2],
            lats[lats.size() * 99 / 100]);
        lats.clear();
        rolling_iter = 0;

        
      }
      
      for(int post_wqe_i = 0; post_wqe_i < nxt_post_wqe_nums; post_wqe_i++){
        real_sz = KB(1);
        cn = hrd_fastrand(&seed) % remote_peer_count;
        rt_assert(cn < total_remote_qps, "Invalid SRM remote QP index");
        rt_assert(clt_qp[cn] != nullptr, "SRM remote QP not connected");

        memset(&wr, 0, sizeof(wr));
        wr.opcode = opcode;
        wr.num_sge = 1;
        wr.next = nullptr;
        wr.sg_list = &sgl;
        wr.wr_id = rolling_iter;

        wr.send_flags = IBV_SEND_SIGNALED;

        sgl.addr = reinterpret_cast<uint64_t>(&cb->conn_buf[0]);
        sgl.length = real_sz;
        sgl.lkey = cb->conn_buf_mr->lkey;

        size_t remote_offset = 0;
        // size_t remote_offset = rolling_iter;
        wr.wr.rdma.remote_addr = clt_qp[cn]->buf_addr + remote_offset;
        // printf("发送端的数据缓存区地址: %p\n", sgl.addr);
        // printf("发送端要写入的缓存区地址: %p\n", wr.wr.rdma.remote_addr);
        wr.wr.rdma.rkey = clt_qp[cn]->rkey;
        wr.qp_type.xrc.remote_srqn = clt_qp[cn]->srqn;

        // printf("ready to post send, rolling_iter%d\n",rolling_iter);

        
        
        if(post_wqe_i == 0)
          lat_st = rdtsc();

        //st_lat = lat_st;
        int ret = ibv_post_send(cb->conn_qp[cn], &wr, &bad_send_wr);
        if (unlikely(ret != 0)) {
          fprintf(stderr,
                  "SRM latency post_send failed: ret=%d (%s) errno=%d (%s) "
                  "srv=%zu cn=%zu qpn=%u rolling_iter=%zu bad_wr=%p wr=%p\n",
                  ret, strerror(ret), errno, strerror(errno), srv_gid, cn,
                  cb->conn_qp[cn]->qp_num, rolling_iter,
                  static_cast<void*>(bad_send_wr), static_cast<void*>(&wr));
        }
        rt_assert(ret == 0);
        rolling_iter++;

        
        // clock_gettime(CLOCK_REALTIME, &lat_end);
        // double lat_sec = (lat_end.tv_sec - lat_start.tv_sec)*1e6 +
        //                     (lat_end.tv_nsec - lat_start.tv_nsec) / 1e3;
        // lats.push_back(lat_sec);
      }



      
      int ret = hrd_poll_cq_ret(cb->conn_cq[0],kAppLatBatch,wc);
      if (ret == -1) {
        printf("lat thread poll cq error\n");
        hrd_ctrl_blk_destroy_srm(cb);
        return;
      }

      // ibv_srm_add_tot_recv_cqes(cb->conn_qp[0],
      //           static_cast<uint64_t>(kAppLatBatch));

      
      lat_ed = rdtsc();
      elapsed_cycles = (double)(lat_ed - lat_st);
      elapsed_time_us = (elapsed_cycles / CPU_FREQUENCY_HZ) * 1000000.0;
      double avg_time_us = elapsed_time_us / kAppLatBatch;
      for(int i = 0;i<kAppLatBatch;i++){
        lats.push_back(avg_time_us);
      }

      // if(ret>0)
      //   printf("poll completions:%d\n",ret);
      
    }
  }
}

void run_client(thread_params_t* params) {
  printf("run_client\n");
  struct sched_param param;
  int policy;
  pthread_getschedparam(pthread_self(), &policy, &param);

  int o_pri = param.sched_priority;

  param.sched_priority = sched_get_priority_max(SCHED_FIFO);
  pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

  size_t num_threads =
      kAppNumClients / kAppNumClientMachines + (FLAGS_test_lat_thread);

  size_t clt_gid = params->id;  // Global ID of this server thread
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : clt_gid % 2;
  int shm_key = kAppBaseSHMKey + clt_gid % num_threads;

  hrd_conn_config_t conn_config{};
  {
    // 等待逻辑：确保按 clt_gid 顺序启动客户端线程
    std::unique_lock<std::mutex> lock(barrier_mutex);
    // 等待条件满足（等待时会释放锁，被唤醒后重新获取锁）
    barrier_cv.wait(lock, [&]() { return barrier_count == clt_gid; });
  }

  // All client threads open the same inode so their XRC resources share an XRCD.
  conn_config.use_xrc = FLAGS_use_xrc != 0;
  conn_config.xrcd_fd = -1;
  if (conn_config.use_xrc) {
    conn_config.xrcd_fd =
        open(SERVER_XRCD_FILE_PATH, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    rt_assert(conn_config.xrcd_fd >= 0, "Failed to open XRCD backing file");
  }
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = shm_key;
  conn_config.is_client = true;
  conn_config.fst_client_t = (clt_gid % num_threads == 0);
  const size_t xrc_qps_per_server =
      conn_config.use_xrc
          ? static_cast<size_t>(FLAGS_xrc_qps_per_server)
          : 1;
  conn_config.num_qps =
      (!conn_config.use_xrc
           ? kAppNumServers
           : (conn_config.fst_client_t
                  ? kAppNumServers * xrc_qps_per_server
                  : 0));
  conn_config.num_srqs = conn_config.use_xrc ? kAppNumServers : 0;
  conn_config.rnum_threads = kAppNumServers;
  if (!conn_config.use_xrc)
    conn_config.sq_depth = kNativeRcSQDepth;
  if (clt_gid == kAppNumClients) {
    conn_config.num_qps = 1;
    conn_config.num_srqs = conn_config.use_xrc ? 1 : 0;
    conn_config.rnum_threads = 1;
    conn_config.fst_client_t = true;
  }

  bool fst_client_t = conn_config.fst_client_t;
  hrd_ctrl_blk_t* cb;
  
  if (clt_gid == 0) {
    srm_cb = new hrd_ctrl_blk_t();
    memset(srm_cb, 0, sizeof(hrd_ctrl_blk_t));
    hrd_resolve_port_index(srm_cb, 0);
    srm_pd = ibv_alloc_pd(srm_cb->resolve.ib_ctx);
    {
        std::unique_lock<std::mutex> lock(shared_mutex);
        shared_ready = true;
        shared_cv.notify_all();
    }
  } else {
    std::unique_lock<std::mutex> lock(shared_mutex);
    shared_cv.wait(lock, []{ return shared_ready; });
  }
 
  if (conn_config.use_xrc)
    cb = hrd_ctrl_blk_init_xrc(clt_gid, ib_port_index, 0, &conn_config, nullptr,
                               fst_client_t);
  else
    cb = hrd_ctrl_blk_init(clt_gid, ib_port_index, 0, &conn_config, nullptr,
                           srm_cb, srm_pd);
  // Set to some non-zero value so the server can detect READ completion
  memset(const_cast<uint8_t*>(cb->conn_buf), 1, kAppBufSize);

  if (clt_gid != kAppNumClients) {
    for (size_t i = 0; i < kAppNumServers; i++) {
      char clt_qp_name[kHrdQPNameSize];
      sprintf(clt_qp_name, "client-%zu-%zu", clt_gid, i);
      if (cb->conn_config.use_xrc)
        hrd_publish_conn_qp_srm(cb, -1, i, clt_qp_name);
      else
        hrd_publish_conn_qp(cb, i, clt_qp_name);
    }
    if (cb->conn_config.use_xrc && fst_client_t) {
      for (size_t server = 0; server < kAppNumServers; server++) {
        for (size_t lane = 0; lane < xrc_qps_per_server; lane++) {
          const size_t qp_idx = server * xrc_qps_per_server + lane;
          char clt_xrc_qp_name[kHrdQPNameSize];
          sprintf(clt_xrc_qp_name, "client-xrc-qp-%zu-%zu", server, lane);
          hrd_publish_conn_qp_srm(cb, static_cast<int>(qp_idx),
                                  static_cast<int>(server),
                                  clt_xrc_qp_name);
        }
      }
    }
    if (!cb->conn_config.use_xrc) {
      for (size_t i = 0; i < kAppNumServers; i++) {
        char srv_qp_name[kHrdQPNameSize];
        sprintf(srv_qp_name, "server-%zu-%zu", i, clt_gid);

        hrd_qp_attr_t* srv_qp = nullptr;
        while (srv_qp == nullptr) {
          srv_qp = hrd_get_published_qp(srv_qp_name);
          if (srv_qp == nullptr) usleep(20000);
        }

        printf("main: Client %zu found server %zu! Connecting..\n", clt_gid, i);
        hrd_connect_qp(cb, i, srv_qp);

        char clt_qp_name[kHrdQPNameSize];
        sprintf(clt_qp_name, "client-%zu-%zu", clt_gid, i);
        hrd_publish_ready(clt_qp_name);
        print_qp_info(srv_qp);
      }
    } else if (fst_client_t) {
      for (size_t server = 0; server < kAppNumServers; server++) {
        for (size_t lane = 0; lane < xrc_qps_per_server; lane++) {
          const size_t qp_idx = server * xrc_qps_per_server + lane;
          char srv_qp_name[kHrdQPNameSize];
          sprintf(srv_qp_name, "server-xrc-%zu-%zu", server, lane);

          hrd_qp_attr_t* srv_qp = nullptr;
          while (srv_qp == nullptr) {
            srv_qp = hrd_get_published_qp(srv_qp_name);
            if (srv_qp == nullptr) usleep(20000);
          }

          printf("main: Client %zu connecting server %zu XRC lane %zu\n",
                 clt_gid, server, lane);
          hrd_connect_qp(cb, qp_idx, srv_qp);

          char clt_xrc_qp_name[kHrdQPNameSize];
          sprintf(clt_xrc_qp_name, "client-xrc-qp-%zu-%zu", server, lane);
          hrd_publish_ready(clt_xrc_qp_name);
        }
      }
    }
  } else {
    for (size_t i = 0; i < 1; i++) {
      char clt_qp_name[kHrdQPNameSize];
      sprintf(clt_qp_name, "client-%zu-%zu", clt_gid, kAppNumServers);
      hrd_publish_conn_qp(cb, i, clt_qp_name);
    }
    if (!cb->conn_config.use_xrc || fst_client_t) {
      for (size_t i = 0; i < 1; i++) {
        char srv_qp_name[kHrdQPNameSize];
        sprintf(srv_qp_name, "server-%zu-%zu", kAppNumServers, clt_gid);

        hrd_qp_attr_t* srv_qp = nullptr;
        while (srv_qp == nullptr) {
          srv_qp = hrd_get_published_qp(srv_qp_name);
          if (srv_qp == nullptr) usleep(20000);
        }

        printf("main: Client %zu found server %zu! Connecting..\n", clt_gid,
               kAppNumServers);
        hrd_connect_qp(cb, i, srv_qp);

        char clt_qp_name[kHrdQPNameSize];
        sprintf(clt_qp_name, "client-%zu-%zu", clt_gid, kAppNumServers);
        hrd_publish_ready(clt_qp_name);
        print_qp_info(srv_qp);
      }
    }
  }
  printf("main: Client %zu READY\n", clt_gid);

  clt_thread_barrier();

  param.sched_priority = o_pri;
  pthread_setschedparam(pthread_self(), policy, &param);

  struct timespec run_start, run_end;
  clock_gettime(CLOCK_REALTIME, &run_start);

  while (true) {
    printf("main: Client %zu: %d\n", clt_gid, cb->conn_buf[0]);

    clock_gettime(CLOCK_REALTIME, &run_end);
    double run_seconds = (run_end.tv_sec - run_start.tv_sec) +
                         (run_end.tv_nsec - run_start.tv_nsec) / 1000000000.0;

    if (run_seconds >= FLAGS_run_time + kAppRunTimeSlack) {
      printf("main: Client %zu: exiting\n", clt_gid);
      hrd_ctrl_blk_destroy(cb);
      return;
    } else {
      printf("main: Client %zu: active for %.2f seconds (of %zu + %zu)\n",
             clt_gid, run_seconds, FLAGS_run_time, kAppRunTimeSlack);
    }

    sleep(1);
  }
}

void run_client_srm(thread_params_t* params) {
  printf("run_client_srm\n");
  struct sched_param param;
  int policy;
  pthread_getschedparam(pthread_self(), &policy, &param);

  int o_pri = param.sched_priority;

  param.sched_priority = sched_get_priority_max(SCHED_FIFO);
  pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);



  size_t num_threads =
      kAppNumClients / kAppNumClientMachines + (FLAGS_test_lat_thread);

  size_t clt_gid = params->id;  // Global ID of this server thread
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : clt_gid % 2;
  int shm_key = kAppBaseSHMKey + clt_gid % num_threads + kAppNumServers + 1;

  hrd_conn_config_t conn_config;
  //xrcd fd
  conn_config.xrcd_fd =
      open(SERVER_XRCD_FILE_PATH, O_RDONLY | O_CREAT, S_IRUSR | S_IRGRP);
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = shm_key;
  conn_config.is_client = true;
  conn_config.fst_client_t = true;
  conn_config.srm_app_threads = (uint32_t)(kAppNumServers + FLAGS_test_lat_thread);

  if (clt_gid == kAppNumClients) {
    conn_config.rnum_threads = 1;
    conn_config.num_qps = 1;
  } else{
    conn_config.rnum_threads = kAppNumServers;
    conn_config.num_qps = kAppNumServers;
  }
  
  if(clt_gid != kAppNumClients)
    conn_config.num_srqs = kAppNumServers;
  else
    conn_config.num_srqs = 1;

  {
    // 等待逻辑
    std::unique_lock<std::mutex> lock(barrier_mutex);
    // 等待条件满足（等待时会释放锁，被唤醒后重新获取锁）
    barrier_cv.wait(lock, [&]() { return barrier_count == clt_gid; });
  }

  if (clt_gid == 0) {
    srm_cb = new hrd_ctrl_blk_t();
    memset(srm_cb, 0, sizeof(hrd_ctrl_blk_t));
    hrd_resolve_port_index(srm_cb, 0);
    srm_pd = ibv_alloc_pd(srm_cb->resolve.ib_ctx);
    {
        std::unique_lock<std::mutex> lock(shared_mutex);
        shared_ready = true;
        shared_cv.notify_all();
    }
  } else {
    std::unique_lock<std::mutex> lock(shared_mutex);
    shared_cv.wait(lock, []{ return shared_ready; });
  }

  hrd_ctrl_blk_t* cb;
  cb = hrd_ctrl_blk_init_srm(clt_gid, ib_port_index, 0, &conn_config, nullptr,
                             conn_config.is_client, srm_cb, srm_pd);
  
  clt_thread_barrier();
  // Set to zero value so the server can detect WRITE completion
  memset(const_cast<uint8_t*>(cb->conn_buf), 0, kAppBufSize);

  if (clt_gid != kAppNumClients) {
    for (size_t i = 0; i < kAppNumServers; i++) {
      char clt_qp_name[kHrdQPNameSize];
      sprintf(clt_qp_name, "client-%zu-%zu", clt_gid, i);
      hrd_publish_conn_qp_srm(cb, i, i, clt_qp_name);
    }
    for (size_t i = 0; i < kAppNumServers; i++) {
      char srv_qp_name[kHrdQPNameSize];
      sprintf(srv_qp_name, "server-%zu-%zu", i, clt_gid);

      hrd_qp_attr_t* srv_qp = nullptr;
      while (srv_qp == nullptr) {
        srv_qp = hrd_get_published_qp(srv_qp_name);
        if (srv_qp == nullptr) usleep(20000);
      }
      printf("客户端lkey：%d，收到的rkey：%d\n", cb->conn_buf_mr->lkey,
            srv_qp->rkey);

      printf("main: Client %zu found server %zu! Connecting..\n", clt_gid, i);
      hrd_connect_qp_srm(cb, i, srv_qp);

      char clt_qp_name[kHrdQPNameSize];
      sprintf(clt_qp_name, "client-%zu-%zu", clt_gid, i);
      hrd_publish_ready(clt_qp_name);
    }
  } else {
    char clt_qp_name[kHrdQPNameSize];
    sprintf(clt_qp_name, "client-%zu-%zu", clt_gid, kAppNumServers);
    hrd_publish_conn_qp_srm(cb, 0, 0, clt_qp_name);

    char srv_qp_name[kHrdQPNameSize];
    sprintf(srv_qp_name, "server-%zu-%zu", kAppNumServers, clt_gid);

    hrd_qp_attr_t* srv_qp = nullptr;
    while (srv_qp == nullptr) {
      srv_qp = hrd_get_published_qp(srv_qp_name);
      if (srv_qp == nullptr) usleep(20000);
    }
    printf("客户端lkey：%d，收到的rkey：%d\n", cb->conn_buf_mr->lkey,
          srv_qp->rkey);

    printf("main: Client %zu found server %zu! Connecting..\n",
           clt_gid, kAppNumServers);
    hrd_connect_qp_srm(cb, 0, srv_qp);

    hrd_publish_ready(clt_qp_name);
  }

  printf("main: Client %zu READY\n", clt_gid);

  param.sched_priority = o_pri;
  pthread_setschedparam(pthread_self(), policy, &param);

  struct timespec run_start, run_end;
  clock_gettime(CLOCK_REALTIME, &run_start);
  while (true) {
    printf("main: Client %zu: %d\n", clt_gid, cb->conn_buf[0]);

    clock_gettime(CLOCK_REALTIME, &run_end);
    double run_seconds = (run_end.tv_sec - run_start.tv_sec) +
                         (run_end.tv_nsec - run_start.tv_nsec) / 1000000000.0;

    if (run_seconds >= FLAGS_run_time + kAppRunTimeSlack) {
      printf("main: Client %zu: exiting\n", clt_gid);
      hrd_ctrl_blk_destroy_srm(cb);
      return;
    } else {
      printf("main: Client %zu: active for %.2f seconds (of %zu + %zu)\n",
             clt_gid, run_seconds, FLAGS_run_time, kAppRunTimeSlack);
    }
    // for(int i=0;i<1000;i++){
    //   printf("%d",cb->conn_buf[i]);
    // }
    printf("\n");

    sleep(1);
  }
}

void set_thread_priority(std::thread& t) {
  // 获取线程的原生句柄
  pthread_t native_handle = t.native_handle();

  // 定义调度参数
  struct sched_param param;
  int policy = SCHED_FIFO;

  // 获取最大优先级
  int max_priority = sched_get_priority_max(policy);
  param.sched_priority = max_priority;

  // 设置调度策略和优先级
  if (pthread_setschedparam(native_handle, policy, &param) != 0) {
    std::cerr << "Failed to set thread priority: " << strerror(errno)
              << std::endl;
  }
}

// 生成随机种子数组
static void generate_random_seeds(uint64_t* seed_array, size_t count) {
  // 选择初始种子（从之前的推荐中选取）
  uint64_t initial_seed = 0xdeadbeefdeadbeef;
  uint64_t current_seed = initial_seed;

  for (size_t i = 0; i < count; i++) {
    // 生成64位种子：高32位 + 低32位，各调用一次随机函数
    uint32_t high = hrd_fastrand(&current_seed);
    uint32_t low = hrd_fastrand(&current_seed);
    seed_array[i] = ((uint64_t)high << 32) | low;
  }
}
int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  rt_assert(FLAGS_dual_port <= 1, "Invalid dual_port");
  rt_assert(FLAGS_use_uc <= 1, "Invalid use_uc");
  rt_assert(FLAGS_is_client <= 1, "Invalid is_client");
  rt_assert(!(FLAGS_use_xrc && FLAGS_use_srm),
            "Native XRC requires --use_srm=0");
  rt_assert(!(FLAGS_use_xrc && FLAGS_use_uc),
            "XRC is a reliable transport and cannot use UC mode");
  rt_assert(!FLAGS_use_xrc || FLAGS_xrc_qps_per_server > 0,
            "XRC QPs per server must be non-zero");
  rt_assert(!FLAGS_use_xrc ||
                FLAGS_xrc_qps_per_server <= kAppNumClients,
            "XRC QPs per server cannot exceed client count");
  rt_assert(kAppNumClients % kAppNumClientMachines == 0,
            "NumClients must can be div by NumMachines");
  if(!FLAGS_use_srm && FLAGS_use_smart_rc){    
    smartshim::Config cfg;
    smartshim::init(cfg);//参数来源于smart.h
    smartshim::start_tuner();
  
  }
  if(!FLAGS_is_client){
    // 初始化wqe表
    //std::ifstream infile("AliStorage2019_traffic_size.txt");
    std::ifstream infile("Twitter-cluster12_traffic_size.txt");
    int val;
    while (infile >> val) {
      traffic_size.push_back(val);
    }
    printf("traffic_size size:%d\n,traffic_size[0]:%d\n", traffic_size.size(),
          traffic_size[0]);

    if (FLAGS_use_srm) {
      memset(kqp_idx_arr, -1, sizeof(kqp_idx_arr));
    }
    // 初始化随机数种子数组
    generate_random_seeds(seed_array, kAppNumServers + 1);


    for(int i=0;i<(kAppNumServers + FLAGS_test_lat_thread);i++){
      for(int j =0;j<NUM_LEVEL*NUM_SCHED;j++){
        free_kqp_cnt[i][j] = KQP_NUM_PER_THREAD;
        for(int k =0;k<KQP_NUM_PER_THREAD;k++){
          free_kqp_idx[i][j][k] = k;
        }
      }
    }
  }
  // //文件MB
  // char filename[64] ;
  // sprintf(filename, "log_FC_1_32.txt");
  // log_file = fopen(filename, "w");
  // if(log_file == NULL){
  //   perror("fopen失败");
  //   return -1;
  // }

  size_t num_threads;
  if (FLAGS_is_client == 1) {
    num_threads = kAppNumClients / kAppNumClientMachines;
  } else {
    // All the buffers sent or received should fit in @conn_buf
    // rt_assert(FLAGS_size * kAppWindowSize < kAppBufSize, "");

    num_threads = kAppNumServers;
    rt_assert(FLAGS_machine_id == std::numeric_limits<size_t>::max(), "");
    rt_assert(FLAGS_size > 0, "Invalid size");

    // if (FLAGS_do_read == 0) rt_assert(FLAGS_size <= kHrdMaxInline, "Inl
    // error");//now not inline
    rt_assert(FLAGS_do_read <= 1, "Invalid do_read");
  }
  if (FLAGS_test_lat_thread) num_threads++;

  // Launch the server or client threads
  printf("main: Launching %zu threads\n", num_threads);
  std::vector<thread_params_t> param_arr(num_threads);
  std::vector<std::thread> thread_arr(num_threads);
  // auto* tput = new double[num_threads];
  double tput[num_threads] = {0}, tput_Gbps[num_threads] = {0};
  double soft_peak_gbps[num_threads] = {0};
  // if (FLAGS_use_srm && !FLAGS_is_client) {
  //   srm_cb = new hrd_ctrl_blk_t();
  //   memset(srm_cb, 0, sizeof(hrd_ctrl_blk_t));
  //   hrd_resolve_port_index(srm_cb, 0);
  //   srm_pd = ibv_alloc_pd(srm_cb->resolve.ib_ctx);
  // }
  for (size_t i = 0; i < num_threads; i++) {
    if (FLAGS_is_client == 1) {
      param_arr[i].id = (FLAGS_machine_id * num_threads) + i;
      if (!FLAGS_use_srm)
        thread_arr[i] = std::thread(run_client, &param_arr[i]);
      else
        thread_arr[i] = std::thread(run_client_srm, &param_arr[i]);
      // if(FLAGS_use_xrc && i==0)
      //   set_thread_priority(thread_arr[i]);
    } else {
      param_arr[i].id = i;
      param_arr[i].tput = tput;
      param_arr[i].tput_Gbps = tput_Gbps;
      param_arr[i].soft_peak_gbps = soft_peak_gbps;
      if (!FLAGS_use_srm)
        thread_arr[i] = std::thread(run_server, &param_arr[i]);
      else {
        thread_arr[i] = std::thread(run_server_srm, &param_arr[i]);
      }
    }
  }

  for (auto& t : thread_arr) t.join();
  // if (FLAGS_use_srm) {
  //   ibv_dealloc_pd(srm_pd);
  //   ibv_close_device(srm_cb->resolve.ib_ctx);
  // }
  if(!FLAGS_use_srm && FLAGS_use_smart_rc){
    smartshim::stop_tuner();
  }
  

  // //文件
  // fclose(log_file);
  return 0;
}

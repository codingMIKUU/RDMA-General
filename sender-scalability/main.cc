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
static constexpr size_t kAppNumServers = 16;
static constexpr size_t kAppNumClients = 1;  // Total client QPs in cluster
static constexpr size_t kAppNumClientMachines = 1;
static constexpr size_t kAppUnsigBatch = 256;
// static_assert(kHrdSQDepth == 128, "");  // Small queues => more scalaing
static_assert(kAppNumClients % kAppNumClientMachines == 0, "");

// We don't use postlist, so we don't need a postlist check
static_assert(kHrdSQDepth >= 2 * kAppUnsigBatch, "");  // Queue capacity check

hrd_ctrl_blk_t* srm_cb;
ibv_pd* srm_pd;

struct thread_params_t {
  size_t id;
  double* tput;
  double* tput_Gbps;
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
// 线程墙实现

// 与内核模块一致的定义
#define BRIDGE_IOCTL_MAGIC 'B'
#define REG_TABLE_TO_MLX5 _IOW(BRIDGE_IOCTL_MAGIC, 0x01, struct user_table_info)

#define __cacheline_aligned __attribute__((__aligned__(64)))

#define NUM_SCHED 4

struct idx_table_entry {
  uint32_t valid;// 0/1
  uint32_t wqe_idx;
  uint32_t hash_table_idx;
} __cacheline_aligned;  // 对齐到多少字节？

struct hash_table_entry { //全部初始化为-1
  uint32_t idx_table_idx;
  uint32_t valid;//-1/0/1
  uint8_t gid[16]; //只用于用户态
} __cacheline_aligned;

struct user_table_info {
  void* table_addr;
  void* level_table_addr;
  void* idx_table_addr[kAppNumServers + 1][NUM_SCHED];
  void* hash_table_addr[kAppNumServers + 1][NUM_SCHED];

  size_t level_table_size;
  size_t table_size;
  size_t idx_table_size[kAppNumServers + 1][NUM_SCHED];
  size_t hash_table_size[kAppNumServers + 1][NUM_SCHED];

  int hash_table_entry_num_per_bucket;
};
#define KERNEL_QP_NUM 2048  // 真实内核qp数量需要KERNEL_QP_NUM*2
#define HASH_TABLE_KEY_NUM (kAppUnsigBatch * 2)
#define HASH_TABLE_ENTRY_NUM_PER_BUCKET (kAppUnsigBatch * 2)
#define HASH_TABLE_ENTRY_NUM HASH_TABLE_KEY_NUM* HASH_TABLE_ENTRY_NUM_PER_BUCKET

#define IDX_TABLE_ENTRY_NUM (kAppUnsigBatch*2)
#define IDX_TABLE_SIZE \
  (sizeof(idx_table_entry) *  IDX_TABLE_ENTRY_NUM)  // 表大小（页对齐，4KB=1页）
#define HASH_TABLE_SIZE (sizeof(hash_table_entry) * HASH_TABLE_ENTRY_NUM)

#define TABLE_SIZE                                                   \
  (sizeof(uint32_t) * (kAppNumServers + FLAGS_test_lat_thread) * 4 * \
   NUM_SCHED)  // 表大小（页对齐，4KB=1页）
#define LEVEL_TABLE_SIZE (sizeof(uint32_t) * 4 * NUM_SCHED)

#define KQP_NUM_PER_THREAD (KERNEL_QP_NUM / kAppNumServers)

// FNV-1a算法的初始偏移量和质数（针对32位哈希）
#define FNV1A_INIT 0x811c9dc5UL
#define FNV1A_PRIME 0x01000193UL

#define DEV_PATH "/dev/mlx5_table_bridge"

const static int golden_step =
    (int)(HASH_TABLE_KEY_NUM * 0.618 + 0.5) +
    ((int)(HASH_TABLE_KEY_NUM * 0.618 + 0.5) % 2 == 0);

uint32_t *wqe_table, *level_table;

struct idx_table_entry* index_table[kAppNumServers + 1]
                                   [NUM_SCHED];  // 每个线程每个内核线程一个idx
                                                 // table,用于0~4KB消息的聚合
int idx_table_cur_post[kAppNumServers + 1][NUM_SCHED];
struct hash_table_entry* hash_table[kAppNumServers + 1]
                                   [NUM_SCHED];  // 每个线程每个内核线程一个hash
                                                 // table，用于0~4KB消息的聚合
int hash_table_cur_post[kAppNumServers + 1][NUM_SCHED][HASH_TABLE_KEY_NUM];

int kqp_idx_arr[kAppNumServers + 1][KQP_NUM_PER_THREAD];
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

/**
 * sched_hash_ip - 对IPv4地址进行哈希计算
 * @addr: 4字节IPv4地址（网络字节序或主机字节序均可，不影响哈希分布）
 * @n: 哈希表大小（返回值范围为[0, n-1]）
 * 返回：哈希索引
 */
int sched_hash_ip(char addr[4], int n) {
  if (n <= 0) return 0;  // 避免除零错误（根据实际场景处理）

  uint32_t hash = FNV1A_INIT;

  // 对IP地址的4个字节依次处理（FNV-1a算法）
  for (int i = 0; i < 4; i++) {
    hash ^= (uint8_t)addr[i];  // 异或当前字节
    hash *= FNV1A_PRIME;       // 乘以质数（扩散哈希值）
  }

  // 确保哈希结果在[0, n-1]范围内（处理n非2的幂的情况）
  return (int)(hash % (uint32_t)n);
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

void run_server(thread_params_t* params) {
  size_t srv_gid = params->id;  // Global ID of this server thread
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : srv_gid % 2;
  int shm_key = kAppBaseSHMKey + static_cast<int>(srv_gid);
  int clt_num_threads = kAppNumClients / kAppNumClientMachines;  // for xrc only

  hrd_conn_config_t conn_config;
  conn_config.num_qps =
      (FLAGS_use_xrc ? kAppNumClientMachines : kAppNumClients);
  if (srv_gid == kAppNumServers && FLAGS_test_lat_thread) {
    // lat thread
    conn_config.num_qps = 1;
  }
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = shm_key;
  conn_config.use_xrc = (FLAGS_use_xrc == 1);
  conn_config.is_client = false;
  conn_config.fst_client_t = false;
  // if(FLAGS_use_xrc)
  //   conn_config.sq_depth = kHrdSQDepth*10;

  hrd_ctrl_blk_t* cb;
  if (conn_config.use_xrc == 0)
    cb = hrd_ctrl_blk_init(srv_gid, ib_port_index, 0, &conn_config, nullptr);
  else
    cb = hrd_ctrl_blk_init_xrc(srv_gid, ib_port_index, 0, &conn_config, nullptr,
                               0);
  // Set the buffer to 0 so that we can detect READ completion by polling.
  memset(const_cast<uint8_t*>(cb->conn_buf), 0, kAppBufSize);

  for (size_t i = 0; i < conn_config.num_qps; i++) {
    char srv_qp_name[kHrdQPNameSize];
    size_t clt_id = (cb->conn_config.use_xrc ? i * clt_num_threads : i);
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

    printf("main: Server %zu found client %zu! Connecting..\n", srv_gid,
           kAppNumClients);
    hrd_connect_qp(cb, 0, clt_qp[0]);
    hrd_wait_till_ready(clt_qp_name);

    print_qp_info(clt_qp[0]);
  } else {
    for (size_t i = 0; i < kAppNumClients; i++) {
      char clt_qp_name[kHrdQPNameSize];
      sprintf(clt_qp_name, "client-%zu-%zu", i, srv_gid);

      clt_qp[i] = nullptr;
      while (clt_qp[i] == nullptr) {
        clt_qp[i] = hrd_get_published_qp(clt_qp_name);
        if (clt_qp[i] == nullptr) usleep(20000);
      }

      if (!cb->conn_config.use_xrc || i % clt_num_threads == 0) {
        printf("main: Server %zu found client %zu! Connecting..\n", srv_gid, i);
        hrd_connect_qp(cb, i, clt_qp[i]);
        hrd_wait_till_ready(clt_qp_name);
      }
      print_qp_info(clt_qp[i]);
    }
  }

  printf("main: Server %zu ready\n", srv_gid);

  struct ibv_send_wr wr, *bad_send_wr;
  struct ibv_sge sgl;
  struct ibv_wc wc;
  size_t rolling_iter = 0;             // For performance measurement
  size_t nb_tx[kAppNumClients] = {0};  // Per-QP signaling
  size_t nb_tx_tot = 0;                // For windowing (for READs only)

  struct timespec run_start, run_end;
  struct timespec msr_start, msr_end;
  struct timespec lat_start, lat_end;
  clock_gettime(CLOCK_REALTIME, &run_start);
  clock_gettime(CLOCK_REALTIME, &msr_start);
  std::vector<double> lats;

  auto opcode = FLAGS_do_read == 0 ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;
  uint64_t seed = 0xdeadbeef;
  size_t cn, qp_cn;
  cn = qp_cn = -1;
  int real_sz;
  double tot_sz = 0;

  uint64_t lat_st, lat_ed;
  double elapsed_cycles, elapsed_time_us;
  thread_barrier();
  while (1) {
    if (srv_gid != kAppNumServers) {
      if (rolling_iter >= MB(1)) {
        clock_gettime(CLOCK_REALTIME, &msr_end);
        double msr_seconds =
            (msr_end.tv_sec - msr_start.tv_sec) +
            (msr_end.tv_nsec - msr_start.tv_nsec) / 1000000000.0;
        double tput = rolling_iter / msr_seconds;
        double tput_Gbps = tot_sz / msr_seconds / 1e9 * 8;

        clock_gettime(CLOCK_REALTIME, &run_end);
        double run_seconds =
            (run_end.tv_sec - run_start.tv_sec) +
            (run_end.tv_nsec - run_start.tv_nsec) / 1000000000.0;
        if (run_seconds >= FLAGS_run_time) {
          printf("main: Server %zu exiting.\n", srv_gid);
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
      // size_t cn = (hrd_fastrand(&seed)) % kAppNumClients;
      cn = (cn + 1) % kAppNumClients;
      qp_cn = cb->conn_config.use_xrc ? cn / clt_num_threads : cn;
      wr.opcode = opcode;
      wr.num_sge = 1;
      wr.next = nullptr;
      wr.sg_list = &sgl;

      wr.send_flags =
          nb_tx[qp_cn] % kAppUnsigBatch == 0 ? IBV_SEND_SIGNALED : 0;
      if (nb_tx[qp_cn] % kAppUnsigBatch == 0 && nb_tx[qp_cn] > 0 &&
          !FLAGS_test_lat) {
        // This can happen if a client dies before the server
        int ret = hrd_poll_cq_ret(cb->conn_cq[qp_cn], 1, &wc);
        if (ret == -1) {
          hrd_ctrl_blk_destroy(cb);
          return;
        }
      }

      // wr.send_flags |= (FLAGS_do_read == 0) ? IBV_SEND_INLINE : 0;
      real_sz = traffic_size[hrd_fastrand(&seed) % traffic_size.size()];
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
      wr.qp_type.xrc.remote_srqn = clt_qp[cn]->srqn;
      nb_tx[qp_cn]++;
      if (FLAGS_test_lat) {
        clock_gettime(CLOCK_REALTIME, &lat_start);
      }
      int ret = ibv_post_send(cb->conn_qp[qp_cn], &wr, &bad_send_wr);
      rt_assert(ret == 0);
      rolling_iter++;
      if (FLAGS_test_lat) {
        int ret = hrd_poll_cq_ret(cb->conn_cq[qp_cn], 1, &wc);
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
      if (rolling_iter >= KB(256)) {
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
      real_sz = traffic_size[hrd_fastrand(&seed) % traffic_size.size()];

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

      lat_st = rdtsc();

      int ret = ibv_post_send(cb->conn_qp[qp_cn], &wr, &bad_send_wr);
      rt_assert(ret == 0);
      rolling_iter++;

      ret = hrd_poll_cq_ret(cb->conn_cq[qp_cn], 1, &wc);
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
    }
  }
}
int hash_next_key(int hash_val, int i, int sz) {
  return (hash_val + i * golden_step) & (sz - 1);
}
void run_server_srm(thread_params_t* params) {
  size_t srv_gid = params->id;  // Global ID of this server thread
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : srv_gid % 2;
  int shm_key = kAppBaseSHMKey + static_cast<int>(srv_gid);
  int clt_num_threads = kAppNumClients / kAppNumClientMachines;

  hrd_conn_config_t conn_config;
  conn_config.num_qps = 4 * NUM_SCHED;
  if (FLAGS_test_lat_thread && srv_gid == kAppNumServers) {
    // lat thread
    conn_config.num_qps = 4 * NUM_SCHED;
  }
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = shm_key;
  conn_config.use_xrc = (FLAGS_use_xrc == 1);
  conn_config.is_client = false;
  conn_config.fst_client_t = false;

  hrd_ctrl_blk_t* cb;

  {
    // 等待逻辑
    std::unique_lock<std::mutex> lock(barrier_mutex);
    // 等待条件满足（等待时会释放锁，被唤醒后重新获取锁）
    barrier_cv.wait(lock, [&]() { return barrier_count == srv_gid; });
  }

  cb = hrd_ctrl_blk_init_srm(srv_gid, ib_port_index, 0, &conn_config, nullptr,
                             conn_config.is_client, srm_cb, srm_pd);
  cb->ahs = new ibv_ah*[kAppNumClientMachines];
  // Set the buffer to 1 so that we can detect WRITE completion in client.
  memset(const_cast<uint8_t*>(cb->conn_buf), 1, kAppBufSize);

  hrd_qp_attr_t* clt_qp[kAppNumClients];
  if (srv_gid != kAppNumServers) {
    for (size_t i = 0; i < kAppNumClients; i++) {
      char srv_qp_name[kHrdQPNameSize];
      sprintf(srv_qp_name, "server-%zu-%zu", srv_gid, i);
      hrd_publish_conn_qp_srm(cb, i, srv_qp_name);
    }

    for (size_t i = 0; i < kAppNumClients; i++) {
      char clt_qp_name[kHrdQPNameSize];
      sprintf(clt_qp_name, "client-%zu-%zu", i, srv_gid);

      clt_qp[i] = nullptr;
      while (clt_qp[i] == nullptr) {
        clt_qp[i] = hrd_get_published_qp(clt_qp_name);
        if (clt_qp[i] == nullptr) usleep(20000);
      }

      if (i % clt_num_threads == 0) {
        printf("main: Server %zu found client %zu! Connecting..\n", srv_gid, i);
        int ret = hrd_connect_qp_srm(cb, i / clt_num_threads, clt_qp[i]);
        if (ret == -1) {
          hrd_ctrl_blk_destroy_srm(cb);
          printf("srm failed creating,删用户态资源\n");
          return;
        }
        hrd_wait_till_ready(clt_qp_name);
      }
      printf("服务端lkey：%d，收到的rkey：%d\n", cb->conn_buf_mr->lkey,
             clt_qp[i]->rkey);
    }
  } else {
    // test lat thread
    for (size_t i = 0; i < 1; i++) {
      char srv_qp_name[kHrdQPNameSize];
      sprintf(srv_qp_name, "server-%zu-%zu", srv_gid, kAppNumClients);
      hrd_publish_conn_qp_srm(cb, i, srv_qp_name);
    }

    for (size_t i = 0; i < 1; i++) {
      char clt_qp_name[kHrdQPNameSize];
      sprintf(clt_qp_name, "client-%zu-%zu", kAppNumClients, srv_gid);

      clt_qp[i] = nullptr;
      while (clt_qp[i] == nullptr) {
        clt_qp[i] = hrd_get_published_qp(clt_qp_name);
        if (clt_qp[i] == nullptr) usleep(20000);
      }

      printf("main: Server %zu found client %zu! Connecting..\n", srv_gid,
             kAppNumClients);
      int ret = hrd_connect_qp_srm(cb, i, clt_qp[i]);
      if (ret == -1) {
        hrd_ctrl_blk_destroy_srm(cb);
        printf("srm failed creating,删用户态资源\n");
        return;
      }
      hrd_wait_till_ready(clt_qp_name);

      printf("服务端lkey：%d，收到的rkey：%d\n", cb->conn_buf_mr->lkey,
             clt_qp[i]->rkey);
    }
  }

  // 初始化该线程可选取的内核qp表
  uint64_t kqp_idx_seed = seed_array[srv_gid];

  for (int i = 0; i < KQP_NUM_PER_THREAD; i++) {
    // auto kqp_in_queue = [=](int kqp_idx) {
    //   for (int j = 0; j < i; j++) {
    //     if (kqp_idx_arr[srv_gid][j] == kqp_idx) {
    //       return true;
    //     }
    //   }
    //   return false;
    // };
    // while (1) {
    //   int kqp_idx = hrd_fastrand(&kqp_idx_seed) % KERNEL_QP_NUM;
    //   if (kqp_in_queue(kqp_idx)) {
    //     continue;
    //   }
    //   kqp_idx_arr[srv_gid][i] = kqp_idx;
    //   break;
    // }
    kqp_idx_arr[srv_gid][i] = (i + KQP_NUM_PER_THREAD * srv_gid)%KERNEL_QP_NUM;
  }

  printf("main: Server %zu ready\n", srv_gid);

  struct ibv_send_wr wr, *bad_send_wr;
  struct ibv_sge sgl;
  struct ibv_wc wc;
  size_t rolling_iter = 0;  // For performance measurement
  size_t nb_tx[kAppNumClients * 4 * NUM_SCHED] = {0};  // Per-QP signaling
  size_t nb_tx_tot = 0;  // For windowing (for READs only)

  struct timespec run_start, run_end;
  struct timespec msr_start, msr_end;
  struct timespec lat_start, lat_end;

  uint64_t lat_st, lat_ed;
  double elapsed_cycles, elapsed_time_us;

  clock_gettime(CLOCK_REALTIME, &run_start);
  clock_gettime(CLOCK_REALTIME, &msr_start);
  std::vector<double> lats;

  auto opcode = FLAGS_do_read == 0 ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;
  uint64_t seed = seed_array[srv_gid];
  uint64_t sched_seed = seed_array[srv_gid];
  uint64_t ker_qp_seed = seed_array[srv_gid];
  size_t qp_cn, cn;
  qp_cn = cn = -1;

  size_t real_sz;
  double tot_sz = 0;
  int sched_idx;
  int group_idx;
  int tot_qp_nums =
      (kAppNumServers + FLAGS_test_lat_thread) * 4;  // 用户态qp单内核线程总数

  thread_barrier();

  while (1) {
    if (srv_gid != kAppNumServers) {
      if (rolling_iter >= KB(512)) {
        clock_gettime(CLOCK_REALTIME, &msr_end);
        double msr_seconds =
            (msr_end.tv_sec - msr_start.tv_sec) +
            (msr_end.tv_nsec - msr_start.tv_nsec) / 1000000000.0;
        double tput = rolling_iter / msr_seconds;
        double tput_Gbps = tot_sz / msr_seconds / 1e9 * 8;

        clock_gettime(CLOCK_REALTIME, &run_end);
        double run_seconds =
            (run_end.tv_sec - run_start.tv_sec) +
            (run_end.tv_nsec - run_start.tv_nsec) / 1000000000.0;
        if (run_seconds >= FLAGS_run_time) {
          printf("main: Server %zu exiting.\n", srv_gid);
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

      size_t window_i =
          nb_tx_tot % kAppWindowSize;  // Current window slot to use

      // // For READs, restrict outstanding ops per-thread to kAppWindowSize
      // if (opcode == IBV_WR_RDMA_READ && nb_tx_tot >= kAppWindowSize) {
      //   while (cb->conn_buf[window_i * FLAGS_size] == 0) {
      //     // Wait for a window slow to open up
      //   }
      //   cb->conn_buf[window_i * FLAGS_size] = 0;
      // }

      // Choose the next client to send a packet to
      // size_t cn = hrd_fastrand(&seed) % kAppNumClients;
      cn = (cn + 1) % kAppNumClients;

      sched_idx = hrd_fastrand(&sched_seed) % NUM_SCHED;  // 两个内核调度器

      real_sz = traffic_size[hrd_fastrand(&seed) % traffic_size.size()];

      // real_sz = std::min(real_sz,KB(500));

      tot_sz += real_sz;

      // 根据real_sz选择对应srm qp
      if (real_sz < KB(4)) {
        group_idx = 0;
      } else if (real_sz < KB(10)) {
        group_idx = 1;
      } else if (real_sz < KB(100)) {
        group_idx = 2;
      } else {
        //>100KB
        group_idx = 3;
      }
      qp_cn = group_idx * NUM_SCHED + sched_idx;

      // 随机选应该poll哪个srm qp（<=4KB，4KB < <=10KB，>10KB）
      if (nb_tx[qp_cn] % kAppUnsigBatch == 0 && nb_tx[qp_cn] > 0 &&
          !FLAGS_test_lat) {
        // printf("ready to poll cq\n");
        //  This can happen if a client dies before the server
        int ret = hrd_poll_cq_ret(cb->conn_cq[qp_cn], 1, &wc);
        if (ret == -1) {
          hrd_ctrl_blk_destroy_srm(cb);
          return;
        }
        // printf("complete to poll cq\n");
      }

      wr.opcode = opcode;
      wr.num_sge = 1;
      wr.next = nullptr;
      wr.sg_list = &sgl;

      wr.send_flags =
          nb_tx[qp_cn] % kAppUnsigBatch == 0 ? IBV_SEND_SIGNALED : 0;

      sgl.addr = reinterpret_cast<uint64_t>(&cb->conn_buf[0]);
      sgl.length = real_sz;
      sgl.lkey = cb->conn_buf_mr->lkey;

      size_t remote_offset = 0;
      // size_t remote_offset = rolling_iter;
      wr.wr.rdma.remote_addr = clt_qp[cn]->buf_addr + remote_offset;
      // printf("发送端的数据缓存区地址: %p\n", sgl.addr);
      // printf("发送端要写入的缓存区地址: %p\n", wr.wr.rdma.remote_addr);
      wr.wr.rdma.rkey = clt_qp[cn]->rkey;
      wr.qp_type.srm.remote_srqn = clt_qp[cn]->srqn;
      wr.qp_type.srm.remote_gid.global.interface_id =
          clt_qp[cn]->gid.global.interface_id;
      wr.qp_type.srm.remote_gid.global.subnet_prefix =
          clt_qp[cn]->gid.global.subnet_prefix;
      uint16_t* tmp = (uint16_t*)&wr.qp_type.srm.remote_gid.raw[14];
      uint32_t kqp_idx =
          kqp_idx_arr[srv_gid][hrd_fastrand(&ker_qp_seed) % KQP_NUM_PER_THREAD];
      *tmp = (uint16_t)kqp_idx ;

      // wr.qp_type.srm.remote_gid.raw[15] = hrd_fastrand(&seed) % 2;//测试多核
      //  printf("interface_id:0x%llx,
      //  subnet_prefix:0x%llx\n",wr.qp_type.srm.remote_gid.global.interface_id,wr.qp_type.srm.remote_gid.global.subnet_prefix);
      nb_tx[qp_cn]++;
      if (FLAGS_test_lat) {
        clock_gettime(CLOCK_REALTIME, &lat_start);
      }

      // printf("ready to post send, rolling_iter%d\n",rolling_iter);

      int ret = ibv_post_send(cb->conn_qp[qp_cn], &wr, &bad_send_wr);
      // wqe_table[qp_cn*(kAppNumServers+FLAGS_test_lat_thread) + srv_gid]++;
      //  printf("当前线程:%d,wqe_table[%d] = %d\n",srv_gid,
      //    qp_cn*(kAppNumServers+FLAGS_test_lat_thread) + srv_gid,
      //    wqe_table[qp_cn*(kAppNumServers+FLAGS_test_lat_thread) + srv_gid]);


      if(group_idx == 0){

        //uint64_t start_cycles = rdtsc();
        int& cur_post = idx_table_cur_post[srv_gid][sched_idx];
        for (;; cur_post = (cur_post + 1) % IDX_TABLE_ENTRY_NUM) {
          if (index_table[srv_gid][sched_idx][cur_post].valid == 0) {
            // 填入该位置
            index_table[srv_gid][sched_idx][cur_post]
                .wqe_idx = wr.wr_id;  

            break;
          }
        }
        // 索引表和哈希表只用于0~4KB的聚合
        bool hash_ok = false;
        int hash_val = sched_hash_ip((char*)&wr.qp_type.srm.remote_gid.raw[12],
                                      HASH_TABLE_KEY_NUM);

        for (int i = 0; i < HASH_TABLE_KEY_NUM; i++) {
          int key = hash_next_key(hash_val, i, HASH_TABLE_KEY_NUM);
          struct hash_table_entry& entry0 =
              hash_table[srv_gid][sched_idx]
                        [key * HASH_TABLE_ENTRY_NUM_PER_BUCKET];
          if (entry0.valid != -1 &&
              memcmp(entry0.gid, wr.qp_type.srm.remote_gid.raw,
                      sizeof(wr.qp_type.srm.remote_gid))) {
            continue;
          }
          if (entry0.valid == -1) {
            memcpy(entry0.gid, wr.qp_type.srm.remote_gid.raw,
                    sizeof(wr.qp_type.srm.remote_gid));
          }
          
          hash_ok = 1;
          struct hash_table_entry& entry =
              hash_table[srv_gid][sched_idx]
                        [key*HASH_TABLE_ENTRY_NUM_PER_BUCKET+ hash_table_cur_post[srv_gid][sched_idx][key]];
          entry.idx_table_idx = cur_post;

          index_table[srv_gid][sched_idx][cur_post].hash_table_idx =
              key*HASH_TABLE_ENTRY_NUM_PER_BUCKET+ hash_table_cur_post[srv_gid][sched_idx][key];

          hash_table_cur_post[srv_gid][sched_idx][key]= (hash_table_cur_post[srv_gid][sched_idx][key]+1)%HASH_TABLE_ENTRY_NUM_PER_BUCKET;

          uint32_t one = 1;
          __atomic_store(&index_table[srv_gid][sched_idx][cur_post].valid, &one,
                          __ATOMIC_SEQ_CST);  // 先写idx table标记位，再写哈希表
          __atomic_store(&entry.valid, &one, __ATOMIC_SEQ_CST);
          break;
        }
        // uint64_t end_cycles = rdtsc();
        // uint64_t elapsed_ns =  (end_cycles - start_cycles)*1e9 / CPU_FREQUENCY_HZ;
        //printf("索引表和哈希表填写用时:%llu(ns)\n",elapsed_ns);
        if (!hash_ok) {
          printf("出现哈希表满了的情况\n");
        }
      }

      // 对4KB以上的等级，采用原先等级表+数量表的方式
      __atomic_fetch_add(
          wqe_table + group_idx * (kAppNumServers + FLAGS_test_lat_thread) +
              srv_gid + sched_idx * tot_qp_nums,
          1, __ATOMIC_SEQ_CST);  // 使用原子操作存储imm值

      // // 插入释放屏障：确保c的更新先于b的更新被可见
      // __atomic_thread_fence(__ATOMIC_RELEASE);

      __atomic_fetch_add(&level_table[group_idx + sched_idx * 4], 1,
                          __ATOMIC_SEQ_CST);
      
      // fprintf(log_file,"当前线程:%d,wqe_table[%d] = %d\n",srv_gid,
      //   qp_cn*(kAppNumServers+FLAGS_test_lat_thread) + srv_gid,
      //   wqe_table[qp_cn*(kAppNumServers+FLAGS_test_lat_thread) + srv_gid]);
      // fflush(log_file);

      rt_assert(ret == 0);
      rolling_iter++;

      // printf("finish to post send, rolling_iter%d\n",rolling_iter);
      if (FLAGS_test_lat) {
        int ret = hrd_poll_cq_ret(cb->conn_cq[qp_cn], 1, &wc);
        if (ret == -1) {
          hrd_ctrl_blk_destroy_srm(cb);
          return;
        }
        clock_gettime(CLOCK_REALTIME, &lat_end);
        double lat_sec = (lat_end.tv_sec - lat_start.tv_sec) * 1e6 +
                         (lat_end.tv_nsec - lat_start.tv_nsec) / 1e3;
        lats.push_back(lat_sec);
      }
    } else {
      // test lat thread
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

      cn = (cn + 1) % kAppNumClients;
      sched_idx = hrd_fastrand(&sched_seed) % NUM_SCHED;  // 两个内核调度器

      real_sz = traffic_size[hrd_fastrand(&seed) % traffic_size.size()];
      // real_sz = traffic_size[hrd_fastrand(&seed) % traffic_size.size()];
      // real_sz = std::min(real_sz,KB(500));

      // 根据real_sz选择对应srm qp
      if (real_sz < KB(4)) {
        group_idx = 0;
      } else if (real_sz < KB(10)) {
        group_idx = 1;
      } else if (real_sz < KB(100)) {
        group_idx = 2;
      } else {
        //>100KB
        group_idx = 3;
      }
      qp_cn = group_idx * NUM_SCHED + sched_idx;

      wr.opcode = opcode;
      wr.num_sge = 1;
      wr.next = nullptr;
      wr.sg_list = &sgl;

      wr.send_flags = IBV_SEND_SIGNALED;

      sgl.addr = reinterpret_cast<uint64_t>(&cb->conn_buf[window_i * 1024]);
      sgl.length = real_sz;
      sgl.lkey = cb->conn_buf_mr->lkey;

      size_t remote_offset = 0;
      // size_t remote_offset = rolling_iter;
      wr.wr.rdma.remote_addr = clt_qp[cn]->buf_addr + remote_offset;
      // printf("发送端的数据缓存区地址: %p\n", sgl.addr);
      // printf("发送端要写入的缓存区地址: %p\n", wr.wr.rdma.remote_addr);
      wr.wr.rdma.rkey = clt_qp[cn]->rkey;
      wr.qp_type.srm.remote_srqn = clt_qp[cn]->srqn;
      wr.qp_type.srm.remote_gid.global.interface_id =
          clt_qp[cn]->gid.global.interface_id;
      wr.qp_type.srm.remote_gid.global.subnet_prefix =
          clt_qp[cn]->gid.global.subnet_prefix;

      uint16_t* tmp = (uint16_t*)&wr.qp_type.srm.remote_gid.raw[14];
      uint32_t kqp_idx =
          kqp_idx_arr[srv_gid][hrd_fastrand(&ker_qp_seed) % KQP_NUM_PER_THREAD];
      *tmp = (uint16_t)kqp_idx ;

      // wr.qp_type.srm.remote_gid.raw[15] = hrd_fastrand(&seed) % 2;//测试多核
      //  printf("interface_id:0x%llx,
      //  subnet_prefix:0x%llx\n",wr.qp_type.srm.remote_gid.global.interface_id,wr.qp_type.srm.remote_gid.global.subnet_prefix);
      nb_tx[qp_cn]++;

      // clock_gettime(CLOCK_REALTIME, &lat_start);

      lat_st = rdtsc();

      // printf("ready to post send, rolling_iter%d\n",rolling_iter);

      // uint64_t st_lat,ed_lat;
      // st_lat = rdtsc();

      int ret = ibv_post_send(cb->conn_qp[qp_cn], &wr, &bad_send_wr);
      // wqe_table[qp_cn*(kAppNumServers+FLAGS_test_lat_thread) + srv_gid]++;
      //  printf("当前线程:%d,wqe_table[%d] = %d\n",srv_gid,
      //    qp_cn*(kAppNumServers+FLAGS_test_lat_thread) + srv_gid,
      //    wqe_table[qp_cn*(kAppNumServers+FLAGS_test_lat_thread) + srv_gid]);

      if(group_idx == 0){

        int& cur_post = idx_table_cur_post[srv_gid][sched_idx];
        for (;; cur_post = (cur_post + 1) % IDX_TABLE_ENTRY_NUM) {
          if (index_table[srv_gid][sched_idx][cur_post].valid == 0) {
            // 填入该位置
            index_table[srv_gid][sched_idx][cur_post]
                .wqe_idx = wr.wr_id;  

            break;
          }
        }
        // 索引表和哈希表只用于0~4KB的聚合
        bool hash_ok = false;
        int hash_val = sched_hash_ip((char*)&wr.qp_type.srm.remote_gid.raw[12],
                                      HASH_TABLE_KEY_NUM);

        for (int i = 0; i < HASH_TABLE_KEY_NUM; i++) {
          int key = hash_next_key(hash_val, i, HASH_TABLE_KEY_NUM);
          struct hash_table_entry& entry0 =
              hash_table[srv_gid][sched_idx]
                        [key * HASH_TABLE_ENTRY_NUM_PER_BUCKET];
          if (entry0.valid != -1 &&
              memcmp(entry0.gid, wr.qp_type.srm.remote_gid.raw,
                      sizeof(wr.qp_type.srm.remote_gid))) {
            continue;
          }
          if (entry0.valid == -1) {
            memcpy(entry0.gid, wr.qp_type.srm.remote_gid.raw,
                    sizeof(wr.qp_type.srm.remote_gid));
          }
          
          hash_ok = 1;
          struct hash_table_entry& entry =
              hash_table[srv_gid][sched_idx]
                        [key*HASH_TABLE_ENTRY_NUM_PER_BUCKET+ hash_table_cur_post[srv_gid][sched_idx][key]];
          entry.idx_table_idx = cur_post;

          index_table[srv_gid][sched_idx][cur_post].hash_table_idx =
              key*HASH_TABLE_ENTRY_NUM_PER_BUCKET+ hash_table_cur_post[srv_gid][sched_idx][key];

          hash_table_cur_post[srv_gid][sched_idx][key]= (hash_table_cur_post[srv_gid][sched_idx][key]+1)%HASH_TABLE_ENTRY_NUM_PER_BUCKET;

          uint32_t one = 1;
          __atomic_store(&index_table[srv_gid][sched_idx][cur_post].valid, &one,
                          __ATOMIC_SEQ_CST);  // 先写idx table标记位，再写哈希表
          __atomic_store(&entry.valid, &one, __ATOMIC_SEQ_CST);
          break;
        }
        if (!hash_ok) {
          printf("出现哈希表满了的情况\n");
        }
      }

      // 对4KB以上的等级，采用原先等级表+数量表的方式
      __atomic_fetch_add(
          wqe_table + group_idx * (kAppNumServers + FLAGS_test_lat_thread) +
              srv_gid + sched_idx * tot_qp_nums,
          1, __ATOMIC_SEQ_CST);  // 使用原子操作存储imm值

      // // 插入释放屏障：确保c的更新先于b的更新被可见
      // __atomic_thread_fence(__ATOMIC_RELEASE);

      __atomic_fetch_add(&level_table[group_idx + sched_idx * 4], 1,
                          __ATOMIC_SEQ_CST);

      // ed_lat = rdtsc();
      // double e_cycles = (double)(ed_lat - st_lat);
      // double e_t_us = (e_cycles / CPU_FREQUENCY_HZ) * 1000000.0;
      // printf("post send cost %.2f us\n",e_t_us);
      rt_assert(ret == 0);
      rolling_iter++;

      ret = hrd_poll_cq_ret(cb->conn_cq[qp_cn], 1, &wc);
      if (ret == -1) {
        hrd_ctrl_blk_destroy_srm(cb);
        return;
      }
      lat_ed = rdtsc();
      if (sgl.length <= KB(10)) {
        elapsed_cycles = (double)(lat_ed - lat_st);
        elapsed_time_us = (elapsed_cycles / CPU_FREQUENCY_HZ) * 1000000.0;
        lats.push_back(elapsed_time_us);
      }
      // clock_gettime(CLOCK_REALTIME, &lat_end);
      // double lat_sec = (lat_end.tv_sec - lat_start.tv_sec)*1e6 +
      //                     (lat_end.tv_nsec - lat_start.tv_nsec) / 1e3;
      // lats.push_back(lat_sec);
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

  hrd_conn_config_t conn_config;

  // xrcd fd
  conn_config.xrcd_fd =
      open(SERVER_XRCD_FILE_PATH, O_RDONLY | O_CREAT, S_IRUSR | S_IRGRP);
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = shm_key;
  conn_config.is_client = true;
  conn_config.fst_client_t = (clt_gid % num_threads == 0);
  conn_config.use_xrc = (FLAGS_use_xrc == 1);
  conn_config.num_qps =
      (!FLAGS_use_xrc || conn_config.fst_client_t ? kAppNumServers : 0);
  conn_config.rnum_threads = kAppNumServers;
  if (clt_gid == kAppNumClients) {
    conn_config.num_qps = 1;
    conn_config.rnum_threads = 1;
  }

  bool fst_client_t = conn_config.fst_client_t;
  hrd_ctrl_blk_t* cb;
  if (conn_config.use_xrc)
    cb = hrd_ctrl_blk_init_xrc(clt_gid, ib_port_index, 0, &conn_config, nullptr,
                               fst_client_t);
  else
    cb = hrd_ctrl_blk_init(clt_gid, ib_port_index, 0, &conn_config, nullptr);
  // Set to some non-zero value so the server can detect READ completion
  memset(const_cast<uint8_t*>(cb->conn_buf), 1, kAppBufSize);

  if (clt_gid != kAppNumClients) {
    for (size_t i = 0; i < kAppNumServers; i++) {
      char clt_qp_name[kHrdQPNameSize];
      sprintf(clt_qp_name, "client-%zu-%zu", clt_gid, i);
      hrd_publish_conn_qp(cb, i, clt_qp_name);
    }
    if (!cb->conn_config.use_xrc || fst_client_t) {
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
  int shm_key = kAppBaseSHMKey + clt_gid % num_threads;

  hrd_conn_config_t conn_config;
  //xrcd fd
  conn_config.xrcd_fd =
      open(SERVER_XRCD_FILE_PATH, O_RDONLY | O_CREAT, S_IRUSR | S_IRGRP);
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = shm_key;
  conn_config.is_client = true;
  conn_config.fst_client_t = (clt_gid % num_threads == 0);
  conn_config.num_qps = 0;
  if (clt_gid == kAppNumClients) {
    conn_config.rnum_threads = 1;
  } else {
    conn_config.rnum_threads = kAppNumServers;
  }

  bool fst_client_t = conn_config.fst_client_t;
  hrd_ctrl_blk_t* cb;
  cb = hrd_ctrl_blk_init_srm(clt_gid, ib_port_index, 0, &conn_config, nullptr,
                             conn_config.is_client, nullptr, nullptr);
  // Set to zero value so the server can detect WRITE completion
  memset(const_cast<uint8_t*>(cb->conn_buf), 0, kAppBufSize);

  if (clt_gid != kAppNumClients) {
    for (size_t i = 0; i < kAppNumServers; i++) {
      char clt_qp_name[kHrdQPNameSize];
      sprintf(clt_qp_name, "client-%zu-%zu", clt_gid, i);
      hrd_publish_conn_qp_srm(cb, i, clt_qp_name);
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
      // hrd_connect_qp_srm(cb, i, srv_qp);

      char clt_qp_name[kHrdQPNameSize];
      sprintf(clt_qp_name, "client-%zu-%zu", clt_gid, i);
      hrd_publish_ready(clt_qp_name);
    }
  } else {
    for (size_t i = 0; i < 1; i++) {
      char clt_qp_name[kHrdQPNameSize];
      sprintf(clt_qp_name, "client-%zu-%zu", clt_gid, kAppNumServers);
      hrd_publish_conn_qp_srm(cb, i, clt_qp_name);
    }
    for (size_t i = 0; i < 1; i++) {
      char srv_qp_name[kHrdQPNameSize];
      sprintf(srv_qp_name, "server-%zu-%zu", kAppNumServers, clt_gid);

      hrd_qp_attr_t* srv_qp = nullptr;
      while (srv_qp == nullptr) {
        srv_qp = hrd_get_published_qp(srv_qp_name);
        if (srv_qp == nullptr) usleep(20000);
      }
      printf("客户端lkey：%d，收到的rkey：%d\n", cb->conn_buf_mr->lkey,
             srv_qp->rkey);

      printf("main: Client %zu found server %zu! Connecting..\n", clt_gid, i);
      // hrd_connect_qp_srm(cb, i, srv_qp);

      char clt_qp_name[kHrdQPNameSize];
      sprintf(clt_qp_name, "client-%zu-%zu", clt_gid, kAppNumServers);
      hrd_publish_ready(clt_qp_name);
    }
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
  rt_assert(kAppNumClients % kAppNumClientMachines == 0,
            "NumClients must can be div by NumMachines");

  // 初始化wqe表
  std::ifstream infile("Twitter-cluster12_traffic_size.txt");
  int val;
  while (infile >> val) {
    traffic_size.push_back(val);
  }
  printf("traffic_size size:%d\n,traffic_size[0]:%d\n", traffic_size.size(),
         traffic_size[0]);

  // mmap表
  int fd, ret;
  struct user_table_info info;

  // 1. 分配页对齐的用户态内存（表）
  void* table =
      mmap(NULL, TABLE_SIZE, PROT_READ | PROT_WRITE,
           MAP_ANONYMOUS | MAP_SHARED | MAP_LOCKED,  // 锁定内存不被换出
           -1, 0);
  if (table == MAP_FAILED) {
    perror("wqe table mmap失败");
    return -1;
  }

  void* l_table =
      mmap(NULL, LEVEL_TABLE_SIZE, PROT_READ | PROT_WRITE,
           MAP_ANONYMOUS | MAP_SHARED | MAP_LOCKED,  // 锁定内存不被换出
           -1, 0);
  if (l_table == MAP_FAILED) {
    perror("level table mmap失败");
    munmap(table, TABLE_SIZE);
    return -1;
  }

  // 2. 初始化表数据
  wqe_table = (uint32_t*)table;
  for (int i = 0; i < TABLE_SIZE / sizeof(uint32_t); i++) {
    wqe_table[i] = 0;
  }

  level_table = (uint32_t*)l_table;
  for (int i = 0; i < LEVEL_TABLE_SIZE / sizeof(uint32_t); i++) {
    level_table[i] = 0;
  }

  info.table_addr = table;
  info.table_size = TABLE_SIZE;
  info.level_table_addr = l_table;
  info.level_table_size = LEVEL_TABLE_SIZE;

  // 创建idx table
  for (int i = 0; i < (kAppNumServers + FLAGS_test_lat_thread); i++) {
    for (int j = 0; j < NUM_SCHED; j++) {
      // 1. 分配页对齐的用户态内存（表）
      void* table =
          mmap(NULL, IDX_TABLE_SIZE, PROT_READ | PROT_WRITE,
               MAP_ANONYMOUS | MAP_SHARED | MAP_LOCKED,  // 锁定内存不被换出
               -1, 0);
      if (table == MAP_FAILED) {
        perror("idx table mmap失败");
        munmap(table, TABLE_SIZE);
        munmap(l_table, LEVEL_TABLE_SIZE);
        for (int k = 0; k <= i; k++) {
          for (int l = 0; l < j; l++) {
            munmap(index_table[k][l], IDX_TABLE_SIZE);
          }
        }
        return -1;
      }

      // 2. 初始化表数据
      index_table[i][j] = (struct idx_table_entry*)table;
      memset(index_table[i][j], 0, IDX_TABLE_SIZE);
      info.idx_table_addr[i][j] = index_table[i][j];
      info.idx_table_size[i][j] = IDX_TABLE_SIZE;
    }
  }

  // 创建并初始化哈希表
  for (int i = 0; i < (kAppNumServers + FLAGS_test_lat_thread); i++) {
    for (int j = 0; j < NUM_SCHED; j++) {
      void* table =
          mmap(NULL, HASH_TABLE_SIZE, PROT_READ | PROT_WRITE,
               MAP_ANONYMOUS | MAP_SHARED | MAP_LOCKED,  // 锁定内存不被换出
               -1, 0);
      if (table == MAP_FAILED) {
        perror("mmap失败");
        munmap(table, TABLE_SIZE);
        munmap(l_table, LEVEL_TABLE_SIZE);
        for (int i = 0; i < (kAppNumServers + FLAGS_test_lat_thread); i++) {
          for (int j = 0; j < NUM_SCHED; j++) {
            munmap(index_table[i][j], IDX_TABLE_SIZE);
          }
        }

        for (int k = 0; k <= i; k++) {
          for (int l = 0; l < j; l++) munmap(hash_table[k][l], HASH_TABLE_SIZE);
        }
        return -1;
      }

      // 2. 初始化表数据
      hash_table[i][j] = (struct hash_table_entry*)table;
      memset(hash_table[i][j], -1, HASH_TABLE_SIZE);

      info.hash_table_addr[i][j] = hash_table[i][j];
      info.hash_table_size[i][j] = HASH_TABLE_SIZE;
    }
  }

  info.hash_table_entry_num_per_bucket = HASH_TABLE_ENTRY_NUM_PER_BUCKET;

  printf("索引表和哈希表创建成功,idx entry大小:%d, hash entry大小:%d, idx table大小:%d, hash table大小:%d\n",
    sizeof(struct idx_table_entry),sizeof(struct hash_table_entry),IDX_TABLE_SIZE,HASH_TABLE_SIZE);

  // 3. 打开内核设备
  fd = open(DEV_PATH, O_RDWR);
  if (fd < 0) {
    perror("打开设备失败");
    munmap(table, TABLE_SIZE);
    munmap(l_table, LEVEL_TABLE_SIZE);
    for (int i = 0; i < (kAppNumServers + FLAGS_test_lat_thread); i++) {
      for (int j = 0; j < NUM_SCHED; j++) {
        munmap(index_table[i][j], IDX_TABLE_SIZE);
      }
    }
    for (int i = 0; i < (kAppNumServers + FLAGS_test_lat_thread); i++) {
      for (int j = 0; j < NUM_SCHED; j++)
        munmap(hash_table[i][j], HASH_TABLE_SIZE);
    }
    return -1;
  }

  // 4. 通过ioctl传递表信息给内核
  ret = ioctl(fd, REG_TABLE_TO_MLX5, &info);
  if (ret < 0) {
    perror("ioctl失败");
    close(fd);
    munmap(table, TABLE_SIZE);
    munmap(l_table, LEVEL_TABLE_SIZE);
    for (int i = 0; i < (kAppNumServers + FLAGS_test_lat_thread); i++) {
      for (int j = 0; j < (NUM_SCHED); j++) {
        munmap(index_table[i][j], IDX_TABLE_SIZE);
      }
    }
    for (int i = 0; i < (kAppNumServers + FLAGS_test_lat_thread); i++) {
      for (int j = 0; j < NUM_SCHED; j++)
        munmap(hash_table[i][j], HASH_TABLE_SIZE);
    }
    return -1;
  }

  // 初始化kqp_idx_arr表
  memset(kqp_idx_arr, -1, sizeof(kqp_idx_arr));

  // log_file = fopen("log.txt", "w");
  // if(log_file == NULL){
  //   perror("fopen失败");
  //   return -1;
  // }

  // 初始化随机数种子数组
  generate_random_seeds(seed_array, kAppNumServers + 1);

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
  double tput[num_threads], tput_Gbps[num_threads];

  if (FLAGS_use_srm && !FLAGS_is_client) {
    srm_cb = new hrd_ctrl_blk_t();
    memset(srm_cb, 0, sizeof(hrd_ctrl_blk_t));
    hrd_resolve_port_index(srm_cb, 0);
    srm_pd = ibv_alloc_pd(srm_cb->resolve.ib_ctx);
  }
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
      if (!FLAGS_use_srm)
        thread_arr[i] = std::thread(run_server, &param_arr[i]);
      else {
        thread_arr[i] = std::thread(run_server_srm, &param_arr[i]);
      }
    }
  }

  for (auto& t : thread_arr) t.join();
  if (FLAGS_use_srm) {
    ibv_dealloc_pd(srm_pd);
    ibv_close_device(srm_cb->resolve.ib_ctx);
  }

  close(fd);
  for (int i = 0; i < (kAppNumServers + FLAGS_test_lat_thread); i++) {
    for (int j = 0; j < 4; j++) {
      munmap(index_table[i][j], IDX_TABLE_SIZE);
    }
  }
  for (int i = 0; i < (kAppNumServers + FLAGS_test_lat_thread); i++) {
    munmap(hash_table[i], HASH_TABLE_SIZE);
  }

  // fclose(log_file);
  return 0;
}

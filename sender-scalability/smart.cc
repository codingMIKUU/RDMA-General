/*
 * The MIT License (MIT)
 *
 * Copyright (C) 2022-2023 Feng Ren, Tsinghua University 
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <unistd.h>

#include "smart.h"
#include <atomic>    // 新增
#include <thread>    // 新增
#include <chrono> 
#include <infiniband/verbs.h>
#include <vector>   // 新增
#include <unordered_map>
#include <algorithm>

size_t kAppNumClients=512;

// ---------------------------------------------------------------------
// Lightweight per-thread send-credit throttler for reuse in main.cc
// This block is self-contained and does not depend on Initiator/manager.
// ---------------------------------------------------------------------
namespace smartshim {


constexpr int kInfCreditValue = 256;
constexpr int kMaxThreads = 256;

static Config gcfg;
static std::atomic<bool> g_inited{false};
static std::atomic<bool> g_tuner_running{false};
static std::thread g_tuner;
static std::atomic<int> g_next_tid{0};
thread_local int g_tls_tid = -1;

struct ThreadLocal {
    std::atomic<int> credit;
    std::atomic<uint64_t> ack_req_estimation;
    std::vector<ibv_cq*> cqs;   // 本线程要轮询的CQ集合
    std::vector<uint8_t> cq_flag;  // 0=未获取过CQE，1=刚在before_post_send获取过CQE
    std::unordered_map<ibv_cq*, size_t> cq_index; // CQ到下标映射，避免遍历
    int rr_cursor;              // 轮询起点游标
    size_t* next_poll_at;
    size_t  next_poll_len;
    ThreadLocal() : credit(0), ack_req_estimation(0), rr_cursor(0), next_poll_at(nullptr), next_poll_len(0)  {}
};

static ThreadLocal g_tls[kMaxThreads];

static inline uint64_t rdtsc_local() {
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

inline int thread_id() {
    if (g_tls_tid < 0) {
        int id = g_next_tid.fetch_add(1, std::memory_order_relaxed);
        if (id >= kMaxThreads) id = kMaxThreads - 1;
        g_tls_tid = id;
        g_tls[g_tls_tid].credit.store(gcfg.initial_credit, std::memory_order_relaxed);
        // printf("[SMART] thread_id() assigned id=%d with initial credit=%d\n", g_tls_tid, gcfg.initial_credit);
        g_tls[g_tls_tid].ack_req_estimation.store(0, std::memory_order_relaxed);
    }
    return g_tls_tid;
}

void init(const Config& cfg) {
    if (g_inited.exchange(true)) return;
    gcfg = cfg;
    // fprintf(stderr, "[SMART] init: initial_credit=%d\n", gcfg.initial_credit);
}

void register_next_poll_table(size_t* next_poll_at, size_t len) {
  auto& tl = g_tls[thread_id()];
  tl.next_poll_at = next_poll_at;
  tl.next_poll_len = len;
}

bool has_credit(int wrs) {
  if (!gcfg.throttler) return true;
  auto& tl = g_tls[thread_id()];
  return tl.credit.load(std::memory_order_relaxed) >= wrs;
}

int get_credit() {
  auto& tl = g_tls[thread_id()];
  return tl.credit.load(std::memory_order_relaxed);
}


// 新增：注册CQ
void register_cq(ibv_cq* cq) {
  auto& tl = g_tls[thread_id()];
  tl.cqs.push_back(cq);
  tl.cq_flag.push_back(0);
  tl.cq_index[cq] = tl.cqs.size() - 1;
}

void register_cqs(ibv_cq** cqs, int n) {
  auto& tl = g_tls[thread_id()];
  tl.cqs.assign(cqs, cqs + n);
  tl.cq_flag.assign(n, 0);
  tl.cq_index.clear();
  for (int i = 0; i < n; ++i) tl.cq_index[tl.cqs[i]] = i;
  tl.rr_cursor = 0;
}

// 若标志为1（刚被before_post_send获取过CQE），则这次不建议poll
bool likely_cq_has_cqe(ibv_cq* cq) {
  auto& tl = g_tls[thread_id()];
  auto it = tl.cq_index.find(cq);
  if (it == tl.cq_index.end()) return true;    // 未注册，保守为true
  return tl.cq_flag[it->second] == 0;
}

// 复位标志位为0
void clear_cq_flag(ibv_cq* cq) {
  auto& tl = g_tls[thread_id()];
  auto it = tl.cq_index.find(cq);
  if (it != tl.cq_index.end()) tl.cq_flag[it->second] = 0;
}

int before_post_send(int wrs, ibv_cq* cq, ibv_wc* wc, size_t kAppUnsigBatch) {
  int ret=0;
  if (!gcfg.throttler) return 0;
  auto& tl = g_tls[thread_id()];
  // printf("[SMART] before_post_send(%d), credit=%d\n", wrs, tl.credit.load(std::memory_order_relaxed));
  // while (tl.credit.load(std::memory_order_relaxed) < wrs) {
  //   int rc = 0;
  //   if (!tl.cqs.empty()) {
  //     int n = static_cast<int>(tl.cqs.size());
  //     int start = tl.rr_cursor % n;
  //     bool got = false;
  //     for (int i = 0; i < n; ++i) {
  //       int idx = (start + i) % n;
  //       rc = ibv_poll_cq(tl.cqs[idx], kAppUnsigBatch, wc);
  //       if (rc > 0) {
  //         tl.rr_cursor = idx + 1;     // 下次从下一个CQ开始
  //         if(rc==kAppUnsigBatch) tl.cq_flag[idx] = 1;        // 记录：该CQ刚获取过CQE
  //         got = true;
  //         // printf("有积分\n");
  //         break;
  //       }
  //     }
  //     if (!got) {
  //       std::this_thread::yield();
  //       // printf("没积分\n");
  //       continue;
  //     }
  //   } else {
  //     // 未注册列表时，仅轮询传入的CQ
  //     rc = ibv_poll_cq(cq, 1, wc);
  //     if (rc <= 0) {
  //       std::this_thread::yield();
  //       continue;
  //     }
  //   }

  //   // 回收积分
  //   tl.credit.fetch_add(rc, std::memory_order_relaxed);
  //   tl.ack_req_estimation.fetch_add(rc, std::memory_order_relaxed);
  //   ret = 2;
  //   if (wc->wr_id == kAppNumClients - 1) ret = 1;
  // }
  // printf("[SMART] before_post_send(%d), credit=%d\n", wrs, tl.credit.load(std::memory_order_relaxed));
    while (tl.credit.load(std::memory_order_relaxed) < wrs) {
    int rc = 0;
    int polled_idx = -1;

    if (!tl.cqs.empty()) {
      int n = (int)tl.cqs.size();
      int start = tl.rr_cursor % n;
      bool got = false;
      for (int i = 0; i < n; ++i) {
        int idx = (start + i) % n;
        rc = ibv_poll_cq(tl.cqs[idx], (int)kAppUnsigBatch, wc);
        if (rc > 0) {
          tl.rr_cursor = idx + 1;
          if (rc == (int)kAppUnsigBatch) tl.cq_flag[idx] = 1;
          polled_idx = idx;  // 记录本次被轮询到的 CQ 的索引
          got = true;
          break;
        }
      }
      if (!got) { std::this_thread::yield(); continue; }
    } else {
      rc = ibv_poll_cq(cq, 1, wc);
      if (rc <= 0) { std::this_thread::yield(); continue; }
      // 尝试映射传入 cq -> idx（若这个 cq 也在注册表里）
      auto it = tl.cq_index.find(cq);
      if (it != tl.cq_index.end()) polled_idx = (int)it->second;
    }

    // 回收积分
    tl.credit.fetch_add(rc, std::memory_order_relaxed);
    tl.ack_req_estimation.fetch_add(rc, std::memory_order_relaxed);

    // 动态推进目标 QP 的 next_poll_at
    if (polled_idx >= 0 && tl.next_poll_at && (size_t)polled_idx < tl.next_poll_len) {
      tl.next_poll_at[polled_idx] += (size_t)rc;
    }

    ret = 2;
    if (wc->wr_id == kAppNumClients - 1) ret = 1;
  }
  // printf("有积分了 before_post_send(%d), credit=%d\n", wrs, tl.credit.load(std::memory_order_relaxed));
  tl.credit.fetch_sub(wrs, std::memory_order_relaxed);
  return ret;
}

void on_cq_completions(int n) {
    if (!gcfg.throttler) return;
    auto& tl = g_tls[thread_id()];
    tl.credit.fetch_add(n, std::memory_order_relaxed);
    tl.ack_req_estimation.fetch_add(n, std::memory_order_relaxed);
    // printf("[SMART] on_cq_completions(%d), credit=%d\n", n, tl.credit.load(std::memory_order_relaxed));
}

void start_tuner() {
    if (!gcfg.throttler || !gcfg.throttler_auto_tuning) return;
    if (g_tuner_running.exchange(true)) return;

    g_tuner = std::thread([](){
        const int kCreditStep = gcfg.credit_step;
        const int kMaxCreditValue = gcfg.max_credit;
        const int kExecutionEpochs = gcfg.execution_epochs;
        const uint64_t kSamplingCycles = gcfg.sample_cycles;
        const double kInfCreditWeight = gcfg.inf_credit_weight;

        uint64_t epoch_clock = rdtsc_local();
        int credit_bound = gcfg.initial_credit;
        bool is_training = true;
        int exec_epochs = 0;

        uint64_t best_ack_req = 0;
        int best_max_credit = -1;
        usleep(12000);

        while (g_tuner_running.load(std::memory_order_relaxed)) {
            int credit_delta = 0;
            uint64_t curr_clock = rdtsc_local();
            while (curr_clock - epoch_clock < kSamplingCycles) {
                usleep(1000);
                curr_clock = rdtsc_local();
                if (!g_tuner_running.load()) return;
            }
            double factor = (curr_clock - epoch_clock) * 1.0 / kSamplingCycles;
            epoch_clock = curr_clock;

            uint64_t ack_req_sum = 0;
            int threads_now = g_next_tid.load(std::memory_order_relaxed);
            for (int i = 0; i < threads_now; ++i) {
                ack_req_sum += g_tls[i].ack_req_estimation.exchange(0, std::memory_order_relaxed);
            }
            ack_req_sum = static_cast<uint64_t>(ack_req_sum / (factor > 0 ? factor : 1.0));

            if (ack_req_sum == 0) {
                usleep(1000);
                continue;
            }

            if (!is_training) {
                ++exec_epochs;
                if (exec_epochs == kExecutionEpochs) {
                    is_training = true;
                    best_ack_req = 0;
                    best_max_credit = -1;
                    credit_delta = gcfg.initial_credit - credit_bound;
                    credit_bound = gcfg.initial_credit;
                    exec_epochs = 0;
                }
            } else {
                if (credit_bound == kInfCreditValue) {
                    if (best_ack_req < static_cast<uint64_t>(ack_req_sum * kInfCreditWeight)) {
                        // keep inf
                    } else {
                        credit_delta = best_max_credit - credit_bound;
                        credit_bound = best_max_credit;
                    }
                    is_training = false;
                    exec_epochs = 0;
                } else {
                    if (best_ack_req < ack_req_sum) {
                        best_ack_req = ack_req_sum;
                        best_max_credit = credit_bound;
                    }
                    if (credit_bound == kMaxCreditValue) {
                        credit_delta = kInfCreditValue - credit_bound;
                    } else {
                        credit_delta = kCreditStep;
                    }
                    credit_bound += credit_delta;
                }
            }

            if (credit_delta != 0) {
                int threads_now2 = g_next_tid.load(std::memory_order_relaxed);
                for (int i = 0; i < threads_now2; ++i) {
                    g_tls[i].credit.fetch_add(credit_delta, std::memory_order_relaxed);
                }
            }
        }
    });
}

void stop_tuner() {
    if (!g_tuner_running.exchange(false)) return;
    if (g_tuner.joinable()) g_tuner.join();
}

} // namespace smartshim

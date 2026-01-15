#pragma once
#include <cstdint>
#include <infiniband/verbs.h>

namespace smartshim {

struct Config {
  bool throttler = true;
  bool throttler_auto_tuning = true;
  int  initial_credit = 4;//12
  int  credit_step = 2;//8
  int  max_credit =12;//48
  int  execution_epochs = 60;
  uint64_t sample_cycles = 19200000;
  double inf_credit_weight = 1.05;
};

// Initialize throttler with config. Call once before threads run.
void init(const Config& cfg);
// Start/stop the background tuner thread (optional if auto_tuning=false)
void start_tuner();
void stop_tuner();
// 只读检查当前线程是否有足够积分
bool has_credit(int wrs);
// 可选：读取当前线程积分
int get_credit();

// 注册当前线程要轮询的 CQ（可多次调用，追加进列表）
void register_cq(ibv_cq* cq);
// 一次性注册一组 CQ
void register_cqs(ibv_cq** cqs, int n);
// 判断某 CQ 是否“可能有”CQE（基于 before_post_send 最近一轮的轮询结果）
bool likely_cq_has_cqe(ibv_cq* cq);

// 清除某 CQ 的标志（置 0，表示准备进入下一轮发送/轮询）
void clear_cq_flag(ibv_cq* cq);

// Call before actually invoking ibv_post_send(). wrs = number of WQEs you are posting now.
int before_post_send(int wrs, ibv_cq* cq, ibv_wc* wc, size_t kAppUnsigBatch);
// 向 smartshim 注册当前线程的 next_poll_at 表（长度为该线程 QP 数）
void register_next_poll_table(size_t* next_poll_at, size_t len);

// Call after polling CQ. n = number of completions you got from this CQ poll.
void on_cq_completions(int n);
// 记录热点CQ：该SQ刚post了wqe_count个WQE，后续需要从此CQ收集wqe_count个CQE
void record_hot_cq(ibv_cq* cq, int wqe_count);

} // namespace smartshim

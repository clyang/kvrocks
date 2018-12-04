#ifndef KVROCKS_STATS_H
#define KVROCKS_STATS_H

#include <map>
#include <atomic>
#include <string>
#include <unistd.h>

class Stats {
 public:
  std::atomic<uint64_t> total_calls = {0};
  std::atomic<uint64_t> total_latency = {0};
  std::atomic<uint64_t> in_bytes = {0};
  std::atomic<uint64_t> out_bytes = {0};

  std::atomic<uint64_t> fullsync_counter = {0};
  std::atomic<uint64_t> psync_err_counter = {0};
  std::atomic<uint64_t> psync_ok_counter = {0};

 public:
  void IncrCalls() { total_calls.fetch_add(1, std::memory_order_relaxed);}
  void AddLatency(uint64_t latency) { total_latency.fetch_add(latency, std::memory_order_relaxed);}
  void AddInbondBytes(uint64_t bytes) { in_bytes.fetch_add(bytes, std::memory_order_relaxed); }
  void AddOutbondBytes(uint64_t bytes) { out_bytes.fetch_add(bytes, std::memory_order_relaxed); }
  void IncrFullSyncCounter() { fullsync_counter.fetch_add(1, std::memory_order_relaxed); }
  void IncrPSyncErrCounter() { psync_err_counter.fetch_add(1, std::memory_order_relaxed); }
  void IncrPSyncOKCounter() { psync_ok_counter.fetch_add(1, std::memory_order_relaxed); }
  static long GetMemoryRSS();
};

#endif //KVROCKS_STATS_H
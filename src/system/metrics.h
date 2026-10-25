// Background metrics service: samples CPU/RAM on a std::jthread into a
// double-buffered snapshot, mirrors it into SharedState and wakes the UI
// thread via a non-blocking PostMessage (WM_APP_DATA_UPDATED).
#pragma once
#include "common.h"
#include "content.h"
#include "sampler.h"

#include <chrono>
#include <mutex>
#include <thread>

// AppStateSnapshot: immutable frame of sampled data shared between threads.
struct AppStateSnapshot {
  SysStats stats{};
  uint64_t seq = 0;
};

class MetricsService {
 public:
  void start(HWND notify, SharedState* st) {
    notify_ = notify;
    st_ = st;
    th_ = std::jthread([this](std::stop_token tok) { loop(std::move(tok)); });
  }
  void stop() {
    if (th_.joinable()) {
      th_.request_stop();
      th_.join();
    }
  }
  ~MetricsService() { stop(); }

  // Latest snapshot: reads the front buffer while the writer fills the back.
  AppStateSnapshot latest() const {
    std::lock_guard<std::mutex> lk(m_);
    return bufs_[readIdx_];
  }

 private:
  void loop(std::stop_token tok) {
    Sampler sampler;
    uint64_t seq = 0;
    while (!tok.stop_requested()) {
      AppStateSnapshot snap;
      snap.seq = ++seq;
      sampler.sample(snap.stats);
      // first sample has no CPU delta yet — take a quick second one
      if (seq == 1) {
        Sleep(300);
        snap.seq = ++seq;
        sampler.sample(snap.stats);
      }
      publish(snap);
      for (int i = 0; i < 10 && !tok.stop_requested(); i++) Sleep(100);  // 1 s
    }
  }

  void publish(const AppStateSnapshot& snap) {
    {
      std::lock_guard<std::mutex> lk(m_);
      int next = readIdx_ ^ 1u;
      bufs_[next] = snap;
      readIdx_ = next;  // readers keep reading the other buffer safely
    }
    if (!st_ || !notify_) return;
    // mirror into SharedState for existing consumers (pipe status, lines)
    AcquireSRWLockExclusive(&st_->lock);
    st_->stats = snap.stats;
    st_->histCpu.push_back(snap.stats.cpuPercent);
    st_->histRam.push_back(snap.stats.ramPercent);
    while (st_->histCpu.size() > 120) st_->histCpu.pop_front();
    while (st_->histRam.size() > 120) st_->histRam.pop_front();
    ReleaseSRWLockExclusive(&st_->lock);
    st_->dirty.store(true, std::memory_order_relaxed);
    PostMessageW(notify_, WM_APP_DATA_UPDATED, 0, 0);  // never blocks the UI
  }

  mutable std::mutex m_;
  AppStateSnapshot bufs_[2]{};
  unsigned readIdx_ = 0;
  std::jthread th_;
  HWND notify_ = nullptr;
  SharedState* st_ = nullptr;
};

// clang-format off
// Copyright 2026 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#pragma once

#include <atomic_expensive_wait_cheap_notify_simple.h>
#include <atomic_wrappers.h>
#include <heavy_fence_clock.h>
#include <utils.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

namespace v5 {

/*
 * everything rcu except for reclamation, on top of heavy_fence_clock.
 */
class rcu_reading_subsystem {
 public:
  class tls;

  // Counter representing ordering in the system
  using rcu_time_t = tools::heavy_fence_clock::count_t;

  // exposed so that the rcu system itself can decide when to advance the clock
  tools::heavy_fence_clock& clock() { return clock_; }

  // retirement support ----------------------

  // Call after no new readers can access the retired object.
  // returns `retired_at` for this object.
  rcu_time_t initiate_retire_wait() const;

  // Tasks with retired_at <= known_safe_retire() can run.
  rcu_time_t known_safe_retire() const;

  // returns false iff already in progress.
  bool try_refresh_safe_retire();

  // blocking -----------------------------

  void synchronize();

 private:
  // reader_tls_vec_m_ has to be locked.
  // Calls on_reader(tls&, started_at) for every reader that is reading.
  template <typename OnReader>
  void refresh_safe_retire_impl(OnReader on_reader);

  tools::heavy_fence_clock clock_;
  // no known safe retires in the beginning
  tools::atomic<rcu_time_t> known_safe_retire_{0};

  tools::mutex reader_tls_vec_m_;
  std::vector<tls*> reader_tls_vec_;
};

class rcu_reading_subsystem::tls : tools::nomove {
 public:
  explicit tls(rcu_reading_subsystem& s) : subsystem_(&s) {
    tools::lock_guard _{subsystem_->reader_tls_vec_m_};
    subsystem_->reader_tls_vec_.push_back(this);
  }

  ~tls() {
    tools::lock_guard _{subsystem_->reader_tls_vec_m_};
    std::iter_swap(std::ranges::find(subsystem_->reader_tls_vec_, this),
                   std::prev(subsystem_->reader_tls_vec_.end()));
    subsystem_->reader_tls_vec_.pop_back();
  }

  void enter() {
    if (nested_readers_++) [[unlikely]] {
      return;
    }

    started_at_.store(subsystem_->clock_.now(), tools::memory_order_relaxed);
    tools::asymmetric_thread_fence_light();
  }

  void exit() {
    if (--nested_readers_) [[unlikely]] {
      return;
    }

    tools::asymmetric_thread_fence_light();
    started_at_.store(0, tools::memory_order_relaxed);

    waiter_.notify_one();
  }

  // 0 means not reading.
  rcu_time_t started_reading_at() const {
    return started_at_.load(tools::memory_order_relaxed);
  }

  // At most one waiter
  void wait_if_started_before(rcu_time_t before) {
    rcu_time_t started_at = started_reading_at();
    if (!started_at || started_at >= before) {
      return;
    }
    waiter_.wait(started_at);
  }

 private:
  tools::atomic<rcu_time_t> started_at_{0};

  std::uint32_t nested_readers_ = 0;
  tools::atomic_expensive_wait_cheap_notify_simple<rcu_time_t> waiter_{
      &started_at_};

  rcu_reading_subsystem* subsystem_;
};

inline auto rcu_reading_subsystem::initiate_retire_wait() const -> rcu_time_t {
  return clock_.light_fence().in_the_future;
}

inline auto rcu_reading_subsystem::known_safe_retire() const -> rcu_time_t {
  return known_safe_retire_.load(tools::memory_order_acquire);
}

inline bool rcu_reading_subsystem::try_refresh_safe_retire() {
  tools::unique_lock lock{reader_tls_vec_m_, tools::try_to_lock};
  if (!lock) {
    return false;
  }
  refresh_safe_retire_impl([](tls&, rcu_time_t) {});
  return true;
}

template <typename OnReader>
inline void rcu_reading_subsystem::refresh_safe_retire_impl(
    OnReader on_reader) {
  // if we won't find any readers, we can be the "oldest found reader"
  // we just need to take now + light_fence.
  rcu_time_t res = clock_.now();
  tools::asymmetric_thread_fence_light();

  for (tls* r : reader_tls_vec_) {
    rcu_time_t started_at = r->started_reading_at();
    if (started_at) {
      res = std::min(res, started_at);
      on_reader(*r, started_at);
    }
  }

  // > is actually possible, because sometimes a reader will read a stale time.
  // it's a known false positive, the reader doesn't actually read the data
  // we already confirmed to be fully retired.
  rcu_time_t previous_known =
      known_safe_retire_.load(tools::memory_order_relaxed);
  if (previous_known >= res) {
    return;
  }

  // We need a heavy fence here to ensure that everything from the readers
  // is visible in retires.
  clock_.heavy_fence_ensure();
  known_safe_retire_.store(res, tools::memory_order_release);
}

inline void rcu_reading_subsystem::synchronize() {
  rcu_time_t initial_fence = clock_.heavy_fence_ensure();
  
  // Waiting here caused bugs that were hard to catch.
  tools::important_pause_to_test();

  tools::lock_guard _{reader_tls_vec_m_};

  // the needed synchronize happened while we were acquiring locks.
  if (known_safe_retire_.load(tools::memory_order_acquire) >= initial_fence) {
    return;
  }

  // First pass - we don't immediately wait, we let the readers finish
  // while we are collecting everyone.
  // It's also a refresh: known_safe_retire_ moves up to the oldest reader.
  std::vector<tls*> waiting;
  waiting.reserve(reader_tls_vec_.size());
  refresh_safe_retire_impl([&](tls& r, rcu_time_t started_at) {
    if (started_at < initial_fence) {
      waiting.push_back(&r);
    }
  });

  // The refresh might have been enough.
  if (known_safe_retire_.load(tools::memory_order_relaxed) >= initial_fence) {
    return;
  }

  // Now we wait
  for (tls* r : waiting) {
    r->wait_if_started_before(initial_fence);
  }

  // Everything the readers did is before this heavy fence.
  clock_.heavy_fence_ensure();
  known_safe_retire_.store(initial_fence, tools::memory_order_release);
}

}  // namespace v5

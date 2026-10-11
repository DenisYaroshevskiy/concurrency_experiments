// clang-format off
// Copyright 2026 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#pragma once

#include <atomic_expensive_wait_cheap_notify_simple.h>
#include <atomic_wrappers.h>
#include <utils.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

namespace v4 {

/*
 * everything rcu except for reclamation.
 *
 * Internal idea:
 * Intead of typical "generations" it uses the idea of "clock"
 * Between every two ticks of the clock we have a heavy_fence.
 *
 * The "clock" gets advanced once in a while (triggered externally)
 *
 * Readers, entering the critical section, indicate which "rcu_time_t": did they
 * enter.
 *
 * Once in a while a time of the latest entered reader gets updated.
 */
class rcu_reading_subsystem {
 public:
  using rcu_time_t = std::uint64_t;

  static constexpr rcu_time_t kVisibilityLag = 3;

  class tls;

  rcu_time_t current_time() const {
    return current_time_.load(tools::memory_order_relaxed);
  }
  rcu_time_t oldest_known_reader() const {
    return oldest_known_reader_.load(tools::memory_order_acquire);
  }

  static bool readers_done_with(rcu_time_t update_time,
                                rcu_time_t oldest_reader) {
    return update_time + kVisibilityLag <= oldest_reader;
  }

  // advancing clock --------
  bool try_advance_clock();
  void advance_clock();  // blocking

  bool try_update_oldest_reader();
  void wait_until_no_one_is_reading(rcu_time_t stamped_before);

  void synchronize();

 private:
  tools::mutex reader_tls_vec_m_;
  std::vector<tls*> reader_tls_vec_;

  tools::mutex current_time_m_;
  tools::atomic<rcu_time_t> current_time_{1};

  tools::atomic<rcu_time_t> oldest_known_reader_{1};
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
    rcu_time_t now = subsystem_->current_time();
    rcu_time_t cur = started_at_.load(tools::memory_order_relaxed);

    if (cur) [[unlikely]] {
      ++nested_readers_;
      return;
    }

    started_at_.store(now, tools::memory_order_relaxed);
    tools::asymmetric_thread_fence_light();
  }
  void exit() {
    if (nested_readers_) [[unlikely]] {
      --nested_readers_;
      return;
    }

    // This light fence is to communicate with the heavy fences after
    // observing that the reader is done.
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
    rcu_time_t first_seen = started_at_.load(tools::memory_order_relaxed);
    if (first_seen == 0 || first_seen >= before) {
      return;
    }
    waiter_.wait(first_seen);
  }

 private:
  // There is a false sharing in theory here but
  // this is not the case where we expect it to be relevant.
  tools::atomic<rcu_time_t> started_at_{0};
  std::uint32_t nested_readers_ = 0;
  tools::atomic_expensive_wait_cheap_notify_simple<rcu_time_t> waiter_{
      &started_at_};

  rcu_reading_subsystem* subsystem_;
};

inline bool rcu_reading_subsystem::try_advance_clock() {
  tools::unique_lock lock{current_time_m_, tools::try_to_lock};
  if (!lock) {
    return false;
  }
  tools::asymmetric_thread_fence_heavy();
  current_time_.store(current_time_.load(tools::memory_order_relaxed) + 1,
                      tools::memory_order_relaxed);
  return true;
}

inline void rcu_reading_subsystem::advance_clock() {
  tools::lock_guard _{current_time_m_};
  tools::asymmetric_thread_fence_heavy();
  current_time_.store(current_time_.load(tools::memory_order_relaxed) + 1,
                      tools::memory_order_relaxed);
}

inline bool rcu_reading_subsystem::try_update_oldest_reader() {
  tools::unique_lock lock{reader_tls_vec_m_, tools::try_to_lock};
  if (!lock) {
    return false;
  }

  rcu_time_t oldest = current_time_.load(tools::memory_order_relaxed);

  for (const tls* r : reader_tls_vec_) {
    rcu_time_t started_at = r->started_reading_at();
    if (started_at) {
      oldest = std::min(oldest, started_at);
    }
  }

  // A reader with a stale stamp can drag the minimum back but it still sees
  // everything that the previous watermark covered.
  if (oldest > oldest_known_reader_.load(tools::memory_order_relaxed)) {
    oldest_known_reader_.store(oldest, tools::memory_order_relaxed);
  }
  return true;
}

inline void rcu_reading_subsystem::wait_until_no_one_is_reading(
    rcu_time_t stamped_before) {
  tools::lock_guard _{reader_tls_vec_m_};

  // Only wait for the readers that are reading now.
  std::vector<tls*> waiting;
  waiting.reserve(reader_tls_vec_.size());
  std::ranges::copy_if(reader_tls_vec_, std::back_inserter(waiting),
                       [stamped_before](const tls* x) {
                         rcu_time_t started_at = x->started_reading_at();
                         return 0 < started_at && started_at < stamped_before;
                       });

  for (tls* r : waiting) {
    r->wait_if_started_before(stamped_before);
  }
}

inline void rcu_reading_subsystem::synchronize() {
  advance_clock();
  wait_until_no_one_is_reading(current_time());

  advance_clock();
}

}  // namespace v4

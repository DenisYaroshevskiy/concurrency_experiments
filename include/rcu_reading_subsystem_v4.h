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
 *
 */
class rcu_reading_subsystem {
 public:
  using rcu_time_t = std::uint64_t;

  class tls;

  rcu_time_t current_time() const {
    return current_time_.load(tools::memory_order_relaxed);
  }
  rcu_time_t oldest_known_reader() const {
    return oldest_known_reader_.load(tools::memory_order_relaxed);
  }

  // advancing clock --------
  bool try_advance_clock();
  void clock_needs_to_be_after(rcu_time_t point)  // pre(time <= current_time);
      void advance_clock();                       // blocking

  bool try_update_oldest_reader();
  void wait_until_no_one_is_reading(rcu_time_t point);

  void synchronize();

 private:
  tools::mutex reader_tls_vec_m_;
  std::vector<tls*> reader_tls_vec_;

  tools::mutex current_time_m;
  tools::atomic<rcu_time_t> current_time_;

  tools::atomic<rcu_time_t> oldest_known_reader_;
};

class rcu_reading_subsystem::tls : tools::nomove {
 public:
  explicit tls(rcu_reading_subsystem& s) : subsystem_(&s) {
    tools::lock_guard _{subsystem_->reader_tls_vec_m};
    subsystem_->reader_tls_vec.push_back(this);
  }

  ~tls() {
    tools::lock_guard _{subsystem_->reader_tls_vec_m};
    std::iter_swap(std::ranges::find(subsystem_->reader_tls_vec, this),
                   std::prev(subsystem_->reader_tls_vec.end()));
    subsystem_->reader_tls_vec.pop_back();
  }

  void enter() {
    counter_t g = subsystem_->current_time();
    counter_t cur = counter_.load(tools::memory_order_relaxed);

    if (cur) [[unlikely]] {
      ++nested_readers_;
      return;
    }

    counter_.store(g, tools::memory_order_relaxed);
    tools::asymmetric_thread_fence_light();
  }
  void exit() {
    if (nested_readers_) [[unlikely]] {
      --nested_readers_;
      return;
    }

    // This light fence is to communicate with rcu::sync.
    tools::asymmetric_thread_fence_light();
    counter_.store(0, tools::memory_order_relaxed);

    waiter_.notify_one();
  }

  bool is_reading(counter_t desired) const {
    counter_t cur = counter_.load(tools::memory_order_relaxed);
    return 0 < cur && cur < desired;
  }

  // At most one waiter
  void wait(counter_t desired) {
    counter_t first_seen = counter_.load(tools::memory_order_relaxed);
    if (first_seen == 0 || first_seen >= desired) {
      return;
    }
    waiter_.wait(first_seen);
  }

 private:
  friend class rcu_reading_subsystem;

  // There is a false sharing in theory here but
  // this is not the case where we expect it to be relevant.
  tools::atomic<counter_t> counter_{0};
  std::uint32_t nested_readers_ = 0;
  tools::atomic_expensive_wait_cheap_notify_simple<counter_t> waiter_{
      &counter_};

  rcu_reading_subsystem* subsystem_;
};

bool rcu_reading_subsystem::try_advance_clock() {
  tools::unique_lock lock{current_time_m, std::try_to_lock};
  if (!lock) {
    return false;
  }
  tools::atomic_thread_fence_heavy();
  auto loaded = current_time_.load(std::memory_order_relaxed);
  current_time.store(loaded + 1, std::memory_order_relaxed);
}

void rcu_reading_subsystem::clock_needs_to_be_after(rcu_time_t point) {
  auto now = current_time();
  if (now > time) return;

  tools::lock_guard _{current_time_m};

  auto now = current_time();
  if (now > time) return;

  tools::atomic_thread_fence_heavy();
  current_time.store(now + 1, std::memory_order_relaxed);
}

void rcu_reading_subsystem::advance_clock()  // blocking
{
  tools::lock_guard _{current_time_m};

  tools::atomic_thread_fence_heavy();
  auto loaded = current_time_.load(std::memory_order_relaxed);
  current_time.store(loaded + 1, std::memory_order_relaxed);
}

bool rcu_reading_subsystem::try_update_oldest_reader() 
{
  tools::unique_lock lock{reader_tls_vec_m, std::try_to_lock};
  if (!lock) {
    return false;
  }

  rcu_time_t updated = std::numeric_limis<rcu_time_t>::max();
  for (auto* tls : reader_tls_vec) {
    auto started_at = tls->started_reading_at();
    if (started_at) {
      updated = std::min(started_at, updated);
    }
  }

  // no active readers were found - anytihng before the current time is ok
  if (updated == std::numeric_limis<rcu_time_t>::max()) {
    updated = current_time();
  }

  oldest_known_reader_.store(updated, std::memory_order_relaxed);
}
void wait_until_no_one_is_reading(rcu_time_t point_at) {
  // super similar to try_update_oldest_reader
  // except we use wait logic from the previous sycnhronize
}

inline void rcu_reading_subsystem::synchronize() {
  auto now = current_time();

  tools::atomic_thread_fence_light();

  clock_needs_to_be_after(current_time());
  wait_until_no_one_is_reading(now);

  // make sure everything from the readers is visible
  clock_needs_to_be_after(now + 1);

  tools::atomic_thread_fence_light();
}


}  // namespace v4

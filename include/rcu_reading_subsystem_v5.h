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

class rcu_reading_subsystem {
 public:
  class tls;

  // Counter representing ordering in the system
  using rcu_time_t = heavy_fence_clock::counter_t;

  // exposed so that the rcu system itself can decide when to advance the clock
  heavy_fence_clock& clock() { return clock_; }

  // retirement support ----------------------

  // returns a rcu_time when it's safe to execute a retired task
  rcu_time_t initiate_retire_wait() const;
  rcu_time_t known_safe_retire() const;

  void try_refresh_safe_retire();

  // blocking -----------------------------

  void synchronize();

 private:
  tools::heavy_fence_clock clock_;
  std::atomic<rcu_time_t> known_safe_retire_;

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
    ++nested_readers_;
    if (nested_readers_ > 1) [[unlikely]] {
      return;
    }

    started_at_.store(subsystem_->clock().light_fence().in_the_past,
                      std::memory_order_relaxed);
  }

  void exit() {
    --nested_readers_;
    if (nested_readers_ > 0) [[unlikely]] {
      return;
    }

    (void)subsystem_->clock().light_fence();
    started_at_.store(0, std::memory_order_relaxed);

    waiter_.notify_one();
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
  tools::atomic<rcu_time_t> started_at_{0};

  std::uint32_t nested_readers_ = 0;
  tools::atomic_expensive_wait_cheap_notify_simple<rcu_time_t> waiter_{
      &started_at_};

  rcu_reading_subsystem* subsystem_;
};


inline auto initiate_retire_wait() const -> rcu_time_t {
  return clock().light_fence().in_the_future;
}

inline auto known_safe_retire() const -> rcu_time_t {
  return known_safe_retire_.load(tools::memory_order_relaxed);
}

void rcu_reading_subsystem::try_refresh_safe_retire() {
  tools::unique_lock lock{reader_tls_vec_m_, tools::try_to_lock};
  if (!lock) {
    return false;
  }
  
  // If there are no readers while we checking but readers after we checked,
  // we need to know what was safe before
  rcu_time_t res = clock_.light_fence().in_the_past;

  for (const tls* r : reader_tls_vec_) {
    rcu_time_t started_at = r->started_reading_at();
    if (started_at) {
      res = std::min(res, started_at);
    }
  }
 
   // > is actually possible, because sometimes a reader will read a stale time.
   // it's a known false positive that might occur but it's not scary.
   auto previous_known = known_safe_retire_.load(tools::memory_order_relaxed);
   if (previous_known >= res) {
    return;
   }

   // We need a heavy fence here to ensure that everything from the readers
   // is visible in retires.
   clock.heavy_fence_ensure();
   known_safe_retire_.store(res, tools::memory_order_relaxed);
}

inline void rcu_reading_subsystem::synchronize() {}


}  // namespace v5

// clang-format off
// Copyright 2026 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#pragma once

#include <algorithms.h>
#include <atomic_wrappers.h>
#include <owner_stealer.h>
#include <rcu_reading_subsystem_v5.h>
#include <utils.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <vector>

/*
 * rcu on top of rcu_reading_subsystem v5. No background thread.
 *
 * retire() stamps the task with initiate_retire_wait() and puts it into a
 * per-thread mailbox. Every retire the owner runs its own ready tasks
 * (retired_at <= known_safe_retire()).
 * Every kAdvanceClockEvery retires the thread advances the clock.
 * Every kRefreshEvery retires it refreshes known_safe_retire and steals
 * stale tasks: the ones that were safe kStaleLag clock advances ago and the
 * owner didn't run them (the owner stopped retiring).
 * Both steps can also be called directly: advance_clock(), refresh_and_steal().
 *
 * barrier() takes all of the tasks, synchronizes and runs them.
 *
 * reader_tls and reclaim_tls are independent.
 * Most threads only need reader_tls.
 */
namespace v5 {

struct rcu_domain : rcu_reading_subsystem {
  using clean_up_task = std::move_only_function<void()>;

  static constexpr std::uint32_t kAdvanceClockEvery = 5;
  static constexpr std::uint32_t kRefreshEvery = 10;
  static constexpr rcu_time_t kStaleLag = 5;

  using reader_tls = rcu_reading_subsystem::tls;
  struct reclaim_tls;

  ~rcu_domain() { barrier(); }

  void barrier();

 private:
  static constexpr rcu_time_t kNoTasks = std::numeric_limits<rcu_time_t>::max();

  struct stamped_task {
    rcu_time_t retired_at_;
    clean_up_task task_;
  };
  using task_list = std::vector<stamped_task>;

  struct reclaim_mailbox {
    // Oldest task the owner hasn't run, kNoTasks if none.
    // Only a hint: a steal racing with the owner can make it wrong
    // until the owner's next take_ready.
    tools::atomic<rcu_time_t> oldest_pending_{kNoTasks};
    tools::owner_stealer<task_list> tasks_;
  };

  static bool is_stale(rcu_time_t oldest_pending, rcu_time_t known_safe) {
    return oldest_pending != kNoTasks &&
           oldest_pending + kStaleLag <= known_safe;
  }

  task_list take_ready(reclaim_mailbox& m, task_list& v) const;
  task_list steal_stale(const reclaim_mailbox* self, rcu_time_t known_safe);
  static void move_all(task_list& from, task_list& to);
  static void run(task_list& v);

  tools::mutex reclaim_mailbox_vec_m_;
  std::vector<tools::shared_ptr<reclaim_mailbox>> reclaim_mailbox_vec_;
};

struct rcu_domain::reclaim_tls : tools::nomove {
  rcu_domain* domain_;
  tools::shared_ptr<reclaim_mailbox> mailbox_;
  std::uint32_t retired_count_ = 0;

  explicit reclaim_tls(rcu_domain& d) : domain_(&d) {
    mailbox_ = tools::make_shared<reclaim_mailbox>();
    tools::lock_guard _{d.reclaim_mailbox_vec_m_};
    d.reclaim_mailbox_vec_.push_back(mailbox_);
  }

  template <typename T, typename D = std::default_delete<T>>
  void retire(T* x, D d = {}) {
    rcu_time_t retired_at = domain_->initiate_retire_wait();

    task_list ready;
    mailbox_->tasks_.owner_access([&](task_list& v) {
      v.push_back({retired_at,
                   clean_up_task([x, d = std::move(d)]() mutable { d(x); })});
      ready = domain_->take_ready(*mailbox_, v);
    });
    run(ready);

    ++retired_count_;
    if (retired_count_ % kRefreshEvery == 0) {
      refresh_and_steal();
      return;
    }
    if (retired_count_ % kAdvanceClockEvery == 0) {
      advance_clock();
    }
  }

  // The periodic steps of retire.
  void advance_clock() { domain_->clock().heavy_fence_try(); }
  void refresh_and_steal();
};

// Owner only.
inline rcu_domain::task_list rcu_domain::take_ready(reclaim_mailbox& m,
                                                    task_list& v) const {
  rcu_time_t known_safe = known_safe_retire();
  rcu_time_t oldest_pending = kNoTasks;

  task_list res;
  res.reserve(v.size());
  // partition_to calls the predicate exactly once per task.
  v.erase(tools::partition_to(
              v.begin(), v.end(), std::back_inserter(res),
              [&](const stamped_task& t) {
                if (t.retired_at_ <= known_safe) {
                  return true;
                }
                oldest_pending = std::min(oldest_pending, t.retired_at_);
                return false;
              }),
          v.end());

  m.oldest_pending_.store(oldest_pending, tools::memory_order_relaxed);
  return res;
}

// Takes the tasks of stale and dead mailboxes, evicts the dead ones
// (use_count == 1 means owning reclaim_tls destroyed).
inline rcu_domain::task_list rcu_domain::steal_stale(
    const reclaim_mailbox* self, rcu_time_t known_safe) {
  task_list res;
  auto move_tasks = [&](task_list& v) { move_all(v, res); };

  tools::lock_guard _{reclaim_mailbox_vec_m_};
  std::erase_if(reclaim_mailbox_vec_, [&](const auto& x) {
    if (x.get() == self) return false;

    // Nobody else is going to run these.
    bool dead = x.use_count() == 1;
    if (dead) {
      return x->tasks_.try_stealer_access(move_tasks);
    }

    rcu_time_t pending = x->oldest_pending_.load(tools::memory_order_relaxed);
    if (!is_stale(pending, known_safe)) return false;
    if (x->tasks_.try_stealer_access(move_tasks)) {
      // If the owner published something new meanwhile, keep it.
      x->oldest_pending_.compare_exchange_strong(pending, kNoTasks,
                                                 tools::memory_order_relaxed,
                                                 tools::memory_order_relaxed);
    }
    return false;
  });
  return res;
}

inline void rcu_domain::move_all(task_list& from, task_list& to) {
  to.insert(to.end(), std::make_move_iterator(from.begin()),
            std::make_move_iterator(from.end()));
  from.clear();
}

inline void rcu_domain::run(task_list& v) {
  for (auto& t : v) t.task_();
}

inline void rcu_domain::reclaim_tls::refresh_and_steal() {
  // Stale is measured against what was known before this refresh.
  rcu_time_t previous_known = domain_->known_safe_retire();
  domain_->try_refresh_safe_retire();

  // Stolen tasks keep their stamps: not all of them have to be ready.
  // They go into our mailbox and we run the ready ones.
  task_list stolen = domain_->steal_stale(mailbox_.get(), previous_known);

  task_list ready;
  mailbox_->tasks_.owner_access([&](task_list& v) {
    move_all(stolen, v);
    ready = domain_->take_ready(*mailbox_, v);
  });
  run(ready);
}

// Collect from every mailbox, waiting for the ones that are busy.
// Also evicts dead entries (use_count == 1 means owning reclaim_tls destroyed).
inline void rcu_domain::barrier() {
  task_list todo;
  auto move_tasks = [&](task_list& v) { move_all(v, todo); };

  {
    tools::lock_guard _{reclaim_mailbox_vec_m_};
    std::vector<reclaim_mailbox*> busy;
    std::erase_if(reclaim_mailbox_vec_, [&](const auto& x) {
      bool dead = x.use_count() == 1;
      if (!x->tasks_.try_stealer_access(move_tasks)) {
        busy.emplace_back(x.get());
        return false;
      }
      return dead;
    });
    for (auto* b : busy) {
      b->tasks_.blocking_stealer_access(move_tasks);
    }
  }

  // Everything we took was retired before the call.
  synchronize();
  run(todo);
}

}  // namespace v5

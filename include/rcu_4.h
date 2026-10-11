// clang-format off
// Copyright 2026 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#pragma once

#include <algorithms.h>
#include <atomic_wrappers.h>
#include <owner_stealer.h>
#include <rcu_reading_subsystem_v4.h>
#include <utils.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <memory>
#include <vector>

/*
 * Clock based rcu (see rcu_reading_subsystem_v4.h).
 *
 * retire() stamps the task with the current rcu time and puts it into a
 * per-thread mailbox. The task can be run once the readers are done with that
 * time (oldest_known_reader watermark).
 * On every retire the owner runs its own ready tasks - that's just a load of
 * the watermark, no synchronization.
 *
 * reclaim_tls::background_task() advances the clock, updates the watermark,
 * steals the tasks from the other mailboxes into its own and runs the ones that
 * are ready. Any thread with a reclaim_tls can do it.
 *
 * barrier() takes all of the tasks, synchronizes and runs them.
 *
 * reader_tls and reclaim_tls are independent.
 * Most threads only need reader_tls.
 */
namespace v4 {

struct rcu_domain : rcu_reading_subsystem {
  using clean_up_task = std::move_only_function<void()>;

  using reader_tls = rcu_reading_subsystem::tls;
  struct reclaim_tls;

  ~rcu_domain() { barrier(); }

  void barrier();

 private:
  struct stamped_task {
    rcu_time_t retired_at_;
    clean_up_task task_;
  };
  using task_list = std::vector<stamped_task>;
  using reclaim_mailbox = tools::owner_stealer<task_list>;

  task_list take_ready(task_list& v) const;
  static void run(task_list& v);

  tools::mutex reclaim_mailbox_vec_m_;
  std::vector<tools::shared_ptr<reclaim_mailbox>> reclaim_mailbox_vec_;
};

struct rcu_domain::reclaim_tls : tools::nomove {
  rcu_domain* domain_;
  tools::shared_ptr<reclaim_mailbox> mailbox_;

  explicit reclaim_tls(rcu_domain& d) : domain_(&d) {
    mailbox_ = tools::make_shared<reclaim_mailbox>();
    tools::lock_guard _{d.reclaim_mailbox_vec_m_};
    d.reclaim_mailbox_vec_.push_back(mailbox_);
  }

  template <typename T, typename D = std::default_delete<T>>
  void retire(T* x, D d = {}) {
    tools::asymmetric_thread_fence_light();
    rcu_time_t retired_at = domain_->current_time();

    task_list ready;
    mailbox_->owner_access([&](task_list& v) {
      v.push_back({retired_at,
                   clean_up_task([x, d = std::move(d)]() mutable { d(x); })});
      ready = domain_->take_ready(v);
    });
    run(ready);
  }

  void background_task();
};

inline rcu_domain::task_list rcu_domain::take_ready(task_list& v) const {
  rcu_time_t oldest = oldest_known_reader();

  task_list res;
  res.reserve(v.size());
  v.erase(tools::partition_to(v.begin(), v.end(), std::back_inserter(res),
                              [&](const stamped_task& t) {
                                return readers_done_with(t.retired_at_, oldest);
                              }),
          v.end());
  return res;
}

inline void rcu_domain::run(task_list& v) {
  for (auto& t : v) t.task_();
}

inline void rcu_domain::reclaim_tls::background_task() {
  domain_->advance_clock();
  domain_->try_update_oldest_reader();

  task_list stolen;
  auto move_tasks = [&](task_list& v) {
    stolen.insert(stolen.end(), std::make_move_iterator(v.begin()),
                  std::make_move_iterator(v.end()));
    v.clear();
  };

  {
    tools::lock_guard _{domain_->reclaim_mailbox_vec_m_};
    for (auto& x : domain_->reclaim_mailbox_vec_) {
      if (x.get() != mailbox_.get()) x->try_stealer_access(move_tasks);
    }
  }

  task_list ready;
  mailbox_->owner_access([&](task_list& v) {
    v.insert(v.end(), std::make_move_iterator(stolen.begin()),
             std::make_move_iterator(stolen.end()));
    ready = domain_->take_ready(v);
  });
  run(ready);
}

// Collect from every mailbox, waiting for the ones that are busy.
// Also evicts dead entries (use_count == 1 means owning reclaim_tls destroyed).
inline void rcu_domain::barrier() {
  task_list todo;
  auto move_tasks = [&](task_list& v) {
    todo.insert(todo.end(), std::make_move_iterator(v.begin()),
                std::make_move_iterator(v.end()));
    v.clear();
  };

  {
    tools::lock_guard _{reclaim_mailbox_vec_m_};
    std::vector<reclaim_mailbox*> busy;
    std::erase_if(reclaim_mailbox_vec_, [&](const auto& x) {
      bool dead = x.use_count() == 1;
      if (!x->try_stealer_access(move_tasks)) {
        busy.emplace_back(x.get());
        return false;
      }
      return dead;
    });
    for (auto* b : busy) {
      b->blocking_stealer_access(move_tasks);
    }
  }

  synchronize();
  run(todo);
}

}  // namespace v4

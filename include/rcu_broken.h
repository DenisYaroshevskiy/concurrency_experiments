// clang-format off
// Copyright 2025 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#pragma once

#include <atomic_wrappers.h>
#include <utils.h>

#include <algorithm>
#include <memory>
#include <vector>

/*
 * +1 entry,
 * +1 exit
 * simplest fully funcitonal design.
 */

namespace v_broken {

struct rcu_domain {
  using counter_t = std::uint64_t;

  struct tls;

  void synchronize();

  tools::mutex readers_mutex;
  std::vector<tls*> readers;
};

struct rcu_domain::tls : tools::nomove {
  tools::atomic<counter_t> counter;
  rcu_domain* domain_;

  explicit tls(rcu_domain& domain) : domain_(&domain) {
    tools::lock_guard _{domain_->readers_mutex};
    domain_->readers.push_back(this);
  }
  ~tls() {
    tools::lock_guard _{domain_->readers_mutex};
    std::erase(domain_->readers, this);
   }

  void enter() { counter.fetch_add(1); }
  void exit() { counter.fetch_add(1); }
};

inline void rcu_domain::synchronize() {
  tools::lock_guard _{readers_mutex};

  for (const tls* reader : readers) {
    counter_t cur = reader->counter.load();

    if (cur % 2 == 0) continue;

    while (reader->counter.load() == cur) {
      tools::this_thread_yield();
    }
  }
}

}  // namespace v_broken

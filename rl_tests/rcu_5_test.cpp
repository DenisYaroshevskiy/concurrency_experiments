// clang-format off
// Copyright 2026 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#include "relacy/context.hpp"
#include "relacy/thread_local.hpp"
#define TOOLS_RL_TEST
#include "rcu_5.h"

#include "rcu_rl_tests.h"

// Thread 0 retires once and stops retiring, but keeps its reclaim_tls.
// Thread 1 keeps retiring: thread 0's task becomes stale and thread 1 can
// steal it.
template <typename Domain>
struct rcu_test_steal_stale
    : rcu_test_base<rcu_test_steal_stale, Domain, 2> {
  static constexpr int kRetires = 8;

  rl::atomic<int> deleted{0};
  rl::atomic<int> done{0};

  static void count(rl::atomic<int>* p) {
    p->fetch_add(1, rl::memory_order_relaxed);
  }

  void thread_(unsigned idx) {
    auto tls = this->make_reclaim_tls();
    if (idx == 0) {
      this->retire(tls, &deleted, &count);
      while (!done.load(rl::memory_order_relaxed)) {
        rl::yield(1, $);
      }
      return;
    }

    for (int i = 0; i != kRetires; ++i) {
      this->retire(tls, &deleted, &count);
    }
    done.store(1, rl::memory_order_relaxed);
  }

  void after() {
    this->barrier();
    RL_ASSERT(deleted.load(rl::memory_order_relaxed) == kRetires + 1);
  }
};

int main() {
  return (full_test<v5::rcu_domain>()
       && simulate<rcu_test_retire_many<v5::rcu_domain>>()
       && simulate<rcu_test_steal_stale<v5::rcu_domain>>()) ? 0 : 1;
}

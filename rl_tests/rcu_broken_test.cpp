// clang-format off
// Copyright 2025 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#include "relacy/context.hpp"
#include "relacy/thread_local.hpp"

#define TOOLS_RL_TEST
#include "rcu_broken.h"

#include "rl_simulate.h"

namespace v_broken {

struct rcu_smoke_test : rl::test_suite<rcu_smoke_test, 2> {
  using config = rl::var<int>;

  rl::atomic<const config*> cfg;

  rcu_domain domain;

  void before() { cfg.store(new config(1)); }

  void thread(unsigned idx) {
    if (idx == 0) {  // updater
      auto* old = cfg.exchange(new config(2), rl::memory_order_acq_rel);
      domain.synchronize();
      delete old;
    } else {  // reader
      rcu_domain::tls tls{domain};

      tls.enter();
      int read = (*cfg.load(rl::memory_order_acquire))($);
      tls.exit();

      RL_ASSERT(read == 1 || read == 2);
    }
  }

  void after() {
    auto* last_cfg = cfg.load(rl::memory_order_acquire);
    RL_ASSERT((*last_cfg)($) == 2);
    delete last_cfg;
  }
};

}  // namespace v_broken

int main() { return simulate<v_broken::rcu_smoke_test>() ? 0 : 1; }

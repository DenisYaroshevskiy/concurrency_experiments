// clang-format off
// Copyright 2025 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#define TOOLS_RL_TEST

#include <relacy/relacy.hpp>
#include <relacy/test_suite.hpp>
#include <relacy/var.hpp>

#include "rl_simulate.h"

#include "atrocious_mutex.h"

/*
https://eel.is/c++draft/intro.races


If a value computation A of an atomic object M happens before a value
computation B of M, and A takes its value from a side effect X on M, then the
value computed by B is either the value stored by X or the value stored by a
side effect Y on M, where Y follows X in the modification order of M.
*/
template <typename M>
struct mutex_gives_barrier : rl::test_suite<mutex_gives_barrier<M>, 3> {
  M m;

  rl::atomic<bool> event_a = false;
  rl::atomic<bool> event_b = false;

  rl::var<bool> first_to_the_mutex{true};

  void thread(unsigned i) {
    if (i == 0) {
      event_a.store(true, rl::memory_order_relaxed);
      return;
    }

    [&] {
      rl::lock_guard _{m};

      if (!first_to_the_mutex($)) {
        return;
      }

      first_to_the_mutex($) = false;
      if (event_a.load(rl::memory_order_relaxed)) {
        event_b.store(true, rl::memory_order_relaxed);
      }
    }();

    if (event_b.load(rl::memory_order_relaxed)) {
      RL_ASSERT(event_a.load(rl::memory_order_relaxed));
    }
  }
};

int main() {
  return simulate_exhaustive<mutex_gives_barrier<rl::mutex>>() &&
         simulate_exhaustive<mutex_gives_barrier<tools::atrocious_mutex>>()
             ? 0
             : 1;
}

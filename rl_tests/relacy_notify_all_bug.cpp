// clang-format off
// Copyright 2026 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#include <relacy/relacy.hpp>
#include <relacy/test_suite.hpp>

template <typename Test>
bool simulate_exhaustive() {
  rl::test_params params;
  params.search_type = rl::fair_full_search_scheduler_type;
  return rl::simulate<Test>(params);
}

template <bool UseNotifyAll>
struct notify_one_vs_all : rl::test_suite<notify_one_vs_all<UseNotifyAll>, 2> {
  rl::atomic<bool> flag{true};

  void thread(unsigned idx) {
    if (idx == 0) {
      flag.store(false, rl::memory_order_relaxed);
      if constexpr (UseNotifyAll) {
        flag.notify_all();
      } else {
        flag.notify_one();
      }
    } else {
      flag.wait(true, rl::memory_order_relaxed);
    }
  }
};

int main() {
  return simulate_exhaustive<notify_one_vs_all<false>>() &&
         simulate_exhaustive<notify_one_vs_all<true>>()
             ? 0
             : 1;
}

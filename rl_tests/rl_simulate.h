// clang-format off
// Copyright 2025 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#ifndef RL_SIMULATE_H
#define RL_SIMULATE_H

#include <relacy/relacy.hpp>

template <typename Test>
bool simulate(int iterations = 100'000) {
  rl::test_params params;
  params.iteration_count = iterations;
  return rl::simulate<Test>(params);
}

// Every schedule with at most context_bound preemptions.
// Finds bugs that need a specific interleaving the random scheduler misses.
template <typename Test>
bool simulate_context_bound(unsigned context_bound) {
  rl::test_params params;
  params.search_type = rl::fair_context_bound_scheduler_type;
  params.context_bound = context_bound;
  return rl::simulate<Test>(params);
}

template <typename Test>
bool simulate_exhaustive() {
  rl::test_params params;
  params.search_type = rl::fair_full_search_scheduler_type;
  return rl::simulate<Test>(params);
}

#endif  // RL_SIMULATE_H

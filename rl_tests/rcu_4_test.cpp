// clang-format off
// Copyright 2026 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#include "relacy/context.hpp"
#include "relacy/thread_local.hpp"
#define TOOLS_RL_TEST
#include "rcu_4.h"

#include "rcu_rl_tests.h"

int main() {
  return (full_test<v4::rcu_domain>()
       && simulate<rcu_test_retire_many<v4::rcu_domain>>()) ? 0 : 1;
}

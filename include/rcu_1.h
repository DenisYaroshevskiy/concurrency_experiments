// clang-format off
// Copyright 2025 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#pragma once

#include <rcu_reading_subsystem_v1.h>
#include <utils.h>

namespace v1 {

struct rcu_domain : rcu_reading_subsystem {
  using reader_tls = rcu_reading_subsystem::tls;
  struct reclaim_tls;

  void barrier() { synchronize(); }


};

struct rcu_domain::reclaim_tls : tools::nomove {
  rcu_domain* domain_ = nullptr;

  explicit reclaim_tls(rcu_domain& d) : domain_(&d) {}

  template <typename T, typename D = std::default_delete<T>>
  void retire(T* x, D d = {}) {
    domain_->synchronize();
    d(x);
  }
};

}  // namespace v1

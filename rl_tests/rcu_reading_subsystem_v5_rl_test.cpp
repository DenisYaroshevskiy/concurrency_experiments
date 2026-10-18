// clang-format off
// Copyright 2026 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#define TOOLS_RL_TEST

#include <relacy/relacy.hpp>
#include <relacy/test_suite.hpp>
#include <relacy/var.hpp>

#include "rl_simulate.h"

#include "rcu_reading_subsystem_v5.h"

#include "rcu_rl_tests.h"

#include <array>

using v5::rcu_reading_subsystem;
using rcu_time_t = rcu_reading_subsystem::rcu_time_t;

// reviewed tests --------------------------------------------------------

// Different ways to wait for the readers.
// refresh_loop: wait until the retire is safe, only through try_refresh.
enum class synchronize_api { synchronize, refresh_loop };

template <synchronize_api Variation>
void call_synchronize(rcu_reading_subsystem& rcu) {
  if constexpr (Variation == synchronize_api::synchronize) {
    rcu.synchronize();
  } else {
    rcu_time_t retired_at = rcu.initiate_retire_wait();
    while (rcu.known_safe_retire() < retired_at) {
      rcu.try_refresh_safe_retire();
    }
  }
}

template <template <synchronize_api> class Test>
bool with_each_synchronize_api() {
  return simulate<Test<synchronize_api::synchronize>>()
      && simulate<Test<synchronize_api::refresh_loop>>();
}

// The config is a relaxed atomic: only rcu orders the reader and the writer.
// A reader that saw the old config reads data, the writer overwrites data
// after waiting for the readers.
template <synchronize_api Variation>
struct relaxed_config : rl::test_suite<relaxed_config<Variation>, 2> {
  rcu_reading_subsystem rcu;
  rl::atomic<int> config{0};
  rl::var<int> data{1};

  void thread(unsigned idx) {
    if (idx == 0) {
      rcu_reading_subsystem::tls tls{rcu};
      tls.enter();
      if (config.load(rl::memory_order_relaxed) == 0) {
        RL_ASSERT(data($) == 1);
      }
      tls.exit();
      return;
    }

    config.store(1, rl::memory_order_relaxed);
    call_synchronize<Variation>(rcu);
    data($) = -1;
  }
};

// unreviewed tests ------------------------------------------------------

// Just enough of a domain for the synchronize tests from rcu_rl_tests.h.
template <synchronize_api Variation>
struct sync_only_domain : rcu_reading_subsystem {
  using reader_tls = rcu_reading_subsystem::tls;
  struct reclaim_tls {};  // not used by the synchronize tests

  void synchronize() { call_synchronize<Variation>(*this); }
};

template <template <typename> class Test>
bool with_each_sync_only_domain() {
  return simulate<Test<sync_only_domain<synchronize_api::synchronize>>>()
      && simulate<Test<sync_only_domain<synchronize_api::refresh_loop>>>();
}

using config_t = rl::var<int>;

// relacy doesn't treat delete as a write: write to catch a missing
// happens-before with the readers.
inline void destroy(config_t* p) {
  (*p)($) = -1;
  delete p;
}

// Readers read the config twice, each time in a new critical section,
// so that they restamp themselves while the writer is waiting.
template <typename Test>
struct readers_base : rl::test_suite<Test, 3> {
  rcu_reading_subsystem rcu;
  rl::atomic<config_t*> config{nullptr};

  void before() { config.store(new config_t(1), rl::memory_order_release); }

  void read() {
    rcu_reading_subsystem::tls tls{rcu};
    for (int i = 0; i != 2; ++i) {
      tls.enter();
      config_t* loaded = config.load(rl::memory_order_acquire);
      int val = (*loaded)($);
      tls.exit();
      RL_ASSERT(val == 1 || val == 2);
    }
  }

  config_t* unlink() {
    return config.exchange(new config_t(2), rl::memory_order_acq_rel);
  }

  void after() { destroy(config.load(rl::memory_order_relaxed)); }
};

// The writer drives the clock and the refresh itself.
struct retire_self_driven : readers_base<retire_self_driven> {
  void thread(unsigned idx) {
    if (idx != 0) {
      read();
      return;
    }

    config_t* old = unlink();
    rcu_time_t retired_at = rcu.initiate_retire_wait();
    while (rcu.known_safe_retire() < retired_at) {
      rcu.clock().heavy_fence_unconditional();
      rcu.try_refresh_safe_retire();
    }
    destroy(old);
  }
};

// One thread advances the clock and refreshes.
// The writer only looks at known_safe_retire().
struct retire_background_refresh
    : readers_base<retire_background_refresh> {
  rl::atomic<int> done{0};

  void thread(unsigned idx) {
    if (idx == 1) {
      read();
      return;
    }

    if (idx == 2) {
      while (!done.load(rl::memory_order_relaxed)) {
        rcu.clock().heavy_fence_try();
        rcu.try_refresh_safe_retire();
      }
      return;
    }

    config_t* old = unlink();
    rcu_time_t retired_at = rcu.initiate_retire_wait();
    while (rcu.known_safe_retire() < retired_at) {
    }
    destroy(old);
    done.store(1, rl::memory_order_relaxed);
  }
};

// synchronize() publishes known_safe_retire too: retire relying only on it,
// while another thread refreshes.
template <synchronize_api Variation>
struct retire_through_synchronize
    : readers_base<retire_through_synchronize<Variation>> {
  void thread(unsigned idx) {
    if (idx == 1) {
      this->read();
      return;
    }

    if (idx == 2) {
      this->rcu.clock().heavy_fence_unconditional();
      this->rcu.try_refresh_safe_retire();
      return;
    }

    config_t* old = this->unlink();
    rcu_time_t retired_at = this->rcu.initiate_retire_wait();
    while (this->rcu.known_safe_retire() < retired_at) {
      call_synchronize<Variation>(this->rcu);
    }
    destroy(old);
  }
};

// Two writers retire, both refresh: the refreshes race on known_safe_retire.
struct retire_two_writers : readers_base<retire_two_writers> {
  rl::atomic<int> unlinked{0};

  void thread(unsigned idx) {
    if (idx == 2) {
      read();
      return;
    }

    config_t* old = unlink();
    rcu_time_t retired_at = rcu.initiate_retire_wait();
    while (rcu.known_safe_retire() < retired_at) {
      rcu.clock().heavy_fence_try();
      rcu.try_refresh_safe_retire();
    }
    destroy(old);
  }
};

// Readers can only make known_safe_retire smaller than the clock.
struct known_safe_behind_clock : rl::test_suite<known_safe_behind_clock, 3> {
  rcu_reading_subsystem rcu;

  void thread(unsigned idx) {
    if (idx == 0) {
      rcu_reading_subsystem::tls tls{rcu};
      tls.enter();
      tls.exit();
      tls.enter();
      tls.exit();
      return;
    }

    rcu_time_t last = 0;
    for (int i = 0; i != 2; ++i) {
      rcu.clock().heavy_fence_unconditional();
      rcu.try_refresh_safe_retire();
      rcu_time_t known = rcu.known_safe_retire();
      RL_ASSERT(last <= known);
      RL_ASSERT(known <= rcu.clock().now());
      last = known;
    }
  }
};

// The nested exit doesn't end the critical section.
template <synchronize_api Variation>
struct nested_read_sync : readers_base<nested_read_sync<Variation>> {
  void thread(unsigned idx) {
    if (idx == 2) {
      rcu_reading_subsystem::tls tls{this->rcu};
      tls.enter();
      config_t* outer = this->config.load(rl::memory_order_acquire);
      tls.enter();
      config_t* inner = this->config.load(rl::memory_order_acquire);
      int inner_val = (*inner)($);
      tls.exit();
      int outer_val = (*outer)($);
      tls.exit();
      RL_ASSERT(inner_val == 1 || inner_val == 2);
      RL_ASSERT(outer_val == 1 || outer_val == 2);
      return;
    }

    if (idx == 1) {
      this->read();
      return;
    }

    config_t* old = this->unlink();
    call_synchronize<Variation>(this->rcu);
    destroy(old);
  }
};

// synchronize() publishes known_safe_retire_: the published value can only
// be a heavy fence the scan of the readers is ordered after.
// Publishing a later count, that came from a relaxed load, lets the scan miss
// a reader that might not see a retire before that count.
// The writer also moves the clock after taking retired_at.
// It frees only if known_safe_retire() says so, otherwise after() does.
// Needs a specific interleaving: run with the context bound scheduler.
struct synchronize_publishes_scanned
    : rl::test_suite<synchronize_publishes_scanned, 3> {
  rcu_reading_subsystem rcu;
  rl::atomic<config_t*> config{nullptr};
  config_t* not_freed = nullptr;
  // Outside of the threads: keeps the tls mutex out of the search.
  rcu_reading_subsystem::tls reader{rcu};

  void before() { config.store(new config_t(1), rl::memory_order_release); }

  void thread(unsigned idx) {
    if (idx == 0) {
      rcu.synchronize();
    } else if (idx == 1) {
      reader.enter();
      config_t* loaded = config.load(rl::memory_order_acquire);
      int val = (*loaded)($);
      reader.exit();
      RL_ASSERT(val == 1 || val == 2);
    } else {
      config_t* old =
          config.exchange(new config_t(2), rl::memory_order_acq_rel);
      rcu_time_t retired_at = rcu.initiate_retire_wait();
      rcu.clock().heavy_fence_unconditional();
      // Let synchronize publish before we look.
      rl::yield(1, $);
      if (rcu.known_safe_retire() >= retired_at) {
        destroy(old);
        return;
      }
      not_freed = old;
    }
  }

  void after() {
    delete not_freed;
    delete config.load(rl::memory_order_relaxed);
  }
};

int main() {
  return (with_each_synchronize_api<relaxed_config>()
       && simulate<rcu_test_no_mutation<
              sync_only_domain<synchronize_api::synchronize>>>()
       && with_each_sync_only_domain<rcu_test_access_and_sync>()
       && with_each_sync_only_domain<rcu_test_min_sync>()
       && with_each_sync_only_domain<rcu_test_enter_twice>()
       && with_each_sync_only_domain<rcu_test_proper_sync>()
       && with_each_sync_only_domain<rcu_test_sync_delete>()
       && with_each_synchronize_api<nested_read_sync>()
       && simulate<retire_self_driven>()
       && simulate<retire_background_refresh>()
       && with_each_synchronize_api<retire_through_synchronize>()
       && simulate<retire_two_writers>()
       && simulate<known_safe_behind_clock>()
       && simulate_context_bound<synchronize_publishes_scanned>(2)) ? 0 : 1;
}

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

#include "heavy_fence_clock.h"

#include <array>

using count_t = tools::heavy_fence_clock::count_t;

inline constexpr count_t kInitial =
    tools::heavy_fence_clock::kInitialClockValue;

// Heavy fence n is the one right before the count is set to n.
// With one thread doing the heavy fences its k-th fence is kInitial + k.

// The heavy fence apis.
// With one thread doing the heavy fences all of them fence every time:
// ensure always sees count < in_the_future and try is retried, since
// try_lock is allowed to fail spuriously (relacy does that).
enum class heavy_fence_api { unconditional, try_, ensure };

template <heavy_fence_api Variation>
void call_heavy_fence(tools::heavy_fence_clock& c) {
  if constexpr (Variation == heavy_fence_api::unconditional) {
    c.heavy_fence_unconditional();
  } else if constexpr (Variation == heavy_fence_api::try_) {
    while (!c.heavy_fence_try()) {
    }
  } else {
    c.heavy_fence_ensure();
  }
}

// Every heavy fence adds exactly one. light_fence() never goes back.
struct clock_counts_fences : rl::test_suite<clock_counts_fences, 3> {
  tools::heavy_fence_clock clock;
  int tries_succeeded = 0;

  void thread(unsigned idx) {
    if (idx == 0) {
      clock.heavy_fence_unconditional();
      clock.heavy_fence_unconditional();
    } else if (idx == 1) {
      tries_succeeded += clock.heavy_fence_try();
      tries_succeeded += clock.heavy_fence_try();
    } else {
      count_t last_past = 0;
      for (int i = 0; i != 3; ++i) {
        auto [past, future] = clock.light_fence();
        RL_ASSERT(past + 2 <= future);
        RL_ASSERT(last_past <= past);
        last_past = past;
      }
    }
  }

  void after() {
    // 2 unconditional fences + the successful tries.
    RL_ASSERT(clock.light_fence().in_the_past ==
              tools::heavy_fence_clock::kInitialClockValue + 2 +
                  static_cast<count_t>(tries_succeeded));
  }
};

// in_the_past: what was before heavy fence n is visible after a light fence
// that returned in_the_past >= n.
template <heavy_fence_api Variation>
struct heavy_to_light_ordering
    : rl::test_suite<heavy_to_light_ordering<Variation>, 3> {
  tools::heavy_fence_clock clock;
  rl::atomic<int> x{0};
  rl::atomic<int> y{0};

  std::array<count_t, 3> past{};
  std::array<int, 3> seen_x{};
  std::array<int, 3> seen_y{};

  void thread(unsigned idx) {
    if (idx == 0) {
      x.store(1, rl::memory_order_relaxed);
      call_heavy_fence<Variation>(clock);  // kInitial + 1
      y.store(1, rl::memory_order_relaxed);
      call_heavy_fence<Variation>(clock);  // kInitial + 2
      return;
    }
    past[idx] = clock.light_fence().in_the_past;
    seen_x[idx] = x.load(rl::memory_order_relaxed);
    seen_y[idx] = y.load(rl::memory_order_relaxed);
  }

  void after() {
    for (unsigned i = 1; i != 3; ++i) {
      if (past[i] >= kInitial + 1) RL_ASSERT(seen_x[i] == 1);
      if (past[i] >= kInitial + 2) RL_ASSERT(seen_y[i] == 1);
    }
  }
};

// in_the_future: what was before a light fence is visible after heavy fence
// in_the_future.
template <heavy_fence_api Variation>
struct light_to_heavy_ordering
    : rl::test_suite<light_to_heavy_ordering<Variation>, 3> {
  static constexpr count_t kFences = 4;
  static constexpr count_t kLastFence = kInitial + kFences;

  tools::heavy_fence_clock clock;
  std::array<rl::atomic<int>, 2> x{0, 0};

  std::array<count_t, 2> future{};
  // seen[n][i]: x[i] after heavy fence n.
  std::array<std::array<int, 2>, kLastFence + 1> seen{};

  void thread(unsigned idx) {
    if (idx == 2) {
      for (count_t n = kInitial + 1; n <= kLastFence; ++n) {
        call_heavy_fence<Variation>(clock);
        for (unsigned i = 0; i != x.size(); ++i) {
          seen[n][i] = x[i].load(rl::memory_order_relaxed);
        }
      }
      return;
    }
    x[idx].store(1, rl::memory_order_relaxed);
    future[idx] = clock.light_fence().in_the_future;
  }

  void after() {
    for (unsigned i = 0; i != x.size(); ++i) {
      for (count_t n = future[i]; n <= kLastFence; ++n) {
        RL_ASSERT(seen[n][i] == 1);
      }
    }
  }
};

// heavy_fence_ensure: there is a heavy fence after we entered, on this thread
// or on another one. It's no later than in_the_past of the next light fence,
// so a reader that's past that number sees what was before ensure.
// Variation is what the other thread uses for its heavy fences.
template <heavy_fence_api Variation>
struct ensure_orders_before_readers
    : rl::test_suite<ensure_orders_before_readers<Variation>, 3> {
  tools::heavy_fence_clock clock;
  rl::atomic<int> x{0};

  count_t ensured = 0;
  count_t reader_past = 0;
  int seen_x = 0;

  void thread(unsigned idx) {
    if (idx == 0) {
      x.store(1, rl::memory_order_relaxed);
      clock.heavy_fence_ensure();
      ensured = clock.light_fence().in_the_past;
    } else if (idx == 1) {
      // Someone else's fences, so that ensure can skip its own.
      for (int i = 0; i != 2; ++i) {
        call_heavy_fence<Variation>(clock);
      }
    } else {
      reader_past = clock.light_fence().in_the_past;
      seen_x = x.load(rl::memory_order_relaxed);
    }
  }

  void after() {
    if (reader_past >= ensured) RL_ASSERT(seen_x == 1);
  }
};

// Two threads communicate through the heavy fences of a background thread.
// Each one writes its x, does a light fence and reads the other's x.
// The write is before the light fence, so it's ordered before heavy fence
// in_the_future. The read is after the light fence, so it's ordered after
// heavy fence in_the_past.
// If j's in_the_past >= i's in_the_future, j sees i's write.
template <heavy_fence_api Variation>
struct light_to_light_through_heavy
    : rl::test_suite<light_to_light_through_heavy<Variation>, 3> {
  static constexpr int kFences = 4;

  tools::heavy_fence_clock clock;
  std::array<rl::atomic<int>, 2> x{0, 0};

  std::array<tools::heavy_fence_clock::light_fence_return, 2> fences{};
  // seen[i]: the other's x, as seen by i.
  std::array<int, 2> seen{};

  void thread(unsigned idx) {
    if (idx == 2) {
      for (int i = 0; i != kFences; ++i) {
        call_heavy_fence<Variation>(clock);
      }
      return;
    }
    unsigned other = 1 - idx;
    x[idx].store(1, rl::memory_order_relaxed);
    fences[idx] = clock.light_fence();
    seen[idx] = x[other].load(rl::memory_order_relaxed);
  }

  void after() {
    // 0 -> 1
    if (fences[1].in_the_past >= fences[0].in_the_future) {
      RL_ASSERT(seen[1] == 1);
    }
    // 1 -> 0
    if (fences[0].in_the_past >= fences[1].in_the_future) {
      RL_ASSERT(seen[0] == 1);
    }
  }
};

template <template <heavy_fence_api> class Test>
bool with_each_heavy_fence_api() {
  return simulate<Test<heavy_fence_api::unconditional>>()
      && simulate<Test<heavy_fence_api::try_>>()
      && simulate<Test<heavy_fence_api::ensure>>();
}

int main() {
  return (simulate<clock_counts_fences>()
       && with_each_heavy_fence_api<heavy_to_light_ordering>()
       && with_each_heavy_fence_api<light_to_heavy_ordering>()
       && with_each_heavy_fence_api<ensure_orders_before_readers>()
       && with_each_heavy_fence_api<light_to_light_through_heavy>()) ? 0 : 1;
}

// clang-format off
// Copyright 2026 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#pragma once

#include <atomic_wrappers.h>

#include <cstdint>

namespace tools {

// counter of heavy fences.
// Between any two "times" there was a heavy fence.
// We can use that to get a lot of implicit ordering in the
// system that we'd otherwise miss.
//
// NOTE: 0 is an unused count, the users can have it.
class heavy_fence_clock {
 public:
  using count_t = std::uint64_t;

  // trigger heavy fence--------------------------

  // Unconditionally do a heavy fence.
  // Heavy fence will happen, on this thread.
  void heavy_fence_unconditional();

  // Makes sure that the heavy fence "happened after"
  // we enter this function. (might be us, might be someone else but we get to
  // use that one).
  void heavy_fence_ensure();

  // Do a heavy fence if noone else is doing it at this point.
  // Returns true iff you got a fence.
  bool heavy_fence_try();

  // access the fences --------------------------

  struct light_fence_return {
    count_t in_the_past;
    count_t in_the_future;
  };

  // returns two numbers:
  // - the heavy fence that is guaranteed to be "in the past" - happened before
  // this light fence.
  // - the heavy fence that is guaranteed to be "in the future" - the code
  // before the fence "happened before" that
  //   upcoming heavy fence.
  light_fence_return light_fence() const;

 private:
  void do_heavy_fence();  // m_ has to be locked

  tools::mutex m_;
  tools::atomic<count_t> count_{1};
};

inline void heavy_fence_clock::do_heavy_fence() {
  tools::asymmetric_thread_fence_heavy();
  count_.store(count_.load(tools::memory_order_relaxed) + 1,
               tools::memory_order_relaxed);
}

inline void heavy_fence_clock::heavy_fence_unconditional() {
  tools::lock_guard _{m_};
  do_heavy_fence();
}

inline void heavy_fence_clock::heavy_fence_ensure() {
  count_t after = light_fence().in_the_future;

  tools::lock_guard _{m_};
  if (count_.load(tools::memory_order_relaxed) < after) {
    do_heavy_fence();
  }
}

inline bool heavy_fence_clock::heavy_fence_try() {
  tools::unique_lock lock{m_, tools::try_to_lock};
  if (!lock) {
    return false;
  }

  do_heavy_fence();
  return true;
}

inline auto heavy_fence_clock::light_fence() const -> light_fence_return {
  auto before = count_.load(tools::memory_order_relaxed);

  // At this point the fence "before" is guaranteed to happen before
  // this light fence because we have a coherence order on count.
  tools::asymmetric_thread_fence_light();

  auto loaded_after = count_.load(tools::memory_order_relaxed);
  // How much offset do we need after load_after?

  // One:   hw,  inc,  light, load
  // Two:   hw1, inc1, hw2, light, load, incr2
  // Three: hw1, inc1, hw2, inc2, hw3, load <- if there was 3 fences,
  // inc2 has to happen before the light fence.
  // So + 2.

  return {before, loaded_after + 2};
}

}  // namespace tools

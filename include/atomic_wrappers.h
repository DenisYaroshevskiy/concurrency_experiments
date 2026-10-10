// clang-format off
// Copyright 2025 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on


#pragma once

#ifdef TOOLS_RL_TEST
#include <relacy/atomic.hpp>
#include <relacy/atomic_fence.hpp>
#include <relacy/backoff.hpp>
#include <relacy/dyn_thread.hpp>
#include <relacy/stdlib/mutex.hpp>
#include <relacy/var.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include "shared_ptr.h"
#else
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#endif

namespace tools {

#ifdef TOOLS_RL_TEST
template <typename T>
using atomic = rl::atomic<T>;

using mutex = rl::mutex;

void this_thread_yield(rl::debug_info_param info DEFAULTED_DEBUG_INFO) {
  rl::yield(1, info);
}

inline constexpr auto memory_order_relaxed = rl::memory_order::relaxed;
inline constexpr auto memory_order_acquire = rl::memory_order::acquire;
inline constexpr auto memory_order_release = rl::memory_order::release;
inline constexpr auto memory_order_acq_rel = rl::memory_order::acq_rel;
inline constexpr auto memory_order_seq_cst = rl::memory_order::seq_cst;

struct std_shared_mutex;
class shared_mutex : rl::generic_mutex<std_shared_mutex>, rl::nocopy<> {
  rl::debug_info_param constructor_info_;

  using tag = std_shared_mutex;

 public:
  /*implicit*/ shared_mutex(rl::debug_info_param info DEFAULTED_DEBUG_INFO)
      : constructor_info_(info) {
    generic_mutex<tag>::init(false, false, false, true, constructor_info_);
  }

  ~shared_mutex() { rl::generic_mutex<tag>::deinit(constructor_info_); }

  void lock(rl::debug_info_param info DEFAULTED_DEBUG_INFO) {
    generic_mutex<tag>::lock_exclusive(info);
  }

  bool try_lock(rl::debug_info_param info DEFAULTED_DEBUG_INFO) {
    return generic_mutex<tag>::try_lock_exclusive(info);
  }

  void unlock(rl::debug_info_param info DEFAULTED_DEBUG_INFO) {
    generic_mutex<tag>::unlock_exclusive(info);
  }
};

template<typename T>
struct var {
  T value_{};
  mutable rl::var<int> sentinel_{0};

  var() = default;
  var(T val) : value_(std::move(val)) {}

  const T& read(rl::debug_info_param info DEFAULTED_DEBUG_INFO) const {
    (void)(int)sentinel_(info);
    return value_;
  }

  T& write(rl::debug_info_param info DEFAULTED_DEBUG_INFO) {
    ++sentinel_(info);
    return value_;
  }
};

using rl::lock_guard;

using std::try_to_lock;

// rl::unique_lock has no try_to_lock and std::unique_lock loses the debug info.
template <typename M>
class unique_lock {
 public:
  unique_lock(M& m, rl::debug_info_param info DEFAULTED_DEBUG_INFO)
      : m_(&m), info_(info) {
    m_->lock(info);
    locked_ = true;
  }

  unique_lock(M& m, std::try_to_lock_t,
              rl::debug_info_param info DEFAULTED_DEBUG_INFO)
      : m_(&m), info_(info) {
    locked_ = m_->try_lock(info);
  }

  unique_lock(unique_lock&& x) noexcept
      : m_(std::exchange(x.m_, nullptr)),
        info_(x.info_),
        locked_(std::exchange(x.locked_, false)) {}

  unique_lock& operator=(unique_lock&& x) noexcept {
    if (this == &x) return *this;
    if (locked_) m_->unlock(info_);
    m_ = std::exchange(x.m_, nullptr);
    info_ = x.info_;
    locked_ = std::exchange(x.locked_, false);
    return *this;
  }

  ~unique_lock() {
    if (locked_) m_->unlock(info_);
  }

  void unlock(rl::debug_info_param info DEFAULTED_DEBUG_INFO) {
    m_->unlock(info);
    locked_ = false;
  }

  bool owns_lock() const noexcept { return locked_; }
  explicit operator bool() const noexcept { return locked_; }

 private:
  M* m_;
  rl::debug_info info_;
  bool locked_ = false;
};

void asymmetric_thread_fence_light(rl::debug_info_param info DEFAULTED_DEBUG_INFO) {
  rl::atomic_thread_fence(memory_order_seq_cst, info);
}

void asymmetric_thread_fence_heavy(rl::debug_info_param info DEFAULTED_DEBUG_INFO) {
  rl::atomic_thread_fence(memory_order_seq_cst, info);
}

void thread_fence_seq_cst(rl::debug_info_param info DEFAULTED_DEBUG_INFO) {
  rl::atomic_thread_fence(memory_order_seq_cst, info);
}

template <typename T>
using shared_ptr = rl_extra::shared_ptr<T>;

template <typename T>
shared_ptr<T> make_shared() { return rl_extra::make_shared<T>(); }

#else

template <typename T>
using atomic = std::atomic<T>;

using mutex = std::mutex;

template <typename... Ts>
using lock_guard = std::lock_guard<Ts...>;

template <typename M>
using unique_lock = std::unique_lock<M>;

using std::try_to_lock;

void this_thread_yield() { std::this_thread::yield(); }

void thread_fence_seq_cst() { std::atomic_thread_fence(std::memory_order_seq_cst); }

inline constexpr auto memory_order_relaxed = std::memory_order::relaxed;
inline constexpr auto memory_order_acquire = std::memory_order::acquire;
inline constexpr auto memory_order_release = std::memory_order::release;
inline constexpr auto memory_order_acq_rel = std::memory_order::acq_rel;
inline constexpr auto memory_order_seq_cst = std::memory_order::seq_cst;

template<typename T>
struct var {
  T value_{};

  var() = default;
  var(T val) : value_(std::move(val)) {}

  const T& read() const { return value_; }
  T& write() { return value_; }
};

template <typename T>
using shared_ptr = std::shared_ptr<T>;

template <typename T>
shared_ptr<T> make_shared() { return std::make_shared<T>(); }

// TODO: production periodic_runner

#endif

}  // namespace tools

// clang-format off
// Copyright 2026 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <cstddef>
#include <utility>

#include <relacy/atomic.hpp>

namespace rl_extra {
namespace detail {

struct count_base {
  using count_t = std::size_t;

  rl::atomic<count_t> strong_count{1};
  virtual ~count_base() = default;

  void increase_count() {
    strong_count.fetch_add(1, rl::memory_order_relaxed);
  }

  bool decrease_count() {
    return strong_count.fetch_sub(1, rl::memory_order_acq_rel) == 1;
  }
};

template <typename T>
struct store_together final : count_base {
  T data;
  ~store_together() override = default;
};

template <typename T>
struct store_separately final : count_base {
  T* data;
  explicit store_separately(T* p) : data(p) {}
  ~store_separately() override { delete data; }
};

}  // namespace detail

template <typename T>
class shared_ptr {
 public:
  using count_t = detail::count_base::count_t;

  shared_ptr() = default;

  explicit shared_ptr(T* ptr)
      : ptr_(ptr), count_(new detail::store_separately<T>(ptr)) {}

  shared_ptr(const shared_ptr& other) noexcept
      : ptr_(other.ptr_), count_(other.count_) {
    if (count_) count_->increase_count();
  }

  shared_ptr(shared_ptr&& other) noexcept
      : ptr_(other.ptr_), count_(other.count_) {
    other.ptr_ = nullptr;
    other.count_ = nullptr;
  }

  shared_ptr& operator=(shared_ptr other) noexcept {
    swap(other);
    return *this;
  }

  ~shared_ptr() {
    if (count_ && count_->decrease_count()) {
      delete count_;
    }
  }

  T* get() const { return ptr_; }
  T* operator->() const { return ptr_; }
  T& operator*() const { return *ptr_; }

  count_t use_count() const {
    if (!count_) return 0;
    return count_->strong_count.load(rl::memory_order_acquire);
  }

  explicit operator bool() const { return ptr_ != nullptr; }

  void swap(shared_ptr& other) noexcept {
    std::swap(ptr_, other.ptr_);
    std::swap(count_, other.count_);
  }

 private:
  template <typename U>
  friend shared_ptr<U> make_shared();

  shared_ptr(T* ptr, detail::count_base* count)
      : ptr_(ptr), count_(count) {}

  T* ptr_ = nullptr;
  detail::count_base* count_ = nullptr;
};

template <typename T>
shared_ptr<T> make_shared() {
  auto* ctrl = new detail::store_together<T>();
  return shared_ptr<T>(&ctrl->data, ctrl);
}

}  // namespace rl_extra

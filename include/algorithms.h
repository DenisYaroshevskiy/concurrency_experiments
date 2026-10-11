// clang-format off
// Copyright 2026 Denis Yaroshevskiy
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at https://www.boost.org/LICENSE_1_0.txt)
// clang-format on

#pragma once

#include <algorithm>
#include <iterator>
#include <utility>

namespace tools {

/*
 * remove_if that moves the removed elements to `o`.
 *
 * Elements satisfying `p` are moved to `o`, the rest are stably moved to the
 * front. Returns the end of the kept elements.
 */
template <std::forward_iterator I, typename O, typename P>
I partition_to(I f, I l, O o, P p) {
  f = std::find_if(f, l, p);
  if (f == l) return f;

  *o++ = std::move(*f);
  for (I i = std::next(f); i != l; ++i) {
    if (p(*i)) {
      *o++ = std::move(*i);
    } else {
      *f = std::move(*i);
      ++f;
    }
  }
  return f;
}

}  // namespace tools

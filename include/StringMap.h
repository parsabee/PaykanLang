// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// String-keyed containers for the compiler core.
//
// The core is standard C++ only, so the LLVM ADT containers (StringMap,
// StringSet, DenseMap, ...) are not available to it.  These aliases keep the
// call sites short and give every string-keyed table the same properties:
//
//   * heterogeneous lookup: find / count / contains / erase accept a
//     std::string_view (or a const char *) without building a std::string;
//   * ordered, deterministic iteration (by key), so anything that walks a
//     table -- diagnostics, exports, emission order -- is reproducible across
//     platforms and standard-library implementations.
//
// Insertion through operator[] / try_emplace still needs a std::string key.

#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>

namespace paykan {

/// Ordered map from string to V with transparent (string_view) lookup.
template <typename V> using StringMap = std::map<std::string, V, std::less<>>;

/// Ordered set of strings with transparent (string_view) lookup.
using StringSet = std::set<std::string, std::less<>>;

} // namespace paykan

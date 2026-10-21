// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The OBJ section (docs/design/pkm.md §5.13): a module's code, compiled by
// a backend when the module was written.  CODE then holds only the
// module's shared copies of instantiations and a declaration of
// everything else; the backend that compiled the bytes links them.  The
// bytes are the backend's (the mlir backend's are LLVM bitcode); this
// library only frames them.

#pragma once

#include "paykan/Status.h"
#include "paykan/pkm/Format.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace paykan::pkm {

/// The OBJ blob's version: the release's, like every format.
inline constexpr uint32_t kObjectVersion = kVersionNumber;

struct Object {
  /// What made the bytes, which a backend checks before it links them: its
  /// registered name ("mlir"), the bytes' format ("llvm-bc"), the target
  /// they are for (a triple) and the backend's version (its describe()).
  std::string Backend, Format, Target, Producer;
  std::vector<uint8_t> Bytes;

  /// The same backend, format, target and producer as @p o.
  bool sameKind(const Object &o) const {
    return Backend == o.Backend && Format == o.Format && Target == o.Target &&
           Producer == o.Producer;
  }
  bool operator==(const Object &) const = default;
};

std::vector<uint8_t> writeObject(const Object &o);
StatusOr<Object> readObject(std::span<const uint8_t> bytes);

} // namespace paykan::pkm

// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "paykan/Backend.h"

#ifndef PAYKAN_DEFAULT_BACKEND
#error "PAYKAN_DEFAULT_BACKEND must be defined by the build"
#endif

namespace paykan::backend {

std::string_view defaultBackend() { return PAYKAN_DEFAULT_BACKEND; }

StatusOr<int> Backend::run(const Input &, std::span<const std::string>,
                           const RunOptions &) {
  return Status::error("backend '" + std::string(name()) +
                       "' cannot run programs");
}

} // namespace paykan::backend

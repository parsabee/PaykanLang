// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0

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

StatusOr<ModuleObject> Backend::compileModule(const pir::Module &) {
  return Status::error("backend '" + std::string(name()) +
                       "' cannot compile modules");
}

Status Backend::emitModuleObject(const ModuleObject &, unsigned,
                                 const std::string &) {
  return Status::error("backend '" + std::string(name()) +
                       "' cannot compile modules");
}

Status Backend::checkModuleObject(const ModuleObject &) { return Status::ok(); }

} // namespace paykan::backend

// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The PIR pass manager (see Pass.h).

#include "paykan/pir/Pass.h"

#include "paykan/pir/Verifier.h"

#include <string>

namespace paykan::pir {

Status PassManager::run(Program &program) const {
  for (const auto &pass : Passes) {
    if (Status s = pass->run(program); !s)
      return Status::error("PIR pass '" + std::string(pass->name()) +
                           "' failed: " + s.message());
    if (!VerifyEach)
      continue;
    if (auto errors = verify(program); !errors.empty())
      return Status::error("PIR pass '" + std::string(pass->name()) +
                           "' left a program that does not verify:\n" +
                           formatErrors(errors));
  }
  return Status::ok();
}

} // namespace paykan::pir

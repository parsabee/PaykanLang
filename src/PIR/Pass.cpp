// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The PIR pass manager and its statistics (see Pass.h).

#include "paykan/pir/Pass.h"

#include "paykan/pir/Verifier.h"

#include <string>

namespace paykan::pir {

void Statistics::add(std::string_view pass, std::string_view counter,
                     uint64_t n) {
  for (Counter &c : Counters)
    if (c.Pass == pass && c.Name == counter) {
      c.N += n;
      return;
    }
  Counters.push_back({std::string(pass), std::string(counter), n});
}

uint64_t Statistics::get(std::string_view pass,
                         std::string_view counter) const {
  for (const Counter &c : Counters)
    if (c.Pass == pass && c.Name == counter)
      return c.N;
  return 0;
}

void Statistics::print(std::ostream &os) const {
  os << "paykan pass stats:\n";
  for (const Counter &c : Counters)
    os << "  " << c.Pass << ' ' << c.Name << " : " << c.N << '\n';
}

Status PassManager::run(Program &program, Statistics &stats) const {
  for (const auto &pass : Passes) {
    if (Status s = pass->run(program, stats); !s)
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

Status PassManager::run(Program &program) const {
  Statistics dropped;
  return run(program, dropped);
}

} // namespace paykan::pir

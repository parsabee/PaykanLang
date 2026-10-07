// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// `paykan fmt`: the source formatter's command line (paykan/format/Format.h).

#pragma once

namespace paykan::driver {

/// Run `paykan fmt ...`; @p argv[1] is "fmt".  Returns the exit status.
int fmtTool(int argc, const char *const *argv);

} // namespace paykan::driver

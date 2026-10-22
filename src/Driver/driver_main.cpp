// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The `paykan` executable.  main() lives in the paykan_driver library
// (main.cpp) so that out-of-tree plugins can build their own driver from the
// installed package; this file only gives the executable a translation unit.

namespace paykan::driver {
// Nothing to do: the linker pulls main() from libpaykan_driver.
} // namespace paykan::driver

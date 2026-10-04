# An out-of-tree Paykan backend: the smallest backend there is.
#
# Build it against an installed Paykan (see docs/writing-a-backend.md):
#
#   cmake --install <paykan-build> --prefix <prefix>
#   cmake -B build -DCMAKE_PREFIX_PATH=<prefix> utils/print-pir
#       [-DPRINT_PIR_BUILT_WITH=<version>]   # pin the PaykanLang version
#       [-DLLVM_DIR=<llvm>/lib/cmake/llvm]   # if the installation has the llvm backend
#   cmake --build build
#   build/paykan-print-pir --backend=print-pir --emit-source program.pkn
#
# The result is a `paykan` driver that has every plugin of the installation
# plus this backend.
cmake_minimum_required(VERSION 3.24)
project(PaykanPrintPIRBackend LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Paykan REQUIRED)

# The PaykanLang version this plugin is built with.  Empty means the installed
# one (PAYKAN_TOOLCHAIN_VERSION); set it to pin the version the plugin is
# written for.  Configure fails if the installed Paykan does not accept it
# (its PAYKAN_PLUGIN_COMPATIBLE_VERSIONS, docs/writing-a-backend.md).
set(PRINT_PIR_BUILT_WITH "" CACHE STRING
    "PaykanLang version print-pir is built with (empty: the installed one)")

# The backend: a static library that registers itself (PAYKAN_REGISTER_BACKEND)
# and links Paykan::backend (the interface and, through it, Paykan::pir).
paykan_add_backend_plugin(paykan_backend_print_pir
    BUILT_WITH "${PRINT_PIR_BUILT_WITH}"
    PrintPIRBackend.cpp)
target_compile_options(paykan_backend_print_pir PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-rtti -fno-exceptions
)

# A driver with the installation's plugins plus ours.
paykan_add_driver(paykan-print-pir PLUGINS paykan_backend_print_pir)

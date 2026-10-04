# An out-of-tree Paykan backend: the smallest backend there is, in plain C.
#
# Build it against an installed PaykanLang (see docs/writing-a-backend.md)
# and load it into the installed `paykan`, no rebuild of PaykanLang:
#
#   cmake -B build -DCMAKE_PREFIX_PATH=<prefix> utils/print-pir
#       [-DPRINT_PIR_BUILT_WITH=<version>]   # pin the PaykanLang version
#   cmake --build build
#   <prefix>/bin/paykan --plugin=build/libpaykan_backend_print_pir.so \
#       --backend=print-pir --emit-source program.pkn
#
#   cmake --install build    # into <prefix>/lib/paykan/plugins/<version>/:
#   <prefix>/bin/paykan --backend=print-pir --emit-source program.pkn
#
# Only a C compiler is needed: the plugin uses paykan/plugin_api.h and links
# nothing of PaykanLang.
cmake_minimum_required(VERSION 3.24)
project(PaykanPrintPIRBackend LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Paykan REQUIRED)

# The PaykanLang version this plugin is built with.  Empty means the installed
# one (PAYKAN_TOOLCHAIN_VERSION); set it to pin the version the plugin is
# written for.  Configure fails if the installed Paykan does not accept it
# (its PAYKAN_PLUGIN_COMPATIBLE_VERSIONS, docs/writing-a-backend.md).
set(PRINT_PIR_BUILT_WITH "" CACHE STRING
    "PaykanLang version print-pir is built with (empty: the installed one)")

# The backend: a loadable module (libpaykan_backend_print_pir.so / .dylib)
# that exports paykan_plugin_init.
paykan_add_backend_plugin(paykan_backend_print_pir
    BUILT_WITH "${PRINT_PIR_BUILT_WITH}"
    print_pir.c)
target_compile_options(paykan_backend_print_pir PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

# `cmake --install` puts it into the installation's plugin directory, where
# `paykan` finds it by itself.
paykan_install_plugin(paykan_backend_print_pir)

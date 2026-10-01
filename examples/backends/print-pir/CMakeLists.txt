# An out-of-tree Paykan backend: the smallest backend there is.
#
# Build it against an installed Paykan (see docs/writing-a-backend.md):
#
#   cmake --install <paykan-build> --prefix <prefix>
#   cmake -B build -DCMAKE_PREFIX_PATH=<prefix> examples/backends/print-pir
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

# The backend: a static library that registers itself (PAYKAN_REGISTER_BACKEND).
add_library(paykan_backend_print_pir STATIC PrintPIRBackend.cpp)
target_link_libraries(paykan_backend_print_pir
    PUBLIC Paykan::backend   # the interface (and, through it, Paykan::pir)
)
target_compile_options(paykan_backend_print_pir PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-rtti -fno-exceptions
)

# A driver with the installation's plugins plus ours.
paykan_add_driver(paykan-print-pir PLUGINS paykan_backend_print_pir)

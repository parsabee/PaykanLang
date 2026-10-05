# The advanced static path (docs/writing-a-backend.md section 4): a C++
# backend built out of tree as a static library and linked into a `paykan`
# driver of its own with paykan_add_driver.  tests/OutOfTree/PrintPIR.cmake
# builds it against a scratch install and runs it.
cmake_minimum_required(VERSION 3.24)
project(PaykanStaticDriver LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Paykan REQUIRED)

add_library(paykan_backend_static_echo STATIC StaticEchoBackend.cpp)
target_link_libraries(paykan_backend_static_echo PUBLIC Paykan::backend)
target_compile_options(paykan_backend_static_echo PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-rtti -fno-exceptions)

paykan_add_driver(paykan-static-echo PLUGINS paykan_backend_static_echo)

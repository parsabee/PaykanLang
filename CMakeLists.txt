cmake_minimum_required(VERSION 3.20)

project(PakanLang
    VERSION 0.1.0
    DESCRIPTION "A clean and simple, statically typed, object-oriented programming language"
    LANGUAGES C CXX
)

# Set C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Third-party dependency versions
set(LLVM_VERSION  "17.0.6")
set(BISON_VERSION "3.8")
set(FLEX_VERSION  "2.6.4")
set(GTEST_VERSION "1.15.2")

# Third-party install directories
set(THIRD_PARTY_DIR  ${CMAKE_BINARY_DIR}/third-party)
set(LLVM_INSTALL_DIR ${THIRD_PARTY_DIR}/llvm)
set(BISON_INSTALL_DIR ${THIRD_PARTY_DIR}/bison)
set(FLEX_INSTALL_DIR  ${THIRD_PARTY_DIR}/flex)
set(GTEST_INSTALL_DIR ${THIRD_PARTY_DIR}/gtest)

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
else()
    message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. Only Clang and GCC are supported.")
endif()

# Add cmake/ to module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Third-party setup
include(LLVMSetup)
include(BisonFlexSetup)
include(GTestSetup)

include(CTest)
enable_testing()

# Disable RTTI and exceptions for project source files
# Applied here to guarantee HandleLLVMOptions cannot override them
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")

# Subdirectories
add_subdirectory(src)
add_subdirectory(tests)

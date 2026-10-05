# An out-of-tree Paykan frontend: the smallest frontend there is, in plain C.
# Its source language is the AST interchange format itself
# (docs/plugins/ast-format.md).
#
#   cmake -B build -DCMAKE_PREFIX_PATH=<prefix> src/Frontends/ASTText
#   cmake --build build
#   <prefix>/bin/paykan --emit-ast program.pkn > program.ast
#   <prefix>/bin/paykan --plugin=build/libpaykan_frontend_ast_text.so \
#       --frontend=ast-text program.ast
#   cmake --install build    # into <prefix>/lib/paykan/plugins/<version>/
cmake_minimum_required(VERSION 3.24)
project(PaykanASTTextFrontend LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Paykan REQUIRED)

paykan_add_frontend_plugin(paykan_frontend_ast_text ast_text.c)
target_compile_options(paykan_frontend_ast_text PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
paykan_install_plugin(paykan_frontend_ast_text)

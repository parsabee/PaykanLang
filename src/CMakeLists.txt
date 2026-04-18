# src/CMakeLists.txt
# ----------------------------------------------------------------------------
# Paykan compiler front-end:
#   Parser/  – paykan_parser static library (Bison + Flex + Driver)
#   Driver/  – executable entry points (mains for all project binaries)
# ----------------------------------------------------------------------------

add_subdirectory(AST)
add_subdirectory(Parser)
add_subdirectory(ASTPrinter)
add_subdirectory(Sema)
add_subdirectory(CodeGen)
add_subdirectory(Runtime)
add_subdirectory(Driver)

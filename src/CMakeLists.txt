# src/CMakeLists.txt
# ----------------------------------------------------------------------------
# Paykan compiler:
#   AST/ Diag/ Frontend/ ASTPrinter/ Sema/  – the core (standard C++ only)
#   Frontends/<name>/ – frontend plugins, built when listed in PAYKAN_FRONTENDS
#   CodeGen/ JIT/     – LLVM code generation and JIT
#   Runtime/          – the C runtime linked into every program
#   Driver/           – executable entry points (mains for all project binaries)
# ----------------------------------------------------------------------------

add_subdirectory(AST)
add_subdirectory(Diag)
add_subdirectory(Frontend)
foreach(fe IN LISTS PAYKAN_FRONTENDS)
    if(fe STREQUAL "bison")
        add_subdirectory(Frontends/Bison)
    endif()
endforeach()
add_subdirectory(ASTPrinter)
add_subdirectory(Sema)
add_subdirectory(CodeGen)
add_subdirectory(Runtime)
add_subdirectory(JIT)
add_subdirectory(Driver)

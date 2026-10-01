# src/CMakeLists.txt
# ----------------------------------------------------------------------------
# Paykan compiler:
#   AST/ Diag/ Frontend/ ASTPrinter/ Sema/ PIR/ Backend/
#                     – the core (standard C++ only)
#   Frontends/<name>/ – frontend plugins, built when listed in PAYKAN_FRONTENDS
#   Backends/<name>/  – backend plugins, built when listed in PAYKAN_BACKENDS
#                       (Backends/LLVM pulls in CodeGen/ and JIT/)
#   Runtime/          – the C runtime linked into every program
#   Driver/           – executable entry points (mains for all project binaries)
# ----------------------------------------------------------------------------

add_subdirectory(AST)
add_subdirectory(Diag)
add_subdirectory(Frontend)
foreach(fe IN LISTS PAYKAN_FRONTENDS)
    if(fe STREQUAL "handwritten")
        add_subdirectory(Frontends/Handwritten)
    elseif(fe STREQUAL "bison")
        add_subdirectory(Frontends/Bison)
    endif()
endforeach()
add_subdirectory(ASTPrinter)
add_subdirectory(PIR)
add_subdirectory(Sema)
add_subdirectory(Backend)
add_subdirectory(Runtime)
foreach(be IN LISTS PAYKAN_BACKENDS)
    if(be STREQUAL "llvm")
        add_subdirectory(Backends/LLVM)
    elseif(be STREQUAL "mlir")
        add_subdirectory(Backends/MLIR)
    endif()
endforeach()
add_subdirectory(Driver)

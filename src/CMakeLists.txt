# Paykan compiler:
#   Plugin/ AST/ ASTInterchange/ Diag/ Frontend/ ASTPrinter/ Sema/ PIR/
#   PKM/ Modules/ Backend/ Lowering/
#                     – the core (standard C++ only)
#   Frontends/<name>/ – frontend plugins, built when listed in PAYKAN_FRONTENDS
#   Backends/MLIR/    – the mlir backend, the in-tree backend plugin (its
#                       dialect in Dialect/); Backends/LLVM/JIT/ and CodeGen/
#                       are its LLVM half: the JIT, the bitcode cache and
#                       native builds
#   Backends/Toolchain/ – the system C toolchain / runtime link helpers a
#                       native build links with
#   PluginHost/       – loads plugins at run time (the C plugin interface,
#                       include/paykan/plugin_api.h)
#   Runtime/          – the C runtime linked into every program
#   Driver/           – executable entry points (mains for all project binaries)

add_subdirectory(Plugin)
add_subdirectory(AST)
add_subdirectory(ASTInterchange)
add_subdirectory(Diag)
add_subdirectory(Frontend)
foreach(fe IN LISTS PAYKAN_FRONTENDS)
    if(fe STREQUAL "recursive-descent")
        add_subdirectory(Frontends/RecursiveDescent)
    endif()
endforeach()
add_subdirectory(ASTPrinter)
add_subdirectory(PIR)
add_subdirectory(PKM)
add_subdirectory(Modules)
add_subdirectory(Sema)
add_subdirectory(Backend)
add_subdirectory(Lowering)
add_subdirectory(Runtime)
add_subdirectory(Backends/Toolchain)
# The mlir backend's LLVM half: the JIT, the bitcode cache and native
# builds.
add_subdirectory(Backends/LLVM/JIT)
add_subdirectory(Backends/LLVM/CodeGen)
add_subdirectory(Backends/MLIR)
add_subdirectory(PluginHost)
add_subdirectory(Driver)

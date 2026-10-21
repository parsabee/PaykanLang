# Paykan compiler:
#   Plugin/ AST/ ASTInterchange/ Diag/ Frontend/ ASTPrinter/ Sema/ PIR/
#   PKM/ Modules/ Backend/ Lowering/
#                     – the core (standard C++ only)
#   Frontends/<name>/ – frontend plugins, built when listed in PAYKAN_FRONTENDS
#   Backends/<name>/  – backend plugins, built when listed in PAYKAN_BACKENDS
#                       (Backends/LLVM/JIT/ and CodeGen/ are what the llvm
#                       and mlir backends share: the JIT, native builds;
#                       Backends/MLIR/Dialect/ the mlir backend's dialect)
#   Backends/Toolchain/ – the system C toolchain / runtime link helpers the
#                       native backends (C, LLVM) share
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
if(PAYKAN_NEEDS_LLVM)
    # What the backends that produce LLVM IR share: the JIT, the bitcode
    # cache and native builds.
    add_subdirectory(Backends/LLVM/JIT)
    add_subdirectory(Backends/LLVM/CodeGen)
endif()
foreach(be IN LISTS PAYKAN_BACKENDS)
    if(be STREQUAL "llvm")
        add_subdirectory(Backends/LLVM)
    elseif(be STREQUAL "mlir")
        add_subdirectory(Backends/MLIR)
    elseif(be STREQUAL "c")
        add_subdirectory(Backends/C)
    endif()
endforeach()
add_subdirectory(PluginHost)
add_subdirectory(Driver)

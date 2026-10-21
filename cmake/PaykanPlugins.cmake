# Plugin options and helpers.
#
# A plugin (a frontend or a backend) is a static library that registers
# itself with the core's plugin registry from a static initialiser
# (include/paykan/Registry.h).  Nothing references that initialiser, so a
# plugin library must be linked into an executable WHOLE, or the linker drops
# it.  paykan_add_plugin() records a plugin target; paykan_link_plugins()
# links every recorded plugin into an executable that way.
#
# Provides:
#   PAYKAN_FRONTENDS          -- cache list of frontends to build
#   PAYKAN_DEFAULT_FRONTEND   -- the frontend used when --frontend is absent
#   PAYKAN_BACKENDS           -- cache list of backends to build
#   PAYKAN_DEFAULT_BACKEND    -- the backend used when --backend is absent
#   PAYKAN_NEEDS_LLVM         -- TRUE: the mlir backend depends on LLVM ...
#   PAYKAN_NEEDS_MLIR         -- ... and on MLIR
#   paykan_add_plugin(<target>)
#   paykan_link_plugins(<target>)

# -- Frontends ----------------------------------------------------------------
# The in-tree frontends: `recursive-descent` (standard C++ only), always
# built and the default.  Out-of-tree frontends are not listed here: they are
# built against an installed Paykan (find_package(Paykan)) and linked into a
# driver of their own with paykan_add_driver (docs/writing-a-frontend-plugin.md).
set(PAYKAN_KNOWN_FRONTENDS recursive-descent)
set(PAYKAN_FRONTENDS "recursive-descent" CACHE STRING
    "Semicolon-separated list of frontends to build (available: ${PAYKAN_KNOWN_FRONTENDS}); the first one is the default")

if(NOT PAYKAN_FRONTENDS)
    message(FATAL_ERROR "PAYKAN_FRONTENDS must list at least one frontend (available: ${PAYKAN_KNOWN_FRONTENDS})")
endif()
foreach(fe IN LISTS PAYKAN_FRONTENDS)
    if(NOT fe IN_LIST PAYKAN_KNOWN_FRONTENDS)
        message(FATAL_ERROR "Unknown frontend '${fe}' in PAYKAN_FRONTENDS (available: ${PAYKAN_KNOWN_FRONTENDS}). "
                            "PAYKAN_FRONTENDS lists the in-tree frontends only; an out-of-tree frontend is "
                            "built as a plugin against an installed Paykan (docs/writing-a-frontend-plugin.md).")
    endif()
endforeach()

# The default frontend is the first one listed.
list(GET PAYKAN_FRONTENDS 0 PAYKAN_DEFAULT_FRONTEND)
# The core frontend is part of every build, whatever the list says.
if(NOT "recursive-descent" IN_LIST PAYKAN_FRONTENDS)
    list(APPEND PAYKAN_FRONTENDS recursive-descent)
endif()
list(LENGTH PAYKAN_FRONTENDS PAYKAN_NUM_FRONTENDS)
message(STATUS "Frontends: ${PAYKAN_FRONTENDS} (default: ${PAYKAN_DEFAULT_FRONTEND})")

# -- Backends -----------------------------------------------------------------
# The in-tree backend is `mlir` (docs/design/mlir-backend.md): PIR through
# MLIR to LLVM (the JIT, native objects and executables, --emit-llvm) and to
# C (--emit-c).  It depends on LLVM 22 with MLIR, which a configure finds
# (-DLLVM_DIR) or downloads (cmake/LLVMSetup.cmake).  Always built and the
# default; other backends are plugins, built against an installed Paykan
# (docs/writing-a-backend.md).  The c and llvm backends it replaced are
# accepted in the list for old build directories and scripts, and stand for
# it.
set(PAYKAN_KNOWN_BACKENDS mlir)
set(PAYKAN_BACKENDS "mlir" CACHE STRING
    "Semicolon-separated list of backends to build (available: ${PAYKAN_KNOWN_BACKENDS}; mlir is always built and is the default)")

set(_paykan_backends "")
foreach(be IN LISTS PAYKAN_BACKENDS)
    if(be STREQUAL "c" OR be STREQUAL "llvm")
        message(WARNING "PAYKAN_BACKENDS: the ${be} backend was replaced by "
                        "mlir, which emits both C and LLVM IR: building mlir")
        set(be mlir)
    elseif(NOT be IN_LIST PAYKAN_KNOWN_BACKENDS)
        message(FATAL_ERROR "Unknown backend '${be}' in PAYKAN_BACKENDS (available: ${PAYKAN_KNOWN_BACKENDS})")
    endif()
    list(APPEND _paykan_backends ${be})
endforeach()
list(APPEND _paykan_backends mlir)
list(REMOVE_DUPLICATES _paykan_backends)
set(PAYKAN_BACKENDS "${_paykan_backends}")

set(PAYKAN_DEFAULT_BACKEND mlir)
set(PAYKAN_NEEDS_LLVM TRUE)
set(PAYKAN_NEEDS_MLIR TRUE)
message(STATUS "Backends: ${PAYKAN_BACKENDS} (default: ${PAYKAN_DEFAULT_BACKEND})")

# -- Helpers ------------------------------------------------------------------
define_property(GLOBAL PROPERTY PAYKAN_PLUGIN_TARGETS
    BRIEF_DOCS "Plugin libraries to link whole into every paykan executable")
set_property(GLOBAL PROPERTY PAYKAN_PLUGIN_TARGETS "")

# Record <target> as a plugin library.
function(paykan_add_plugin target)
    set_property(GLOBAL APPEND PROPERTY PAYKAN_PLUGIN_TARGETS ${target})
endfunction()

# Link every recorded plugin library whole into <target> (an executable), so
# their static registrations run at startup.
function(paykan_link_plugins target)
    get_property(plugins GLOBAL PROPERTY PAYKAN_PLUGIN_TARGETS)
    foreach(plugin IN LISTS plugins)
        target_link_libraries(${target} PRIVATE
            "$<LINK_LIBRARY:WHOLE_ARCHIVE,${plugin}>")
    endforeach()
endfunction()

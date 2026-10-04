# PaykanPlugins.cmake
# ----------------------------------------------------------------------------
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
#   PAYKAN_NEEDS_LLVM         -- TRUE when a listed backend depends on LLVM
#   paykan_add_plugin(<target>)
#   paykan_link_plugins(<target>)
# ----------------------------------------------------------------------------

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
# `c` is the C backend (part of the core: it needs only a C compiler at run
# time), always built and the default; `llvm` is the LLVM IR / ORC JIT
# backend, which is opt-in and depends on LLVM: LLVM is fetched only when
# `llvm` is listed (#123).  So a plain configure downloads nothing.
set(PAYKAN_KNOWN_BACKENDS c llvm)
set(PAYKAN_BACKENDS "c" CACHE STRING
    "Semicolon-separated list of backends to build (available: ${PAYKAN_KNOWN_BACKENDS}; c is always built and is the default)")

foreach(be IN LISTS PAYKAN_BACKENDS)
    if(NOT be IN_LIST PAYKAN_KNOWN_BACKENDS)
        message(FATAL_ERROR "Unknown backend '${be}' in PAYKAN_BACKENDS (available: ${PAYKAN_KNOWN_BACKENDS})")
    endif()
endforeach()
if(NOT "c" IN_LIST PAYKAN_BACKENDS)
    list(APPEND PAYKAN_BACKENDS c)
endif()

# The default backend is `c` whenever it is built, wherever the list puts it
# (`llvm;c` and `c;llvm` both default to `c`; `llvm` is selected with
# --backend=llvm).  `c` is appended above, so in this tree it always is.  A
# configuration without `c` (which this file never produces, but a fork or a
# downstream copy of it might) falls back to the first backend listed.
if("c" IN_LIST PAYKAN_BACKENDS)
    set(PAYKAN_DEFAULT_BACKEND c)
else()
    list(GET PAYKAN_BACKENDS 0 PAYKAN_DEFAULT_BACKEND)
endif()
set(PAYKAN_NEEDS_LLVM FALSE)
if("llvm" IN_LIST PAYKAN_BACKENDS)
    set(PAYKAN_NEEDS_LLVM TRUE)
endif()
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

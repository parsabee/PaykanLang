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
#   paykan_add_plugin(<target>)
#   paykan_link_plugins(<target>)
# ----------------------------------------------------------------------------

# -- Frontends ----------------------------------------------------------------
set(PAYKAN_KNOWN_FRONTENDS bison)
set(PAYKAN_FRONTENDS "bison" CACHE STRING
    "Semicolon-separated list of frontends to build (available: ${PAYKAN_KNOWN_FRONTENDS})")

if(NOT PAYKAN_FRONTENDS)
    message(FATAL_ERROR "PAYKAN_FRONTENDS must list at least one frontend (available: ${PAYKAN_KNOWN_FRONTENDS})")
endif()
foreach(fe IN LISTS PAYKAN_FRONTENDS)
    if(NOT fe IN_LIST PAYKAN_KNOWN_FRONTENDS)
        message(FATAL_ERROR "Unknown frontend '${fe}' in PAYKAN_FRONTENDS (available: ${PAYKAN_KNOWN_FRONTENDS})")
    endif()
endforeach()

# The default frontend is the first one listed.
list(GET PAYKAN_FRONTENDS 0 PAYKAN_DEFAULT_FRONTEND)
message(STATUS "Frontends: ${PAYKAN_FRONTENDS} (default: ${PAYKAN_DEFAULT_FRONTEND})")

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

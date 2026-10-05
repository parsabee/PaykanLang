# PluginCompat.cmake
# ----------------------------------------------------------------------------
# The plugin compatibility list: the one place that says which plugin builds
# this PaykanLang release accepts (#103).
#
# Every frontend and backend plugin records the PaykanLang version it was
# BUILT WITH: PAYKAN_REGISTER_FRONTEND / PAYKAN_REGISTER_BACKEND capture
# PAYKAN_PLUGIN_BUILD_VERSION from the installed headers the plugin is
# compiled against (include/paykan/PluginCompat.h.in).  The registry accepts a
# plugin only if that version is one of PAYKAN_PLUGIN_COMPATIBLE_VERSIONS;
# any other plugin is listed as incompatible and never instantiated
# (include/paykan/Registry.h).  The same list is exported in the CMake package
# (PaykanConfig.cmake), so an out-of-tree plugin fails at configure time
# instead (paykan_add_frontend_plugin / paykan_add_backend_plugin).
#
# Versions are compared as exact strings, pre-release label included:
# "0.1.0-alpha" accepts plugins built with "0.1.0-alpha" only, not "0.1.0"
# or "0.1.0-beta".  There is no range and no ordering; list every accepted
# version explicitly.
#
# Maintaining the list, once per release (docs/writing-a-backend.md,
# "Plugin compatibility"):
#   - The release's own version (project() VERSION plus
#     PAYKAN_VERSION_PRERELEASE) must be on the list: the built-in plugins are
#     built with it.  Configure fails otherwise, so a version bump cannot
#     forget it.
#   - Keep an older version on the list only when plugins built with it still
#     work with this release: Frontend.h, Backend.h, Registry.h, the AST and
#     ASTContext a frontend sees, PIR, and the runtime ABI (Runtime.h)
#     are unchanged since it, or changed compatibly.  Drop it as soon as one
#     of them changes incompatibly.
#   - Say in the CHANGELOG entry of the release which versions it accepts.
#
# Provides (after PAYKAN_VERSION is set):
#   PAYKAN_PLUGIN_COMPATIBLE_VERSIONS  -- the list
#   PAYKAN_PLUGIN_API_VERSION          -- the C plugin interface's version
#                                         (include/paykan/plugin_api.h)
#   PAYKAN_PLUGIN_COMPATIBLE_VERSIONS_CXX -- the list as a C++ initializer
#                                         ("a", "b") for the generated header
# ----------------------------------------------------------------------------

set(PAYKAN_PLUGIN_COMPATIBLE_VERSIONS
    "0.1.0-alpha"
)

if(NOT PAYKAN_VERSION)
    message(FATAL_ERROR "PluginCompat.cmake: PAYKAN_VERSION is not set yet")
endif()
if(NOT PAYKAN_VERSION IN_LIST PAYKAN_PLUGIN_COMPATIBLE_VERSIONS)
    message(FATAL_ERROR
        "cmake/PluginCompat.cmake: PaykanLang ${PAYKAN_VERSION} is not on its "
        "own plugin compatibility list (${PAYKAN_PLUGIN_COMPATIBLE_VERSIONS}); "
        "add it, since the built-in plugins are built with it")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/include/paykan/plugin_api.h" _paykan_api_line
    REGEX "^#define PAYKAN_PLUGIN_API_VERSION ")
if(NOT _paykan_api_line MATCHES "VERSION ([0-9]+)")
    message(FATAL_ERROR "include/paykan/plugin_api.h: no PAYKAN_PLUGIN_API_VERSION")
endif()
set(PAYKAN_PLUGIN_API_VERSION ${CMAKE_MATCH_1})

set(PAYKAN_PLUGIN_COMPATIBLE_VERSIONS_CXX "")
foreach(v IN LISTS PAYKAN_PLUGIN_COMPATIBLE_VERSIONS)
    if(NOT v MATCHES "^[0-9A-Za-z.+-]+$")
        message(FATAL_ERROR
            "cmake/PluginCompat.cmake: '${v}' is not a version string")
    endif()
    if(PAYKAN_PLUGIN_COMPATIBLE_VERSIONS_CXX)
        string(APPEND PAYKAN_PLUGIN_COMPATIBLE_VERSIONS_CXX ", ")
    endif()
    string(APPEND PAYKAN_PLUGIN_COMPATIBLE_VERSIONS_CXX "\"${v}\"")
endforeach()
message(STATUS "Plugins accepted: built with ${PAYKAN_PLUGIN_COMPATIBLE_VERSIONS}")

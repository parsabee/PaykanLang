# src/PluginHost/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_plugin_host -- loads plugins at run time (include/paykan/PluginLoader.h):
# discovery (--plugin, $PAYKAN_PLUGIN_PATH, the user and system plugin
# directories), the version check before any plugin callback runs, and the
# host side of the C plugin interface (include/paykan/plugin_api.h): the host
# table and the adapters that make a plugin's C frontend a
# frontend::Frontend (reading the AST interchange format it returns) and its
# C backend a backend::Backend.
#
# Standard C++; the dynamic loader is behind the portability layer
# (src/Backends/Toolchain/Platform.h).  The plugins themselves link nothing
# of PaykanLang.
# ----------------------------------------------------------------------------

include(GNUInstallDirs)

# The system plugin directory, relative to the installed executable:
# <prefix>/<libdir>/paykan/plugins/<version> from <prefix>/<bindir>.
file(RELATIVE_PATH PAYKAN_PLUGIN_DIR_FROM_BINDIR
    "${CMAKE_INSTALL_FULL_BINDIR}"
    "${CMAKE_INSTALL_FULL_LIBDIR}/paykan/plugins/${PAYKAN_VERSION}")

add_library(paykan_plugin_host STATIC
    PluginLoader.cpp
    Host.cpp
    FrontendAdapter.cpp
    BackendAdapter.cpp
)
target_include_directories(paykan_plugin_host
    PUBLIC  ${PROJECT_SOURCE_DIR}/include
    # The portability layer (Platform.h) and this directory's private header.
    PRIVATE ${PROJECT_SOURCE_DIR}/src/Backends/Toolchain
            ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(paykan_plugin_host PRIVATE
    PAYKAN_PLUGIN_VERSION_DIR="${PAYKAN_VERSION}"
    PAYKAN_PLUGIN_DIR_FROM_BINDIR="${PAYKAN_PLUGIN_DIR_FROM_BINDIR}"
)
target_link_libraries(paykan_plugin_host
    PUBLIC  paykan_plugin
            paykan_frontend
            paykan_ast_interchange
            paykan_backend
            paykan_pir
            paykan_backend_toolchain
    PRIVATE paykan_compile_options
)

# src/Plugin/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_plugin -- the plugin registry's compatibility check
# (include/paykan/Registry.h): the running toolchain's version and the plugin
# build versions it accepts (cmake/PluginCompat.cmake, #103).  Standard C++
# only.  paykan_frontend and paykan_backend link it publicly, so every plugin
# and driver gets it.
# ----------------------------------------------------------------------------

include(GNUInstallDirs)

add_library(paykan_plugin STATIC
    PluginCompat.cpp
)

# The generated paykan/PluginCompat.h is part of the plugin interface: the
# PAYKAN_REGISTER_* macros expand its PAYKAN_PLUGIN_BUILD_VERSION.
target_include_directories(paykan_plugin
    PUBLIC ${PROJECT_SOURCE_DIR}/include
           ${PAYKAN_GENERATED_INCLUDE_DIR}
)

target_link_libraries(paykan_plugin
    PRIVATE paykan_compile_options
)

# paykan_plugin_api -- the C plugin interface (include/paykan/plugin_api.h),
# header only: what a loadable plugin compiles against (and all it needs of
# PaykanLang).  Exported as Paykan::plugin_api.
add_library(paykan_plugin_api INTERFACE)
target_include_directories(paykan_plugin_api INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

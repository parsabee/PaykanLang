# src/Plugin/CMakeLists.txt
# ----------------------------------------------------------------------------
# paykan_plugin -- the plugin registry's compatibility check
# (include/paykan/Registry.h): the running toolchain's version and the plugin
# build versions it accepts (cmake/PluginCompat.cmake, #103).  Standard C++
# only.  paykan_frontend and paykan_backend link it publicly, so every plugin
# and driver gets it.
# ----------------------------------------------------------------------------

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

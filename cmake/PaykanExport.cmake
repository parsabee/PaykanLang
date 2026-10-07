# Install rules and the `find_package(Paykan)` package, so that a backend or
# frontend can be written out of tree against the installed interfaces and
# linked into a custom `paykan` build (docs/writing-a-backend.md).
#
# Installed layout (relative to the prefix):
#   bin/paykan                        the driver with the built-in plugins
#   lib/paykan/plugins/<version>/     the system plugin directory (empty)
#   include/paykan/plugin_api.h       the C plugin interface (+ the generated
#                                     plugin_api_version.h it includes)
#   lib/libpaykan_*.a                 core (incl. lowering), driver, runtime and plugin libraries
#   lib/cmake/Paykan/                 PaykanConfig.cmake + exported targets
#   include/paykan/Runtime.h          the runtime ABI (C)
#   include/paykan/compiler/          the compiler's headers (AST.h, Sema.h,
#                                     paykan/Frontend.h, paykan/Backend.h, ...)
#
# Exported targets are namespaced Paykan:: and lose their paykan_ prefix:
# Paykan::plugin, Paykan::backend, Paykan::pir, Paykan::frontend,
# Paykan::driver, ... plus every built plugin (Paykan::frontend_recursive_descent,
# Paykan::backend_llvm, ...).

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(PAYKAN_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/Paykan)
set(PAYKAN_INSTALL_COMPILER_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR}/paykan/compiler)

# -- What gets exported ---------------------------------------------------------
set(PAYKAN_EXPORT_TARGETS
    paykan_compile_options
    paykan_plugin_api
    paykan_plugin
    paykan_ast
    paykan_ast_interchange
    paykan_diag
    paykan_frontend
    paykan_ast_printer
    paykan_pkm
    paykan_modules
    paykan_sema
    paykan_pir
    paykan_backend
    paykan_backend_toolchain
    paykan_lowering
    paykan_runtime
    paykan_plugin_host
    paykan_driver
)
get_property(PAYKAN_PLUGIN_TARGETS_LIST GLOBAL PROPERTY PAYKAN_PLUGIN_TARGETS)
list(APPEND PAYKAN_EXPORT_TARGETS ${PAYKAN_PLUGIN_TARGETS_LIST})
# The LLVM backend's private dependency is exported with it.
if(TARGET paykan_jit)
    list(APPEND PAYKAN_EXPORT_TARGETS paykan_jit)
endif()

# Public include directories point into the source/build tree; an installed
# consumer must see the install location instead.
foreach(t IN LISTS PAYKAN_EXPORT_TARGETS)
    string(REGEX REPLACE "^paykan_" "" short ${t})
    set_target_properties(${t} PROPERTIES EXPORT_NAME ${short})

    get_target_property(incs ${t} INTERFACE_INCLUDE_DIRECTORIES)
    if(incs)
        set(rewritten "")
        foreach(dir IN LISTS incs)
            if(dir MATCHES "^\\$<")
                list(APPEND rewritten "${dir}")           # already a genex
            elseif(dir STREQUAL "${PROJECT_SOURCE_DIR}/src/Runtime")
                list(APPEND rewritten "$<BUILD_INTERFACE:${dir}>"
                                      "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/paykan>")
            else()
                list(APPEND rewritten "$<BUILD_INTERFACE:${dir}>"
                                      "$<INSTALL_INTERFACE:${PAYKAN_INSTALL_COMPILER_INCLUDEDIR}>")
            endif()
        endforeach()
        set_target_properties(${t} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${rewritten}")
    endif()
    # LLVM's include directories (SYSTEM, on the backend's private deps) are
    # resolved through find_dependency(LLVM) in the config file.
    get_target_property(sysincs ${t} INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
    if(sysincs)
        set(rewritten "")
        foreach(dir IN LISTS sysincs)
            list(APPEND rewritten "$<BUILD_INTERFACE:${dir}>")
        endforeach()
        set_target_properties(${t} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${rewritten}")
    endif()
endforeach()

# -- Install ------------------------------------------------------------------
install(TARGETS paykan
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(TARGETS ${PAYKAN_EXPORT_TARGETS}
    EXPORT PaykanTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES ${PROJECT_SOURCE_DIR}/src/Runtime/Runtime.h
              ${PROJECT_SOURCE_DIR}/include/paykan/plugin_api.h
              ${PAYKAN_GENERATED_INCLUDE_DIR}/paykan/plugin_api_version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/paykan
)
# The system plugin directory, empty: `paykan` loads every plugin in it
# (include/paykan/PluginLoader.h), and plugin projects install into it
# (PAYKAN_PLUGIN_INSTALL_DIR, paykan_install_plugin).
set(PAYKAN_INSTALL_PLUGINDIR ${CMAKE_INSTALL_LIBDIR}/paykan/plugins/${PAYKAN_VERSION})
install(DIRECTORY DESTINATION ${PAYKAN_INSTALL_PLUGINDIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/
    DESTINATION ${PAYKAN_INSTALL_COMPILER_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)
install(FILES ${PAYKAN_GENERATED_INCLUDE_DIR}/Version.h
    DESTINATION ${PAYKAN_INSTALL_COMPILER_INCLUDEDIR}
)
install(FILES ${PAYKAN_GENERATED_INCLUDE_DIR}/paykan/PluginCompat.h
              ${PAYKAN_GENERATED_INCLUDE_DIR}/paykan/plugin_api_version.h
    DESTINATION ${PAYKAN_INSTALL_COMPILER_INCLUDEDIR}/paykan
)
install(EXPORT PaykanTargets
    NAMESPACE Paykan::
    DESTINATION ${PAYKAN_INSTALL_CMAKEDIR}
)

# -- Package config -----------------------------------------------------------
# The plugins built into this installation, as exported names, so a custom
# driver links them all.
set(PAYKAN_CONFIG_PLUGINS "")
foreach(t IN LISTS PAYKAN_PLUGIN_TARGETS_LIST)
    string(REGEX REPLACE "^paykan_" "" short ${t})
    list(APPEND PAYKAN_CONFIG_PLUGINS "Paykan::${short}")
endforeach()

# Where the runtime is installed (relative to the prefix; the config file
# makes the paths absolute wherever the package ends up), so a driver built
# against the package finds it there (paykan_add_driver).
set(PAYKAN_CONFIG_RUNTIME_LIBRARY
    "${CMAKE_INSTALL_LIBDIR}/${CMAKE_STATIC_LIBRARY_PREFIX}paykan_runtime${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(PAYKAN_CONFIG_RUNTIME_INCLUDE_DIR "${CMAKE_INSTALL_INCLUDEDIR}/paykan")
set(PAYKAN_CONFIG_PLUGIN_INSTALL_DIR "${PAYKAN_INSTALL_PLUGINDIR}")
set(PAYKAN_CONFIG_EXECUTABLE "${CMAKE_INSTALL_BINDIR}/paykan${CMAKE_EXECUTABLE_SUFFIX}")

configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/PaykanConfig.cmake.in
    ${CMAKE_BINARY_DIR}/cmake/PaykanConfig.cmake
    INSTALL_DESTINATION ${PAYKAN_INSTALL_CMAKEDIR}
    PATH_VARS PAYKAN_CONFIG_RUNTIME_LIBRARY PAYKAN_CONFIG_RUNTIME_INCLUDE_DIR
              PAYKAN_CONFIG_PLUGIN_INSTALL_DIR PAYKAN_CONFIG_EXECUTABLE
)
write_basic_package_version_file(
    ${CMAKE_BINARY_DIR}/cmake/PaykanConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)
install(FILES
    ${CMAKE_BINARY_DIR}/cmake/PaykanConfig.cmake
    ${CMAKE_BINARY_DIR}/cmake/PaykanConfigVersion.cmake
    DESTINATION ${PAYKAN_INSTALL_CMAKEDIR}
)

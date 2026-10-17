# The standard library: the modules of `import ::name;` (stdlib/*.pkn).
#
# Each module is staged in the build tree where `paykan` looks for its
# installed standard library, <libdir>/paykan/stdlib/<version>/ next to
# <bindir> (PAYKAN_STDLIB_DIR_FROM_BINDIR, CMakeLists.txt), with its .pkm
# compiled by the `paykan` just built.  The source goes next to the .pkm: a
# module with generic declarations is built from its source until a .pkm
# holds templates (docs/language/11-generics.md, "Modules").  `cmake
# --install` installs both.

if(NOT PAYKAN_BACKENDS)
    message(STATUS "Standard library not compiled: PAYKAN_BACKENDS is empty, "
                   "and --emit-pkm needs a backend")
    return()
endif()

file(GLOB PAYKAN_STDLIB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.pkn")
cmake_path(SET PAYKAN_STDLIB_STAGE_DIR NORMALIZE
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${PAYKAN_STDLIB_DIR_FROM_BINDIR}")
set(PAYKAN_STDLIB_STAGE_DIR ${PAYKAN_STDLIB_STAGE_DIR} PARENT_SCOPE)

set(staged_sources "")
foreach(src IN LISTS PAYKAN_STDLIB_SOURCES)
    get_filename_component(name ${src} NAME)
    set(staged ${PAYKAN_STDLIB_STAGE_DIR}/${name})
    add_custom_command(OUTPUT ${staged}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${src} ${staged}
        DEPENDS ${src}
        VERBATIM)
    list(APPEND staged_sources ${staged})
endforeach()

# The environment `paykan` runs in here: the staged library, no plugins, and
# for an instrumented build the settings the test jobs give it (an ASan
# `paykan` with the LLVM backend reports container-overflow in LLVM's
# uninstrumented static initialisers; a coverage one writes its profile in
# this build directory, not in the current one).
set(paykan_env PAYKAN_STDLIB=${PAYKAN_STDLIB_STAGE_DIR} PAYKAN_NO_PLUGINS=1)
if(PAYKAN_ASAN)
    list(APPEND paykan_env ASAN_OPTIONS=detect_container_overflow=0:detect_leaks=0)
endif()
if(PAYKAN_COVERAGE)
    list(APPEND paykan_env LLVM_PROFILE_FILE=${CMAKE_CURRENT_BINARY_DIR}/%p.profraw)
endif()

# A module's .pkm depends on every staged source: it may import any of them.
set(module_files "")
foreach(src IN LISTS PAYKAN_STDLIB_SOURCES)
    get_filename_component(module ${src} NAME_WE)
    set(pkm ${PAYKAN_STDLIB_STAGE_DIR}/${module}.pkm)
    add_custom_command(OUTPUT ${pkm}
        COMMAND ${CMAKE_COMMAND} -E env ${paykan_env}
                $<TARGET_FILE:paykan> --no-module-cache --emit-pkm
                -o ${pkm} ${PAYKAN_STDLIB_STAGE_DIR}/${module}.pkn
        DEPENDS paykan ${staged_sources}
        COMMENT "Compiling the standard library module ::${module}"
        VERBATIM)
    list(APPEND module_files ${pkm})
endforeach()

add_custom_target(paykan_stdlib ALL DEPENDS ${staged_sources} ${module_files})

install(FILES ${staged_sources} ${module_files}
    DESTINATION ${PAYKAN_INSTALL_STDLIBDIR})

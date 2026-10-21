# The standard library: the modules of `import ::name;` (stdlib/*.pkn).
#
# Each module is compiled by the `paykan` just built into a .pkm named
# `::name`, staged in the build tree where `paykan` looks for its installed
# standard library, <libdir>/paykan/stdlib/<version>/ next to <bindir>
# (PAYKAN_STDLIB_DIR_FROM_BINDIR, CMakeLists.txt).  Only the .pkm files are
# staged and installed: a .pkm holds the module's templates too
# (docs/design/pkm.md §4), so nothing reads the sources once they are
# compiled.  A module is compiled against the .pkm files of the modules it
# imports, which are built before it.

file(GLOB PAYKAN_STDLIB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.pkn")
cmake_path(SET PAYKAN_STDLIB_STAGE_DIR NORMALIZE
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${PAYKAN_STDLIB_DIR_FROM_BINDIR}")
set(PAYKAN_STDLIB_STAGE_DIR ${PAYKAN_STDLIB_STAGE_DIR} PARENT_SCOPE)

# A build tree configured when the sources were staged still has them, and
# `paykan` would read them rather than the .pkm files.
file(GLOB stale_sources "${PAYKAN_STDLIB_STAGE_DIR}/*.pkn")
if(stale_sources)
    file(REMOVE ${stale_sources})
endif()

# The environment `paykan` runs in here: the staged library, no plugins, and
# for an instrumented build the settings the test jobs give it (an ASan
# `paykan`, which links LLVM, reports container-overflow in LLVM's
# uninstrumented static initialisers, and use-after-poison in the slabs of
# MLIR's allocators; a coverage one writes its profile in this build
# directory, not in the current one).
set(paykan_env PAYKAN_STDLIB=${PAYKAN_STDLIB_STAGE_DIR} PAYKAN_NO_PLUGINS=1)
if(PAYKAN_ASAN)
    list(APPEND paykan_env
         ASAN_OPTIONS=detect_container_overflow=0:detect_leaks=0:allow_user_poisoning=0)
endif()
if(PAYKAN_COVERAGE)
    list(APPEND paykan_env LLVM_PROFILE_FILE=${CMAKE_CURRENT_BINARY_DIR}/%p.profraw)
endif()

set(module_files "")
foreach(src IN LISTS PAYKAN_STDLIB_SOURCES)
    get_filename_component(module ${src} NAME_WE)
    # The .pkm files of the modules it imports (`import ::name ...;`).
    file(STRINGS ${src} imports REGEX "^import ::")
    set(imported "")
    foreach(line IN LISTS imports)
        string(REGEX MATCH "^import ::([A-Za-z_][A-Za-z0-9_]*)" _ "${line}")
        list(APPEND imported ${PAYKAN_STDLIB_STAGE_DIR}/${CMAKE_MATCH_1}.pkm)
    endforeach()
    set(pkm ${PAYKAN_STDLIB_STAGE_DIR}/${module}.pkm)
    add_custom_command(OUTPUT ${pkm}
        COMMAND ${CMAKE_COMMAND} -E env ${paykan_env}
                $<TARGET_FILE:paykan> --no-module-cache --emit-pkm
                --module-name=::${module} -o ${pkm} ${src}
        DEPENDS paykan ${src} ${imported}
        COMMENT "Compiling the standard library module ::${module}"
        VERBATIM)
    list(APPEND module_files ${pkm})
endforeach()

add_custom_target(paykan_stdlib ALL DEPENDS ${module_files})

install(FILES ${module_files}
    DESTINATION ${PAYKAN_INSTALL_STDLIBDIR})

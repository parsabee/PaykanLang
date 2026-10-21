# paykan_modules -- the module resolver (docs/design/pkm.md §8, prototype):
# where an import comes from (source, cache entry, prebuilt .pkm), the
# freshness verdict, and writing .pkm files; and what a .pkm keeps of a
# module's code (ModuleCode.h, §5.13).  Format and PIR only: Sema and
# the lowering call it, it never calls them.

add_library(paykan_modules STATIC
    ModuleCode.cpp
    ModuleResolver.cpp
)

target_include_directories(paykan_modules
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    # Runtime.h, for the runtime ABI version the manifests record.
    PRIVATE ${PROJECT_SOURCE_DIR}/src/Runtime
)

# The build ID it stamps into cache entries (BuildId.h, CMakeLists.txt).
add_dependencies(paykan_modules paykan_build_id)

target_link_libraries(paykan_modules
    PUBLIC  paykan_pkm
            paykan_pir
            paykan_plugin
    PRIVATE paykan_compile_options
)

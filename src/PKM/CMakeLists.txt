# paykan_pkm -- the .pkm module file format (docs/design/pkm.md): the
# container, the MANIFEST and IFACE codecs, the compatibility verdict and
# the `pkm dump` text.  Format only, standard C++: no Sema, no PIR, so
# plugins and out-of-tree tools can link it alone.

add_library(paykan_pkm STATIC
    File.cpp
    Manifest.cpp
    Interface.cpp
    Dump.cpp
)

target_include_directories(paykan_pkm
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(paykan_pkm
    PRIVATE paykan_compile_options
)

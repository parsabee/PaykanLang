# A PaykanLang backend plugin in Rust: the `pir-stats` backend (src/lib.rs),
# a cdylib against the C plugin interface (docs/plugins/plugin-api.md).
#
# Built with rustc alone (no crates, no network), against an installed
# PaykanLang only for its version check and its plugin directory:
#
#   cmake -B build -DCMAKE_PREFIX_PATH=<prefix> utils/pir-stats-rust
#   cmake --build build
#   <prefix>/bin/paykan --plugin=build/libpaykan_backend_pir_stats.so \
#       --backend=pir-stats --emit-source program.pkn
#   cmake --install build    # into <prefix>/lib/paykan/plugins/<version>/
#
# `cargo build --release` builds the same library (Cargo.toml).
cmake_minimum_required(VERSION 3.24)
project(PaykanPIRStatsRust LANGUAGES NONE)

find_package(Paykan REQUIRED)
find_program(RUSTC rustc REQUIRED)

# The PaykanLang version the plugin is built with (empty: the installed
# one); configure fails unless the installation accepts it (#103).
set(PIR_STATS_BUILT_WITH "" CACHE STRING
    "PaykanLang version pir-stats is built with (empty: the installed one)")
paykan_check_plugin_built_with(backend pir-stats "${PIR_STATS_BUILT_WITH}" built_with)

if(APPLE)
    set(suffix ".dylib")
else()
    set(suffix ".so")
endif()
set(lib ${CMAKE_CURRENT_BINARY_DIR}/libpaykan_backend_pir_stats${suffix})
add_custom_command(
    OUTPUT ${lib}
    COMMAND ${CMAKE_COMMAND} -E env PAYKAN_PLUGIN_BUILD_VERSION=${built_with}
            ${RUSTC} --edition 2021 --crate-type cdylib
                     --crate-name paykan_backend_pir_stats
                     -C opt-level=2 -C panic=unwind -D warnings
                     -o ${lib} ${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs
    COMMENT "rustc: libpaykan_backend_pir_stats${suffix}"
    VERBATIM)
add_custom_target(paykan_backend_pir_stats ALL DEPENDS ${lib})

install(FILES ${lib} DESTINATION "${PAYKAN_PLUGIN_INSTALL_DIR}")

# The paykan driver.
#
# paykan_driver -- the driver as a static library (it defines main()), so an
#                  out-of-tree plugin can be linked into a custom driver with
#                  paykan_add_driver() from the installed package.
# paykan        -- the driver executable with every plugin of this build.

add_library(paykan_driver STATIC main.cpp Options.cpp)
# Where `paykan` looks for its installed standard library (CMakeLists.txt).
target_compile_definitions(paykan_driver PRIVATE
    PAYKAN_STDLIB_DIR_FROM_BINDIR="${PAYKAN_STDLIB_DIR_FROM_BINDIR}"
)
target_link_libraries(paykan_driver
    PUBLIC  paykan_ast_printer
            paykan_sema
            paykan_frontend
            paykan_backend
            paykan_lowering
            paykan_modules
            paykan_pkm
            paykan_pir
            paykan_plugin_host
            paykan_ast_interchange
    PRIVATE paykan_compile_options
)

add_executable(paykan driver_main.cpp)
target_link_libraries(paykan
    PRIVATE paykan_driver
            paykan_compile_options
)
paykan_link_plugins(paykan)

# src/Driver/CMakeLists.txt
# ----------------------------------------------------------------------------
# The paykan driver.
#
# paykan_driver -- the driver as a static library (it defines main()), so an
#                  out-of-tree plugin can be linked into a custom driver with
#                  paykan_add_driver() from the installed package.
# paykan        -- the driver executable with every plugin of this build.
# ----------------------------------------------------------------------------

add_library(paykan_driver STATIC main.cpp Options.cpp)
target_link_libraries(paykan_driver
    PUBLIC  paykan_ast_printer
            paykan_sema
            paykan_frontend
            paykan_backend
            paykan_lowering
            paykan_pir
    PRIVATE paykan_compile_options
)

add_executable(paykan driver_main.cpp)
target_link_libraries(paykan
    PRIVATE paykan_driver
            paykan_compile_options
)
paykan_link_plugins(paykan)

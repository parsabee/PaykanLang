# paykan_lowering -- AST -> PIR lowering (standard C++ only).  The single home
# of the ownership semantics every backend relies on.

add_library(paykan_lowering STATIC
    Lowering.cpp
    LoweringExpr.cpp
    LoweringMatch.cpp
    LoweringClass.cpp
    LoweringClone.cpp
    LoweringProgram.cpp
)
target_include_directories(paykan_lowering
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(paykan_lowering
    PUBLIC  paykan_sema
            paykan_pir
    PRIVATE paykan_compile_options
)

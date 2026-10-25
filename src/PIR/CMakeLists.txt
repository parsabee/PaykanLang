# paykan_pir -- the Paykan IR: data structures (include/paykan/pir/PIR.h),
# text printer, text parser, verifier, the binary codec of the .pkm CODE
# section (docs/design/pkm.md §5), and the passes over a program (Pass.h,
# DeadCode.h, MoveAtLastUse.h, Lend.h, RCElimination.h).  Standard C++
# only; every backend consumes it.

add_library(paykan_pir STATIC
    Binary.cpp
    DeadCode.cpp
    Lend.cpp
    MoveAtLastUse.cpp
    Pass.cpp
    PIR.cpp
    Printer.cpp
    Parser.cpp
    RCElimination.cpp
    Verifier.cpp
)

target_include_directories(paykan_pir
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(paykan_pir
    PRIVATE paykan_compile_options
)

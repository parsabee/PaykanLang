// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The `pir` MLIR dialect (PIR.td): its dialect, types and ops.

#pragma once

#include <mlir/Bytecode/BytecodeOpInterface.h>
#include <mlir/IR/BuiltinTypes.h>
#include <mlir/IR/Dialect.h>
#include <mlir/IR/OpDefinition.h>
#include <mlir/IR/OpImplementation.h>
#include <mlir/IR/SymbolTable.h>
#include <mlir/Interfaces/CallInterfaces.h>
#include <mlir/Interfaces/FunctionInterfaces.h>
#include <mlir/Interfaces/InferTypeOpInterface.h>
#include <mlir/Interfaces/SideEffectInterfaces.h>

#include "PIRDialect.h.inc"
#include "PIREnums.h.inc"

#define GET_TYPEDEF_CLASSES
#include "PIRTypes.h.inc"

#define GET_OP_CLASSES
#include "PIROps.h.inc"

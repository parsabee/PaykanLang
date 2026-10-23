// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The `pir` MLIR dialect: registration and the parts ODS does not generate.

#include "PIRDialect.h"

#include <llvm/ADT/TypeSwitch.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/DialectImplementation.h>
#include <mlir/Interfaces/FunctionImplementation.h>

using namespace mlir;
using namespace paykan::pir_dialect;

#include "PIRDialect.cpp.inc"
#include "PIREnums.cpp.inc"

#define GET_TYPEDEF_CLASSES
#include "PIRTypes.cpp.inc"

#define GET_OP_CLASSES
#include "PIROps.cpp.inc"

void PIRDialect::initialize() {
  addTypes<
#define GET_TYPEDEF_LIST
#include "PIRTypes.cpp.inc"
      >();
  addOperations<
#define GET_OP_LIST
#include "PIROps.cpp.inc"
      >();
}

// -- pir.func: printed and parsed like func.func

ParseResult FuncOp::parse(OpAsmParser &parser, OperationState &result) {
  auto buildFuncType =
      [](Builder &builder, ArrayRef<Type> argTypes, ArrayRef<Type> results,
         function_interface_impl::VariadicFlag,
         std::string &) { return builder.getFunctionType(argTypes, results); };
  return function_interface_impl::parseFunctionOp(
      parser, result, /*allowVariadic=*/false,
      getFunctionTypeAttrName(result.name), buildFuncType,
      getArgAttrsAttrName(result.name), getResAttrsAttrName(result.name));
}

void FuncOp::print(OpAsmPrinter &p) {
  function_interface_impl::printFunctionOp(
      p, *this, /*isVariadic=*/false, getFunctionTypeAttrName(),
      getArgAttrsAttrName(), getResAttrsAttrName());
}

// -- pir.switch: a case value of the value's type per case region

LogicalResult SwitchOp::verify() {
  if (getCases().size() != getCaseRegions().size())
    return emitOpError("has ") << getCases().size() << " case values for "
                               << getCaseRegions().size() << " case regions";
  for (Attribute c : getCases()) {
    auto typed = dyn_cast<TypedAttr>(c);
    if (!typed || typed.getType() != getValue().getType())
      return emitOpError("case ")
             << c << " is not a constant of type " << getValue().getType();
  }
  return success();
}

// -- pir.local: printed with the local's name

void LocalOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getSlot(), getName());
}

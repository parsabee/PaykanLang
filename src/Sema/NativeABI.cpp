// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0

#include "NativeABI.h"

namespace paykan::sema {

using pir::Type;

pir::Type pirType(const ast::ASTContext &ctx, ast::Type *ty) {
  if (auto *bt = ast::dyn_cast<ast::BuiltinType>(ty)) {
    // Every integer type is held sign- or zero-extended in an i64 and every
    // floating-point type as the f64 it is (docs/design/sized-types.md).
    switch (bt->getTypeKind()) {
    case ast::BuiltinType::Int:
    case ast::BuiltinType::I8:
    case ast::BuiltinType::I16:
    case ast::BuiltinType::I32:
    case ast::BuiltinType::U8:
    case ast::BuiltinType::U16:
    case ast::BuiltinType::U32:
    case ast::BuiltinType::U64:
      return Type::I64;
    case ast::BuiltinType::Float:
    case ast::BuiltinType::F16:
    case ast::BuiltinType::F32:
      return Type::F64;
    case ast::BuiltinType::Bool:
      return Type::Bool;
    case ast::BuiltinType::Char:
      return Type::Char;
    case ast::BuiltinType::Void:
      return Type::Void;
    }
  }
  // A parser ClassType stub may name an enum or a struct (resolved by
  // Sema): an enum is an i64 value, not a box, and a tagged union or a
  // struct is held in memory.
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    if (auto *et = ctx.lookupEnumType(ct->getName()))
      return pirType(ctx, et);
    if (ctx.lookupStructType(ct->getName()))
      return Type::Mem;
  }
  if (ast::isa<ast::EnumType>(ty))
    return ast::isPayloadEnum(ty) ? Type::Mem : Type::I64;
  // An optional of a value type is a value too (#96): `int?` is opt_i64.
  if (ast::isValueOptional(ty))
    return pir::optionalOf(
        pirType(ctx, ast::cast<ast::OptionalType>(ty)->getInnerType()));
  if (ast::isRefType(ty))
    return Type::Box;
  // Never a value: a fixed-size array or a tuple is held in a `mem` slot
  // (LoweringMem.cpp).
  if (ast::isMemType(ty))
    return Type::Mem;
  return Type::Void;
}

pir::Signature nativeSignature(const ast::ASTContext &ctx,
                               const ast::FuncDecl &fn) {
  ast::Type *retTy = fn.getReturnType();
  const Type ret = retTy ? pirType(ctx, retTy) : Type::Void;
  const bool resultByAddress = fn.resultByAddress() && ret != Type::Void;
  const bool viewResult = fn.getResultMode() == ast::ParamMode::View;
  pir::Signature sig;
  sig.Params.assign(fn.getTypeArgs().size(), Type::Ptr);
  if (resultByAddress && !viewResult)
    sig.Params.push_back(Type::Ptr); // where C writes the result
  for (size_t i = 0; i < fn.getParams().size(); ++i) {
    const ast::Param &p = fn.getParams()[i];
    if (p.Mode == ast::ParamMode::Inout || ast::isMemType(p.ParamType) ||
        fn.paramByAddress(i)) {
      sig.Params.push_back(Type::Ptr);
      continue;
    }
    const Type t = pirType(ctx, p.ParamType);
    sig.Params.push_back(t == Type::Box ? Type::Obj : t); // C borrows it
  }
  sig.Ret = !resultByAddress ? ret : viewResult ? Type::Ptr : Type::Void;
  return sig;
}

} // namespace paykan::sema

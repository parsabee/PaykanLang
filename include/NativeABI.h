// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// How a value and a `native fn` cross into PIR and C: the PIR type a value
// of a Paykan type is held in, and the signature of the C function a native
// function calls.  Sema checks the C signatures of the natives a module
// binds with it, and the lowering declares and calls the C functions with
// it, so the two cannot disagree.

#pragma once

#include "AST.h"
#include "ASTContext.h"
#include "paykan/pir/PIR.h"

namespace paykan::sema {

/// The PIR type a value of type @p ty is held in (docs/pir.md §1): an
/// integer of any size an i64, a float of any size an f64, a bool, a char,
/// an enum without fields an i64, an optional of one of these its inline
/// optional, a reference a box, a value held in memory (a tuple, a struct,
/// a tagged union, a fixed-size array) mem, and void for anything else.
/// A parser's ClassType naming an enum or a struct is looked up in @p ctx.
pir::Type pirType(const ast::ASTContext &ctx, ast::Type *ty);

/// The signature of the C function native function @p fn calls (#198;
/// docs/language/03-functions.md, "Native Functions"), the `$c.<symbol>`
/// extern's: a `PaykanType` pointer per type argument, then the pointer to
/// a `-> T` result's storage, then each parameter (an `inout` one, a value
/// held in memory or what a generic one's template typed `T` by address, a
/// reference as its raw object, a scalar as itself); the result is the
/// scalar or the owned box returned, the address of a `-> view T` result,
/// or nothing when C writes it.
pir::Signature nativeSignature(const ast::ASTContext &ctx,
                               const ast::FuncDecl &fn);

} // namespace paykan::sema

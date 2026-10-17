// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// AST -> PIR lowering: the builtin `Buffer<T>` (ASTContext::getBufferType,
// docs/language/05-arrays.md, "Buffer").
//
// A buffer is an object of the runtime's (PaykanBuffer), shared like any
// class's.  Its methods are calls of the PaykanBuffer_* functions, passed
// T's type descriptor (emitTypeDescriptor) and then the buffer's object.  A
// value crosses by address: `set` reads it from a local, `get` returns the
// slot's address, and `copy` writes a copy, its references retained, to a
// local.  A reference read with `get` is retained too: a method's reference
// result is its caller's.

#include "LoweringInternal.h"
#include "Names.h"

namespace paykan::lowering {

using namespace names;
using pir::Type;

Val ModuleLowering::ExprEmitter::emitBufferNew(ast::CallExpr *node) {
  auto *ct = ast::cast<ast::ClassType>(node->getResolvedType());
  Val type = L.emitTypeDescriptor(L.ASTCtx.getBufferElemType(ct));
  return L.callRuntime(kPaykanBufferNew, {type, Val::i64(kBufferInitialLength)},
                       "buf");
}

Val ModuleLowering::ExprEmitter::emitBufferCall(ast::MethodCallExpr *node,
                                                const Val &buf,
                                                ast::Type *elemTy) {
  const std::string &name = node->getMethodName();
  Val type = L.emitTypeDescriptor(elemTy);
  if (name == kLen)
    return L.callRuntime(kPaykanBufferLen, {type, buf}, "len");
  // Every other method's first argument is an index or a count.
  Val n = visit(node->getArguments()[0]);
  if (!n)
    return Val();
  n = L.coerceTo(n, Type::I64);
  const Type slot = L.toPIRType(elemTy);
  if (name == kBufferGet) {
    Val addr = L.callRuntime(kPaykanBufferGet, {type, buf, n}, "slot");
    Val v = L.B.ptrLoad(addr, slot, "elem");
    if (v.Ty == Type::Box)
      L.emitRetain(v);
    return v;
  }
  if (name == kBufferCopy) {
    pir::LocalId out = L.B.addLocal("copied", slot);
    L.callRuntime(kPaykanBufferCopy,
                  {type, L.B.localAddr(out, "copied.addr"), buf, n});
    return L.B.load(out, "copied");
  }
  if (name == kBufferSet) {
    ast::Expr *arg = node->getArguments()[1];
    std::vector<Val> keep;
    Val v = slot == Type::Box ? L.emitBorrowedBox(arg, keep) : visit(arg);
    if (!v)
      return Val();
    pir::LocalId value = L.B.addLocal("value", slot);
    L.B.store(value, L.coerceTo(v, slot));
    L.callRuntime(kPaykanBufferSet,
                  {type, buf, n, L.B.localAddr(value, "value.addr")});
    L.releaseAfterCall(keep);
    return Val();
  }
  const char *fn = name == kBufferReserve ? kPaykanBufferReserve
                   : name == kBufferGrow  ? kPaykanBufferGrow
                                          : kPaykanBufferShrink;
  L.callRuntime(fn, {type, buf, n});
  return Val();
}

} // namespace paykan::lowering

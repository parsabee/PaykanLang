// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// CodeGenNames.h — IR label / name constants shared across CodeGen translation
// units.  File-local helpers (e.g. getAllFieldsInOrder) stay in anonymous
// namespaces inside their respective .cpp files.

#pragma once

namespace paykan {
namespace codegen {

// ---------------------------------------------------------------------------
// Control-flow block names
// ---------------------------------------------------------------------------

inline constexpr const char *kIREntry = "entry";
inline constexpr const char *kIRIfThen = "if.then";
inline constexpr const char *kIRIfElse = "if.else";
inline constexpr const char *kIRIfEnd = "if.end";
inline constexpr const char *kIRWhileCond = "while.cond";
inline constexpr const char *kIRWhileBody = "while.body";
inline constexpr const char *kIRWhileEnd = "while.end";
inline constexpr const char *kIRBreakDead = "break.dead";
inline constexpr const char *kIRContDead = "cont.dead";
inline constexpr const char *kIRTernThen = "tern.then";
inline constexpr const char *kIRTernElse = "tern.else";
inline constexpr const char *kIRTernEnd = "tern.end";
inline constexpr const char *kIRAndRhs = "and.rhs";
inline constexpr const char *kIRAndEnd = "and.end";
inline constexpr const char *kIROrRhs = "or.rhs";
inline constexpr const char *kIROrEnd = "or.end";
inline constexpr const char *kIRDivZeroChk = "divzero.chk";
inline constexpr const char *kIRDivZeroPanic = "divzero.panic";
inline constexpr const char *kIRDivZeroCont = "divzero.cont";
inline constexpr const char *kIRMatchEnd = "match.end";
inline constexpr const char *kIRMatchWildcard = "match.wildcard";
inline constexpr const char *kIRMatchArmPfx = "match.arm";
inline constexpr const char *kIRMatchCheckPfx = "match.check";
inline constexpr const char *kIRMatchEq = "match.eq";
inline constexpr const char *kIRMatchNone = "match.none";
inline constexpr const char *kIRMatchSome = "match.some";
inline constexpr const char *kIROptNone = "opt.none";
inline constexpr const char *kIROptSome = "opt.some";
inline constexpr const char *kIROptMerge = "opt.merge";

// ---------------------------------------------------------------------------
// Value names (optional types)
// ---------------------------------------------------------------------------

inline constexpr const char *kIROptIsNone = "opt.isnone";
inline constexpr const char *kIROptAnyNone = "opt.anynone";
inline constexpr const char *kIROptBothNone = "opt.bothnone";
inline constexpr const char *kIROptNoneBox = "opt.none.box";
inline constexpr const char *kIROptBox = "opt.box";
inline constexpr const char *kIROptObj = "opt.obj";

// ---------------------------------------------------------------------------
// Value names (general)
// ---------------------------------------------------------------------------

inline constexpr const char *kIRStr = "str";
inline constexpr const char *kIRNewBox = "new.box";
inline constexpr const char *kIROldBox = "old.box";
inline constexpr const char *kIRShared = "shared";
inline constexpr const char *kIRSubjObj = "subj.obj";
inline constexpr const char *kIRRecvObj = "recv.obj";
inline constexpr const char *kIRLhsObj = "lhs.obj";
inline constexpr const char *kIRRhsObj = "rhs.obj";
inline constexpr const char *kIRUnboxed = "unboxed";
inline constexpr const char *kIRBoolExt = "boolext";
inline constexpr const char *kIRArgShared = "arg.shared";
inline constexpr const char *kIRCall = "call";
inline constexpr const char *kIRConcat = "concat";
inline constexpr const char *kIRAnd = "and";
inline constexpr const char *kIROr = "or";
inline constexpr const char *kIRTern = "tern";

// ---------------------------------------------------------------------------
// Value names (vtable / method dispatch)
// ---------------------------------------------------------------------------

inline constexpr const char *kIRVtable = "vtable";
inline constexpr const char *kIRVtSlot = "vtslot";
inline constexpr const char *kIRVfn = "vfn";
inline constexpr const char *kIRMcall = "mcall";
inline constexpr const char *kIRMcallShared = "mcall.shared";
inline constexpr const char *kIRArrLen = "arr.len";

// ---------------------------------------------------------------------------
// Value names (class / object internals)
// ---------------------------------------------------------------------------

inline constexpr const char *kIRObj = "obj";
inline constexpr const char *kIRVtableSlot = "vtable.slot";
inline constexpr const char *kIRSharedSlot = "shared.slot";
inline constexpr const char *kIRVtablePtr = "vtable.ptr";
inline constexpr const char *kIRVtablePrefix = "vtable.";
inline constexpr const char *kIRVtableExpected = "vtable.expected.";
inline constexpr const char *kIRIsPrefix = "is.";
inline constexpr const char *kIRIsNull = "is.null";
inline constexpr const char *kIRFieldPrefix = "field.";
inline constexpr const char *kIRFieldShared = "field.shared";
inline constexpr const char *kIROldField = "old.field";
inline constexpr const char *kIRFieldRel = "field.rel";
inline constexpr const char *kIRFieldAfter = "field.after";

// ---------------------------------------------------------------------------
// Value names (array / subscript emission)
// ---------------------------------------------------------------------------

inline constexpr const char *kIRArr = "arr";
inline constexpr const char *kIRArrShared = "arr.shared";
inline constexpr const char *kIRArrData = ".arr.data";
inline constexpr const char *kIRElemRaw = "elem.raw";
inline constexpr const char *kIRElemShared = "elem.shared";
inline constexpr const char *kIRElemObj = "elem.obj";
inline constexpr const char *kIRElemF64 = "elem.f64";
inline constexpr const char *kIRElemBool = "elem.bool";
inline constexpr const char *kIRIdxExt = "idx.ext";
inline constexpr const char *kIRF64Bits = "f64.bits";
inline constexpr const char *kIRBoolExtArr = "bool.ext";

// ---------------------------------------------------------------------------
// Value names (arithmetic / comparison)
// ---------------------------------------------------------------------------

inline constexpr const char *kIRFNeg = "fneg";
inline constexpr const char *kIRNeg = "neg";
inline constexpr const char *kIRNot = "not";
inline constexpr const char *kIRFAdd = "fadd";
inline constexpr const char *kIRAdd = "add";
inline constexpr const char *kIRFSub = "fsub";
inline constexpr const char *kIRSub = "sub";
inline constexpr const char *kIRFMul = "fmul";
inline constexpr const char *kIRMul = "mul";
inline constexpr const char *kIRFDiv = "fdiv";
inline constexpr const char *kIRSDiv = "sdiv";
inline constexpr const char *kIRFMod = "fmod";
inline constexpr const char *kIRSRem = "srem";
inline constexpr const char *kIRFLT = "flt";
inline constexpr const char *kIRSLT = "slt";
inline constexpr const char *kIRFGT = "fgt";
inline constexpr const char *kIRSGT = "sgt";
inline constexpr const char *kIRFLE = "fle";
inline constexpr const char *kIRSLE = "sle";
inline constexpr const char *kIRFGE = "fge";
inline constexpr const char *kIRSGE = "sge";
inline constexpr const char *kIRFEQ = "feq";
inline constexpr const char *kIREQ = "eq";
inline constexpr const char *kIRFNE = "fne";
inline constexpr const char *kIRNE = "ne";

} // namespace codegen
} // namespace paykan

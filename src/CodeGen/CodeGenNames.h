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

inline constexpr const char *kIREntry          = "entry";
inline constexpr const char *kIRIfThen         = "if.then";
inline constexpr const char *kIRIfElse         = "if.else";
inline constexpr const char *kIRIfEnd          = "if.end";
inline constexpr const char *kIRWhileCond      = "while.cond";
inline constexpr const char *kIRWhileBody      = "while.body";
inline constexpr const char *kIRWhileEnd       = "while.end";
inline constexpr const char *kIRBreakDead      = "break.dead";
inline constexpr const char *kIRContDead       = "cont.dead";
inline constexpr const char *kIRTernThen       = "tern.then";
inline constexpr const char *kIRTernElse       = "tern.else";
inline constexpr const char *kIRTernEnd        = "tern.end";
inline constexpr const char *kIRAndRhs         = "and.rhs";
inline constexpr const char *kIRAndEnd         = "and.end";
inline constexpr const char *kIROrRhs          = "or.rhs";
inline constexpr const char *kIROrEnd          = "or.end";
inline constexpr const char *kIRMatchEnd       = "match.end";
inline constexpr const char *kIRMatchWildcard  = "match.wildcard";
inline constexpr const char *kIRMatchArmPfx    = "match.arm";
inline constexpr const char *kIRMatchCheckPfx  = "match.check";

// ---------------------------------------------------------------------------
// Value names (general)
// ---------------------------------------------------------------------------

inline constexpr const char *kIRStr            = "str";
inline constexpr const char *kIRNewBox         = "new.box";
inline constexpr const char *kIROldBox         = "old.box";
inline constexpr const char *kIRShared         = "shared";
inline constexpr const char *kIRSubjObj        = "subj.obj";
inline constexpr const char *kIRRecvObj        = "recv.obj";
inline constexpr const char *kIRLhsObj         = "lhs.obj";
inline constexpr const char *kIRRhsObj         = "rhs.obj";
inline constexpr const char *kIRUnboxed        = "unboxed";
inline constexpr const char *kIRBoolExt        = "boolext";
inline constexpr const char *kIRArgShared      = "arg.shared";
inline constexpr const char *kIRCall           = "call";
inline constexpr const char *kIRConcat         = "concat";
inline constexpr const char *kIRAnd            = "and";
inline constexpr const char *kIROr             = "or";
inline constexpr const char *kIRTern           = "tern";

// ---------------------------------------------------------------------------
// Value names (vtable / method dispatch)
// ---------------------------------------------------------------------------

inline constexpr const char *kIRVtable         = "vtable";
inline constexpr const char *kIRVtSlot         = "vtslot";
inline constexpr const char *kIRVfn            = "vfn";
inline constexpr const char *kIRMcall          = "mcall";
inline constexpr const char *kIRMcallShared    = "mcall.shared";

// ---------------------------------------------------------------------------
// Value names (class / object internals)
// ---------------------------------------------------------------------------

inline constexpr const char *kIRObj            = "obj";
inline constexpr const char *kIRVtableSlot     = "vtable.slot";
inline constexpr const char *kIRVtablePtr      = "vtable.ptr";
inline constexpr const char *kIRVtablePrefix   = "vtable.";
inline constexpr const char *kIRVtableExpected = "vtable.expected.";
inline constexpr const char *kIRIsPrefix       = "is.";
inline constexpr const char *kIRIsNull         = "is.null";
inline constexpr const char *kIRFieldPrefix    = "field.";
inline constexpr const char *kIRFieldShared    = "field.shared";
inline constexpr const char *kIROldField       = "old.field";
inline constexpr const char *kIRFieldRel       = "field.rel";
inline constexpr const char *kIRFieldAfter     = "field.after";

} // namespace codegen
} // namespace paykan

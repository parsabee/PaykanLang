// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Centralized string constants for the Paykan compiler.

#pragma once

namespace paykan {
namespace names {

// -- Paykan-level built-in names (visible to user code) ---------------------

inline constexpr const char *kOut          = "out";
inline constexpr const char *kErr          = "err";

// Built-in class names
inline constexpr const char *kObj          = "Obj";
inline constexpr const char *kString       = "Str";

// Built-in literals
inline constexpr const char *kNone         = "None";

// Built-in method names
inline constexpr const char *kMethodInit     = "__init__";
inline constexpr const char *kMethodSuper    = "__super__";
inline constexpr const char *kMethodDestroy  = "destroy";
inline constexpr const char *kMethodToString = "toString";
inline constexpr const char *kMethodEquals   = "equals";
inline constexpr const char *kMethodLength   = "length";
inline constexpr const char *kMethodConcat   = "concat";

// Built-in field names
inline constexpr const char *kFieldData    = "_data";
inline constexpr const char *kFieldLen     = "_len";

// Built-in type keyword names
inline constexpr const char *kTypeInt      = "int";
inline constexpr const char *kTypeFloat    = "float";
inline constexpr const char *kTypeBool     = "bool";
inline constexpr const char *kTypeVoid     = "void";

inline constexpr const char *kStringInt    = "StringInt";
inline constexpr const char *kStringFloat  = "StringFloat";
inline constexpr const char *kStringBool   = "StringBool";

// -- Runtime C symbol names -------------------------------------------------

// Object
inline constexpr const char *kPaykanObjectNew       = "PaykanObject_new";
inline constexpr const char *kPaykanObjectDestroy   = "PaykanObject_destroy";
inline constexpr const char *kPaykanObjectToString   = "PaykanObject_toString";
inline constexpr const char *kPaykanObjectEquals     = "PaykanObject_equals";
inline constexpr const char *kPaykanObjectVtable     = "PaykanObject_vtable";
inline constexpr const char *kPaykanObjectNone       = "PaykanObject_None";

// String
inline constexpr const char *kPaykanStringNew        = "PaykanString_new";
inline constexpr const char *kPaykanStringDestroy    = "PaykanString_destroy";
inline constexpr const char *kPaykanStringFromInt    = "PaykanString_from_int";
inline constexpr const char *kPaykanStringFromFloat  = "PaykanString_from_float";
inline constexpr const char *kPaykanStringFromBool   = "PaykanString_from_bool";
inline constexpr const char *kPaykanStringToString   = "PaykanString_toString";
inline constexpr const char *kPaykanStringEquals     = "PaykanString_equals";
inline constexpr const char *kPaykanStringLength     = "PaykanString_length";
inline constexpr const char *kPaykanStringConcat     = "PaykanString_concat";
inline constexpr const char *kPaykanStringVtable     = "PaykanString_vtable";

// IO
inline constexpr const char *kPaykanOut              = "Paykan_out";
inline constexpr const char *kPaykanErr              = "Paykan_err";

// Reference counting
inline constexpr const char *kPaykanSharedNew        = "PaykanShared_new";
inline constexpr const char *kPaykanSharedGet        = "PaykanShared_get";
inline constexpr const char *kPaykanRetain           = "Paykan_retain";
inline constexpr const char *kPaykanRelease          = "Paykan_release";

// -- Environment variables --------------------------------------------------

inline constexpr const char *kPaykanStdlibEnv         = "PAYKAN_STDLIB";

// -- Module system ----------------------------------------------------------

inline constexpr const char *kQualSep                 = "::"; ///< Module qualifier separator
inline constexpr const char *kStdlibDir               = "stdlib"; ///< Standard library subdirectory

// -- Paykan language keywords -----------------------------------------------

inline constexpr const char *kSelf                    = "self";   ///< Receiver parameter in methods

// -- IR / ABI naming conventions --------------------------------------------

inline constexpr const char *kNameSep                 = "_";       ///< Separator between class and method in mangled names
inline constexpr const char *kVTableSuffix            = "_vtable"; ///< Suffix for vtable global symbols
inline constexpr const char *kStructSuffix            = "_struct"; ///< Suffix for LLVM struct type names

// -- C runtime symbols ------------------------------------------------------

inline constexpr const char *kMalloc                  = "malloc";

// -- Cache ------------------------------------------------------------------

inline constexpr const char *kCacheDir                = ".paykan_cache";

// -- LLVM IR internal names -------------------------------------------------

inline constexpr const char *kStrGlobalName          = ".str";
inline constexpr const char *kInt2FPName             = "int2fp";

} // namespace names
} // namespace paykan

// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Centralized string constants for the Paykan compiler.

#pragma once

namespace paykan {
namespace names {

// -- Paykan-level built-in names (visible to user code) ---------------------

inline constexpr const char *kOut          = "out";
inline constexpr const char *kErr          = "err";
inline constexpr const char *kString       = "String";
inline constexpr const char *kStringInt    = "StringInt";
inline constexpr const char *kStringFloat  = "StringFloat";
inline constexpr const char *kStringBool   = "StringBool";

// -- Runtime C symbol names -------------------------------------------------

// Object
inline constexpr const char *kPaykanObjectNew       = "PaykanObject_new";
inline constexpr const char *kPaykanObjectDelete     = "PaykanObject_delete";
inline constexpr const char *kPaykanObjectToString   = "PaykanObject_toString";
inline constexpr const char *kPaykanObjectEquals     = "PaykanObject_equals";
inline constexpr const char *kPaykanObjectVtable     = "PaykanObject_vtable";

// String
inline constexpr const char *kPaykanStringNew        = "PaykanString_new";
inline constexpr const char *kPaykanStringFromInt    = "PaykanString_from_int";
inline constexpr const char *kPaykanStringFromFloat  = "PaykanString_from_float";
inline constexpr const char *kPaykanStringFromBool   = "PaykanString_from_bool";
inline constexpr const char *kPaykanStringDelete     = "PaykanString_delete";
inline constexpr const char *kPaykanStringToString   = "PaykanString_toString";
inline constexpr const char *kPaykanStringEquals     = "PaykanString_equals";
inline constexpr const char *kPaykanStringLength     = "PaykanString_length";
inline constexpr const char *kPaykanStringConcat     = "PaykanString_concat";
inline constexpr const char *kPaykanStringVtable     = "PaykanString_vtable";

// IO
inline constexpr const char *kPaykanOut              = "Paykan_out";
inline constexpr const char *kPaykanErr              = "Paykan_err";

// Shared / ownership
inline constexpr const char *kPaykanSharedNew        = "PaykanShared_new";
inline constexpr const char *kPaykanSharedGet        = "PaykanShared_get";
inline constexpr const char *kPaykanRetain           = "Paykan_retain";
inline constexpr const char *kPaykanRelease          = "Paykan_release";

// -- Environment variables --------------------------------------------------

inline constexpr const char *kPaykanStdlibEnv         = "PAYKAN_STDLIB";

// -- Cache ------------------------------------------------------------------

inline constexpr const char *kCacheDir                = ".paykan_cache";

// -- LLVM IR internal names -------------------------------------------------

inline constexpr const char *kStrGlobalName          = ".str";
inline constexpr const char *kInt2FPName             = "int2fp";

} // namespace names
} // namespace paykan

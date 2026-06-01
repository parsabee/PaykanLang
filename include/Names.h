// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Centralized string constants for the Paykan compiler.

#pragma once

namespace paykan {
namespace names {

// -- Paykan-level built-in names (visible to user code) ---------------------

inline constexpr const char *kPrint        = "print";
inline constexpr const char *kPrintln      = "println";
inline constexpr const char *kErrPrint     = "printerr";
inline constexpr const char *kErrPrintln   = "printerrln";
inline constexpr const char *kLen          = "len";
inline constexpr const char *kPush         = "push";
inline constexpr const char *kPop          = "pop";
inline constexpr const char *kOpen         = "open";
inline constexpr const char *kArray        = "Array";

// Built-in class names
inline constexpr const char *kObj          = "Obj";
inline constexpr const char *kString       = "Str";
inline constexpr const char *kFile         = "File";
inline constexpr const char *kError        = "Error";

// Built-in literals
inline constexpr const char *kNone         = "None";

// Built-in method names
inline constexpr const char *kMethodInit     = "__init__";
inline constexpr const char *kMethodSuper    = "__super__";
inline constexpr const char *kMethodDestroy  = "destroy";
inline constexpr const char *kMethodToString = "toString";
inline constexpr const char *kMethodEquals   = "equals";
inline constexpr const char *kMethodLength   = "len";
inline constexpr const char *kMethodConcat   = "concat";
inline constexpr const char *kMethodWrite    = "write";
inline constexpr const char *kMethodReadln   = "readln";

// Built-in type keyword names
inline constexpr const char *kTypeInt      = "int";
inline constexpr const char *kTypeFloat    = "float";
inline constexpr const char *kTypeBool     = "bool";
inline constexpr const char *kTypeVoid     = "void";

inline constexpr const char *kStrInt    = "StrInt";
inline constexpr const char *kStrFloat  = "StrFloat";
inline constexpr const char *kStrBool   = "StrBool";

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
inline constexpr const char *kPaykanStringAt         = "PaykanString_at";
inline constexpr const char *kPaykanStringVtable     = "PaykanString_vtable";

// Array
inline constexpr const char *kPaykanArrayNew          = "PaykanArray_new";
inline constexpr const char *kPaykanArrayNewObj       = "PaykanArray_new_obj";
inline constexpr const char *kPaykanArrayNewFromData  = "PaykanArray_new_from_data";
inline constexpr const char *kPaykanArrayDestroy      = "PaykanArray_destroy";
inline constexpr const char *kPaykanArrayDestroyObj   = "PaykanArray_destroy_obj";
inline constexpr const char *kPaykanArrayToString     = "PaykanArray_toString";
inline constexpr const char *kPaykanArrayEquals       = "PaykanArray_equals";
inline constexpr const char *kPaykanArrayLength       = "PaykanArray_length";
inline constexpr const char *kPaykanArrayGet          = "PaykanArray_get";
inline constexpr const char *kPaykanArraySet          = "PaykanArray_set";
inline constexpr const char *kPaykanArraySetObj       = "PaykanArray_set_obj";
inline constexpr const char *kPaykanArrayPush         = "PaykanArray_push";
inline constexpr const char *kPaykanArrayPushObj      = "PaykanArray_push_obj";
inline constexpr const char *kPaykanArrayPop          = "PaykanArray_pop";
inline constexpr const char *kPaykanArrayPopObj       = "PaykanArray_pop_obj";
inline constexpr const char *kPaykanArrayVtable       = "PaykanArray_vtable";
inline constexpr const char *kPaykanArrayObjVtable    = "PaykanArray_obj_vtable";

// File
inline constexpr const char *kPaykanFileNew       = "PaykanFile_new";
inline constexpr const char *kPaykanFileOpen      = "PaykanFile_open";
inline constexpr const char *kPaykanFileDestroy   = "PaykanFile_destroy";
inline constexpr const char *kPaykanFileToString  = "PaykanFile_toString";
inline constexpr const char *kPaykanFileEquals    = "PaykanFile_equals";
inline constexpr const char *kPaykanFileWrite     = "PaykanFile_write";
inline constexpr const char *kPaykanFileReadln    = "PaykanFile_readln";
inline constexpr const char *kPaykanFileVtable    = "PaykanFile_vtable";

// Error
inline constexpr const char *kPaykanErrorNew      = "PaykanError_new";
inline constexpr const char *kPaykanErrorDestroy  = "PaykanError_destroy";
inline constexpr const char *kPaykanErrorToString = "PaykanError_toString";
inline constexpr const char *kPaykanErrorEquals   = "PaykanError_equals";
inline constexpr const char *kPaykanErrorVtable   = "PaykanError_vtable";

// IO
inline constexpr const char *kPaykanPrint            = "Paykan_print";
inline constexpr const char *kPaykanPrintln          = "Paykan_println";
inline constexpr const char *kPaykanErrPrint         = "Paykan_printerr";
inline constexpr const char *kPaykanErrPrintln       = "Paykan_printerrln";

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

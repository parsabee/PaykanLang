// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Centralized string constants for the Paykan compiler.

#pragma once

namespace paykan {
namespace names {

// -- Paykan-level built-in names (visible to user code) ---------------------

inline constexpr const char *kPrint = "print";
inline constexpr const char *kPrintln = "println";
inline constexpr const char *kErrPrint = "printerr";
inline constexpr const char *kErrPrintln = "printerrln";
inline constexpr const char *kLen = "len";
inline constexpr const char *kPush = "push";
inline constexpr const char *kPop = "pop";
inline constexpr const char *kOpen = "open";
inline constexpr const char *kArray = "Array";
inline constexpr const char *kTuple = "Tuple";

// Built-in class names
inline constexpr const char *kObj = "Obj";
inline constexpr const char *kString = "Str";
inline constexpr const char *kFile = "File";
inline constexpr const char *kError = "Error";
inline constexpr const char *kStdin = "Stdin";
inline constexpr const char *kIntBox = "Int";
inline constexpr const char *kFloatBox = "Float";
inline constexpr const char *kBoolBox = "Bool";
inline constexpr const char *kCharBox = "Char";

// Built-in method names
inline constexpr const char *kMethodInit = "__init__";
inline constexpr const char *kMethodSuper = "__super__";
inline constexpr const char *kMethodDestroy = "destroy";
inline constexpr const char *kMethodToString = "toString";
inline constexpr const char *kMethodEquals = "equals";
inline constexpr const char *kMethodLength = "len";
inline constexpr const char *kMethodConcat = "concat";
inline constexpr const char *kMethodWrite = "write";
inline constexpr const char *kMethodReadln = "readln";
inline constexpr const char *kMethodReadBytes = "readbytes";
inline constexpr const char *kMethodRead = "read";

// Built-in type keyword names
inline constexpr const char *kTypeInt = "int";
inline constexpr const char *kTypeFloat = "float";
inline constexpr const char *kTypeBool = "bool";
inline constexpr const char *kTypeChar = "char";
inline constexpr const char *kTypeVoid = "void";

// Conversion constructors `Target<Source>(value)` (#64, #88): the closed set
// of specializations of the builtin types.  Sema rebinds a conversion call's
// callee to its spelled form, which is what the lowering dispatches on.
inline constexpr const char *kConvStrInt = "Str<int>";
inline constexpr const char *kConvStrFloat = "Str<float>";
inline constexpr const char *kConvStrBool = "Str<bool>";
inline constexpr const char *kConvStrChar = "Str<char>";
inline constexpr const char *kConvIntStr = "int<Str>";
inline constexpr const char *kConvFloatStr = "float<Str>";
inline constexpr const char *kConvIntFloat = "int<float>";
inline constexpr const char *kConvFloatInt = "float<int>";
inline constexpr const char *kConvIntBool = "int<bool>";
inline constexpr const char *kConvBoolInt = "bool<int>";
inline constexpr const char *kConvIntChar = "int<char>";
inline constexpr const char *kConvCharInt = "char<int>";
inline constexpr const char *kConvBoolStr = "bool<Str>";
// The boxed forms (#88): a boxed source formats like its primitive, a boxed
// target parses into the optional box.
inline constexpr const char *kConvStrIntBox = "Str<Int>";
inline constexpr const char *kConvStrFloatBox = "Str<Float>";
inline constexpr const char *kConvStrBoolBox = "Str<Bool>";
inline constexpr const char *kConvStrCharBox = "Str<Char>";
inline constexpr const char *kConvIntBoxStr = "Int<Str>";
inline constexpr const char *kConvFloatBoxStr = "Float<Str>";
inline constexpr const char *kConvBoolBoxStr = "Bool<Str>";

// -- Runtime C symbol names -------------------------------------------------

// Object
inline constexpr const char *kPaykanObjectNew = "PaykanObject_new";
inline constexpr const char *kPaykanObjectDestroy = "PaykanObject_destroy";
inline constexpr const char *kPaykanObjectToString = "PaykanObject_toString";
inline constexpr const char *kPaykanObjectEquals = "PaykanObject_equals";
inline constexpr const char *kPaykanObjectVtable = "PaykanObject_vtable";
inline constexpr const char *kPaykanObjectNone = "PaykanObject_None";

// String
inline constexpr const char *kPaykanStringFromChar = "PaykanString_from_char";
inline constexpr const char *kPaykanStringCharAt = "PaykanString_char_at";
inline constexpr const char *kPaykanStringNew = "PaykanString_new";
inline constexpr const char *kPaykanStringDestroy = "PaykanString_destroy";
inline constexpr const char *kPaykanStringFromInt = "PaykanString_from_int";
inline constexpr const char *kPaykanStringFromFloat = "PaykanString_from_float";
inline constexpr const char *kPaykanStringFromBool = "PaykanString_from_bool";
inline constexpr const char *kPaykanStringToString = "PaykanString_toString";
inline constexpr const char *kPaykanStringEquals = "PaykanString_equals";
inline constexpr const char *kPaykanStringLength = "PaykanString_length";
inline constexpr const char *kPaykanStringConcat = "PaykanString_concat";
inline constexpr const char *kPaykanStringAt = "PaykanString_at";
inline constexpr const char *kPaykanStringVtable = "PaykanString_vtable";

// Array
inline constexpr const char *kPaykanArrayNew = "PaykanArray_new";
inline constexpr const char *kPaykanArrayNewObj = "PaykanArray_new_obj";
inline constexpr const char *kPaykanArrayNewFromData =
    "PaykanArray_new_from_data";
inline constexpr const char *kPaykanArrayDestroy = "PaykanArray_destroy";
inline constexpr const char *kPaykanArrayDestroyObj = "PaykanArray_destroy_obj";
inline constexpr const char *kPaykanArrayToString = "PaykanArray_toString";
inline constexpr const char *kPaykanArrayEquals = "PaykanArray_equals";
inline constexpr const char *kPaykanArrayLength = "PaykanArray_length";
inline constexpr const char *kPaykanArrayGet = "PaykanArray_get";
inline constexpr const char *kPaykanArraySet = "PaykanArray_set";
inline constexpr const char *kPaykanArraySetObj = "PaykanArray_set_obj";
inline constexpr const char *kPaykanArrayPush = "PaykanArray_push";
inline constexpr const char *kPaykanArrayPushObj = "PaykanArray_push_obj";
inline constexpr const char *kPaykanArrayPop = "PaykanArray_pop";
inline constexpr const char *kPaykanArrayPopObj = "PaykanArray_pop_obj";
inline constexpr const char *kPaykanArrayVtable = "PaykanArray_vtable";
inline constexpr const char *kPaykanArrayObjVtable = "PaykanArray_obj_vtable";

// Tuple (prototype)
inline constexpr const char *kPaykanTupleNew = "PaykanTuple_new";
inline constexpr const char *kPaykanTupleSet = "PaykanTuple_set";
inline constexpr const char *kPaykanTupleSetObj = "PaykanTuple_set_obj";
inline constexpr const char *kPaykanTupleGet = "PaykanTuple_get";
inline constexpr const char *kPaykanTupleDestroy = "PaykanTuple_destroy";
inline constexpr const char *kPaykanTupleToString = "PaykanTuple_toString";
inline constexpr const char *kPaykanTupleEquals = "PaykanTuple_equals";
inline constexpr const char *kPaykanTupleVtable = "PaykanTuple_vtable";

// File
inline constexpr const char *kPaykanFileNew = "PaykanFile_new";
inline constexpr const char *kPaykanFileOpen = "PaykanFile_open";
inline constexpr const char *kPaykanFileDestroy = "PaykanFile_destroy";
inline constexpr const char *kPaykanFileToString = "PaykanFile_toString";
inline constexpr const char *kPaykanFileEquals = "PaykanFile_equals";
inline constexpr const char *kPaykanFileWrite = "PaykanFile_write";
inline constexpr const char *kPaykanFileReadln = "PaykanFile_readln";
inline constexpr const char *kPaykanFileReadBytes = "PaykanFile_readbytes";
inline constexpr const char *kPaykanFileRead = "PaykanFile_read";
inline constexpr const char *kPaykanFileVtable = "PaykanFile_vtable";
inline constexpr const char *kPaykanFileStdin = "PaykanFile_Stdin";

// Int / Float / Bool / Char (boxed primitives; the boxes of `int?` & co.)
inline constexpr const char *kPaykanIntNew = "PaykanInt_new";
inline constexpr const char *kPaykanFloatNew = "PaykanFloat_new";
inline constexpr const char *kPaykanBoolNew = "PaykanBool_new";
inline constexpr const char *kPaykanCharNew = "PaykanChar_new";
inline constexpr const char *kPaykanIntValue = "PaykanInt_value";
inline constexpr const char *kPaykanFloatValue = "PaykanFloat_value";
inline constexpr const char *kPaykanBoolValue = "PaykanBool_value";
inline constexpr const char *kPaykanCharValue = "PaykanChar_value";
inline constexpr const char *kPaykanIntFromStr = "PaykanInt_from_str";
inline constexpr const char *kPaykanFloatFromStr = "PaykanFloat_from_str";
inline constexpr const char *kPaykanBoolFromStr = "PaykanBool_from_str";
inline constexpr const char *kPaykanIntVtable = "PaykanInt_vtable";
inline constexpr const char *kPaykanFloatVtable = "PaykanFloat_vtable";
inline constexpr const char *kPaykanBoolVtable = "PaykanBool_vtable";
inline constexpr const char *kPaykanCharVtable = "PaykanChar_vtable";

// Error
inline constexpr const char *kPaykanErrorNew = "PaykanError_new";
inline constexpr const char *kPaykanErrorDestroy = "PaykanError_destroy";
inline constexpr const char *kPaykanErrorToString = "PaykanError_toString";
inline constexpr const char *kPaykanErrorEquals = "PaykanError_equals";
inline constexpr const char *kPaykanErrorVtable = "PaykanError_vtable";

// IO
inline constexpr const char *kPaykanPrint = "Paykan_print";
inline constexpr const char *kPaykanPrintln = "Paykan_println";
inline constexpr const char *kPaykanErrPrint = "Paykan_printerr";
inline constexpr const char *kPaykanErrPrintln = "Paykan_printerrln";

// Runtime panics (noreturn)
inline constexpr const char *kPaykanPanicDivByZero = "Paykan_panic_div_by_zero";
inline constexpr const char *kPaykanPanicDivOverflow =
    "Paykan_panic_div_overflow";
inline constexpr const char *kPaykanPanicFloatToInt =
    "Paykan_panic_float_to_int";
inline constexpr const char *kPaykanPanicIntToChar = "Paykan_panic_int_to_char";

// Reference counting
inline constexpr const char *kPaykanSharedNew = "PaykanShared_new";
inline constexpr const char *kPaykanSharedGet = "PaykanShared_get";
inline constexpr const char *kPaykanRetain = "Paykan_retain";
inline constexpr const char *kPaykanRelease = "Paykan_release";

// Tracking heap allocator.  Object structs emitted by CodeGen are allocated
// through kPaykanMalloc as well, so JIT/AOT-generated allocations are counted
// alongside the runtime's own (see src/Runtime/Heap.c).
inline constexpr const char *kPaykanMalloc = "Paykan_malloc";
inline constexpr const char *kPaykanRealloc = "Paykan_realloc";
inline constexpr const char *kPaykanFree = "Paykan_free";
inline constexpr const char *kPaykanHeapReset = "Paykan_heap_reset";
inline constexpr const char *kPaykanHeapLiveBlocks = "Paykan_heap_live_blocks";
inline constexpr const char *kPaykanHeapLiveBytes = "Paykan_heap_live_bytes";

// -- Environment variables --------------------------------------------------

inline constexpr const char *kPaykanStdlibEnv = "PAYKAN_STDLIB";

// -- Module system ----------------------------------------------------------

inline constexpr const char *kQualSep = "::"; ///< Module qualifier separator
inline constexpr const char *kStdlibDir =
    "stdlib"; ///< Standard library subdirectory

// -- Paykan language keywords -----------------------------------------------

inline constexpr const char *kSelf = "self"; ///< Receiver parameter in methods

// -- IR / ABI naming conventions --------------------------------------------

inline constexpr const char *kNameSep =
    "_"; ///< Separator between class and method in mangled names
inline constexpr const char *kExternDupSep =
    "."; ///< Suffix separator making a module extern's PIR name unique
inline constexpr const char *kVTableSuffix =
    "_vtable"; ///< Suffix for vtable global symbols
inline constexpr const char *kStructSuffix =
    "_struct"; ///< Suffix for LLVM struct type names

// -- Cache ------------------------------------------------------------------

inline constexpr const char *kCacheDir = ".paykan_cache";

// -- JIT sync list ----------------------------------------------------------
//
// Every symbol that CodeGen may emit as an ExternalLinkage declaration (via
// declareFunction, FunctionTable, getOrInsertGlobal, or a builtin method
// resolved into a user-class vtable slot by findConcreteMethodFuncName) must
// appear in this list.  JIT.cpp asserts in debug builds that every name here
// is present in kRuntimeSymbols, catching Names.h / JIT.cpp drift at startup.
//
inline constexpr const char *kCodeGenRequiredSymbols[] = {
    // Memory / RC
    kPaykanMalloc,
    kPaykanFree,
    kPaykanRetain,
    kPaykanRelease,
    kPaykanSharedNew,
    kPaykanSharedGet,
    // Panics
    kPaykanPanicDivByZero,
    kPaykanPanicDivOverflow,
    kPaykanPanicFloatToInt,
    kPaykanPanicIntToChar,
    // String
    kPaykanStringNew,
    kPaykanStringDestroy,
    kPaykanStringConcat,
    kPaykanStringCharAt,
    kPaykanStringFromInt,
    kPaykanStringFromFloat,
    kPaykanStringFromBool,
    kPaykanStringFromChar,
    // Array
    kPaykanArrayNew,
    kPaykanArrayNewObj,
    kPaykanArrayNewFromData,
    kPaykanArrayGet,
    kPaykanArraySet,
    kPaykanArraySetObj,
    kPaykanArrayPush,
    kPaykanArrayPushObj,
    kPaykanArrayPop,
    kPaykanArrayPopObj,
    // Tuple
    kPaykanTupleNew,
    kPaykanTupleSet,
    kPaykanTupleSetObj,
    kPaykanTupleGet,
    // File
    kPaykanFileOpen,
    // IO
    kPaykanPrint,
    kPaykanPrintln,
    kPaykanErrPrint,
    kPaykanErrPrintln,
    // Globals (objects and singletons)
    kPaykanObjectNone,
    kPaykanFileStdin,
    // Boxed primitives (from Sema/CodeGen dispatch)
    kPaykanIntFromStr,
    kPaykanFloatFromStr,
    kPaykanBoolFromStr,
    // Boxes of the optional primitives (`int?` & co.)
    kPaykanIntNew,
    kPaykanFloatNew,
    kPaykanBoolNew,
    kPaykanCharNew,
    kPaykanIntValue,
    kPaykanFloatValue,
    kPaykanBoolValue,
    kPaykanCharValue,
    // Builtin methods inherited into user-class vtable slots
    kPaykanObjectDestroy,
    kPaykanObjectToString,
    kPaykanObjectEquals,
    kPaykanStringToString,
    kPaykanStringEquals,
    kPaykanStringLength,
    kPaykanFileDestroy,
    kPaykanFileToString,
    kPaykanFileEquals,
    kPaykanFileWrite,
    kPaykanFileReadln,
    // Runtime vtables referenced by `is` / match checks
    kPaykanArrayVtable,
    kPaykanArrayObjVtable,
    kPaykanIntVtable,
    kPaykanFloatVtable,
    kPaykanBoolVtable,
    kPaykanCharVtable,
};

// -- Tuple slot kinds (CodeGen <-> runtime ABI) ------------------------------
//
// One byte per tuple element, emitted by CodeGen as the `kinds` descriptor
// passed to PaykanTuple_new.  Must match PaykanTupleKind in
// src/Runtime/Runtime.h (checked by a static_assert in the codegen tests).

enum TupleSlotKind : unsigned char {
  kTupleSlotInt = 0,   // int64_t (also enum values)
  kTupleSlotFloat = 1, // double bits
  kTupleSlotBool = 2,  // int64_t 0 / 1
  kTupleSlotChar = 3,  // int64_t holding one byte
  kTupleSlotRef = 4,   // PaykanShared* retained by the tuple
};

// -- LLVM IR internal names -------------------------------------------------

inline constexpr const char *kStrGlobalName = ".str";
inline constexpr const char *kInt2FPName = "int2fp";
inline constexpr const char *kTupleKindsGlobalName = ".tuple.kinds";

} // namespace names
} // namespace paykan

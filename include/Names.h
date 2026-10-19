// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Centralized string constants for the Paykan compiler.

#pragma once

#include <string_view>

namespace paykan {
namespace names {

// -- Paykan-level built-in names (visible to user code)

inline constexpr const char *kPrint = "print";
inline constexpr const char *kPrintln = "println";
inline constexpr const char *kErrPrint = "printerr";
inline constexpr const char *kErrPrintln = "printerrln";
inline constexpr const char *kLen = "len";
inline constexpr const char *kOpen = "open";
inline constexpr const char *kMain = "main"; ///< the program's entry point
/// How an absent optional prints (the None object's toString).
inline constexpr const char *kNoneText = "None";

// Built-in class names
inline constexpr const char *kObj = "Obj";
inline constexpr const char *kString = "Str";
inline constexpr const char *kFile = "File";
inline constexpr const char *kError = "Error";
inline constexpr const char *kStdin = "Stdin";

// Built-in method names
inline constexpr const char *kMethodInit = "__init__";
inline constexpr const char *kMethodSuper = "__super__";

/// A name the `__` prefix makes private to its class or struct
/// (docs/design/visibility.md): a field or method reachable only from that
/// type's own methods.  The reserved `__init__` and `__super__` are not.
inline bool isTypePrivateName(std::string_view name) {
  return name.size() > 2 && name[0] == '_' && name[1] == '_' &&
         name != kMethodInit && name != kMethodSuper;
}
/// A name the `_` prefix makes private to its module: a free function, a
/// type, or a field or method reachable anywhere in the module that declares
/// it and from no importer.
inline bool isModulePrivateName(std::string_view name) {
  return name.size() > 1 && name[0] == '_' && name[1] != '_';
}
/// A member name private either way.
inline bool isPrivateName(std::string_view name) {
  return isTypePrivateName(name) || isModulePrivateName(name);
}
inline constexpr const char *kMethodDestroy = "destroy";
inline constexpr const char *kMethodToString = "toString";
inline constexpr const char *kMethodEquals = "equals";

// Traits
inline constexpr const char *kTraitShow =
    "Show"; ///< `view fn toString() -> Str`
inline constexpr const char *kTraitHashable =
    "Hashable"; ///< `view fn hash() -> int`
/// No methods: the types held in place and copied whole, which a class
/// cannot implement.
inline constexpr const char *kTraitValueType = "ValueType";
inline constexpr const char *kMethodHash = "hash";
inline constexpr const char *kSelfType = "Self"; ///< The implementing type
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
// The sized numeric types (docs/design/sized-types.md); `i64`, `u64` and
// `f64` are the other spellings of `int`, `unsigned` and `float`.
inline constexpr const char *kTypeI8 = "i8";
inline constexpr const char *kTypeI16 = "i16";
inline constexpr const char *kTypeI32 = "i32";
inline constexpr const char *kTypeI64 = "i64";
inline constexpr const char *kTypeU8 = "u8";
inline constexpr const char *kTypeU16 = "u16";
inline constexpr const char *kTypeU32 = "u32";
inline constexpr const char *kTypeU64 = "u64";
inline constexpr const char *kTypeUnsigned = "unsigned";
inline constexpr const char *kTypeF16 = "f16";
inline constexpr const char *kTypeF32 = "f32";
inline constexpr const char *kTypeF64 = "f64";

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

// -- Runtime C symbol names

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
inline constexpr const char *kPaykanStringFromUint = "PaykanString_from_uint";
inline constexpr const char *kPaykanStringParseUint = "PaykanString_parse_uint";
inline constexpr const char *kPaykanStringParseInt = "PaykanString_parse_int";
inline constexpr const char *kPaykanStringParseFloat =
    "PaykanString_parse_float";
inline constexpr const char *kPaykanStringParseBool = "PaykanString_parse_bool";
inline constexpr const char *kPaykanStringFromFloat = "PaykanString_from_float";
inline constexpr const char *kPaykanStringFromBool = "PaykanString_from_bool";
inline constexpr const char *kPaykanStringToString = "PaykanString_toString";
inline constexpr const char *kPaykanStringEquals = "PaykanString_equals";
inline constexpr const char *kPaykanStringLength = "PaykanString_length";
inline constexpr const char *kPaykanStringConcat = "PaykanString_concat";
inline constexpr const char *kPaykanStringAt = "PaykanString_at";
inline constexpr const char *kPaykanStringVtable = "PaykanString_vtable";

// Tuples and structs, through their layouts
inline constexpr const char *kPaykanTupleToString = "PaykanTuple_toString";
inline constexpr const char *kPaykanTupleEquals = "PaykanTuple_equals";
inline constexpr const char *kPaykanStructToString = "PaykanStruct_toString";
inline constexpr const char *kPaykanStructEquals = "PaykanStruct_equals";

// Type descriptors: for the C code of generic native functions
inline constexpr const char *kPaykanTypeCopy = "PaykanType_copy";
inline constexpr const char *kPaykanTypeDrop = "PaykanType_drop";
inline constexpr const char *kPaykanTypeIsUnset = "PaykanType_isUnset";

// Buffer: called only by the standard library's native functions
inline constexpr const char *kPaykanBufferNew = "PaykanBuffer_new";
inline constexpr const char *kPaykanBufferLen = "PaykanBuffer_len";
inline constexpr const char *kPaykanBufferGet = "PaykanBuffer_get";
inline constexpr const char *kPaykanBufferCopy = "PaykanBuffer_copy";
inline constexpr const char *kPaykanBufferSet = "PaykanBuffer_set";
inline constexpr const char *kPaykanBufferReserve = "PaykanBuffer_reserve";
inline constexpr const char *kPaykanBufferGrow = "PaykanBuffer_grow";
inline constexpr const char *kPaykanBufferShrink = "PaykanBuffer_shrink";

// Reached only through a `native fn` of the standard library
inline constexpr const char *kPaykanPanicMessage = "Paykan_panic_message";
inline constexpr const char *kPaykanPanicValue = "Paykan_panic_value";

// Hash: `hash()` of the builtin Hashable types
inline constexpr const char *kPaykanIntHash = "PaykanInt_hash";
inline constexpr const char *kPaykanStringHash = "PaykanString_hash";

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
// Program arguments (the entry point's, for `main(args: view Str[])`)
inline constexpr const char *kPaykanArgsNew = "Paykan_args_new";
inline constexpr const char *kPaykanArgsFree = "Paykan_args_free";

// Runtime panics (noreturn)
inline constexpr const char *kPaykanPanicDivByZero = "Paykan_panic_div_by_zero";
inline constexpr const char *kPaykanPanicDivOverflow =
    "Paykan_panic_div_overflow";
inline constexpr const char *kPaykanPanicFloatToInt =
    "Paykan_panic_float_to_int";
inline constexpr const char *kPaykanPanicIntToChar = "Paykan_panic_int_to_char";
inline constexpr const char *kPaykanPanicFloatToType =
    "Paykan_panic_float_to_type";
/// `f16` rounding (docs/design/sized-types.md)
inline constexpr const char *kPaykanRoundF16 = "Paykan_round_f16";
inline constexpr const char *kPaykanPanicIndex = "Paykan_panic_index";

// Reference counting
inline constexpr const char *kPaykanSharedNew = "PaykanShared_new";
inline constexpr const char *kPaykanSharedGet = "PaykanShared_get";
inline constexpr const char *kPaykanDispose = "Paykan_dispose";
inline constexpr const char *kPaykanWeakNew = "Paykan_weak_new";
inline constexpr const char *kPaykanWeakLoad = "Paykan_weak_load";
inline constexpr const char *kPaykanWeakRelease = "Paykan_weak_release";
inline constexpr const char *kPaykanRetain = "Paykan_retain";
inline constexpr const char *kPaykanRelease = "Paykan_release";

// Tracking heap allocator.  Generated code allocates object structs through
// kPaykanMalloc too, so its allocations are counted with the runtime's own
// (src/Runtime/Heap.c).
inline constexpr const char *kPaykanMalloc = "Paykan_malloc";
inline constexpr const char *kPaykanRealloc = "Paykan_realloc";
inline constexpr const char *kPaykanFree = "Paykan_free";
inline constexpr const char *kPaykanHeapReset = "Paykan_heap_reset";
inline constexpr const char *kPaykanHeapLiveBlocks = "Paykan_heap_live_blocks";
inline constexpr const char *kPaykanHeapLiveBytes = "Paykan_heap_live_bytes";

// -- Environment variables

inline constexpr const char *kPaykanStdlibEnv = "PAYKAN_STDLIB";
/// `:`-separated directories of prebuilt `.pkm` modules, searched after the
/// --module-path directories.
inline constexpr const char *kPaykanModulePathEnv = "PAYKAN_MODULE_PATH";

// -- Module system

inline constexpr const char *kQualSep = "::"; ///< Module qualifier separator
/// Prefix of the qualifier an importer registers a module under for the
/// bodies of its templates (Sema::hiddenQualifier); no source can spell it.
inline constexpr const char *kHiddenQualPrefix = "$";
inline constexpr const char *kStdlibDir =
    "stdlib"; ///< Standard library subdirectory

// -- Paykan language keywords

inline constexpr const char *kSelf = "self"; ///< Receiver parameter in methods

// -- IR / ABI naming conventions

/// Separator between class and method in the PIR names of methods and
/// destructors (`K.w`, `K.destroy`, `K.__init__`).  '.' cannot occur in a
/// Paykan identifier, so these never clash with a user function (`fn K_w`).
inline constexpr const char *kNameSep = ".";
inline constexpr const char *kExternDupSep =
    "."; ///< Suffix separator making a module extern's PIR name unique
inline constexpr const char *kVTableSuffix =
    "_vtable"; ///< Suffix for vtable global symbols
inline constexpr const char *kStructSuffix =
    "_struct"; ///< Suffix for LLVM struct type names

// -- Cache

inline constexpr const char *kCacheDir = ".paykan_cache";

// -- JIT sync list
//
// Every runtime symbol the lowering may reference: a function or global it
// declares as a `$rt.` extern, including a builtin method resolved into a
// user class's vtable slot.  JIT.cpp asserts in debug builds that every name
// here is in kRuntimeSymbols, catching Names.h / JIT.cpp drift at startup.
inline constexpr const char *kCodeGenRequiredSymbols[] = {
    // Memory / RC
    kPaykanMalloc,
    kPaykanFree,
    kPaykanRetain,
    kPaykanRelease,
    kPaykanSharedNew,
    kPaykanSharedGet,
    kPaykanDispose,
    kPaykanWeakNew,
    kPaykanWeakLoad,
    kPaykanWeakRelease,
    // Panics
    kPaykanPanicDivByZero,
    kPaykanPanicDivOverflow,
    kPaykanPanicFloatToInt,
    kPaykanPanicFloatToType,
    kPaykanPanicIntToChar,
    kPaykanPanicIndex,
    // String
    kPaykanStringNew,
    kPaykanStringDestroy,
    kPaykanStringConcat,
    kPaykanStringCharAt,
    kPaykanStringFromInt,
    kPaykanStringFromUint,
    kPaykanStringFromFloat,
    kPaykanStringFromBool,
    kPaykanStringFromChar,
    kPaykanStringParseInt,
    kPaykanStringParseUint,
    kPaykanStringParseFloat,
    kPaykanStringParseBool,
    // Sized numeric types
    kPaykanRoundF16,
    // Hash
    kPaykanIntHash,
    kPaykanStringHash,
    // Tuples and structs
    kPaykanTupleToString,
    kPaykanTupleEquals,
    kPaykanStructToString,
    kPaykanStructEquals,
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
};

// -- Kinds of values
//
// The `kind` word of a PaykanType (src/Runtime/Runtime.h, "Generic native
// functions"), which the lowering emits for each type argument, and of each
// element of a tuple's layout ("Tuples").  Must match PaykanTypeKind (a
// static_assert in the codegen tests checks it).

enum NativeTypeKind : unsigned char {
  kNativeKindInt = 0,      // int64_t (also enum values)
  kNativeKindFloat = 1,    // double
  kNativeKindBool = 2,     // bool
  kNativeKindChar = 3,     // int8_t
  kNativeKindObject = 4,   // PaykanShared*, never null
  kNativeKindOptional = 5, // PaykanShared*, null for None, or inline
  kNativeKindTuple = 6,    // its elements in place
  kNativeKindStruct = 7,   // a struct's fields in place, with their names
                           // (only in a layout, never a PaykanType's kind)
  kNativeKindUInt = 8,     // uint64_t (`unsigned`); the narrower sized
                           // integers and floats are kInt and kFloat
};

// -- Names of lowering-generated globals and values

inline constexpr const char *kStrGlobalName = ".str";
inline constexpr const char *kInt2FPName = "int2fp";
inline constexpr const char *kTupleLayoutGlobalName = ".tuple";
inline constexpr const char *kStructLayoutGlobalName = ".struct";

// -- Function values (docs/language/15-function-types.md)
//
// A closure is an object of a class the lowering makes: `fn.<module>.N` for
// a lambda, `fn.ref.<module>.<function>` for a named function used as a value
// (names no identifier can spell, unique across the program's modules,
// mangled by the backends like `Box<int>`).  Its vtable is an object's three
// slots, then `call`.
inline constexpr const char *kClosureClassPrefix = "fn.";
inline constexpr const char *kClosureRefPrefix = "fn.ref.";
inline constexpr const char *kClosureCall = "call";
inline constexpr unsigned kClosureCallSlot = 3;

} // namespace names
} // namespace paykan

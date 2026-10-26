// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Centralized string constants for the Paykan compiler.

#pragma once

#include <string>
#include <string_view>

namespace paykan {
namespace names {

// -- Paykan-level built-in names (visible to user code)

inline constexpr const char *kLen = "len";
inline constexpr const char *kMain = "main"; ///< the program's entry point
/// How an absent optional prints (the None object's toString).
inline constexpr const char *kNoneText = "None";

// Built-in class names
inline constexpr const char *kObj = "Obj";
inline constexpr const char *kString = "Str";

// Built-in method names
inline constexpr const char *kMethodInit = "__init__";
inline constexpr const char *kMethodSuper = "__super__";
/// `___init__`: the initializer, private to its module (docs/language/
/// 05-classes.md, "Private fields and methods").  The parser records it as
/// `__init__` with FuncDecl::isModulePrivateInit; the AST interchange form
/// and a `.pkm` interface write the method under this name again.
inline constexpr const char *kMethodInitModulePrivate = "___init__";

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
/// Why @p symbol cannot be the C symbol of a `native fn`, or "" when it can:
/// it must be a C identifier (an ASCII letter or `_`, then ASCII letters,
/// digits and `_`) other than a C keyword (C11 6.4.1), so that the C
/// declaration `--emit-c` writes compiles and a linker can name it.
inline std::string_view cSymbolProblem(std::string_view symbol) {
  constexpr std::string_view kKeywords[] = {
      "auto",       "break",     "case",           "char",
      "const",      "continue",  "default",        "do",
      "double",     "else",      "enum",           "extern",
      "float",      "for",       "goto",           "if",
      "inline",     "int",       "long",           "register",
      "restrict",   "return",    "short",          "signed",
      "sizeof",     "static",    "struct",         "switch",
      "typedef",    "union",     "unsigned",       "void",
      "volatile",   "while",     "_Alignas",       "_Alignof",
      "_Atomic",    "_Bool",     "_Complex",       "_Generic",
      "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local"};
  if (symbol.empty() || (symbol[0] >= '0' && symbol[0] <= '9'))
    return "is not a C identifier";
  for (char c : symbol)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_'))
      return "is not a C identifier";
  for (std::string_view keyword : kKeywords)
    if (symbol == keyword)
      return "is a C keyword";
  return "";
}
/// The type parameter an untyped parameter of a named function adds
/// (`fn double(x) = x * 2;` is `fn double<typeof(x)>(x: typeof(x))`): a
/// name no program can write, so it shadows no type.
inline std::string untypedParamType(std::string_view param) {
  return "typeof(" + std::string(param) + ")";
}
/// The parameter whose type @p typeParam is, when it is one an untyped
/// parameter added (see untypedParamType), else empty.
inline std::string_view untypedParamOf(std::string_view typeParam) {
  constexpr std::string_view open = "typeof(";
  if (typeParam.size() <= open.size() + 1 ||
      typeParam.substr(0, open.size()) != open || typeParam.back() != ')')
    return {};
  return typeParam.substr(open.size(), typeParam.size() - open.size() - 1);
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
inline constexpr const char *kPaykanEnumToString = "PaykanEnum_toString";
inline constexpr const char *kPaykanEnumEquals = "PaykanEnum_equals";

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
inline constexpr const char *kPaykanBufferIsSet = "PaykanBuffer_isSet";
inline constexpr const char *kPaykanBufferTryGrow = "PaykanBuffer_tryGrow";
inline constexpr const char *kPaykanBufferTryReserve =
    "PaykanBuffer_tryReserve";
/// What each Buffer function may do to the values a buffer holds: drop some
/// of them, of its type argument's type (the one it overwrites, the ones it
/// shrinks away), or none.  None of them replaces the buffer an `inout Obj`
/// handle names, keeps a value, or runs program code.  The lowering reads a
/// call of one by it (src/Lowering/LoweringHolds.cpp, nativeEffect), and
/// tests/Runtime/BufferTests.cpp checks it against src/Runtime/Buffer.c.
struct BufferFunction {
  const char *Symbol;
  bool Drops;
};
inline constexpr BufferFunction kPaykanBufferFunctions[] = {
    {kPaykanBufferNew, false},        {kPaykanBufferLen, false},
    {kPaykanBufferGet, false},        {kPaykanBufferCopy, false},
    {kPaykanBufferSet, true},         {kPaykanBufferReserve, false},
    {kPaykanBufferGrow, false},       {kPaykanBufferShrink, true},
    {kPaykanBufferIsSet, false},      {kPaykanBufferTryGrow, false},
    {kPaykanBufferTryReserve, false},
};

// The standard library's ::io (stdlib/io.pkn): reached only through its
// `native fn`s
inline constexpr const char *kPaykanIoOpen = "Paykan_io_open";
inline constexpr const char *kPaykanIoWrite = "Paykan_io_write";
inline constexpr const char *kPaykanIoReadln = "Paykan_io_readln";
inline constexpr const char *kPaykanIoReadBytes = "Paykan_io_readbytes";
inline constexpr const char *kPaykanIoRead = "Paykan_io_read";
inline constexpr const char *kPaykanIoStdin = "Paykan_io_stdin";
// ... and ::error's (stdlib/error.pkn)
inline constexpr const char *kPaykanErrorCode = "Paykan_error_code";
inline constexpr const char *kPaykanErrorText = "Paykan_error_text";

// Reached only through a `native fn` of the standard library
inline constexpr const char *kPaykanPanicMessage = "Paykan_panic_message";
inline constexpr const char *kPaykanPanicValue = "Paykan_panic_value";
inline constexpr const char *kPaykanUnreachableView = "Paykan_unreachable_view";

// Hash: `hash()` of the builtin Hashable types
inline constexpr const char *kPaykanIntHash = "PaykanInt_hash";
inline constexpr const char *kPaykanStringHash = "PaykanString_hash";

// Printing: reached only through the natives of the standard library's ::io
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
inline constexpr const char *kPaykanPanicForStep = "Paykan_panic_for_step";
inline constexpr const char *kPaykanPanicPureVirtual =
    "Paykan_panic_pure_virtual";
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
// Their fast paths, Runtime.h's static inline functions (not ABI symbols):
// what the C of --emit-c calls instead.
inline constexpr const char *kPaykanRetainInline = "Paykan_retain_inline";
inline constexpr const char *kPaykanReleaseInline = "Paykan_release_inline";

// Tracking heap allocator.  Generated code allocates object structs through
// kPaykanMalloc too, so its allocations are counted with the runtime's own
// (src/Runtime/Heap.c).
inline constexpr const char *kPaykanMalloc = "Paykan_malloc";
inline constexpr const char *kPaykanRealloc = "Paykan_realloc";
inline constexpr const char *kPaykanFree = "Paykan_free";
inline constexpr const char *kPaykanHeapReset = "Paykan_heap_reset";
// What an executable's entry code calls under PAYKAN_TRACK_HEAP, with
// kPaykanHeapReset.
inline constexpr const char *kPaykanHeapSetTracking =
    "Paykan_heap_set_tracking";
inline constexpr const char *kPaykanHeapDump = "Paykan_heap_dump";
inline constexpr const char *kPaykanHeapLiveBlocks = "Paykan_heap_live_blocks";
inline constexpr const char *kPaykanHeapLiveBytes = "Paykan_heap_live_bytes";

// -- Environment variables

inline constexpr const char *kPaykanStdlibEnv = "PAYKAN_STDLIB";
/// Set (to any value): a program tracks its heap and prints the statistics
/// at exit, as under `paykan run --track-heap` (an executable's entry code
/// reads it too).
inline constexpr const char *kPaykanTrackHeapEnv = "PAYKAN_TRACK_HEAP";
/// `:`-separated directories of prebuilt `.pkm` modules, searched after the
/// --module-path directories.
inline constexpr const char *kPaykanModulePathEnv = "PAYKAN_MODULE_PATH";
/// Set by the plugin host for a program it runs with no arguments, which it
/// starts with one empty argv[0]: the runtime's Paykan_args_new then gives
/// an empty slice, and unsets it (src/Runtime/IO.c spells it too).  Private:
/// not for users to set.
inline constexpr const char *kPaykanHostNoArgsEnv = "__PAYKAN_HOST_NO_ARGS";

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

/// The type a PIR function of a type is named after: @p name up to its
/// first '.' or "::" outside `<...>` and `(...)` (`Box<int>.get`,
/// `Box<int>::make` -> `Box<int>`), or all of it (a constructor, a free
/// function).  A function type's `->` closes nothing.
inline std::string_view ownerTypeName(std::string_view name) {
  int depth = 0;
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (c == '<' || c == '(')
      ++depth;
    else if (c == '-' && i + 1 < name.size() && name[i + 1] == '>')
      ++i;
    else if (c == '>' || c == ')')
      --depth;
    else if (depth == 0 && (c == '.' || (c == ':' && i + 1 < name.size() &&
                                         name[i + 1] == ':')))
      return name.substr(0, i);
  }
  return name;
}
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
    kPaykanPanicForStep,
    kPaykanPanicPureVirtual,
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
    kPaykanEnumToString,
    kPaykanEnumEquals,
    // Globals (objects and singletons)
    kPaykanObjectNone,
    // Builtin methods inherited into user-class vtable slots
    kPaykanObjectDestroy,
    kPaykanObjectToString,
    kPaykanObjectEquals,
    kPaykanStringToString,
    kPaykanStringEquals,
    kPaykanStringLength,
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
  kNativeKindEnum = 9,     // an enum's tag, and a tagged union's fields
                           // (only in a layout, never a PaykanType's kind)
};

// -- Names of lowering-generated globals and values

inline constexpr const char *kStrGlobalName = ".str";
inline constexpr const char *kInt2FPName = "int2fp";
inline constexpr const char *kTupleLayoutGlobalName = ".tuple";
inline constexpr const char *kStructLayoutGlobalName = ".struct";
inline constexpr const char *kEnumLayoutGlobalName = ".enum";

// -- Function values (docs/language/14-function-types.md)
//
// A function value is its code and its closure, held in place.  The code of
// a lambda is the function `fn.<module>.N.call`, and its closure, when it
// captures anything, an object of the class `fn.<module>.N` with an object's
// vtable; a named function used as a value is the code
// `fn.ref.<module>.<function>.call`, which forwards to it.  The names are
// ones no identifier can spell, unique across the program's modules, and
// mangled by the backends like `Box<int>`.
inline constexpr const char *kClosureClassPrefix = "fn.";
inline constexpr const char *kClosureRefPrefix = "fn.ref.";
inline constexpr const char *kClosureCall = "call";

} // namespace names
} // namespace paykan

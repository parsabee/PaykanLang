// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Centralized C-language string constants of the C backend: every C keyword,
// type name, operator, punctuator, standard-library / macro name, string
// escape and name-mangling affix the emitter writes (CEmitter.cpp), and the
// environment variables the build sets (CBuild.cpp).  The C compiler's
// command line is in src/Backends/Toolchain/ToolchainNames.h, the Paykan
// runtime's own C symbols (Paykan_retain, ...) in include/Names.h.

#pragma once

namespace paykan::backend_c::cnames {

// -- C keywords

inline constexpr const char *kBreak = "break";
inline constexpr const char *kConst = "const";
inline constexpr const char *kContinue = "continue";
inline constexpr const char *kElse = "else";
inline constexpr const char *kExtern = "extern";
inline constexpr const char *kFalse = "false"; ///< <stdbool.h>
inline constexpr const char *kFor = "for";
inline constexpr const char *kIf = "if";
inline constexpr const char *kInline = "inline";
inline constexpr const char *kReturn = "return";
inline constexpr const char *kSizeof = "sizeof";
inline constexpr const char *kStatic = "static";
inline constexpr const char *kStruct = "struct";
inline constexpr const char *kTrue = "true"; ///< <stdbool.h>

// -- C type names

inline constexpr const char *kVoid = "void";
inline constexpr const char *kBool = "bool"; ///< <stdbool.h>
inline constexpr const char *kChar = "char";
inline constexpr const char *kInt = "int";
inline constexpr const char *kDouble = "double";
inline constexpr const char *kUnsignedLong = "unsigned long";
inline constexpr const char *kInt8 = "int8_t";     ///< <stdint.h>
inline constexpr const char *kUint8 = "uint8_t";   ///< <stdint.h>
inline constexpr const char *kInt64 = "int64_t";   ///< <stdint.h>
inline constexpr const char *kUint64 = "uint64_t"; ///< <stdint.h>
inline constexpr const char *kIntptr = "intptr_t"; ///< <stdint.h>
inline constexpr const char *kSize = "size_t";     ///< <stdlib.h>

// Pointer types, spelled as the emitter writes them (`T *`).
inline constexpr const char *kVoidPtr = "void *";
inline constexpr const char *kConstVoidPtr = "const void *";
inline constexpr const char *kConstCharPtr = "const char *";
inline constexpr const char *kConstUint8Ptr = "const uint8_t *";
inline constexpr const char *kCharPtrPtr = "char **";

// The runtime's C types (Runtime.h) and pointers to them.
inline constexpr const char *kRtShared = "PaykanShared";
inline constexpr const char *kRtString = "PaykanString";
inline constexpr const char *kRtArray = "PaykanArray";
inline constexpr const char *kRtObjPtr = "PaykanObject *";
inline constexpr const char *kRtBoxPtr = "PaykanShared *";
inline constexpr const char *kRtStrPtr = "PaykanString *";
inline constexpr const char *kRtArrPtr = "PaykanArray *";
inline constexpr const char *kRtTupPtr = "PaykanTuple *";
inline constexpr const char *kRtIntPtr = "PaykanInt *";
inline constexpr const char *kRtFloatPtr = "PaykanFloat *";
inline constexpr const char *kRtBoolPtr = "PaykanBool *";
inline constexpr const char *kRtCharPtr = "PaykanChar *";
/// A vtable slot (Runtime.h): the generic function-pointer type every
/// vtable -- the runtime's and the generated `pkvt_<class>` -- is an array of.
inline constexpr const char *kRtMethod = "PaykanMethod";
/// Reads any object's vtable pointer (Runtime.h), whatever its struct type.
inline constexpr const char *kRtVTableOf = "Paykan_vtable_of";

// The header fields of every Paykan object struct (PaykanObject's layout).
inline constexpr const char *kFieldVTable = "vtable";
inline constexpr const char *kFieldShared = "shared";

// -- C operators

inline constexpr const char *kOpAdd = "+";
inline constexpr const char *kOpSub = "-";
inline constexpr const char *kOpMul = "*";
inline constexpr const char *kOpDiv = "/";
inline constexpr const char *kOpRem = "%";
inline constexpr const char *kOpNeg = "-"; ///< unary minus
inline constexpr const char *kOpNot = "!";
inline constexpr const char *kOpBitAnd = "&";
inline constexpr const char *kOpAddrOf = "&";
inline constexpr const char *kOpDeref = "*";
inline constexpr const char *kOpEq = "==";
inline constexpr const char *kOpNe = "!=";
inline constexpr const char *kOpLt = "<";
inline constexpr const char *kOpLe = "<=";
inline constexpr const char *kOpGt = ">";
inline constexpr const char *kOpGe = ">=";
inline constexpr const char *kOpAssign = "=";
inline constexpr const char *kOpPreInc = "++";
inline constexpr const char *kOpArrow = "->";
inline constexpr const char *kOpCondQ = "?"; ///< `c ? a : b`
inline constexpr const char *kOpCondColon = ":";

// -- C punctuation

inline constexpr const char *kLParen = "(";
inline constexpr const char *kRParen = ")";
inline constexpr const char *kLBrace = "{";
inline constexpr const char *kRBrace = "}";
inline constexpr const char *kLBracket = "[";
inline constexpr const char *kRBracket = "]";
inline constexpr const char *kSemi = ";";
inline constexpr const char *kListSep = ", "; ///< between arguments / items
inline constexpr const char *kSpace = " ";
inline constexpr const char *kNewline = "\n";
inline constexpr const char *kIndentUnit = "  "; ///< one level of indentation
inline constexpr const char *kCommentOpen = "/* ";
inline constexpr const char *kCommentClose = " */";
inline constexpr const char *kQuote = "\""; ///< string-literal delimiter
inline constexpr char kPointerStar = '*';   ///< ends a pointer type's spelling
inline constexpr const char *kInclude = "#include ";
inline constexpr const char *kSysHeaderOpen = "<";
inline constexpr const char *kSysHeaderClose = ">";

// -- Standard headers, macros and functions

inline constexpr const char *kMathH = "math.h";
inline constexpr const char *kStdboolH = "stdbool.h";
inline constexpr const char *kStdintH = "stdint.h";
inline constexpr const char *kStdlibH = "stdlib.h";
inline constexpr const char *kStringH = "string.h";
inline constexpr const char *kRuntimeH = "Runtime.h"; ///< the Paykan runtime

inline constexpr const char *kNull = "NULL";
inline constexpr const char *kInt64Min = "INT64_MIN";
inline constexpr const char *kInt64C = "INT64_C"; ///< INT64_C(<digits>)
inline constexpr const char *kNan = "NAN";
inline constexpr const char *kInfinity = "INFINITY";

inline constexpr const char *kAbort = "abort";
inline constexpr const char *kFmod = "fmod";
inline constexpr const char *kGetenv = "getenv";
inline constexpr const char *kMemcpy = "memcpy";
inline constexpr const char *kStrlen = "strlen";

// -- Literals

inline constexpr const char *kZero = "0";
inline constexpr const char *kOne = "1";
inline constexpr const char *kZeroF64 = "0.0";
/// printf format of a double literal: enough digits to round-trip.
inline constexpr const char *kF64Format = "%.17g";
/// A formatted double containing none of these reads as an integer ...
inline constexpr const char *kF64Marks = ".eE";
/// ... and gets this suffix to stay a double literal.
inline constexpr const char *kF64Suffix = ".0";

// -- String-literal escapes (escapeCString)

inline constexpr const char *kEscBackslash = "\\\\";
inline constexpr const char *kEscQuote = "\\\"";
inline constexpr const char *kEscQuestion = "\\?"; ///< trigraph-proof
inline constexpr const char *kEscNewline = "\\n";
inline constexpr const char *kEscTab = "\\t";
inline constexpr const char *kEscReturn = "\\r";
/// Any other non-printable byte: exactly three octal digits, so it never
/// swallows a following digit.
inline constexpr const char *kEscOctalFormat = "\\%03o";
/// Printable ASCII range [kPrintableFirst, kPrintableEnd) kept verbatim.
inline constexpr unsigned char kPrintableFirst = 0x20;
inline constexpr unsigned char kPrintableEnd = 0x7f;

// -- Identifier sanitizing (sanitize)

/// Kept verbatim besides [A-Za-z0-9].
inline constexpr char kIdentUnderscore = '_';
/// Any other byte: `_` and two hex digits.
inline constexpr const char *kIdentHexEscapeFormat = "_%02X";
/// Prepended to an identifier that would be empty or start with a digit.
inline constexpr const char *kIdentLeadPrefix = "_";

// -- Name mangling
//
// Every emitted name carries one of these prefixes, which keeps it apart from
// C keywords, the C library's names and macros (`errno`, `fmod`, `int64_t`,
// ...), the runtime (`Paykan*`) and every other kind of emitted name.

/// Module-defined symbols: `pk_<module stem>_<name>`.
inline constexpr const char *kSymbolPrefix = "pk_";
/// Separator between the parts of a mangled name, and before the counter
/// that makes a clashing symbol unique (`<sym>_<n>`).
inline constexpr const char *kMangleSep = "_";
/// Module string / data / tag-byte globals: `pk_<stem>_str<i>`, ...
inline constexpr const char *kCStrSuffix = "_str";
inline constexpr const char *kDataSuffix = "_data";
inline constexpr const char *kBytesSuffix = "_kinds";
/// A class's vtable array and allocator: `pkvt_<class sym>`,
/// `pknew_<class sym>`.
inline constexpr const char *kVTablePrefix = "pkvt_";
inline constexpr const char *kNewPrefix = "pknew_";
/// The emitter's own helpers (prelude, emitted only when a unit uses them).
inline constexpr const char *kHelperF64Bits = "pkrt_f64_bits";
inline constexpr const char *kHelperBitsF64 = "pkrt_bits_f64";
/// PIR values: `v<id>` or `v<id>_<name>`.
inline constexpr const char *kValuePrefix = "v";
/// A '.' in a value's (PIR) name becomes a '_'.
inline constexpr char kValueNameDot = '.';
inline constexpr char kValueDotReplacement = '_';
/// Class fields: `f_<name>`.
inline constexpr const char *kFieldPrefix = "f_";
/// Locals: `l_<name>`, or `l<k>_<name>` for the k-th shadowing local.
inline constexpr const char *kLocalPrefix = "l_";
inline constexpr const char *kLocalShadowPrefix = "l";
inline constexpr char kLocalShadowSep = '_';

// -- The generated `main`

inline constexpr const char *kMain = "main"; ///< C entry point and PIR name
inline constexpr const char *kArgc = "argc";
inline constexpr const char *kArgv = "argv";
inline constexpr const char *kMainTrack = "track";
inline constexpr const char *kMainArgs = "args";
inline constexpr const char *kMainIndex = "i";
inline constexpr const char *kMainStr = "s";
inline constexpr const char *kMainBox = "b";
inline constexpr const char *kMainRc = "rc";
/// New-object helpers' local: `o`.
inline constexpr const char *kNewObj = "o";
/// Prelude helper parameters / locals.
inline constexpr const char *kHelperDouble = "d";
inline constexpr const char *kHelperInt = "i";
/// Runtime heap tracking used by `main` (src/Runtime/Heap.c).
inline constexpr const char *kRtHeapSetTracking = "Paykan_heap_set_tracking";
inline constexpr const char *kRtHeapDump = "Paykan_heap_dump";

// -- Environment variables (generated `main`; set by CBuild.cpp)

/// Non-empty: track heap blocks and dump the live ones at exit.
inline constexpr const char *kEnvTrackHeap = "PAYKAN_TRACK_HEAP";
/// Set: run `main` with an empty argument list (argc == 0).
inline constexpr const char *kEnvNoArgs = "PAYKAN_NO_ARGS";
/// The value CBuild.cpp sets them to.
inline constexpr const char *kEnvOn = "1";

// -- Comments

inline constexpr const char *kBanner = "Generated by the Paykan C backend.";
inline constexpr const char *kModuleBannerOpen = "/* ---- module ";
inline constexpr const char *kModuleBannerClose = " ---- */";
inline constexpr const char *kUnreachableNote = "unreachable";
inline constexpr const char *kSlotNote = "slot";
inline constexpr const char *kSuperNote = ":";
/// Placeholders written where a failed lookup leaves no name (the emitter
/// reports an error; the output is discarded).
inline constexpr const char *kMissing = "/*missing*/";
inline constexpr const char *kUndeclared = "/*undeclared*/";
inline constexpr const char *kNoClass = "/*noclass*/";
inline constexpr const char *kUndef = "/*undef*/0";
inline constexpr const char *kUnknownOperand = "/*?*/0";
inline constexpr const char *kUnknownSymbol = "/*unknown*/0";

} // namespace paykan::backend_c::cnames

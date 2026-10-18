// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan language runtime — the C ABI of the builtin class types, shared by
// the runtime, the generated code of both backends (the C backend includes
// this header) and the JIT's symbol table.
//
// Every Paykan object begins with a pointer to its class's vtable: an array
// of PaykanMethod slots (see "Object" below) whose layout mirrors the
// ClassType::VTable vector built during AST bootstrapping:
//
//   Object vtable  : [0] destroy  [1] toString  [2] equals
//   String vtable  : Object's, then [3] length  [4] concat

#ifndef PAYKAN_RUNTIME_H
#define PAYKAN_RUNTIME_H

#include <float.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Portable noreturn attribute: `_Noreturn` is C11-only and not valid C++,
// but this header is included from both the C runtime and the C++ compiler.
#ifdef __cplusplus
#define PAYKAN_NORETURN [[noreturn]]
#else
#define PAYKAN_NORETURN _Noreturn
#endif

// -- Runtime ABI version
//
// Bumped whenever the generated code's ABI (object layout, vtable slots,
// calling conventions, symbol naming) changes, so code compiled against an
// older runtime is never linked or loaded: both backends' caches and every
// .pkm module file record it (docs/design/pkm.md §2.2, tag 0x09).
//   v3: PIR-based translation.
//   v4: the main module's symbols are prefixed (`pk.`).
//   v5: methods are `<Class>.<method>` (was `<Class>_<method>`), vtables
//       `<Class>..vtable`.
//   v6: imported modules' symbols are prefixed with the canonical module
//       name (`geometry::shapes::describe`), not the absolute file path.
#define PAYKAN_RUNTIME_ABI_VERSION 6

// -- Target assumptions
//
// The runtime, the C backend's generated code (which includes this header)
// and the LLVM backend all rely on these implementation-defined properties.
// docs/c-backend.md ("Target assumptions") explains where each one is used.
// They hold on every supported target (LP64 Linux and macOS); a target that
// breaks one fails to compile here instead of miscompiling.

#ifdef __cplusplus
#define PAYKAN_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define PAYKAN_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#ifndef INTPTR_MAX
#error "Paykan needs intptr_t (pointers are stored in 8-byte integer slots)"
#endif

PAYKAN_STATIC_ASSERT(CHAR_BIT == 8, "Paykan needs 8-bit bytes");
PAYKAN_STATIC_ASSERT(sizeof(void *) == 8 && sizeof(intptr_t) == 8,
                     "Paykan needs 8-byte pointers (an LP64 target)");
PAYKAN_STATIC_ASSERT(sizeof(unsigned long) == 8,
                     "Paykan needs an 8-byte unsigned long (an LP64 target)");
PAYKAN_STATIC_ASSERT(sizeof(void (*)(void)) == sizeof(void *),
                     "Paykan needs function pointers the size of a pointer");
PAYKAN_STATIC_ASSERT(sizeof(double) == 8 && sizeof(int64_t) == sizeof(double),
                     "Paykan needs an 8-byte double");
// IEEE 754 binary64 doubles.  __STDC_IEC_559__ (full Annex F conformance) is
// not required: glibc's compilers define it, Apple's clang does not, and the
// generated code only relies on the binary64 format, its NaN and infinity.
PAYKAN_STATIC_ASSERT(FLT_RADIX == 2 && DBL_MANT_DIG == 53 &&
                         DBL_MAX_EXP == 1024 && DBL_MIN_EXP == -1021,
                     "Paykan needs IEEE 754 binary64 doubles");
// Two's complement, and an out-of-range conversion to a signed type wraps
// modulo 2^N (C11 6.3.1.3p3 leaves it implementation-defined): integer
// arithmetic is done in uint64_t and converted back to int64_t.
PAYKAN_STATIC_ASSERT(~0 == -1 && (int64_t)UINT64_MAX == -1 &&
                         (int64_t)(UINT64_C(1) << 63) == INT64_MIN,
                     "Paykan needs two's complement, wrapping conversions");

// -- Forward declarations

typedef struct PaykanObject PaykanObject;
typedef struct PaykanString PaykanString;
/// A strong reference to an object: the object's own pointer (the reference
/// counts live in the object's header, so an object is one allocation).
/// The name marks a pointer that holds a reference, counted in `strong`:
/// a PaykanShared* is owned (+1) or borrowed, a PaykanObject* is a raw
/// alias.  See "Shared" below.
typedef struct PaykanObject PaykanShared;
/// A weak reference to an object: its pointer again, counted in `weak`.
/// Loading one (Paykan_weak_load) gives a strong reference while the object
/// is alive, and NULL once it has been destroyed.
typedef struct PaykanObject PaykanWeak;

// -- Object
//
// Root of the class hierarchy.  Every Paykan heap object starts with this
// three-word header, so that a PaykanObject* reaches the vtable and the
// object's own reference counts (docs/language/08-memory-model.md):
//
//   [0] vtable  — method dispatch + type identity
//   [1] strong  — the strong reference count: the number of owned
//                 PaykanShared* references.  Above zero while the object is
//                 alive; the destroy runs when it drops to zero.
//   [2] weak    — the weak reference count: the number of PaykanWeak*
//                 references.  The memory is freed when both are zero.
//
// Every constructor (the runtime's and the generated class constructors)
// initialises `strong` and `weak` to 0: a new object is unowned until it is
// boxed (PaykanShared_new).  An immortal static (PaykanObject_None,
// PaykanFile_Stdin) starts with `strong` 1, which nothing ever releases.

// A vtable is an array of PaykanMethod, the generic function-pointer type:
// every slot holds a method converted to `void (*)(void)` and is converted
// back to the method's own type before the call (C11 6.3.2.3p8 guarantees
// the round trip).  The runtime's vtables and the generated code's
// (`pkvt_<class>`, both backends) are all declared this way, so every slot is
// read through its declared type.  The slot numbers below are shared by
// every class: a subclass's vtable starts with its superclass's slots.
typedef void (*PaykanMethod)(void);

enum {
  // Object (every class)
  PAYKAN_SLOT_DESTROY = 0,   // void (*)(PaykanObject *self)
  PAYKAN_SLOT_TO_STRING = 1, // PaykanShared *(*)(PaykanObject *self)
  PAYKAN_SLOT_EQUALS = 2,    // int64_t (*)(PaykanObject *self, PaykanShared *)
  PAYKAN_OBJECT_SLOTS = 3,
  // String
  PAYKAN_SLOT_STRING_LENGTH = 3, // int64_t (*)(PaykanObject *self)
  PAYKAN_SLOT_STRING_CONCAT = 4, // void (*)(PaykanObject *self, *other)
  PAYKAN_STRING_SLOTS = 5,
  // File
  PAYKAN_SLOT_FILE_WRITE = 3,     // void (*)(PaykanObject *self, *str)
  PAYKAN_SLOT_FILE_READLN = 4,    // PaykanShared *(*)(PaykanObject *self)
  PAYKAN_SLOT_FILE_READBYTES = 5, // PaykanShared *(*)(PaykanObject *, int64_t)
  PAYKAN_SLOT_FILE_READ = 6,      // PaykanShared *(*)(PaykanObject *self)
  PAYKAN_FILE_SLOTS = 7
};

struct PaykanObject {
  PaykanMethod *vtable; // this class's vtable (PAYKAN_SLOT_*)
  int64_t strong;       // strong reference count (header slot 1)
  int64_t weak;         // weak reference count (header slot 2)
};

/// The vtable of any Paykan object, whatever its struct type: the header's
/// first word, copied out with memcpy so that it is never read through an
/// lvalue of another struct type (C11 6.5p7).  The generated C reads every
/// vtable this way.
static inline PaykanMethod *Paykan_vtable_of(const void *obj) {
  PaykanMethod *vtable;
  memcpy(&vtable, obj, sizeof vtable);
  return vtable;
}

// Constructor.  Test-only: generated code constructs `Obj()` through the
// class machinery, never through this symbol.
PaykanObject *PaykanObject_new(void);

// Default method implementations.
void PaykanObject_destroy(PaykanObject *self);
PaykanShared *PaykanObject_toString(PaykanObject *self);
int64_t PaykanObject_equals(PaykanObject *self, PaykanShared *other);

// Global vtable instance.
extern PaykanMethod PaykanObject_vtable[PAYKAN_OBJECT_SLOTS];

// Singleton None instance — an Obj whose toString returns "None".
extern PaykanObject PaykanObject_None;

// -- String
//
// Inherits Object.  The first three vtable slots match Object's layout;
// String-specific slots follow (PAYKAN_SLOT_STRING_*).

struct PaykanString {
  PaykanMethod *vtable; // points to PaykanString_vtable
  int64_t strong;       // object header (see PaykanObject)
  int64_t weak;
  char *data;  // heap-allocated NUL-terminated buffer
  int64_t len; // length in bytes (excludes NUL)
};

// Constructor / destructor.
PaykanString *PaykanString_new(const char *data, int64_t len);
PaykanString *PaykanString_from_int(int64_t value);
/// The bits of @p value printed as an unsigned 64-bit integer (`unsigned`).
PaykanString *PaykanString_from_uint(int64_t value);
PaykanString *PaykanString_from_float(double value);
PaykanString *PaykanString_from_bool(int64_t value);
PaykanString *PaykanString_from_char(int8_t c);

// The parses behind `int<Str>`, `float<Str>` and `bool<Str>`: each returns 1
// and stores the value in *out when the whole string is one, else returns 0
// and stores 0 (the lowering makes the `int?` & co. of the two).
/// A decimal integer: an optional sign and digits, the whole string, no
/// surrounding whitespace, within int64.
int64_t PaykanString_parse_int(PaykanObject *str, int64_t *out);
/// A decimal unsigned integer: digits, the whole string, no sign, no
/// surrounding whitespace, within uint64 (stored as its bits).
int64_t PaykanString_parse_uint(PaykanObject *str, int64_t *out);
/// A floating-point number in C `strtod` syntax, including `nan` and `inf`:
/// the whole string, no leading whitespace, not overflowing or
/// underflowing (a subnormal result is accepted).
int64_t PaykanString_parse_float(PaykanObject *str, double *out);
/// Exactly "True" (1) or "False" (0), the spellings `Str<bool>` prints.
int64_t PaykanString_parse_bool(PaykanObject *str, int64_t *out);
int8_t PaykanString_char_at(PaykanObject *self, int64_t idx);

// Method implementations.
void PaykanString_destroy(PaykanObject *self);
PaykanShared *PaykanString_toString(PaykanObject *self);
int64_t PaykanString_equals(PaykanObject *self, PaykanShared *other);
int64_t PaykanString_length(PaykanObject *self);
PaykanObject *PaykanString_concat(PaykanObject *self, PaykanObject *other);
void PaykanString_concat_inplace(PaykanObject *self, PaykanObject *other);
// Test-only: generated code subscripts a Str through PaykanString_char_at.
PaykanShared *PaykanString_at(PaykanObject *self, int64_t idx);

// Global vtable instance.
extern PaykanMethod PaykanString_vtable[PAYKAN_STRING_SLOTS];

// -- File
//
// Inherits Object.  The first three vtable slots match Object's layout;
// File-specific slots follow (PAYKAN_SLOT_FILE_*).

typedef struct PaykanFile {
  PaykanMethod *vtable; // points to PaykanFile_vtable
  int64_t strong;       // object header (see PaykanObject)
  int64_t weak;
  FILE *handle; // underlying C file handle (NULL if closed)
} PaykanFile;

// Constructor / destructor.
PaykanFile *PaykanFile_new(void);

/// Open a file at `path` with the given `mode` string (e.g. "r", "w").
/// Returns a PaykanShared* wrapping a PaykanFile on success, or a
/// PaykanShared* wrapping a PaykanError on failure.
PaykanShared *PaykanFile_open(PaykanObject *path, PaykanObject *mode);
void PaykanFile_destroy(PaykanObject *self);
PaykanShared *PaykanFile_toString(PaykanObject *self);
int64_t PaykanFile_equals(PaykanObject *self, PaykanShared *other);
void PaykanFile_write(PaykanObject *self, PaykanObject *str);
PaykanShared *PaykanFile_readln(PaykanObject *self);
PaykanShared *PaykanFile_readbytes(PaykanObject *self, int64_t n);
PaykanShared *PaykanFile_read(PaykanObject *self);

// Global vtable instance.
extern PaykanMethod PaykanFile_vtable[PAYKAN_FILE_SLOTS];

// Stdin singleton — immortal PaykanFile wrapping C's stdin.
extern PaykanFile PaykanFile_Stdin;

// -- Error
//
// Inherits Object.  Returned by open() when fopen fails.
// Carries a human-readable message string.

typedef struct PaykanError {
  PaykanMethod *vtable; // points to PaykanError_vtable
  int64_t strong;       // object header (see PaykanObject)
  int64_t weak;
  PaykanString *message;
} PaykanError;

// Constructor: creates an Error with the given message.
PaykanError *PaykanError_new(const char *msg, int64_t len);
void PaykanError_destroy(PaykanObject *self);
PaykanShared *PaykanError_toString(PaykanObject *self);
int64_t PaykanError_equals(PaykanObject *self, PaykanShared *other);

// Global vtable instance.
extern PaykanMethod PaykanError_vtable[PAYKAN_OBJECT_SLOTS];

// -- Shared: strong and weak references to a PaykanObject
//
// An object carries its own reference counts (the header above): there is
// no separate control block, so a reference is the object's pointer.  The
// runtime keeps the two names apart to say what a pointer holds:
//
//   PaykanObject*  a raw alias, counted nowhere (`self` in a method, a
//                  match-arm binding, a temporary not yet boxed);
//   PaykanShared*  a strong reference, counted in `strong` (a variable, a
//                  field, an argument, a result; NULL for an absent `T?`);
//   PaykanWeak*    a weak reference, counted in `weak` (a `weak` variable or
//                  field, docs/language/08-memory-model.md).
//
// "Boxing" a raw pointer (PaykanShared_new) is a strong retain: generated
// code that holds an object only as a raw PaykanObject* can always box it
// and get an owned reference to the same object.  When `strong` drops to
// zero the object's destroy runs (it releases what the object holds and
// tears down its non-object resources, and must not free the object) and
// every weak reference to it reads as NULL from then on; the memory is freed
// right after the destroy, or, while weak references remain, by the last
// weak release.

/// A strong (+1) reference to `obj`: `obj` itself, with `strong` incremented.
/// NULL for NULL.
PaykanShared *PaykanShared_new(PaykanObject *obj);

/// Increment the strong count (NULL-safe).
void Paykan_retain(PaykanShared *shared);

/// Decrement the strong count (NULL-safe).  At zero the object's destroy
/// (vtable slot 0) runs, and the object is freed unless weak references to
/// it remain, in which case the last Paykan_weak_release frees it.
/// Destruction is iterative (Shared.c): an object released to zero inside a
/// destroy is deferred and destroyed by the outermost release before it
/// returns, so deep chains are freed in bounded C stack space.
void Paykan_release(PaykanShared *shared);

/// The object a strong reference names: the same pointer (NULL for NULL).
PaykanObject *PaykanShared_get(PaykanShared *shared);

/// Destroy and free an object nothing references (`strong` and `weak` both
/// zero): a temporary that was never boxed, such as a string the generated
/// code made and did not keep.  The same as boxing it and releasing that
/// reference.  NULL-safe.
void Paykan_dispose(PaykanObject *obj);

/// A weak reference to the object `shared` names (NULL for NULL): the same
/// pointer, with `weak` incremented.  The strong reference is untouched.
PaykanWeak *Paykan_weak_new(PaykanShared *shared);

/// A strong (+1) reference to the object `weak` names while it is alive
/// (its `strong` count is above zero), NULL once it has been destroyed, or
/// is being destroyed, or for a NULL `weak`.
PaykanShared *Paykan_weak_load(PaykanWeak *weak);

/// Decrement the weak count (NULL-safe).  Frees the object when it was
/// destroyed already and this was the last weak reference to it.
void Paykan_weak_release(PaykanWeak *weak);

// -- Generic native functions
//
// An instance of a generic `native fn` (`native fn get<T>(b: view Obj, i: int)
// -> view T = "..."`, docs/language/02-functions-and-calling.md) receives a
// PaykanType for each type argument, in order, ahead of its arguments.  A
// parameter typed by a type parameter crosses by address: a `view T` as a
// `const void *` C borrows for the call, an `inout T` as a `void *` C may
// change in place.  A `-> T` result is written to a `void *out` passed after
// the PaykanTypes, a `-> view T` result is returned as a `const void *` into
// a `view` or `inout` argument.  Every other parameter and result crosses as
// it does for a non-generic native function.
//
// A PaykanType describes a value of the type as it is stored: its size and
// alignment, its kind, and where it holds references (PaykanShared* boxes),
// which a copy retains and a drop releases.  The compiler emits one per
// type, as constant data; the kind codes must match
// paykan::names::NativeTypeKind in include/Names.h.

typedef enum PaykanTypeKind {
  PAYKAN_KIND_INT = 0,      // int64_t (also enum values)
  PAYKAN_KIND_FLOAT = 1,    // double
  PAYKAN_KIND_BOOL = 2,     // bool
  PAYKAN_KIND_CHAR = 3,     // int8_t
  PAYKAN_KIND_OBJECT = 4,   // PaykanShared*, never NULL
  PAYKAN_KIND_OPTIONAL = 5, // PaykanShared*, NULL for None; an inline one
                            // (int? ...) is a bool presence flag and then
                            // its payload
  PAYKAN_KIND_TUPLE = 6,    // its elements in place ("Tuples", below)
  PAYKAN_KIND_STRUCT = 7,   // a struct's fields in place, with their names:
                            // only in a layout ("Structs", below), never
                            // the kind of a PaykanType (a struct's is TUPLE)
  PAYKAN_KIND_UINT = 8,     // uint64_t (`unsigned`); the narrower sized
                            // integers and floats are INT and FLOAT
} PaykanTypeKind;

/// Followed in memory by `nrefs` int64_t words, one per reference: its byte
/// offset in the value << 1, | 1 if it may be NULL (PaykanType_ref).
typedef struct PaykanType {
  int64_t size;  ///< bytes of a value
  int64_t align; ///< its alignment
  int64_t kind;  ///< PaykanTypeKind
  int64_t nrefs; ///< references in a value
} PaykanType;

/// The word of the @p k-th reference of a @p type value.
static inline int64_t PaykanType_ref(const PaykanType *type, int64_t k) {
  return ((const int64_t *)(type + 1))[k];
}

/// Make @p dst (uninitialised storage for a @p type value) a copy of the
/// value at @p src, retaining the references it holds.
void PaykanType_copy(const PaykanType *type, void *dst, const void *src);
/// Release the references the @p type value at @p value holds; the storage
/// is then uninitialised.
void PaykanType_drop(const PaykanType *type, void *value);
/// True if a reference the @p type value at @p value holds that may not be
/// NULL is: storage that was never set.
int PaykanType_isUnset(const PaykanType *type, const void *value);

// -- Tuples
//
// A tuple is a value held in place: its elements laid out like a C struct's
// fields, a reference element as its PaykanShared* box.  The compiler copies
// and releases one inline; rendering and comparing one go through the
// tuple's layout, constant data the compiler emits per tuple type, int64_t
// words: the element count, then for each element its PaykanTypeKind, its
// byte offset, and
//   - for an OPTIONAL, the kind of its payload: OBJECT for a box (NULL for
//     None), or that of an inline optional's payload, which follows its
//     bool presence flag at the payload's alignment (1 for a bool or char,
//     8 for an int or float);
//   - for a TUPLE, the nested tuple's layout (offsets within it).

/// The text of the tuple at @p value, `(1, a)`: each element as `print`
/// shows it, None for an absent optional.  A new string.
PaykanString *PaykanTuple_toString(const void *value, const int64_t *layout);
/// Whether the tuples at @p a and @p b are equal: element by element,
/// floats by IEEE 754 `==`, references by their `equals` (two None equal).
int64_t PaykanTuple_equals(const void *a, const void *b, const int64_t *layout);

// -- Structs
//
// A struct (docs/language/14-structs.md) is a value held in place like a
// tuple, with a name and named fields.  Its layout is a tuple's with the
// names in it: the field count, the struct's name, then for each field its
// name and the words a tuple element has (the kind and the byte offset, then
// an OPTIONAL's payload kind or a nested TUPLE's layout).  A name is its byte
// length in one word, then its bytes packed eight to a word, the first byte
// in the lowest eight bits.  A struct nested in a tuple or in another struct
// is an element of kind STRUCT: its kind and offset, then its own layout.

/// The text of the struct at @p value, `Point(x: 1, y: 2)`: its name, then
/// each field's name and value as `print` shows it.  A new string.
PaykanString *PaykanStruct_toString(const void *value, const int64_t *layout);
/// Whether the structs at @p a and @p b are equal: field by field, as
/// PaykanTuple_equals compares elements.
int64_t PaykanStruct_equals(const void *a, const void *b,
                            const int64_t *layout);

// -- Buffer
//
// The storage of the standard library's memory::Buffer<T>: `len` live values
// of a PaykanType `type`, `cap` of them allocated, reached through generic
// native functions (above).  A function is passed the buffer as its object
// (a `view Obj`), or one that changes it as the address of its box (an
// `inout Obj`).  They are declared with the C types the backends give a
// native function's parameters: `void *` for a PaykanType and any other
// address.  A new value is all zero bytes: 0, False or None.  Destroying the
// buffer drops the values.  An index out of bounds, a negative count,
// shrinking by more than len and reading a never-set reference of a type
// that may not be NULL panic.

typedef struct PaykanBuffer {
  PaykanMethod *vtable; // points to PaykanBuffer_vtable
  int64_t strong;       // object header (see PaykanObject)
  int64_t weak;
  const PaykanType *type; // of the values (the compiler's constant data)
  unsigned char *data;    // cap values of type->size bytes
  int64_t len;            // live values
  int64_t cap;            // allocated values
} PaykanBuffer;

extern PaykanMethod PaykanBuffer_vtable[PAYKAN_OBJECT_SLOTS];

PaykanShared *PaykanBuffer_new(void *t, int64_t n); // owned box
int64_t PaykanBuffer_len(void *t, PaykanObject *b);
/// The address of value @p i, valid until the buffer next changes.
void *PaykanBuffer_get(void *t, PaykanObject *b, int64_t i);
/// Copy value @p i to @p out (uninitialised), retaining its references.
void PaykanBuffer_copy(void *t, void *out, PaykanObject *b, int64_t i);
void PaykanBuffer_set(void *t, void *b, int64_t i, void *v);
void PaykanBuffer_reserve(void *t, void *b, int64_t n);
void PaykanBuffer_grow(void *t, void *b, int64_t by);
void PaykanBuffer_shrink(void *t, void *b, int64_t by);

// -- Hash: the builtin `Hashable` trait's `hash()` of the scalars and Str
//
// The same value always hashes the same within and across runs, on every
// backend; values that are `==` hash alike.  The bits are well mixed, so a
// table may index by the low bits of a hash.

/// `n.hash()` of an int, and of a bool (0 or 1) or char (its code).
int64_t PaykanInt_hash(int64_t n);
/// `s.hash()` of a Str: of its bytes.
int64_t PaykanString_hash(PaykanObject *self);

// -- Runtime panics

/// Print a diagnostic and abort: the lowering's trap target for integer `/`
/// and `%` by zero.
PAYKAN_NORETURN void Paykan_panic_div_by_zero(void);

/// Print a diagnostic and abort: the trap target for `INT64_MIN / -1`, whose
/// quotient is not representable (the hardware divide would raise SIGFPE).
PAYKAN_NORETURN void Paykan_panic_div_overflow(void);

/// Print a diagnostic naming @p value and abort: `int<float>(value)` with a
/// NaN, an infinity, or a value outside the int64 range (the lowering guards
/// the conversion and calls this instead of performing it).
PAYKAN_NORETURN void Paykan_panic_float_to_int(double value);

/// Print a diagnostic naming @p value and abort: `char<int>(value)` outside
/// the char range 0..255.
PAYKAN_NORETURN void Paykan_panic_int_to_char(int64_t value);

/// Paykan_panic_float_to_int for a conversion to the integer type named by
/// the @p len bytes at @p target (`i8<float>(value)`, ...).
PAYKAN_NORETURN void
Paykan_panic_float_to_type(double value, const char *target, int64_t len);

// -- Sized numeric types (docs/design/sized-types.md)

/// @p value rounded to the nearest binary16 (`f16`) value, ties to even:
/// subnormals are kept, and a magnitude of 65520 or more becomes infinity.
double Paykan_round_f16(double value);

/// Print a diagnostic naming @p index and @p len and abort: a fixed-size
/// array's element read or written at an index outside 0..len-1.
PAYKAN_NORETURN void Paykan_panic_index(int64_t index, int64_t len);

/// Print `paykan: <message>` and abort: the standard library's `native fn`
/// for a broken precondition (`pop` on an empty list, say).
PAYKAN_NORETURN void Paykan_panic_message(PaykanObject *message);
/// Paykan_panic_message as a generic `native fn` with a result of type @p t
/// (`-> T`), for where the program needs a value: it never stores one.
PAYKAN_NORETURN void Paykan_panic_value(void *t, void *out,
                                        PaykanObject *message);

// -- I/O builtins

/// Print one object (via toString) to stdout, without a newline.
void Paykan_print(PaykanObject *obj);

/// Print one object (via toString) to stdout, followed by a newline.
void Paykan_println(PaykanObject *obj);

/// Print one object (via toString) to stderr, without a newline.
void Paykan_printerr(PaykanObject *obj);

/// Print one object (via toString) to stderr, followed by a newline.
void Paykan_printerrln(PaykanObject *obj);

/// Flush stdout.  Not reachable from the language (no `flush()` builtin);
/// for programs that embed the runtime.
void Paykan_flush(void);

// -- Program arguments
//
// `fn main(args: view Str[])` is passed a slice of the program's arguments:
// one string per argv entry, or none when PAYKAN_NO_ARGS is set (the kernel
// always supplies an argv[0], so a caller that wants none says so).  The
// entry point makes them before main runs and drops them after it returns.

/// The boxed strings of argv, @p *count of them.
PaykanShared **Paykan_args_new(int argc, char **argv, int64_t *count);
/// Release the @p count strings and the slice itself.
void Paykan_args_free(PaykanShared **args, int64_t count);

// -- Pluggable heap allocator
//
// Every heap allocation of the runtime and of generated code (object structs
// are allocated through the `Paykan_malloc` symbol) goes through these
// functions, which dispatch to one of two back-ends: passthrough (libc, the
// default) or tracking, which counts every live block so a leak check can
// assert that a program frees everything it allocates:
//
//       Paykan_heap_set_tracking(1);
//       Paykan_heap_reset();
//       <run program>
//       assert(Paykan_heap_live_blocks() == 0);   // no leaks
//
// The back-end is selected once at start-up (--track-heap in the driver, or
// the test harness), before any allocation.  Immortal singletons (None,
// Stdin) are static and never pass through here.

typedef struct PaykanHeapStats {
  int64_t liveBlocks;    // currently-allocated blocks (alloc - free)
  int64_t liveBytes;     // currently-allocated payload bytes
  int64_t totalAllocs;   // cumulative successful allocations
  int64_t totalFrees;    // cumulative frees of non-NULL pointers
  int64_t totalReallocs; // cumulative Paykan_realloc calls (incl. NULL ptr)
  int64_t peakBytes;     // high-water mark of liveBytes
} PaykanHeapStats;

/// Allocate `size` bytes.  Returns NULL on failure.
void *Paykan_malloc(size_t size);

/// Resize a block previously returned by Paykan_malloc/Paykan_realloc.
/// Passing NULL behaves like Paykan_malloc.
void *Paykan_realloc(void *ptr, size_t size);

/// Free a block previously returned by Paykan_malloc/Paykan_realloc.
/// Passing NULL is a no-op.
void Paykan_free(void *ptr);

// -- Back-end selection

/// Select the allocator back-end.  Pass non-zero to enable the tracking
/// allocator, zero to use the plain passthrough allocator (the default).
/// Call this once at start-up, before any allocation, so that every pointer is
/// allocated and freed by the same back-end.
void Paykan_heap_set_tracking(int enable);

/// Returns non-zero if the tracking back-end is currently selected.
int Paykan_heap_tracking_enabled(void);

// -- Test / diagnostic hooks (meaningful only while tracking is enabled)

/// Reset all counters to zero.  Call immediately before running a program
/// whose allocations you want to measure in isolation.
void Paykan_heap_reset(void);

/// Convenience: number of blocks currently live (alloc - free).
/// This is the value a leak check asserts to be zero after a clean run.
int64_t Paykan_heap_live_blocks(void);

/// Convenience: number of payload bytes currently live.
int64_t Paykan_heap_live_bytes(void);

/// Number of Paykan_realloc calls since the last reset (e.g. to check that a
/// container's resize policy is amortised).
int64_t Paykan_heap_total_reallocs(void);

/// Print the current heap statistics to stderr (a leak warning is appended if
/// any blocks remain live).  If tracking is disabled, prints a notice instead.
void Paykan_heap_dump(void);

// Every object struct starts with PaykanObject's header (the vtable, then the
// strong and the weak count), so a pointer to it can be converted to and used
// as a PaykanObject * (docs/c-backend.md, "Target assumptions").
#define PAYKAN_CHECK_HEADER(T)                                                 \
  PAYKAN_STATIC_ASSERT(offsetof(T, vtable) == 0 &&                             \
                           offsetof(T, strong) ==                              \
                               offsetof(PaykanObject, strong) &&               \
                           offsetof(T, weak) == offsetof(PaykanObject, weak),  \
                       #T " must start with the object header")
PAYKAN_CHECK_HEADER(PaykanString);
PAYKAN_CHECK_HEADER(PaykanFile);
PAYKAN_CHECK_HEADER(PaykanError);
PAYKAN_CHECK_HEADER(PaykanBuffer);
#undef PAYKAN_CHECK_HEADER

#ifdef __cplusplus
}

namespace paykan {
/// PAYKAN_RUNTIME_ABI_VERSION for C++ callers (the backends, the .pkm host
/// identity).
inline constexpr uint32_t kRuntimeABIVersion = PAYKAN_RUNTIME_ABI_VERSION;
} // namespace paykan
#endif

#endif // PAYKAN_RUNTIME_H

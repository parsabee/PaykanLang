// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan language runtime — C ABI definitions for builtin class types.
//
// Every Paykan object begins with a pointer to its class's vtable.
// The vtable is a flat array of function pointers whose layout mirrors
// the ClassType::VTable vector built during AST bootstrapping:
//
//   Object vtable  : [0] toString  [1] equals
//   String vtable  : [0] toString  [1] equals  [2] length  [3] concat
//
// All runtime functions use C linkage so LLVM IR can reference them
// directly by name.

#ifndef PAYKAN_RUNTIME_H
#define PAYKAN_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct PaykanObject PaykanObject;
typedef struct PaykanString PaykanString;
typedef struct PaykanShared PaykanShared;
typedef struct PaykanArray PaykanArray;

// ============================================================================
// Object
// ============================================================================
//
// Root of the class hierarchy.  Every Paykan heap object starts with this
// two-word header so that a PaykanObject* can always reach the vtable AND its
// owning PaykanShared box:
//
//   [0] vtable  — method dispatch + type identity
//   [1] shared  — backpointer to this object's unique PaykanShared box
//                 (the unique-box invariant, see PaykanShared below), or NULL
//                 while the object has never been boxed / is not currently
//                 boxed.
//
// Every constructor (runtime C constructors here, and the generated class
// constructors in CodeGen) must initialise `shared` to NULL; PaykanShared_new
// installs the backpointer and Paykan_release clears it when the box dies.

typedef struct PaykanObjectVTable {
  void (*destroy)(PaykanObject *self);
  PaykanShared *(*toString)(PaykanObject *self);
  int64_t (*equals)(PaykanObject *self, PaykanObject *other);
} PaykanObjectVTable;

struct PaykanObject {
  PaykanObjectVTable *vtable;
  PaykanShared *shared; // unique-box backpointer (header slot 1)
};

// Constructor.  Test-only: CodeGen never emits a call to this symbol (user
// `Obj()` construction goes through the generated class machinery); it is kept
// for the unit tests and remains JIT-mapped for completeness.
PaykanObject *PaykanObject_new(void);

// Default method implementations.
void PaykanObject_destroy(PaykanObject *self);
PaykanShared *PaykanObject_toString(PaykanObject *self);
int64_t PaykanObject_equals(PaykanObject *self, PaykanObject *other);

// Global vtable instance.
extern PaykanObjectVTable PaykanObject_vtable;

// Singleton None instance — an Obj whose toString returns "None".
extern PaykanObject PaykanObject_None;

// ============================================================================
// Int
// ============================================================================
//
// Boxed 64-bit signed integer.  Inherits Object.

typedef struct PaykanInt {
  PaykanObjectVTable *vtable;
  PaykanShared *shared; // object header (see PaykanObject)
  int64_t value;
} PaykanInt;

PaykanInt *PaykanInt_new(int64_t value);
/// The value of a boxed Int (the unwrapped `int` of a present `int?`).
int64_t PaykanInt_value(PaykanObject *self);
void PaykanInt_destroy(PaykanObject *self);
PaykanShared *PaykanInt_toString(PaykanObject *self);
int64_t PaykanInt_equals(PaykanObject *self, PaykanObject *other);

extern PaykanObjectVTable PaykanInt_vtable;

/// Parse a Str as a decimal integer.
/// Returns a PaykanShared* wrapping a PaykanInt on success,
/// or a PaykanShared* wrapping a PaykanError on failure.
PaykanShared *PaykanInt_from_str(PaykanObject *str);

// ============================================================================
// Float
// ============================================================================
//
// Boxed 64-bit IEEE 754 double.  Inherits Object.

typedef struct PaykanFloat {
  PaykanObjectVTable *vtable;
  PaykanShared *shared; // object header (see PaykanObject)
  double value;
} PaykanFloat;

PaykanFloat *PaykanFloat_new(double value);
/// The value of a boxed Float (the unwrapped `float` of a present `float?`).
double PaykanFloat_value(PaykanObject *self);
void PaykanFloat_destroy(PaykanObject *self);
PaykanShared *PaykanFloat_toString(PaykanObject *self);
int64_t PaykanFloat_equals(PaykanObject *self, PaykanObject *other);

extern PaykanObjectVTable PaykanFloat_vtable;

/// Parse a Str as a floating-point number.
/// Returns a PaykanShared* wrapping a PaykanFloat on success,
/// or a PaykanShared* wrapping a PaykanError on failure.
PaykanShared *PaykanFloat_from_str(PaykanObject *str);

// ============================================================================
// Bool
// ============================================================================
//
// Boxed boolean (stored as int64_t 0/1).  Inherits Object.

typedef struct PaykanBool {
  PaykanObjectVTable *vtable;
  PaykanShared *shared; // object header (see PaykanObject)
  int64_t value;        // 0 = False, 1 = True
} PaykanBool;

// Bools are unboxed i1/i64 values in generated code; the lowering boxes one
// only for a present `bool?`.
PaykanBool *PaykanBool_new(int64_t value);
/// The value (0 or 1) of a boxed Bool (the unwrapped `bool` of a `bool?`).
int64_t PaykanBool_value(PaykanObject *self);
void PaykanBool_destroy(PaykanObject *self);
PaykanShared *PaykanBool_toString(PaykanObject *self);
int64_t PaykanBool_equals(PaykanObject *self, PaykanObject *other);

extern PaykanObjectVTable PaykanBool_vtable;

// ============================================================================
// Char
// ============================================================================
//
// Boxed character (a signed 8-bit byte, like `char` in generated code).
// Inherits Object.  Only a present `char?` is boxed.

typedef struct PaykanChar {
  PaykanObjectVTable *vtable;
  PaykanShared *shared; // object header (see PaykanObject)
  int8_t value;
} PaykanChar;

PaykanChar *PaykanChar_new(int8_t value);
/// The value of a boxed Char (the unwrapped `char` of a present `char?`).
int8_t PaykanChar_value(PaykanObject *self);
void PaykanChar_destroy(PaykanObject *self);
PaykanShared *PaykanChar_toString(PaykanObject *self);
int64_t PaykanChar_equals(PaykanObject *self, PaykanObject *other);

extern PaykanObjectVTable PaykanChar_vtable;

// ============================================================================
// String
// ============================================================================
//
// Inherits Object.  The first two vtable slots match Object's layout;
// additional slots follow for String-specific methods.

typedef struct PaykanStringVTable {
  // Inherited (Object-compatible prefix)
  void (*destroy)(PaykanObject *self);
  PaykanShared *(*toString)(PaykanObject *self);
  int64_t (*equals)(PaykanObject *self, PaykanObject *other);
  // String-specific
  int64_t (*length)(PaykanObject *self);
  void (*concat)(PaykanObject *self, PaykanObject *other);
} PaykanStringVTable;

struct PaykanString {
  PaykanObjectVTable *vtable; // points to PaykanString_vtable (cast-compatible)
  PaykanShared *shared;       // object header (see PaykanObject)
  char *data;                 // heap-allocated NUL-terminated buffer
  int64_t len;                // length in bytes (excludes NUL)
};

// Constructor / destructor.
PaykanString *PaykanString_new(const char *data, int64_t len);
PaykanString *PaykanString_from_int(int64_t value);
PaykanString *PaykanString_from_float(double value);
PaykanString *PaykanString_from_bool(int64_t value);
PaykanString *PaykanString_from_char(int8_t c);
int8_t PaykanString_char_at(PaykanObject *self, int64_t idx);

// Method implementations.
void PaykanString_destroy(PaykanObject *self);
PaykanShared *PaykanString_toString(PaykanObject *self);
int64_t PaykanString_equals(PaykanObject *self, PaykanObject *other);
int64_t PaykanString_length(PaykanObject *self);
PaykanObject *PaykanString_concat(PaykanObject *self, PaykanObject *other);
void PaykanString_concat_inplace(PaykanObject *self, PaykanObject *other);
// Test-only: CodeGen lowers string subscripts through PaykanString_char_at;
// kept for the unit tests and JIT-mapped for completeness.
PaykanShared *PaykanString_at(PaykanObject *self, int64_t idx);

// Global vtable instance.
extern PaykanStringVTable PaykanString_vtable;

// ============================================================================
// File
// ============================================================================
//
// Inherits Object.  Vtable layout matches PaykanObjectVTable exactly
// (destroy / toString / equals); no File-specific slots yet.

typedef struct PaykanFileVTable {
  void (*destroy)(PaykanObject *self);
  PaykanShared *(*toString)(PaykanObject *self);
  int64_t (*equals)(PaykanObject *self, PaykanObject *other);
  // File-specific
  void (*write)(PaykanObject *self, PaykanObject *str);
  PaykanShared *(*readln)(PaykanObject *self);
  PaykanShared *(*readbytes)(PaykanObject *self, int64_t n);
  PaykanShared *(*read)(PaykanObject *self);
} PaykanFileVTable;

typedef struct PaykanFile {
  PaykanObjectVTable *vtable; // points to PaykanFile_vtable
  PaykanShared *shared;       // object header (see PaykanObject)
  FILE *handle;               // underlying C file handle (NULL if closed)
} PaykanFile;

// Constructor / destructor.
PaykanFile *PaykanFile_new(void);

/// Open a file at `path` with the given `mode` string (e.g. "r", "w").
/// Returns a PaykanShared* wrapping a PaykanFile on success, or a
/// PaykanShared* wrapping a PaykanError on failure.
PaykanShared *PaykanFile_open(PaykanObject *path, PaykanObject *mode);
void PaykanFile_destroy(PaykanObject *self);
PaykanShared *PaykanFile_toString(PaykanObject *self);
int64_t PaykanFile_equals(PaykanObject *self, PaykanObject *other);
void PaykanFile_write(PaykanObject *self, PaykanObject *str);
PaykanShared *PaykanFile_readln(PaykanObject *self);
PaykanShared *PaykanFile_readbytes(PaykanObject *self, int64_t n);
PaykanShared *PaykanFile_read(PaykanObject *self);

// Global vtable instance.
extern PaykanFileVTable PaykanFile_vtable;

// Stdin singleton — immortal PaykanFile wrapping C's stdin.
extern PaykanFile PaykanFile_Stdin;

// ============================================================================
// Error
// ============================================================================
//
// Inherits Object.  Returned by open() when fopen fails.
// Carries a human-readable message string.

typedef struct PaykanError {
  PaykanObjectVTable *vtable; // points to PaykanError_vtable
  PaykanShared *shared;       // object header (see PaykanObject)
  PaykanString *message;
} PaykanError;

// Constructor: creates an Error with the given message.
PaykanError *PaykanError_new(const char *msg, int64_t len);
void PaykanError_destroy(PaykanObject *self);
PaykanShared *PaykanError_toString(PaykanObject *self);
int64_t PaykanError_equals(PaykanObject *self, PaykanObject *other);

// Global vtable instance.
extern PaykanObjectVTable PaykanError_vtable;

// ============================================================================
// Array
// ============================================================================
//
// Inherits Object.  The first three vtable slots match Object's layout;
// additional slots follow for Array-specific methods.
//
// Every element slot is pointer-sized (8 bytes on all supported targets).
// Primitive elements (int, float, bool) are stored unboxed as int64_t/double;
// class-type elements store a PaykanShared* (retained by the array).

typedef struct PaykanArray PaykanArray;

typedef struct PaykanArrayVTable {
  // Inherited (Object-compatible prefix)
  void (*destroy)(PaykanObject *self);
  PaykanShared *(*toString)(PaykanObject *self);
  int64_t (*equals)(PaykanObject *self, PaykanObject *other);
  // Array-specific
  int64_t (*length)(PaykanObject *self);
} PaykanArrayVTable;

struct PaykanArray {
  PaykanObjectVTable *vtable; // PaykanArray_vtable (primitive) or
                              // PaykanArray_obj_vtable (object)
  PaykanShared *shared;       // object header (see PaykanObject)
  void *data;                 // heap-allocated element buffer (8 bytes/slot)
  unsigned long len;          // number of live elements
  unsigned long cap;          // allocated capacity (in elements)
};

// -- Constructors ------------------------------------------------------------

/// Primitive array (int / float / bool): elements stored as raw 8-byte values.
PaykanArray *PaykanArray_new(unsigned long len);
PaykanArray *PaykanArray_new_from_data(unsigned long len, const void *data);

/// Object array (class-type elements stored as PaykanShared*):
/// set/destroy manage reference counts automatically.
PaykanArray *PaykanArray_new_obj(unsigned long len);

// -- Element access ----------------------------------------------------------

/// Read an 8-byte slot as a void*.
/// Caller reinterprets as int64_t, double, or PaykanShared* as appropriate.
void *PaykanArray_get(PaykanArray *arr, unsigned long idx);

/// Primitive set: plain 8-byte store, no reference counting.
void PaykanArray_set(PaykanArray *arr, unsigned long idx, void *value);

/// Object set: releases the old PaykanShared* in the slot and retains the new
/// one.
void PaykanArray_set_obj(PaykanArray *arr, unsigned long idx,
                         PaykanShared *value);

// -- Method implementations --------------------------------------------------
void PaykanArray_destroy(PaykanObject *self);
void PaykanArray_destroy_obj(PaykanObject *self);
PaykanShared *PaykanArray_toString(PaykanObject *self);
int64_t PaykanArray_equals(PaykanObject *self, PaykanObject *other);
int64_t PaykanArray_length(PaykanObject *self);

/// Append a primitive value to the array (reallocs the backing buffer).
void PaykanArray_push(PaykanArray *arr, void *value);

/// Append an object (PaykanShared*) to the array; retains the new element.
void PaykanArray_push_obj(PaykanArray *arr, PaykanShared *value);

/// Remove and return the last primitive element.  Aborts if array is empty.
void *PaykanArray_pop(PaykanArray *arr);

/// Remove and return the last object element (transferred ownership — caller
/// is responsible for releasing the returned PaykanShared*).
PaykanShared *PaykanArray_pop_obj(PaykanArray *arr);

// -- VTable instances --------------------------------------------------------
extern PaykanArrayVTable PaykanArray_vtable;     // for primitive-element arrays
extern PaykanArrayVTable PaykanArray_obj_vtable; // for object-element arrays

// ============================================================================
// Tuple (prototype)
// ============================================================================
//
// Inherits Object; vtable layout is exactly PaykanObjectVTable
// (destroy / toString / equals).  ONE generic object backs every tuple type
// `(T1, T2, ...)`: `count` 8-byte slots plus one kind byte per slot telling
// the runtime how to interpret it.  Primitive elements are stored raw;
// reference elements (classes, arrays, nested tuples) store a PaykanShared*
// retained by the tuple.  Tuples are immutable at the language level — the
// set functions exist so a literal can be filled right after construction.
//
// The kind codes are part of the CodeGen <-> runtime ABI; they must match
// paykan::names::TupleSlotKind in include/Names.h.

typedef enum PaykanTupleKind {
  PAYKAN_TUPLE_INT = 0,   // int64_t (also enum values)
  PAYKAN_TUPLE_FLOAT = 1, // IEEE-754 double bits
  PAYKAN_TUPLE_BOOL = 2,  // int64_t 0 / 1
  PAYKAN_TUPLE_CHAR = 3,  // int64_t holding one byte
  PAYKAN_TUPLE_REF = 4,   // PaykanShared* (retained) or NULL
} PaykanTupleKind;

typedef struct PaykanTuple {
  PaykanObjectVTable *vtable; // points to PaykanTuple_vtable
  PaykanShared *shared;       // object header (see PaykanObject)
  int64_t count;              // number of elements (arity)
  uint64_t *slots;            // count 8-byte slots, inline after the header
  uint8_t *kinds;             // count kind bytes, inline after the slots
  // (slots and kinds point into the same heap block as the header: one
  // allocation per tuple.  A pointer rather than a flexible array member
  // because this header is also compiled as C++ with -Wpedantic.)
} PaykanTuple;

/// Allocate a tuple with `count` zeroed slots; `kinds` (count bytes, copied)
/// gives each slot's PaykanTupleKind.
PaykanTuple *PaykanTuple_new(int64_t count, const uint8_t *kinds);

/// Number of elements.  Test-only (the arity is static in generated code).
int64_t PaykanTuple_count(PaykanTuple *t);

/// Kind of slot `idx`.  Test-only.  Aborts if out of range.
int64_t PaykanTuple_kind(PaykanTuple *t, int64_t idx);

/// Read the raw 8-byte slot `idx` (the caller reinterprets as int64_t, double
/// bits, or PaykanShared* — a reference is NOT retained).  Aborts if out of
/// range.
int64_t PaykanTuple_get(PaykanTuple *t, int64_t idx);

/// Store raw bits into a value slot.  Aborts on a reference slot.
void PaykanTuple_set(PaykanTuple *t, int64_t idx, int64_t bits);

/// Store a box into a reference slot: releases the old box (if any) and
/// retains the new one (the caller keeps its own reference).  Aborts on a
/// value slot.
void PaykanTuple_set_obj(PaykanTuple *t, int64_t idx, PaykanShared *value);

// -- Method implementations --------------------------------------------------
void PaykanTuple_destroy(PaykanObject *self); // releases every REF slot
PaykanShared *PaykanTuple_toString(PaykanObject *self); // "(1, a)"
int64_t PaykanTuple_equals(PaykanObject *self, PaykanObject *other);

// Global vtable instance (shared by every tuple type).
extern PaykanObjectVTable PaykanTuple_vtable;

// ============================================================================
// Shared — reference-counted wrapper around any PaykanObject
// ============================================================================
//
// A PaykanShared box holds a strong reference count and a pointer to the
// owned object.  When the count drops to zero, the owned object is deleted
// and the box itself is freed.
//
// -- The unique-box invariant ------------------------------------------------
//
// Every live heap object has AT MOST ONE PaykanShared box, and the object's
// header backpointer (`obj->shared`, slot 1) names it.  Two independent boxes
// around the same object would each destroy it when their own refcount hits
// zero — a double free.  That situation used to arise whenever generated code
// needed ownership of a value it only held as a raw PaykanObject* alias (the
// raw `self` method parameter, a match-arm binding, an array element) and
// wrapped it in a *fresh* box.
//
// Design decision (PAY-1): the invariant is enforced here, in the runtime, by
// giving PaykanShared_new "create OR acquire" semantics — if `obj` already has
// a box, its refcount is bumped and that same box is returned; only an unboxed
// object gets a fresh box (whose backpointer is installed).  The alternative
// (plumbing a backing box through every alias site in CodeGen) cannot cover
// raw `self`, because the method ABI passes the unboxed object pointer and the
// caller's box is unreachable from the callee.  A separate
// `PaykanShared_from_object` entry point was considered and rejected: the JIT
// resolves runtime symbols from a fixed table (src/JIT/JIT.cpp), so recovery
// must ride on the already-registered PaykanShared_new symbol.  With these
// semantics, "box this raw pointer" is *always* correct: it degenerates to the
// old behaviour for freshly constructed objects and to a retain for aliases.

typedef struct PaykanShared {
  int64_t refCount;     // strong reference count (starts at 1)
  PaykanObject *object; // the owned object (never NULL)
} PaykanShared;

/// Return an owned (+1) box for `obj` — the unique-box invariant's single
/// entry point.  If `obj` already has a box (obj->shared != NULL) that box is
/// retained and returned; otherwise a fresh box (refcount 1) is created and
/// installed as `obj->shared`.  The destructor is taken from
/// `obj->vtable->destroy` when the refcount reaches zero.
PaykanShared *PaykanShared_new(PaykanObject *obj);

/// Increment the reference count.
void Paykan_retain(PaykanShared *shared);

/// Decrement the reference count.  Destroys the owned object and frees
/// the box when it reaches zero, clearing the object's box backpointer
/// first (so an immortal object — static None / Stdin, whose destroy is a
/// no-op — is left unboxed rather than dangling).
void Paykan_release(PaykanShared *shared);

/// Convenience: return the underlying object pointer.
PaykanObject *PaykanShared_get(PaykanShared *shared);

// ============================================================================
// Runtime panics
// ============================================================================

/// Print an integer divide-by-zero diagnostic to stderr and abort the process.
/// Emitted by CodeGen as the trap target for integer `/` and `%` by zero,
/// mirroring the runtime abort on out-of-bounds array access.
PAYKAN_NORETURN void Paykan_panic_div_by_zero(void);

/// Print an integer-overflow diagnostic to stderr and abort the process.
/// Emitted by CodeGen as the trap target for `INT64_MIN / -1`, whose quotient
/// is not representable (the hardware divide would raise SIGFPE instead).
PAYKAN_NORETURN void Paykan_panic_div_overflow(void);

// ============================================================================
// I/O builtins
// ============================================================================

/// Print one object (via toString) to stdout, without a newline.
void Paykan_print(PaykanObject *obj);

/// Print one object (via toString) to stdout, followed by a newline.
void Paykan_println(PaykanObject *obj);

/// Print one object (via toString) to stderr, without a newline.
void Paykan_printerr(PaykanObject *obj);

/// Print one object (via toString) to stderr, followed by a newline.
void Paykan_printerrln(PaykanObject *obj);

/// Flush stdout so that prompts appear before blocking reads.
/// NOTE: not currently wired to a Paykan-level builtin — Sema/CodeGen/JIT do
/// not register it and no `flush()` exists in the language.  Kept for direct
/// runtime embedders; exposing a `flush()` builtin is a future language
/// decision.
void Paykan_flush(void);

// ============================================================================
// Pluggable heap allocator
// ============================================================================
//
// Every heap allocation made by the runtime *and* by JIT/AOT-generated code
// (object structs are allocated via the `Paykan_malloc` symbol) flows through
// these functions instead of the C library malloc/free/realloc directly.
//
// They dispatch through a function-pointer table with two back-ends:
//
//   • passthrough (default) — thin wrappers over libc, zero overhead.
//   • tracking — counts every live block so a leak check can assert that a
//     program frees everything it allocates:
//
//       Paykan_heap_set_tracking(1);
//       Paykan_heap_reset();
//       <run program>
//       assert(Paykan_heap_live_blocks() == 0);   // no leaks
//
// The back-end is selected once at start-up (driven by a command-line flag in
// the driver, or by the test harness).  Immortal singletons (e.g.
// PaykanObject_None) are statically allocated and never pass through here, so
// they correctly do not affect the counters.

typedef struct PaykanHeapStats {
  int64_t liveBlocks;  // currently-allocated blocks (alloc - free)
  int64_t liveBytes;   // currently-allocated payload bytes
  int64_t totalAllocs; // cumulative successful allocations
  int64_t totalFrees;  // cumulative frees of non-NULL pointers
  int64_t peakBytes;   // high-water mark of liveBytes
} PaykanHeapStats;

/// Allocate `size` bytes.  Returns NULL on failure.
void *Paykan_malloc(size_t size);

/// Resize a block previously returned by Paykan_malloc/Paykan_realloc.
/// Passing NULL behaves like Paykan_malloc.
void *Paykan_realloc(void *ptr, size_t size);

/// Free a block previously returned by Paykan_malloc/Paykan_realloc.
/// Passing NULL is a no-op.
void Paykan_free(void *ptr);

// -- Back-end selection ------------------------------------------------------

/// Select the allocator back-end.  Pass non-zero to enable the tracking
/// allocator, zero to use the plain passthrough allocator (the default).
/// Call this once at start-up, before any allocation, so that every pointer is
/// allocated and freed by the same back-end.
void Paykan_heap_set_tracking(int enable);

/// Returns non-zero if the tracking back-end is currently selected.
int Paykan_heap_tracking_enabled(void);

// -- Test / diagnostic hooks (meaningful only while tracking is enabled) -----

/// Reset all counters to zero.  Call immediately before running a program
/// whose allocations you want to measure in isolation.
void Paykan_heap_reset(void);

/// Convenience: number of blocks currently live (alloc - free).
/// This is the value a leak check asserts to be zero after a clean run.
int64_t Paykan_heap_live_blocks(void);

/// Convenience: number of payload bytes currently live.
int64_t Paykan_heap_live_bytes(void);

/// Print the current heap statistics to stderr (a leak warning is appended if
/// any blocks remain live).  If tracking is disabled, prints a notice instead.
void Paykan_heap_dump(void);

#ifdef __cplusplus
}
#endif

#endif // PAYKAN_RUNTIME_H

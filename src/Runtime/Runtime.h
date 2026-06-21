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

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct PaykanObject  PaykanObject;
typedef struct PaykanString  PaykanString;
typedef struct PaykanShared  PaykanShared;
typedef struct PaykanArray   PaykanArray;

// ============================================================================
// Object
// ============================================================================
//
// Root of the class hierarchy.  Every Paykan heap object starts with this
// layout so that a PaykanObject* can always reach the vtable.

typedef struct PaykanObjectVTable {
  void          (*destroy) (PaykanObject *self);
  PaykanShared *(*toString)(PaykanObject *self);
  int64_t       (*equals)  (PaykanObject *self, PaykanObject *other);
} PaykanObjectVTable;

struct PaykanObject {
  PaykanObjectVTable *vtable;
};

// Constructor.
PaykanObject *PaykanObject_new(void);

// Default method implementations.
void          PaykanObject_destroy(PaykanObject *self);
PaykanShared *PaykanObject_toString(PaykanObject *self);
int64_t       PaykanObject_equals(PaykanObject *self, PaykanObject *other);

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
  int64_t             value;
} PaykanInt;

PaykanInt    *PaykanInt_new    (int64_t value);
void          PaykanInt_destroy (PaykanObject *self);
PaykanShared *PaykanInt_toString(PaykanObject *self);
int64_t       PaykanInt_equals  (PaykanObject *self, PaykanObject *other);

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
  double              value;
} PaykanFloat;

PaykanFloat  *PaykanFloat_new    (double value);
void          PaykanFloat_destroy (PaykanObject *self);
PaykanShared *PaykanFloat_toString(PaykanObject *self);
int64_t       PaykanFloat_equals  (PaykanObject *self, PaykanObject *other);

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
  int64_t             value; // 0 = False, 1 = True
} PaykanBool;

PaykanBool   *PaykanBool_new    (int64_t value);
void          PaykanBool_destroy (PaykanObject *self);
PaykanShared *PaykanBool_toString(PaykanObject *self);
int64_t       PaykanBool_equals  (PaykanObject *self, PaykanObject *other);

extern PaykanObjectVTable PaykanBool_vtable;

// ============================================================================
// String
// ============================================================================
//
// Inherits Object.  The first two vtable slots match Object's layout;
// additional slots follow for String-specific methods.

typedef struct PaykanStringVTable {
  // Inherited (Object-compatible prefix)
  void          (*destroy) (PaykanObject *self);
  PaykanShared *(*toString)(PaykanObject *self);
  int64_t       (*equals)  (PaykanObject *self, PaykanObject *other);
  // String-specific
  int64_t        (*length)(PaykanObject *self);
  void          (*concat)  (PaykanObject *self, PaykanObject *other);
} PaykanStringVTable;

struct PaykanString {
  PaykanObjectVTable *vtable;   // points to PaykanString_vtable (cast-compatible)
  char               *data;     // heap-allocated NUL-terminated buffer
  int64_t             len;      // length in bytes (excludes NUL)
};

// Constructor / destructor.
PaykanString *PaykanString_new(const char *data, int64_t len);
PaykanString *PaykanString_from_int(int64_t value);
PaykanString *PaykanString_from_float(double value);
PaykanString *PaykanString_from_bool(int64_t value);
PaykanString *PaykanString_from_char(int8_t c);
int8_t        PaykanString_char_at(PaykanObject *self, int64_t idx);

// Method implementations.
void          PaykanString_destroy(PaykanObject *self);
PaykanShared *PaykanString_toString(PaykanObject *self);
int64_t       PaykanString_equals(PaykanObject *self, PaykanObject *other);
int64_t       PaykanString_length(PaykanObject *self);
PaykanObject *PaykanString_concat(PaykanObject *self, PaykanObject *other);
void          PaykanString_concat_inplace(PaykanObject *self, PaykanObject *other);
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
  void          (*destroy)   (PaykanObject *self);
  PaykanShared *(*toString)  (PaykanObject *self);
  int64_t       (*equals)    (PaykanObject *self, PaykanObject *other);
  // File-specific
  void          (*write)     (PaykanObject *self, PaykanObject *str);
  PaykanShared *(*readln)    (PaykanObject *self);
  PaykanShared *(*readbytes) (PaykanObject *self, int64_t n);
  PaykanShared *(*read)      (PaykanObject *self);
} PaykanFileVTable;

typedef struct PaykanFile {
  PaykanObjectVTable *vtable; // points to PaykanFile_vtable
  FILE               *handle; // underlying C file handle (NULL if closed)
} PaykanFile;

// Constructor / destructor.
PaykanFile   *PaykanFile_new(void);

/// Open a file at `path` with the given `mode` string (e.g. "r", "w").
/// Returns a PaykanShared* wrapping a PaykanFile on success, or a
/// PaykanShared* wrapping a PaykanError on failure.
PaykanShared *PaykanFile_open   (PaykanObject *path, PaykanObject *mode);
void          PaykanFile_destroy (PaykanObject *self);
PaykanShared *PaykanFile_toString(PaykanObject *self);
int64_t       PaykanFile_equals  (PaykanObject *self, PaykanObject *other);
void          PaykanFile_write     (PaykanObject *self, PaykanObject *str);
PaykanShared *PaykanFile_readln   (PaykanObject *self);
PaykanShared *PaykanFile_readbytes(PaykanObject *self, int64_t n);
PaykanShared *PaykanFile_read     (PaykanObject *self);

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
  PaykanString       *message;
} PaykanError;

// Constructor: creates an Error with the given message.
PaykanError  *PaykanError_new    (const char *msg, int64_t len);
void          PaykanError_destroy (PaykanObject *self);
PaykanShared *PaykanError_toString(PaykanObject *self);
int64_t       PaykanError_equals  (PaykanObject *self, PaykanObject *other);

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
  void          (*destroy) (PaykanObject *self);
  PaykanShared *(*toString)(PaykanObject *self);
  int64_t       (*equals)  (PaykanObject *self, PaykanObject *other);
  // Array-specific
  int64_t       (*length)  (PaykanObject *self);
} PaykanArrayVTable;

struct PaykanArray {
  PaykanObjectVTable *vtable; // PaykanArray_vtable (primitive) or PaykanArray_obj_vtable (object)
  void               *data;  // heap-allocated element buffer (8 bytes/slot)
  unsigned long       len;   // number of live elements
  unsigned long       cap;   // allocated capacity (in elements)
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

/// Object set: releases the old PaykanShared* in the slot and retains the new one.
void PaykanArray_set_obj(PaykanArray *arr, unsigned long idx, PaykanShared *value);

// -- Method implementations --------------------------------------------------
void          PaykanArray_destroy    (PaykanObject *self);
void          PaykanArray_destroy_obj(PaykanObject *self);
PaykanShared *PaykanArray_toString   (PaykanObject *self);
int64_t       PaykanArray_equals     (PaykanObject *self, PaykanObject *other);
int64_t       PaykanArray_length     (PaykanObject *self);

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

/// Read element at `idx` as a pointer-sized value.  Aborts on out-of-bounds.
void *PaykanArray_get(PaykanArray *arr, unsigned long idx);

/// Write element at `idx`.  Aborts on out-of-bounds.
void PaykanArray_set(PaykanArray *arr, unsigned long idx, void *value);

// Method implementations (also used directly by CodeGen).
void          PaykanArray_destroy (PaykanObject *self);
PaykanShared *PaykanArray_toString(PaykanObject *self);
int64_t       PaykanArray_equals  (PaykanObject *self, PaykanObject *other);
int64_t       PaykanArray_length  (PaykanObject *self);

// Global vtable instance.
extern PaykanArrayVTable PaykanArray_vtable;

// ============================================================================
// Shared — reference-counted wrapper around any PaykanObject
// ============================================================================
//
// A PaykanShared box holds a strong reference count and a pointer to the
// owned object.  When the count drops to zero, the owned object is deleted
// and the box itself is freed.

typedef struct PaykanShared {
  int64_t       refCount;  // strong reference count (starts at 1)
  PaykanObject *object;    // the owned object (never NULL)
} PaykanShared;

/// Create a shared wrapper around `obj`.  The destructor is taken from
/// `obj->vtable->destroy` when the refcount reaches zero.
PaykanShared *PaykanShared_new(PaykanObject *obj);

/// Increment the reference count.
void Paykan_retain(PaykanShared *shared);

/// Decrement the reference count.  Destroys the owned object and frees
/// the box when it reaches zero.
void Paykan_release(PaykanShared *shared);

/// Convenience: return the underlying object pointer.
PaykanObject *PaykanShared_get(PaykanShared *shared);

// ============================================================================
// I/O builtins
// ============================================================================

/// Print object arguments (via toString) to stdout, without a newline.
void Paykan_print(int64_t argc, ...);

/// Print object arguments (via toString) to stdout, followed by a newline.
void Paykan_println(int64_t argc, ...);

/// Print object arguments (via toString) to stderr, without a newline.
void Paykan_printerr(int64_t argc, ...);

/// Print object arguments (via toString) to stderr, followed by a newline.
void Paykan_printerrln(int64_t argc, ...);

/// Flush stdout so that prompts appear before blocking reads.
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

/// Snapshot the current counters.
PaykanHeapStats Paykan_heap_stats(void);

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

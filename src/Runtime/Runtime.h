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

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct PaykanObject  PaykanObject;
typedef struct PaykanString  PaykanString;

// ============================================================================
// Object
// ============================================================================
//
// Root of the class hierarchy.  Every Paykan heap object starts with this
// layout so that a PaykanObject* can always reach the vtable.

typedef struct PaykanObjectVTable {
  PaykanString *(*toString)(PaykanObject *self);
  int64_t       (*equals)  (PaykanObject *self, PaykanObject *other);
} PaykanObjectVTable;

struct PaykanObject {
  PaykanObjectVTable *vtable;
};

// Constructor / destructor.
PaykanObject *PaykanObject_new(void);
void          PaykanObject_delete(PaykanObject *self);

// Default method implementations.
PaykanString *PaykanObject_toString(PaykanObject *self);
int64_t       PaykanObject_equals(PaykanObject *self, PaykanObject *other);

// Global vtable instance.
extern PaykanObjectVTable PaykanObject_vtable;

// ============================================================================
// String
// ============================================================================
//
// Inherits Object.  The first two vtable slots match Object's layout;
// additional slots follow for String-specific methods.

typedef struct PaykanStringVTable {
  // Inherited (Object-compatible prefix)
  PaykanString *(*toString)(PaykanObject *self);
  int64_t       (*equals)  (PaykanObject *self, PaykanObject *other);
  // String-specific
  int64_t        (*length)(PaykanObject *self);
  PaykanObject  *(*concat)(PaykanObject *self, PaykanObject *other);
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
void          PaykanString_delete(PaykanString *self);

// Method implementations.
PaykanString *PaykanString_toString(PaykanObject *self);
int64_t       PaykanString_equals(PaykanObject *self, PaykanObject *other);
int64_t       PaykanString_length(PaykanObject *self);
PaykanObject *PaykanString_concat(PaykanObject *self, PaykanObject *other);

// Global vtable instance.
extern PaykanStringVTable PaykanString_vtable;

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
  void        (*destroy)(PaykanObject *); // destructor for the owned object
} PaykanShared;

/// Create a shared wrapper around `obj`.  `destroyFn` is the destructor
/// to call on the owned object when refCount reaches zero
/// (e.g. PaykanObject_delete, PaykanString_delete cast to the right sig).
PaykanShared *PaykanShared_new(PaykanObject *obj,
                               void (*destroyFn)(PaykanObject *));

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

/// Print object arguments (via toString) to stdout, followed by a newline.
void Paykan_out(int64_t argc, ...);

/// Print object arguments (via toString) to stderr, followed by a newline.
void Paykan_err(int64_t argc, ...);

#ifdef __cplusplus
}
#endif

#endif // PAYKAN_RUNTIME_H

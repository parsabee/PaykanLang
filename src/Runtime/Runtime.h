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
  void          (*destroy) (PaykanObject *self);
  PaykanString *(*toString)(PaykanObject *self);
  int64_t       (*equals)  (PaykanObject *self, PaykanObject *other);
} PaykanObjectVTable;

struct PaykanObject {
  PaykanObjectVTable *vtable;
};

// Constructor.
PaykanObject *PaykanObject_new(void);

// Default method implementations.
void          PaykanObject_destroy(PaykanObject *self);
PaykanString *PaykanObject_toString(PaykanObject *self);
int64_t       PaykanObject_equals(PaykanObject *self, PaykanObject *other);

// Global vtable instance.
extern PaykanObjectVTable PaykanObject_vtable;

// Singleton None instance — an Obj whose toString returns "None".
extern PaykanObject PaykanObject_None;

// ============================================================================
// String
// ============================================================================
//
// Inherits Object.  The first two vtable slots match Object's layout;
// additional slots follow for String-specific methods.

typedef struct PaykanStringVTable {
  // Inherited (Object-compatible prefix)
  void          (*destroy) (PaykanObject *self);
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

// Method implementations.
void          PaykanString_destroy(PaykanObject *self);
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

/// Print object arguments (via toString) to stdout, followed by a newline.
void Paykan_out(int64_t argc, ...);

/// Print object arguments (via toString) to stderr, followed by a newline.
void Paykan_err(int64_t argc, ...);

#ifdef __cplusplus
}
#endif

#endif // PAYKAN_RUNTIME_H

// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — strong and weak references (Runtime.h, "Shared").
//
// An object carries its reference counts in its header, so a reference is
// the object's pointer: boxing is a strong retain, unboxing is the identity.

#include "RuntimeInternal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// -- The `strong` word
//
// Above zero it is the strong count of a live object.  At and below zero the
// object is dead and the word records how far its destruction has got:
//
//   kDying    its destroy is running (destroy_dead_object);
//   kPending  destruction is pending (deferred, below): the object is on the
//             pending stack;
//   0         destroyed: the memory is freed as soon as `weak` is zero, and
//             stays a zombie, read as NULL by every weak reference, until
//             the last weak release frees it.
//
// A weak load therefore gives an object only while `strong` is above zero:
// never one that is dying, pending or destroyed.  Both counts are 32 bits
// (Runtime.h, PaykanObject); one more reference than PAYKAN_REF_MAX panics
// instead of wrapping to a count that would free a live object.

enum { kDying = -1, kPending = -2 };

#if defined(__GNUC__) || defined(__clang__)
#define PAYKAN_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define PAYKAN_UNLIKELY(x) (x)
#endif

PAYKAN_NORETURN static void count_overflow(const char *kind) {
  Paykan_runtime_panic("too many %s references to one object (the limit is "
                       "%d)",
                       kind, (int)PAYKAN_REF_MAX);
}

// -- Strong references

PaykanShared *PaykanShared_new(PaykanObject *obj) {
  if (obj) {
    if (PAYKAN_UNLIKELY(obj->strong == PAYKAN_REF_MAX))
      count_overflow("strong");
    ++obj->strong;
  }
  return obj;
}

void Paykan_retain(PaykanShared *shared) {
  if (shared) {
    if (PAYKAN_UNLIKELY(shared->strong == PAYKAN_REF_MAX))
      count_overflow("strong");
    ++shared->strong;
  }
}

PaykanObject *PaykanShared_get(PaykanShared *shared) { return shared; }

// -- Deferred destruction
//
// Destroying an object releases the references it holds, and a release that
// drops a count to zero destroys that object in turn.  Done recursively, every
// link of a chain (a `Node?` list, a deep tree) costs at least one C stack
// frame, so dropping a long enough chain overflows the stack (#118).
// Destruction is therefore iterative:
//
//   * The release that drops a count to zero while no destroy is running is
//     the OUTERMOST release.  It destroys its object, then keeps destroying
//     pending objects until none are left, and only then returns.
//   * A release that drops a count to zero while a destroy is running (it is
//     called from inside a destroy) does not destroy anything: it pushes the
//     dead object on the pending stack and returns at once.
//   * When a destroy returns, the objects it pushed are reversed in place, so
//     that the first one it released is on top; the next object destroyed is
//     the one on top.
//
// The C stack depth of a release is therefore bounded (outermost release ->
// destroy -> nested release) whatever the shape of the object graph.  The
// order is deterministic and is the order the recursive scheme started its
// destroys in: a depth-first pre-order walk of the objects that die, each
// object's references taken in the order its destroy releases them (field
// order for a class, index order for a buffer).  The difference
// is only that a destroy now finishes before the objects it released are
// destroyed.  Depth-first order also keeps the walk cache-friendly: the
// memory freed last is reused first, as with the recursive scheme.  Every
// object is destroyed and freed before the outermost release returns, so
// destruction stays prompt.
//
// The stack holds kInlinePending objects without allocating; past that it
// grows on the C heap (never Paykan_malloc, so --track-heap counts are
// unaffected) and keeps its storage for the next drain.  A dead object's
// header has no room for the link (its counts are 32 bits, a pointer 64),
// which is why the stack is not threaded through the objects.
//
// Like the reference counts themselves this state is not thread-safe: the
// runtime assumes a single thread of execution (08-memory-model.md).

enum { kInlinePending = 256 };

static int g_destroying; // nonzero while a release is draining
static PaykanObject *g_inline_pending[kInlinePending];
static PaykanObject **g_pending = g_inline_pending; // the pending stack ...
static size_t g_pending_len;                        // ... its height ...
static size_t g_pending_cap = kInlinePending;       // ... and its room

static void push_pending(PaykanObject *obj) {
  if (PAYKAN_UNLIKELY(g_pending_len == g_pending_cap)) {
    size_t cap = g_pending_cap * 2;
    PaykanObject **grown =
        g_pending == g_inline_pending
            ? (PaykanObject **)malloc(cap * sizeof *grown)
            : (PaykanObject **)realloc(g_pending, cap * sizeof *grown);
    if (!grown)
      Paykan_runtime_panic("out of memory destroying %zu objects",
                           g_pending_len + 1);
    if (g_pending == g_inline_pending)
      memcpy(grown, g_inline_pending, sizeof g_inline_pending);
    g_pending = grown;
    g_pending_cap = cap;
  }
  obj->strong = kPending;
  g_pending[g_pending_len++] = obj;
}

// Run a dead object's destroy, free the object (or leave it a zombie for its
// weak references), then reverse what that destroy pushed, so that the first
// object it released is destroyed next.
static void destroy_dead_object(PaykanObject *obj) {
  const size_t mark = g_pending_len;
  obj->strong = kDying;
  Paykan_vcall_destroy(obj);
  obj->strong = 0;
  if (obj->weak == 0)
    Paykan_free(obj);
  for (size_t i = mark, j = g_pending_len; i + 1 < j; ++i, --j) {
    PaykanObject *t = g_pending[i];
    g_pending[i] = g_pending[j - 1];
    g_pending[j - 1] = t;
  }
}

void Paykan_release(PaykanShared *shared) {
  // Keep the common case (the count stays above zero) as the fall-through.
  // With an early `if (!shared || --strong > 0) return;` GCC lays that path
  // out as a taken jump to a distant `ret`, which measurably slows
  // retain/release-heavy programs; this shape gives `test; jle slow; pop; ret`.
  if (shared && --shared->strong <= 0) {
    if (g_destroying) {
      // A destroy is running: defer to the outermost release.
      push_pending(shared);
      return;
    }
    g_destroying = 1;
    destroy_dead_object(shared);
    while (g_pending_len)
      destroy_dead_object(g_pending[--g_pending_len]);
    g_destroying = 0;
  }
}

void Paykan_dispose(PaykanObject *obj) {
  Paykan_release(PaykanShared_new(obj));
}

// -- Weak references

PaykanWeak *Paykan_weak_new(PaykanShared *shared) {
  if (shared) {
    if (PAYKAN_UNLIKELY(shared->weak == PAYKAN_REF_MAX))
      count_overflow("weak");
    ++shared->weak;
  }
  return shared;
}

PaykanShared *Paykan_weak_load(PaykanWeak *weak) {
  if (weak && weak->strong > 0) {
    if (PAYKAN_UNLIKELY(weak->strong == PAYKAN_REF_MAX))
      count_overflow("strong");
    ++weak->strong;
    return weak;
  }
  return (PaykanShared *)0;
}

void Paykan_weak_release(PaykanWeak *weak) {
  if (weak && --weak->weak == 0 && weak->strong == 0)
    Paykan_free(weak);
}

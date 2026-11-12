// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Paykan runtime — strong and weak references (Runtime.h, "Shared").
//
// An object carries its reference counts in its header, so a reference is
// the object's pointer: boxing is a strong retain, unboxing is the identity.

#include "RuntimeInternal.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// -- The `strong` word
//
// Above zero it is the strong count of a live object.  At and below zero the
// object is dead and the word records how far its destruction has got:
//
//   kDying    its destroy is running (destroy_dead_object);
//   kDeinit   its `__deinit__`s are running (Paykan_deinit_begin), the first
//             part of its destroy: kDeinit + n while the code they run holds
//             n references to it of its own, which it gives back;
//   kPending  destruction is pending (deferred, below): the object is on the
//             pending stack;
//   0         destroyed: the memory is freed as soon as `weak` is zero, and
//             stays a zombie, read as NULL by every weak reference, until
//             the last weak release frees it.
//
// A weak load therefore gives an object only while `strong` is above zero:
// never one that is dying, pending or destroyed.  Both counts are 32 bits
// (Runtime.h, PaykanObject); one more reference than the limit panics
// instead of wrapping to a count that would free a live object.  The limit
// is PAYKAN_REF_ATOMIC - 1 for a plain strong count (below), and
// PAYKAN_REF_MAX for a weak count.

enum { kDying = -1, kPending = -2, kDeinit = INT32_MIN / 2 };

PAYKAN_NORETURN static void count_overflow(const char *kind, int limit) {
  Paykan_runtime_panic("too many %s references to one object (the limit is "
                       "%d)",
                       kind, limit);
}

// -- Strong references

// A count below PAYKAN_REF_ATOMIC is one thread's, plain.  A cell's, or that
// of an object in an `atomic shared` value, is PAYKAN_REF_ATOMIC + n, on any
// thread (docs/design/concurrency.md, "Atomic counts"); an immortal object's
// is never written.

#define ATOMIC_COUNT(obj) ((_Atomic int32_t *)&(obj)->strong)

static inline void strong_retain(PaykanObject *obj) {
  const int32_t s = obj->strong;
  if (s < PAYKAN_REF_ATOMIC - 1) {
    ++obj->strong;
    return;
  }
  if (PAYKAN_UNLIKELY(s == PAYKAN_REF_ATOMIC - 1))
    count_overflow("strong", PAYKAN_REF_ATOMIC - 1);
  if (s == PAYKAN_REF_MAX)
    return;
  if (PAYKAN_UNLIKELY(atomic_fetch_add_explicit(ATOMIC_COUNT(obj), 1,
                                                memory_order_relaxed) >=
                      PAYKAN_REF_MAX - 1))
    count_overflow("strong", PAYKAN_REF_MAX - 1 - PAYKAN_REF_ATOMIC);
}

PaykanShared *PaykanShared_new(PaykanObject *obj) {
  if (obj)
    strong_retain(obj);
  return obj;
}

void Paykan_retain(PaykanShared *shared) {
  if (shared)
    strong_retain(shared);
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
// The state is each thread's own (the concurrency prototype runs tasks on
// several threads): a NULL `pending` means the inline array, since a
// thread-local's address is no constant initializer.

enum { kInlinePending = 256 };

static _Thread_local int g_destroying; // nonzero while a release is draining
static _Thread_local PaykanObject *g_inline_pending[kInlinePending];
static _Thread_local PaykanObject **g_pending; // the pending stack ...
static _Thread_local size_t g_pending_len;     // ... its height ...
static _Thread_local size_t g_pending_cap = kInlinePending; // ... its room
// The height below which the pending objects are an enclosing drain's: one
// that is running a `__deinit__` (Paykan_deinit_begin), whose code's own
// releases drain what they push and no more.
static _Thread_local size_t g_pending_base;

static void push_pending(PaykanObject *obj) {
  if (!g_pending)
    g_pending = g_inline_pending;
  if (PAYKAN_UNLIKELY(g_pending_len == g_pending_cap)) {
    size_t cap = g_pending_cap * 2;
    PaykanObject **grown =
        g_pending == g_inline_pending
            ? (PaykanObject **)malloc(cap * sizeof(PaykanObject *))
            : (PaykanObject **)realloc(g_pending, cap * sizeof(PaykanObject *));
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
  // Keep the common case (the count stays above zero) short: an early
  // `if (!shared || --strong > 0) return;` made GCC lay that path out as a
  // taken jump to a distant `ret`, which measurably slowed retain/release-
  // heavy programs; the decrement and its test stand on their own lines for
  // clang-tidy (bugprone-inc-dec-in-conditions), the shape is the same.
  if (shared && shared->strong >= PAYKAN_REF_ATOMIC) {
    if (shared->strong == PAYKAN_REF_MAX ||
        atomic_fetch_sub_explicit(ATOMIC_COUNT(shared), 1,
                                  memory_order_acq_rel) !=
            PAYKAN_REF_ATOMIC + 1)
      return;
    // The last reference, after every reader's last read: this thread has
    // it alone, and destroys it as any object.
    shared->strong = 1;
  }
  if (shared) {
    shared->strong -= 1;
    if (shared->strong > 0)
      return;
    // Its `__deinit__` is running (kDeinit + n): a reference that code took
    // of the object, given back.
    if (shared->strong < kPending)
      return;
    if (g_destroying) {
      // A destroy is running: defer to the outermost release.
      push_pending(shared);
      return;
    }
    g_destroying = 1;
    destroy_dead_object(shared);
    while (g_pending_len > g_pending_base && g_pending)
      destroy_dead_object(g_pending[--g_pending_len]);
    g_destroying = 0;
  }
}

// -- `__deinit__` (#378)
//
// A class's generated destroy runs the `__deinit__`s of the object's class
// and its superclasses between these two, before it releases the fields.
// That is the program's code, so for its duration:
//
//   * the object can be referred to: a `view` argument, `==`, its text.
//     Its count is kDeinit + n, which a retain and a release move as they
//     move a live count, and which no weak reference upgrades (it is below
//     zero).  The code gives every reference back, as the borrow of `self`
//     has it; one that did not would be a reference to freed memory, so
//     that is a panic;
//   * no destroy is running, as far as that code's own releases go: an
//     object it drops is destroyed where it is dropped, not when the
//     object being destroyed is done.  What an enclosing drain has pending
//     stays below g_pending_base, for that drain.
//
// The token is the state to put back.

int64_t Paykan_deinit_begin(PaykanObject *obj) {
  const int64_t token =
      (int64_t)(((uint64_t)g_pending_base << 1) | (g_destroying ? 1u : 0u));
  obj->strong = kDeinit;
  g_pending_base = g_pending_len;
  g_destroying = 0;
  return token;
}

void Paykan_deinit_end(PaykanObject *obj, int64_t token) {
  if (PAYKAN_UNLIKELY(obj->strong != kDeinit))
    Paykan_runtime_panic("an object was kept by its '__deinit__': %d "
                         "reference(s) to it remain",
                         (int)(obj->strong - kDeinit));
  obj->strong = kDying;
  g_destroying = (int)(token & 1);
  g_pending_base = (size_t)((uint64_t)token >> 1);
}

void Paykan_dispose(PaykanObject *obj) {
  Paykan_release(PaykanShared_new(obj));
}

// -- Weak references

PaykanWeak *Paykan_weak_new(PaykanShared *shared) {
  if (shared) {
    if (PAYKAN_UNLIKELY(shared->weak == PAYKAN_REF_MAX))
      count_overflow("weak", PAYKAN_REF_MAX);
    ++shared->weak;
  }
  return shared;
}

PaykanShared *Paykan_weak_load(PaykanWeak *weak) {
  if (weak && weak->strong > 0) {
    strong_retain(weak);
    return weak;
  }
  return (PaykanShared *)0;
}

void Paykan_weak_release(PaykanWeak *weak) {
  if (weak && --weak->weak == 0 && weak->strong == 0)
    Paykan_free(weak);
}

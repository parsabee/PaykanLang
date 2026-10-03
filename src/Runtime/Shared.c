// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — Shared (reference-counted) wrapper implementation.

#include "RuntimeInternal.h"

#include <stdint.h>
#include <stdlib.h>

// -- Constructor / acquire ---------------------------------------------------
//
// PaykanShared_new enforces the unique-box invariant (see Runtime.h): an
// object that already carries a box gets that SAME box back, retained — never
// a second, independently-counted box that would double-free the object.

PaykanShared *PaykanShared_new(PaykanObject *obj) {
  if (obj && obj->shared) {
    // Acquire: the object is already boxed — hand out another +1 reference to
    // its unique box.
    ++obj->shared->refCount;
    return obj->shared;
  }
  PaykanShared *s = (PaykanShared *)Paykan_malloc(sizeof(PaykanShared));
  s->refCount = 1;
  s->object = obj;
  if (obj)
    obj->shared = s; // install the unique-box backpointer
  return s;
}

// -- Reference counting ------------------------------------------------------

void Paykan_retain(PaykanShared *shared) {
  if (shared)
    ++shared->refCount;
}

// -- Deferred destruction ----------------------------------------------------
//
// Destroying an object releases the references it holds, and a release that
// drops a count to zero destroys that object in turn.  Done recursively, every
// link of a chain (a `Node?` list, a nested array or tuple, a deep tree) costs
// at least one C stack frame, so dropping a long enough chain overflows the
// stack (#118).  Destruction is therefore iterative:
//
//   * The release that drops a count to zero while no destroy is running is
//     the OUTERMOST release.  It destroys its object, then keeps destroying
//     pending objects until none are left, and only then returns.
//   * A release that drops a count to zero while a destroy is running (it is
//     called from inside a destroy) does not destroy anything: it appends the
//     dead box to the running destroy's BATCH and returns at once.
//   * When a destroy returns, its batch is moved, in order, to the FRONT of
//     the pending list; the next object destroyed is the pending list's head.
//
// The C stack depth of a release is therefore bounded (outermost release ->
// destroy -> nested release) whatever the shape of the object graph.  The
// order is deterministic and is the order the recursive scheme started its
// destroys in: a depth-first pre-order walk of the objects that die, each
// object's references taken in the order its destroy releases them (field
// order for a class, index order for an array or a tuple).  The difference
// is only that a destroy now finishes before the objects it released are
// destroyed.  Depth-first order also keeps the walk cache-friendly: the
// memory freed last is reused first, as with the recursive scheme.  Every
// object is destroyed and freed before the outermost release returns, so
// destruction stays prompt.
//
// The lists are intrusive and allocate nothing (so --track-heap counts are
// unaffected and a release cannot fail): they are threaded through the dead
// boxes themselves.  A box at count zero is unreachable, so its refCount word
// is free to hold the link to the next box until the box is freed.
//
// Like the reference counts themselves this state is not thread-safe: the
// runtime assumes a single thread of execution (08-memory-model.md).

_Static_assert(sizeof(intptr_t) <= sizeof(int64_t),
               "a dead box's link is stored in its refCount word");

static int g_destroying;           // nonzero while a release is draining
static PaykanShared *g_pending;    // next dead box to destroy, or NULL
static PaykanShared *g_batch_head; // released by the running destroy ...
static PaykanShared *g_batch_tail; // ... in release order, or NULL

static void dead_box_set_next(PaykanShared *box, PaykanShared *next) {
  box->refCount = (int64_t)(intptr_t)next;
}

// A pointer converted to intptr_t and back compares equal to the original
// (C11 7.20.1.4), and int64_t holds every intptr_t value.
static PaykanShared *dead_box_next(const PaykanShared *box) {
  // NOLINTNEXTLINE(performance-no-int-to-ptr): the link is stored as integer
  return (PaykanShared *)(intptr_t)box->refCount;
}

// Destroy a dead box's object, free the box, then move everything that
// destroy released to the front of the pending list (keeping its order).
static void destroy_dead_box(PaykanShared *box) {
  Paykan_vcall_destroy(box->object);
  Paykan_free(box);
  if (g_batch_head) {
    dead_box_set_next(g_batch_tail, g_pending);
    g_pending = g_batch_head;
    g_batch_head = g_batch_tail = (PaykanShared *)0;
  }
}

void Paykan_release(PaykanShared *shared) {
  if (!shared || --shared->refCount > 0)
    return;
  PaykanObject *obj = shared->object;
  if (!obj) {
    Paykan_free(shared);
    return;
  }
  // Break the object→box backpointer now, before destroy (or before the box
  // is deferred).  For ordinary objects the memory is about to be freed
  // anyway; for immortal statics (None, Stdin) whose destroy is a no-op this
  // returns them to the unboxed state instead of leaving a pointer to a dead
  // box — so they can be boxed afresh even while the dead box is pending.
  if (obj->shared == shared)
    obj->shared = (PaykanShared *)0;
  if (g_destroying) {
    // A destroy is running: defer to the outermost release.
    dead_box_set_next(shared, (PaykanShared *)0);
    if (g_batch_tail)
      dead_box_set_next(g_batch_tail, shared);
    else
      g_batch_head = shared;
    g_batch_tail = shared;
    return;
  }
  g_destroying = 1;
  destroy_dead_box(shared);
  while (g_pending) {
    PaykanShared *box = g_pending;
    g_pending = dead_box_next(box);
    destroy_dead_box(box);
  }
  g_destroying = 0;
}

// -- Accessor ----------------------------------------------------------------

PaykanObject *PaykanShared_get(PaykanShared *shared) {
  return shared ? shared->object : (PaykanObject *)0;
}

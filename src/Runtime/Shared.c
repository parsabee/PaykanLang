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

// -- The `strong` word
//
// Above zero it is the strong count of a live object.  At and below zero the
// object is dead and the word records how far its destruction has got:
//
//   kDying    its destroy is running (destroy_dead_object);
//   <= kDead  destruction is pending (deferred, below): the word holds the
//             link to the next pending object, as kDead - the pointer;
//   0         destroyed: the memory is freed as soon as `weak` is zero, and
//             stays a zombie, read as NULL by every weak reference, until
//             the last weak release frees it.
//
// A weak load therefore gives an object only while `strong` is above zero:
// never one that is dying, pending or destroyed.

enum { kDying = -1, kDead = -2 };

// -- Strong references

PaykanShared *PaykanShared_new(PaykanObject *obj) {
  if (obj)
    ++obj->strong;
  return obj;
}

void Paykan_retain(PaykanShared *shared) {
  if (shared)
    ++shared->strong;
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
//     called from inside a destroy) does not destroy anything: it appends the
//     dead object to the running destroy's BATCH and returns at once.
//   * When a destroy returns, its batch is moved, in order, to the FRONT of
//     the pending list; the next object destroyed is the pending list's head.
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
// The lists are intrusive and allocate nothing (so --track-heap counts are
// unaffected and a release cannot fail): they are threaded through the dead
// objects themselves.  A dead object's `strong` word is free to hold the
// link to the next one until its destroy runs, encoded below zero so that a
// weak load of a pending object sees it dead.
//
// Like the reference counts themselves this state is not thread-safe: the
// runtime assumes a single thread of execution (08-memory-model.md).

_Static_assert(sizeof(intptr_t) <= sizeof(int64_t),
               "a dead object's link is stored in its strong word");

static int g_destroying;           // nonzero while a release is draining
static PaykanObject *g_pending;    // next dead object to destroy, or NULL
static PaykanObject *g_batch_head; // released by the running destroy ...
static PaykanObject *g_batch_tail; // ... in release order, or NULL

// The link is kDead - the pointer's value, so a NULL link is kDead and any
// other link is below it.  A pointer converted to intptr_t and back compares
// equal to the original (C11 7.20.1.4), int64_t holds every intptr_t value,
// and a user-space address is never negative as an intptr_t on the 64-bit
// platforms the runtime supports, so the encoded word stays below zero.
static void dead_set_next(PaykanObject *obj, PaykanObject *next) {
  obj->strong = kDead - (int64_t)(intptr_t)next;
}

static PaykanObject *dead_next(const PaykanObject *obj) {
  // NOLINTNEXTLINE(performance-no-int-to-ptr): the link is stored as integer
  return (PaykanObject *)(intptr_t)(kDead - obj->strong);
}

// Run a dead object's destroy, free the object (or leave it a zombie for its
// weak references), then move everything that destroy released to the front
// of the pending list (keeping its order).
static void destroy_dead_object(PaykanObject *obj) {
  obj->strong = kDying;
  Paykan_vcall_destroy(obj);
  obj->strong = 0;
  if (obj->weak == 0)
    Paykan_free(obj);
  if (g_batch_head) {
    dead_set_next(g_batch_tail, g_pending);
    g_pending = g_batch_head;
    g_batch_head = g_batch_tail = (PaykanObject *)0;
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
      dead_set_next(shared, (PaykanObject *)0);
      if (g_batch_tail)
        dead_set_next(g_batch_tail, shared);
      else
        g_batch_head = shared;
      g_batch_tail = shared;
      return;
    }
    g_destroying = 1;
    destroy_dead_object(shared);
    while (g_pending) {
      PaykanObject *obj = g_pending;
      g_pending = dead_next(obj);
      destroy_dead_object(obj);
    }
    g_destroying = 0;
  }
}

void Paykan_dispose(PaykanObject *obj) {
  Paykan_release(PaykanShared_new(obj));
}

// -- Weak references

PaykanWeak *Paykan_weak_new(PaykanShared *shared) {
  if (shared)
    ++shared->weak;
  return shared;
}

PaykanShared *Paykan_weak_load(PaykanWeak *weak) {
  if (weak && weak->strong > 0) {
    ++weak->strong;
    return weak;
  }
  return (PaykanShared *)0;
}

void Paykan_weak_release(PaykanWeak *weak) {
  if (weak && --weak->weak == 0 && weak->strong == 0)
    Paykan_free(weak);
}

// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The concurrency prototype's `locked T` cell (docs/design/concurrency.md,
// "locked and lock blocks"): one heap object holding an object's box behind
// a mutex, which a `lock` block takes for its length.  Counted atomically,
// as the atomic cell is (Atomic.c); its contents are plain, under the mutex.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <pthread.h>

typedef struct PaykanLocked {
  PaykanMethod *vtable; // points to PaykanLocked_vtable
  int32_t strong;       // object header (see PaykanObject)
  int32_t weak;
  pthread_mutex_t mutex;
  PaykanShared *payload; // the contents, owned by the cell
} PaykanLocked;

static void PaykanLocked_destroy(PaykanObject *self) {
  PaykanLocked *c = (PaykanLocked *)self;
  Paykan_release(c->payload);
  pthread_mutex_destroy(&c->mutex);
}

static PaykanShared *PaykanLocked_toString(PaykanObject *self) {
  (void)self;
  return PaykanShared_new((PaykanObject *)PaykanString_new("locked", 6));
}

/// Two handles are equal when they name the same cell.
static int64_t PaykanLocked_equals(PaykanObject *self, PaykanShared *other,
                                   uint64_t owned) {
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && self == o;
  return Paykan_equals_drop_other(other, owned, result);
}

PaykanMethod PaykanLocked_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanLocked_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanLocked_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanLocked_equals,
    [PAYKAN_SLOT_WALK] = (PaykanMethod)PaykanObject_walk,
};

PaykanShared *PaykanLocked_new(PaykanShared *payload) {
  PaykanLocked *c = (PaykanLocked *)Paykan_malloc(sizeof(PaykanLocked));
  c->vtable = PaykanLocked_vtable;
  c->strong = PAYKAN_REF_ATOMIC + 1;
  c->weak = 0;
  if (pthread_mutex_init(&c->mutex, NULL) != 0)
    Paykan_runtime_panic("cannot create a lock");
  c->payload = payload;
  return (PaykanShared *)c;
}

// The cells whose mutex this thread holds, innermost last.  A `lock` block
// takes no other lock, and the compiler checks that of what the block calls
// (Sema's par-safe pass).  What it does not follow may still take one: a
// `__deinit__`, run where the block lets go of an object.  Of another cell,
// the lock is taken as any is.  Of a cell the thread holds already, it
// would wait for ever for itself: that is a panic, where the program
// stopped answering.  (The cells past the first kHeldCells are held without
// being remembered: a block eight deep in other blocks' destructors.)
enum { kHeldCells = 8 };
static _Thread_local PaykanLocked *g_held[kHeldCells];
static _Thread_local int g_held_count;

void *PaykanLocked_lock(PaykanObject *cell) {
  PaykanLocked *c = (PaykanLocked *)cell;
  for (int i = 0; i < g_held_count && i < kHeldCells; ++i)
    if (g_held[i] == c)
      Paykan_runtime_panic("lock taken twice: code that a 'lock' block ran "
                           "(a '__deinit__' of what the block let go of) "
                           "locks the cell the block holds, and would wait "
                           "for ever");
  if (pthread_mutex_lock(&c->mutex) != 0)
    Paykan_runtime_panic("cannot take a lock");
  if (g_held_count < kHeldCells)
    g_held[g_held_count] = c;
  ++g_held_count;
  return &c->payload;
}

void PaykanLocked_unlock(PaykanObject *cell) {
  PaykanLocked *c = (PaykanLocked *)cell;
  // (Blocks end innermost first.)
  --g_held_count;
  if (pthread_mutex_unlock(&c->mutex) != 0)
    Paykan_runtime_panic("cannot release a lock");
}

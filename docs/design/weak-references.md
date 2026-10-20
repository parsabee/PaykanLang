# Weak references and one allocation per object

The design behind the `weak` declarations of `docs/language/08-memory-model.md`
("Weak references") and the object layout that carries them: what a weak
reference is, where one may be declared, how it is read and written, and how
the runtime keeps an object's counts in the object itself.  The decisions fit
the language as it is (objects behind reference counts, destruction
deterministic and iterative, `view` / `inout` borrows checked at compile
time).  The PIR format, the runtime ABI and the `.pkm` format change shape but
not version: their versions stay fixed until 0.2.0 (`docs/design/pkm.md`),
and a compiler with this change does not trust the caches of an older one
([clear them](../internals.md#the-compilation-caches)).  The open questions at the end have the default this
implementation takes.

## Summary of the decisions

| Topic | Decision |
|-------|----------|
| Syntax | `weak y: Point? = x;` declares a local, `weak parent: Node?;` a class field; `weak u = x;` infers `Point?` from `x: Point` or `x: Point?`; `weak e: Point?;` starts as None |
| Type | `T?` of a class `T` (a user class, `Str`, `Obj`), always optional: a weak reference reads as None once its object is destroyed.  Not a plain class, a value, a struct, an enum or a function type |
| Reading | a `weak` variable or field reads as a `T?`: a strong reference to the object while it is alive (one the expression owns for its own duration), None otherwise; it goes everywhere a `T?` goes (`match`, `== None`, an argument, a result, a strong variable or field) |
| Writing | assigning a `T` or a `T?` to a `weak` variable or field stores a weak reference to its object (none for None) and drops the one it held; the object's strong count is untouched |
| Lifetime | a weak reference never keeps its object alive; the object is destroyed with its last strong reference, and from then on every weak reference to it reads as None |
| Places | a weak reference is not a place a strong reference could be written to: no `inout` parameter, local or `match` arm names a `weak` variable or field (`view` is fine: it reads a copy) |
| Lambdas | a lambda captures a `weak` variable weakly: the closure holds a weak reference, and a call reads it as the variable reads (held strongly for the call's duration) |
| Not yet | `weak` on a struct field, a tuple or array element, a parameter or a result; `unowned` (an unchecked weak reference); a weak reference to a function value |
| Layout | every object is one allocation: `{ vtable, strong count, weak count, fields… }`.  A strong reference and a weak reference are the object's pointer; "boxing" a raw pointer is a strong retain, and unboxing is the identity |
| Destroy | the `destroy` slot releases what the object holds and tears down its non-object resources; it does not free the object.  The runtime frees the memory right after the destroy, or, while weak references remain, when the last of them is released |

## Syntax

```
statement   ::= ... | weakDecl
weakDecl    ::= "weak" IDENT ( ":" typeAnnotation )? ( "=" expression )? ";"
classMember ::= "weak"? varDecl ";" | funcDecl
```

`weak` is a keyword.  A local needs a type or an initializer (both are
allowed); it cannot be a borrow (`weak y: view Point? = x` is an error) and
`let` and `weak` are not combined (`let` fixes a variable; a weak variable is
assigned when its object changes).  A struct body rejects `weak` (`a struct
field cannot be 'weak'`): a struct is a value, copied with its fields, and
the runtime's type descriptors, which copy and drop a struct's references,
describe strong references only.

In the AST interchange format a weak declaration is a `var` with a trailing
`(weak)`, after `(let)` or a `(qual ...)` where those would go (neither may
accompany it).

## Typing

Sema checks a `weak` declaration's type after resolving it (and after
inferring it from the initializer, wrapping a class `T` into `T?`): it must be
an `OptionalType` whose inner type is a `ClassType`.  The messages name what
is wrong:

* `'weak' variable 'w' must be optional: a weak reference reads as None once
  its object is destroyed; write 'Point?'` for a plain class;
* `'weak' variable 'n' has type 'int?'; a weak reference is an optional class
  type ('Node?'): only an object is referred to weakly` for anything else
  (a field says `'weak' field 'f' of class 'C'`).

Everything else about the variable is an optional's: assignment checks
`T`/`T?` against `T?`, `match` takes a `T` arm and a `None` arm, `==`
compares two optionals.  A weak local has the variable kind `Weak`, which
`inoutPlaceError` rejects (`'w' is a 'weak' reference and cannot be passed to
'inout' parameter 'p'`), and `ClassType` records its weak fields
(`isWeakField`, walking the ancestors) for the same check on `obj.field`
(`field 'peek' of class 'Holder' is a 'weak' reference and cannot be ...`),
for the lowering and for the module interface.

A lambda that captures a `weak` variable records the capture as weak
(`LambdaExpr::Capture::Weak`); inside the lambda the name is read-only, as
every capture is.

## Run-time representation

### The object header

```c
struct PaykanObject {
  PaykanMethod *vtable; // method dispatch and type identity
  int32_t strong;       // strong references: owned PaykanShared* pointers
  int32_t weak;         // weak references: PaykanWeak* pointers
};                      // 16 bytes
```

Each count is 32 bits: a retain, a weak load or a new weak reference that
would take a count past `PAYKAN_REF_MAX` (`INT32_MAX`) panics (`too many
strong references to one object`) instead of wrapping to a count that would
free a live object.

`PaykanShared` and `PaykanWeak` are both `PaykanObject`: a reference is the
object's address, and the two names say what a pointer holds (a strong
reference, counted in `strong`; a weak one, counted in `weak`; a raw
`PaykanObject*` alias, counted nowhere).  A new object has both counts at
zero until it is boxed; an immortal static (`PaykanObject_None`,
`PaykanFile_Stdin`) starts with `strong` 1, which nothing releases.

The separate control block is gone.  `PaykanShared_new(obj)` is `++strong`
and returns `obj`; `PaykanShared_get` is the identity; `Paykan_retain` and
`Paykan_release` work on the object.  Both backends lower `unbox` to the
identity and `box` to `PaykanShared_new`, keep `PaykanShared_get` in the
runtime for native code, and lay out a class as the 16-byte header followed
by its fields (the LLVM backend's struct type is `{ptr, i32, i32,
fields...}` and a field is at index 3 + i; the C backend's struct starts
`PaykanMethod *vtable; int32_t strong; int32_t weak;`).

### Destroy and free

A destroy (vtable slot 0) releases what the object holds and tears down its
non-object resources (a string's bytes, a file's handle, a buffer's values)
and returns; it never frees the object.  Generated destructors therefore no
longer end in `free`, `PaykanObject_destroy` does nothing, and
`PaykanString_destroy` frees the bytes only.  The runtime frees the object:

* `Paykan_release` at strong zero runs the destroy, then frees the object if
  `weak` is zero, else leaves it a **zombie**: destroyed, its memory kept
  for the weak references, read as NULL by every `Paykan_weak_load`;
* `Paykan_weak_release` frees a zombie when it drops `weak` to zero and
  `strong` is zero (destroyed).

An object made by generated code and dropped without ever being boxed (a
string temporary) is destroyed and freed by `Paykan_dispose(obj)`, which is
boxing it and releasing that reference; the lowering calls it where it
called `PaykanString_destroy` before.

### The `strong` word's states

```
> 0   alive: the strong count
-1    dying: its destroy is running
-2    dead, destruction pending: on the pending stack
0     destroyed: a zombie while weak > 0, freed otherwise
```

Deferred destruction (#118) keeps its pending objects on a stack of the
runtime's (`Shared.c`: 256 inline slots, then the C heap, kept for the next
drain) and marks each one `-2`, below zero so that a weak load of a pending
object (`strong > 0` is the liveness test) sees it dead, exactly as it sees
a dying or destroyed one: a weak reference never gives out an object whose
destroy has started, or is about to.  The pending list used to be threaded
through the dead objects' `strong` words, but a 32-bit word cannot hold a
pointer.  The marker `-1` during
the destroy also keeps a weak release inside the destroy (an object holding
a weak reference to itself) from freeing the object under the destroy's
feet: the free waits for `destroy_dead_object` to set `strong` to 0 and
check `weak`.

### Runtime functions

```c
PaykanWeak   *Paykan_weak_new(PaykanShared *s);   // weak + 1; NULL for NULL
PaykanShared *Paykan_weak_load(PaykanWeak *w);    // strong + 1 while alive, else NULL
void          Paykan_weak_release(PaykanWeak *w); // weak - 1; frees a zombie at 0
void          Paykan_dispose(PaykanObject *obj);  // destroy and free an unboxed object
```

## PIR

A new type and three ops, with the reserved codes #99 set aside: type `weak`
(code 16), `weak.load` (30), `weak.new` (31) and `weak.release` (49).

```
%w = weak.new %b       ; box -> weak    Paykan_weak_new (null -> null)
%b = weak.load %w      ; weak -> box    Paykan_weak_load: a +1 box, or null
weak.release %w        ; weak -> void   Paykan_weak_release (null-safe)
```

`weak` is the type of a `weak` local and of a `weak` field (a field type,
like `box`); it is never addressed (`local.addr` and `field.addr` reject
it, matching Sema's `inout` rule), cast or compared.  The verifier checks
the operand and result types.

The lowering's rules (docs/pir.md §7):

* a `weak` local is a `local` of type `weak`; its declaration makes the
  initializer a +1 box (`emitAsShared`), takes `weak.new` of it and releases
  the box; an assignment does the same and `weak.release`s the old value;
  scope exit `weak.release`s the local (in declaration order with the owned
  variables);
* a read of a `weak` variable (`load` then `weak.load`) is a fresh +1 box,
  classified like a call's result (`exprAlreadyShared` and
  `exprProducesFreshBox` are true for the identifier), so every consumer
  that owns a fresh box owns this one: an argument, a result, a `match`
  subject (pending release), a print (released after the call), a strong
  variable or field (stored as-is);
* a `weak` field is a field of type `weak`: a read is `field.load` then
  `weak.load` (a fresh +1 box, whatever the receiver), a store `weak.new` of
  the +1 value then `weak.release` of the old field, and `C.destroy`
  `weak.release`s it;
* a closure capturing a `weak` variable has a `weak` field for it, stored
  with `weak.new` from the variable's read and `weak.release`d by the
  closure's destroy; the call's prologue loads it (`weak.load`) into an
  owned local of the capture's name.

## Module interface

A class field record in the `.pkm` interface carries a flags byte after its
type name (bit 0: weak); a struct's field records carry none.  `pkm dump`
prints `weak field parent: Node?`.  The `.pkm` format version is unchanged
(pre-release): a `.pkm` written before this change is read as if its fields
had no flags byte and is rejected or misread, which the cache check and
[clearing the caches](../internals.md#the-compilation-caches) cover.

## Diagnostics

| Situation | Message |
|-----------|---------|
| `weak w: Point = p;` | `'weak' variable 'w' must be optional: a weak reference reads as None once its object is destroyed; write 'Point?'` |
| `weak n: int? = 1;` | `'weak' variable 'n' has type 'int?'; a weak reference is an optional class type ('Node?'): only an object is referred to weakly` |
| `f(w)` with `p: inout Point?` | `'w' is a 'weak' reference and cannot be passed to 'inout' parameter 'p'` |
| `f(h.peek)` with a weak field | `field 'peek' of class 'Holder' is a 'weak' reference and cannot be passed to 'inout' parameter 'p'` |
| `v: inout Point? = w;` | `'w' is a 'weak' reference and cannot be named by 'inout' local 'v'` |
| `match w { n: inout Point {} ... }` | `'w' is a 'weak' reference and cannot be bound by 'inout' arm 'n'` |
| `weak n;` | `a 'weak' declaration needs a type or an initial value: 'weak n: Point? = x;'` |
| `weak n: view Point? = x;` | `a 'weak' local is not a borrow: it holds a weak reference of its own, so it cannot be 'view'` |
| `struct S { weak p: P?; }` | `a struct field cannot be 'weak': a weak reference is a field of a class or a local (a struct is a value, copied with its fields)` |

## Costs

* An object is one allocation of a 16-byte header plus its fields, instead
  of a 16-byte header (the vtable and a pointer to the box) plus a separate
  16-byte box: fewer allocations and pointer chases, and 16 bytes less per
  object.
* A destroyed object with weak references outstanding keeps its whole
  allocation until the last weak reference goes (its fields are released,
  so what it held is freed).  This is the trade-off Swift's `weak` makes with
  an in-object header; a side table would cost every object a word or every
  weak load a lookup.  `--track-heap` counts such a zombie as a live block
  until it is freed; a program whose weak references all go out of scope
  ends with none.
* A weak load is a null check, a count check and an increment; a weak store
  is two count changes.  Reading a `weak` variable twice loads it twice: a
  program that reads one in a loop copies it into a strong variable first.

## Open questions

Each with the default taken here.

1. **Should a struct field be `weak`?**  Not yet: a struct's references are
   described by a runtime type descriptor (`PaykanType`) that copies and
   drops strong references; a weak field would need a kind of its own in
   the descriptor's reference words and in the tuple and struct layouts.
   Changes: `PaykanType_ref`, `PaykanType_copy`/`_drop`, the layouts of
   `PaykanTuple_*` and `PaykanStruct_*`, and the lowering of struct fields.
2. **`unowned`?**  Swift's unchecked weak reference (a trap on a dead
   object) is not offered: a `weak` read costs one check, and the language
   has no other trapping dereference.  Changes: a keyword, a variable kind
   and a load that panics.
3. **Weak references to function values?**  A closure is an object, so the
   runtime supports it; the language does not, since a function type is not
   a class type and `T?` of a function type is rarely a back link.  Changes:
   `checkWeakType` and the lowering's field type for a closure capture.
4. **Capture by reference for `weak` variables?**  A lambda copies the weak
   reference when it is made, as it copies every capture.  A later
   assignment to the variable does not reach the closure; this is the
   capture rule of `docs/design/function-types.md`.

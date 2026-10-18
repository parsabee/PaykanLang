# PaykanLang — Memory Model

PaykanLang manages object lifetimes with **automatic reference counting (ARC)**. There is no
garbage collector and no manual `free`. Every heap object carries a strong reference count; when
the last reference goes away, the object is destroyed immediately and deterministically. The
compiler inserts the counting operations for you — the model below is what those operations
guarantee, not something you write by hand.

---

## Quick Summary

- **Value types** (`int`, `float`, `bool`, `char`, the sized numeric types, `enum`) live on
  the stack, are copied on assignment, and are never reference-counted.
- **Arrays** (`T[n]`) and **tuples** (`(T1, T2)`) are values too: held inline in their
  variable, field or array element and copied whole on assignment. A reference element
  is a reference: copying the array or tuple retains it.
- **Reference types** (class instances, `Str`, `File`, `Error`, and their optionals) live
  on the heap and are reference-counted.
- Each heap object is owned through a **shared box** holding a strong count and a pointer to the
  object. Copying a reference **retains** (count `+1`); dropping one **releases** (count `-1`).
- When the count reaches zero the object's **`destroy`** runs — releasing the references it holds
  — and the memory is freed. Destruction is **deterministic**: it happens at the moment of the
  last release, not at some later collection.
- `destroy` is compiler-generated and **final** — it cannot be written, overridden, or called.
- Reference **cycles are not collected** and will leak (see *Reference Cycles* below).

---

## Value Types vs Reference Types

The category of a type decides how it is stored and whether ARC applies.

| Category | Types | Storage | On assignment / passing |
|----------|-------|---------|--------------------------|
| **Value** | `int`, `float`, `bool`, `char`, the sized numeric types (`u8`, `f32`, …, each in an 8-byte slot), `enum`, and their optionals `int?`, `float?`, `bool?`, `char?`, `u8?` | Stack / register (inline in a field) | Copied bit-for-bit; independent thereafter |
| **Array** | `T[n]` of any element type | Inline in the variable, the field or the enclosing array | Elements copied; reference elements retained |
| **Tuple** | `(T1, T2, ...)` of any element types | Inline in the variable, the field or the enclosing array or tuple | Elements copied; reference elements retained |
| **Struct** | `struct Name { ... }` of any field types | Inline, like a tuple of its fields | Fields copied; reference fields retained |
| **Function value** | `(T1, T2) -> R`: a lambda or a named function | Heap: a closure object holding the captured variables (copied when the lambda is made) | Reference shared; count adjusted; the captures are released with the closure |
| **Reference** | class instances, `Str`, `File`, `Error`, optionals `T?` of these | Heap | Reference shared; count adjusted |

A value type has no identity beyond its bits: assigning one `int` to another produces two
independent values. A reference type has identity: assigning one variable to another makes both
names refer to the **same** object, and a mutation through one name is visible through the other.

```pkn
a: int = 1;
b: int = a;      // independent copy
b = 2;           // a is still 1

p: Point = Point(1, 2);
q: Point = p;    // same object — both names share it
q.x = 99;        // p.x is now 99 too
```

An array is a value: `ys = xs` copies its elements, so a later `ys[0] = 99` leaves `xs[0]` as
it was. A tuple is one too: `u = t` copies `t`'s elements, and `u` keeps them when `t` is
assigned a new tuple.

---

## The Shared Box

Every reference-counted object is reached through a small heap **box** that pairs a strong count
with the owned object:

```
box: { refCount, object }   // refCount starts at 1 when the box is created
```

Two operations maintain the count:

- **retain** — increments the count when a new reference to the object is created (an assignment,
  passing the object into a place that keeps it, storing it in a field or array).
- **release** — decrements the count when a reference goes away (a variable leaves scope, a field
  or slot is overwritten, a temporary is discarded).

The object is reached the same way regardless of its concrete class because every heap object
begins with a pointer to its **vtable** — the table of its methods, including `destroy`,
`toString`, and `equals` — followed by a pointer back to its box. A class instance's layout is
`{ vtable pointer, box pointer, fields… }`.

The box is **unique**: each object has exactly one box for its entire lifetime, recorded in its
header. Whenever a new owning reference to an object is needed — no matter which alias it is
reached through (`self`, a `match` binding, a field, an array element) — the existing box is
found through the backpointer and retained, rather than a second box being created. This is
what makes the reference count a single source of truth and rules out double-frees from
aliased references.

---

## Destruction

When a release drops the count to zero:

1. The object's `destroy` method runs. The compiler generates `destroy` to **release every
   reference the object holds** — its reference-typed fields, and the reference-typed elements
   of its array and tuple fields — so destruction cascades transitively.
2. The object's memory and its box are freed.

This is **deterministic**: an object is destroyed at the exact point its last reference is
released, in a well-defined order, with no background collector and no pause. A `File`, for
example, is closed as soon as the last reference to it goes away.

Destruction is **iterative**: deep chains are freed without stack growth. A linked list of a
million nodes, a deeply nested tree, or a long chain of objects linked through array or tuple
fields are destroyed in constant C stack space, so dropping them cannot overflow the stack. The
release that drops the last reference destroys its object; every object that `destroy` releases to zero is set aside
rather than destroyed on the spot, and the same release then destroys the set-aside objects one
by one until none remain. The order is fixed: a depth-first pre-order walk of the objects that
die, each object's references taken in field order (index order in an array or a tuple) — the
order in which a recursive destruction would start them — except that an object's `destroy`
finishes before the objects it released are destroyed. All of them are destroyed and freed before
that release returns.

`destroy` is **final**. A class cannot declare its own `destroy`, override it, or call
`obj.destroy()` explicitly — the compiler owns it entirely (see `04-classes.md`). There is no
user-defined finalizer; cleanup that must happen at end of life belongs in the fields the object
holds, which are released automatically.

---

## Assignment, Passing, and Returning

For reference types, the counting is balanced so an object stays alive exactly as long as some
reference names it:

- **Assignment** `x = y` retains the new object and releases whatever `x` referred to before, so
  reassigning a variable cannot leak the previous value.
- **Passing to a function** keeps the object alive for the duration of the call; the reference the
  caller holds is unaffected.
- **Returning** transfers ownership of the result to the caller.
- **Storing into a field or array slot** retains the stored object; overwriting a slot releases
  the value it previously held.

You do not write any of these retains and releases — they are emitted by the compiler. The
guarantee you can rely on is that an object is alive for as long as it is reachable through a live
reference, and is destroyed promptly once it is not.

---

## Ownership transfers

Assigning or passing a reference shares the object: `t = s` retains it, and `s` and `t` are
both live until their scopes end. There is no way to *consume* a variable by hand. Where a
variable is not read again after it is handed on, the hand-off can become a transfer — the
reference moves to the destination without a retain, and the source is not released at scope
exit. That is an optimization the compiler makes, never something a program has to spell out
(the last-use pass, [#186](https://github.com/parsabee/PaykanLang/issues/186)).

Earlier releases had a `mov` keyword for this. It was removed in v0.2.0: `mov` is still a
reserved word, and using it is an error:

```
error: 'mov' was removed in v0.2.0; ownership transfers are inferred
```

Drop the keyword: `t = mov s;` becomes `t = s;`. The program prints the same output and frees
the same objects. The variable `s` simply stays readable afterwards.

---

## Strings, Arrays and Tuples

`Str` is a heap, reference-counted class. String literals and the results of `+` concatenation
are reference-counted objects like any other.

An array `T[n]` is a value, not an object: it has no box and no count of its own, and lives
where its variable or field does, inline in the stack frame or in the object. How it treats its
**elements** depends on the element type:

- An array of a **value type** (`int[3]`, `float[3]`, `bool[3]`, an enum array) stores the
  scalar values directly; there is nothing to reference-count per element.
- An array of a **reference type** (`Str[3]`, `Point[3]`, …) stores references and **retains**
  each element it holds. Overwriting a slot releases the old element, and the array releases
  all its elements when its variable goes out of scope or the object holding it is destroyed.

Assigning an array, or passing it to a plain parameter, copies it: the copy retains each element's object, which the two
arrays then share. See `05-arrays.md`.

A tuple is a value in the same way: it has no box and no count, and its elements are laid out
in place, one after another like a C struct's fields, in its variable, its field or the array
element that holds it. A tuple of value types (`(int, float)`) allocates nothing; a reference
element (`Str`, a class, `Node?`) is its box, which the tuple **retains**. Each copy of a tuple
(an assignment, a plain parameter, a result, an array element) retains the references in it
once more; a variable's are released when it goes out of scope, a field's when its object is
destroyed, an array element's with its array. See `09-tuples.md`.

A struct is a value in exactly the same way: it is laid out like the tuple of its fields and
retains and releases the references it holds as a tuple does. Assigning a field in place
(`p.name = s`) retains the new reference and releases the one it replaces. A method's `self`
is the receiver's storage, so a method allocates nothing to be called. See `14-structs.md`.

---

## `None`

`None` is a singleton `Obj` representing the absence of a value. Because it is a shared global
rather than an allocation, retaining and releasing it is harmless and it is never destroyed.
`None` is what operations like `readln` yield at end of input, distinguished with `match`
(see `07-match-statements.md`).

### Optional types

A value of optional type `T?` of a reference type `T` (see `10-optionals.md`) is the **same
shared box** as a `T`; the
absent value is represented by *having no box at all* rather than by the `None` singleton. This
means an optional costs nothing extra: retaining, releasing, moving or destroying an absent
`T?` is a no-op, a `None` stored into an optional field or array slot releases the previous
value and allocates nothing, and `x == None` is a plain null test. A `T?` never becomes an `Obj`
(an optional is not an object); when one is printed (`println(maybe)`), the compiler prints the
`None` singleton for an absent value.

An optional primitive (`int?`, `float?`, `bool?`, `char?`) is a value instead (#96): the
primitive and a presence flag, copied like an `int` and never counted, inline in a variable, a
field, an array element or a tuple element. It is not an object, so it never becomes an
`Obj`. A `match` arm binding the `int` copies the
value out and owns nothing.

---

## Reference Cycles

Reference counting reclaims an object when its count reaches zero. If two or more objects refer to
each other so that their counts never fall to zero — a parent holding a child that holds the
parent back — the group becomes unreachable from the program yet keeps itself alive. **Such cycles
are not collected and will leak.** PaykanLang currently has no cycle collector and no weak
references. Until weak references land, avoid building reference cycles among long-lived objects,
or break them explicitly by overwriting one of the linking fields before the objects go out of
scope.

A closure is an object too, and its captures are its fields: a lambda that captures an object
and is then stored in that object (`h.cb = fn() -> int { return h.n; };`) forms a cycle like any
other, and leaks the same way unless the field is overwritten before `h` goes out of scope.

---

## Thread Safety

The reference counting in this release is **not atomic**: it assumes a single thread of execution.
That is sound for this release, which is single-threaded. Thread-safe ARC — counts that can be shared
safely across threads — is part of the language's stated direction toward safe concurrency and
heterogeneous execution, and is on the roadmap rather than in this release. Do not assume that
sharing a reference-counted object across threads is safe today.

---

## What You Can Rely On

- Objects are freed **promptly and deterministically** at last release — no GC pauses, no
  non-determinism.
- You never write `free`, and you cannot double-free or use-after-free a value through ordinary
  language constructs — ownership is tracked by the compiler.
- A reference keeps its object alive for exactly as long as it is reachable.
- `destroy` runs automatically and releases everything an object owns, transitively.

The two things the model does **not** do for you today are collecting reference cycles and making
reference counting safe across threads — both are known and on the roadmap.

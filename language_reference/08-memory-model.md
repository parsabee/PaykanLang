# PaykanLang — Memory Model

PaykanLang manages object lifetimes with **automatic reference counting (ARC)**. There is no
garbage collector and no manual `free`. Every heap object carries a strong reference count; when
the last reference goes away, the object is destroyed immediately and deterministically. The
compiler inserts the counting operations for you — the model below is what those operations
guarantee, not something you write by hand.

---

## Quick Summary

- **Value types** (`int`, `float`, `bool`, `char`, `enum`) live on the stack, are copied on
  assignment, and are never reference-counted.
- **Reference types** (class instances, `Str`, `File`, `Error`, arrays, tuples, boxed
  `Int`/`Float`) live on the heap and are reference-counted.
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
| **Value** | `int`, `float`, `bool`, `char`, `enum` | Stack / register | Copied bit-for-bit; independent thereafter |
| **Reference** | class instances, `Str`, arrays, tuples, `File`, `Error`, boxed `Int`/`Float` | Heap | Reference shared; count adjusted |

A value type has no identity beyond its bits: assigning one `int` to another produces two
independent values. A reference type has identity: assigning one variable to another makes both
names refer to the **same** object, and a mutation through one name is visible through the other.

```pkn
a: int = 1;
b: int = a;      // independent copy
b = 2;           // a is still 1

xs: int[] = [1, 2, 3];
ys: int[] = xs;  // same array — both names share it
ys[0] = 99;      // xs[0] is now 99 too
```

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
   reference the object holds** — its reference-typed fields, and for an array its reference-typed
   elements — so destruction cascades transitively.
2. The object's memory and its box are freed.

This is **deterministic**: an object is destroyed at the exact point its last reference is
released, in a well-defined order, with no background collector and no pause. A `File`, for
example, is closed as soon as the last reference to it goes away.

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

## Moving with `mov`

`mov <expr>` **transfers ownership** of a value instead of sharing it. It can be applied to a
local variable or to a temporary (any rvalue):

```pkn
s: Str = "Hello world";
t = mov s;      // t takes ownership of the object; s no longer exists here
println(t);
```

For a **reference type**, `mov` hands the source's existing reference to the destination **without
bumping the reference count**, and the source variable is *consumed*: it drops out of the scope, so
no release is emitted for it at scope exit. This is exactly one retain and one release cheaper than
an ordinary `t = s` copy, and it expresses intent — the object now lives in `t`, not `s`.

For a **value type** (`int`, `float`, …) `mov` simply forwards the value; there is no reference
count to manage.

Once a variable has been moved it may not be read again — doing so is a compile-time error:

```pkn
a: int = 3;
b = mov a;
c = a;          // error: use of moved variable 'a'
```

A moved variable can be brought back to life by **re-assigning** it, after which it is an
ordinary live variable again:

```pkn
s: Str = "first";
t = mov s;
s = "second";   // revives s
println(s);     // ok
```

Only whole local variables and temporaries may be moved. Moving a **member variable**
(`obj.field`) or an **array element** (`arr[i]`) is rejected, because that would leave a hole in an
aggregate whose lifetime `mov` cannot account for.

Inside a method, **`self` may not be moved**: `self` is a *borrowed* reference to the receiver —
the method does not own it, so there is no ownership to transfer. Ordinary **parameters** are
owned by the callee for the duration of the call and *may* be moved, exactly like locals.

### `mov` across branches

The moved-or-live state of a variable is tracked **per control-flow path**. Each branch of an
`if`/`else` — and each arm of a `match` — is checked against the state at entry to the construct,
so a `mov` in one branch never poisons a sibling branch that can only run instead of it:

```pkn
if (f) {
  consume(mov s);
} else {
  println(s);      // ok — this path did not move s
}
```

After the construct the compiler merges paths conservatively: a variable moved on **any** path
counts as moved, unless **every** path through the construct re-assigned it (for an `if` without
`else`, or a `match` without a wildcard arm, the implicit not-taken path counts as a path that
did not re-assign):

```pkn
if (f) { consume(mov s); }
println(s);        // error: s may have been moved

t = mov s;
if (f) { s = "a"; } else { s = "b"; }
println(s);        // ok — revived on both paths
```

The same per-path rule applies to the **ternary expression** `if c then A else B`. The condition
`c` always runs first, so a `mov` inside it is visible to both `A` and `B` and is definite
afterwards. `A` and `B` are sibling branches: each is checked against the state after the
condition, so a `mov` in one never affects the other — moving the *same* variable in both is fine.
After the expression the paths are merged exactly like an `if`/`else` statement: a variable moved
in either branch counts as moved.

```pkn
y = if f then mov s else s;   // ok — the else branch did not move s
println(y);
println(s);                   // error: s may have been moved

z = if f then mov s else mov s;
println(z);                   // ok — exactly one branch ran; s is moved either way
```

The short-circuit operators **`a && b`** and **`a || b`** evaluate `b` only when `a` does not
already decide the result, so `b` is a conditional branch that runs *after* `a`. A `mov` in `a` is
definite and is visible in `b`; a `mov` in `b` may or may not have happened, so it is
conservatively treated as moved after the expression (the "skip `b`" path is merged in, just like
an `if` without `else`):

```pkn
ok = check(mov s) && other(s);  // error: s was moved by the left operand
ok = f && check(mov s);         // ok on its own …
println(s);                     // … but: error, s may have been moved
```

Function-call arguments are evaluated **left to right**, so `f(mov s, s)` is an error: the second
argument reads `s` after the first consumed it.

### `mov` in loops

A loop body (and a loop condition) re-executes, so a variable **declared outside a loop** may not
be left moved when the loop repeats — on the next iteration it would be read after being
consumed. This is a compile-time error unless the body re-assigns the variable on every path
before the loop repeats:

```pkn
s: Str = "x";
while (i < 2) {
  consume(mov s);  // error: s is declared outside the loop and still moved
  i = i + 1;       //        when the loop repeats
}
```

Either re-assign it before the end of the body, or declare it inside the loop (a loop-local
variable is fresh on every iteration):

```pkn
while (i < 2) {
  s: Str = "x";
  consume(mov s);  // ok
  i = i + 1;
}
```

---

## Strings and Arrays

`Str` is a heap, reference-counted class. String literals and the results of `+` concatenation
are reference-counted objects like any other.

Arrays are reference-counted heap objects too, and how they treat their **elements** depends on
the element type:

- An array of a **value type** (`int[]`, `float[]`, `bool[]`, an enum array) stores the scalar
  values directly inline; there is nothing to reference-count per element.
- An array of a **reference type** (`Str[]`, `Point[]`, …) stores references and **retains** each
  element it holds. Overwriting a slot or `pop`-ing releases the old element, and destroying the
  array releases all remaining elements.

Assigning an array shares it (the reference is retained); arrays are not deep-copied. See
`05-arrays.md`.

---

## `None`

`None` is a singleton `Obj` representing the absence of a value. Because it is a shared global
rather than an allocation, retaining and releasing it is harmless and it is never destroyed.
`None` is what operations like `readln` yield at end of input, distinguished with `match`
(see `07-match-statements.md`).

---

## Reference Cycles

Reference counting reclaims an object when its count reaches zero. If two or more objects refer to
each other so that their counts never fall to zero — a parent holding a child that holds the
parent back — the group becomes unreachable from the program yet keeps itself alive. **Such cycles
are not collected and will leak.** PaykanLang currently has no cycle collector and no weak
references. Until weak references land, avoid building reference cycles among long-lived objects,
or break them explicitly by overwriting one of the linking fields before the objects go out of
scope.

---

## Thread Safety

The reference counting in this release is **not atomic**: it assumes a single thread of execution.
That is sound for v0.0, which is single-threaded. Thread-safe ARC — counts that can be shared
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

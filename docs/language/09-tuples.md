# PaykanLang — Tuples (prototype)

> **Prototype.** Tuples are experimental (issue #4): implemented and tested, but the syntax,
> the rules and the diagnostics below may change before they are declared stable.

A tuple is a fixed-size, ordered group of values that may have **different
types**: `(1, "a")` is an `(int, Str)`.  Tuples are the way to return more
than one value from a function.

---

## Quick Summary

- Type notation: `(T1, T2, ...)` with **at least two** element types, e.g.
  `(int, Str)`, `(int, (Str, bool))`, `(int, Str)[]`, `(int[], Str)`.
- Literals: `(1, "a")`, `(x, y + 1)`.  A parenthesised single expression
  `(x)` is just `x`; there are no 1-tuples and no empty tuple.
- Element access: `t.0`, `t.1`, … — the index is a compile-time constant and
  is checked against the tuple's size.  Chain for nesting: `t.1.0`.
- Tuples are **immutable**: `t.0 = v` is a compile-time error.  Build a new
  tuple instead.  (Objects *inside* a tuple keep their own mutability.)
- Destructuring: `q, r = divmod(7, 2);` binds each element to a name; `_`
  skips an element; `q: int, r: int = ...` annotates the targets.
- Tuples are **`Obj` subtypes** (reference-counted heap objects, like arrays):
  they can be printed, stored in an `Obj`, passed and returned freely.
- `==` / `!=` compare **element-wise**; `toString` renders `(1, a)`.

---

## Types and Literals

```pkn
t: (int, Str) = (1, "a");
u = (2.5, True, 'c');                     // inferred: (float, bool, char)
n: (int, (Str, bool)) = (1, ("x", True)); // nesting
pairs: (int, Str)[] = [(1, "one"), (2, "two")];
mixed: (int[], Str) = ([1, 2, 3], "nums");
```

A literal's type is exactly the tuple of its element types — there is no
promotion inside a tuple literal, so `t: (float, int) = (1, 2)` is an error
(write `(1.0, 2)`).  Reference-typed elements are covariant: an `(int, Str)`
can be assigned to an `(int, Obj)`.  Where the literal flows into a typed slot,
its optional elements come from that slot: `p: (Node?, int) = (None, 1)` and
`ps: (Str, int?)[] = [("a", 1), ("b", None)]` work (see `10-optionals.md`).

---

## Element Access

```pkn
t = (1, "a", 2.5);
x: int = t.0;
s: Str = t.1;
println(Str<float>(t.2));
println(n.1.0);          // "x" — chained access into a nested tuple
println(mk().0);         // works on any tuple-valued expression
```

`t.3` on a 3-tuple is a compile-time error:

```
error: tuple index '.3' is out of range for type '(int, Str, float)' (valid indices are .0 to .2)
```

`t.0` is lexed as a single token, so there is no clash with float literals:
`1.5` is still a float, and `t.0.1` is two element accesses.

---

## Multiple Return and Destructuring

```pkn
fn divmod(a: int, b: int) -> (int, int) {
  return (a / b, a % b);
}

fn main() -> int {
  q, r = divmod(7, 2);          // declares q and r with the element types
  _, rem = divmod(9, 4);        // `_` discards an element
  lo: int, hi: int = (10, 25);  // annotated targets
  q, r = divmod(20, 6);         // re-assigns the existing q and r
  return q + r + rem;
}
```

Rules:

- The right-hand side must be a tuple with **exactly as many elements as
  targets**.
- A bare name follows the usual assignment rule: it is declared on first use
  and re-assigned (with the usual type check) if it is already in scope.
- An annotated name `a: T` is always a fresh declaration in the current scope;
  the element must be assignable to `T` (so `f: float, _ = (1, 2)` promotes).
- A name may appear only once per statement; nested patterns like
  `(a, (b, c)) = e` are not supported.

---

## Tuples as Values

Tuples are reference-counted heap objects, exactly like class instances and
arrays, so they need no special handling:

```pkn
class Range {
  bounds: (int, int);                        // field
  fn __init__(lo: int, hi: int) { self.bounds = (lo, hi); }
  fn get() -> (int, int) { return self.bounds; }
}

fn swap(p: (int, Str)) -> (Str, int) { return (p.1, p.0); }  // parameter + return

o: Obj = (1, 2);           // any tuple is an Obj
println(o);                // (1, 2)
m = mov t;                 // ownership transfer, like any reference value
```

---

## Equality and `toString`

```pkn
a = (1, "s");
b = (1, "s");
println(Str(a == b));               // True  — element-wise, Str by content
println(Str<bool>(a != (2, "s")));        // True
println(Str(((1, "x"), 2.5) == ((1, "x"), 2.5)));   // True — nested
println(a.toString());                  // (1, s)
```

Both operands of `==` / `!=` must have the **same tuple type**.  Primitive
elements compare by value; reference elements use their own `equals` (so class
instances compare by identity unless the class overrides `equals`).  Ordering
operators (`<`, …) are not defined for tuples.

Comparison is element-wise even when both sides are the same tuple object;
there is no identity shortcut.  So `float` elements follow IEEE 754: a tuple
holding a NaN (directly, in a nested tuple, or in a `float?` element) is
unequal to every tuple, itself included, exactly as the NaN is unequal to
itself, while `-0.0` and `0.0` elements compare equal:

```pkn
fn main() -> int {
  nan = 0.0 / 0.0;
  t = (1, nan);
  println(Str(t == t));               // nan != nan
  println(Str(t != t));
  println(Str((1, 2.5) == (1, 2.5)));
  return 0;
}
```

Output:

```
False
True
True
```

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Index out of range | `t.2` on a 2-tuple |
| Index on non-tuple | `x.0` where `x` is not a tuple |
| Immutable | `t.0 = v`, `mov t.0` |
| Arity mismatch | `a, b, c = (1, 2)`; `f((1, 2, 3))` where `f` takes `(int, Str)` |
| Not a tuple | `a, b = 42` |
| Target type mismatch | `a: Str, b: int = (1, 2)` |
| Duplicate target | `a, a = (1, 2)` |
| No element promotion | `t: (float, int) = (1, 2)` |
| Mismatched equality | `(1, "a") == (1, 2)` |
| `void` element | `(nothing(), 1)` |
| Empty array element | `([], 1)` — bind `[]` to an annotated variable first |

---

## Not Yet Supported

Tuples in `match` patterns, nested destructuring, named elements, mutation,
1-tuples, and spreading a tuple into call arguments. A `match` arm cannot name a
tuple type (`(int, int) { … }` is rejected with "match arm type must be a class
or array type"): every tuple shares one runtime class, so there is no per-type
identity to test.

---

## Notes

- Every tuple is an instance of the builtin class `Tuple`, an `Obj` subtype. The
  name is reserved (`class Tuple {}` is an error), and `x: Tuple = (1, 2)`
  type-checks, but like `Obj` it only offers `toString` and `equals`.
- `toString` prints the elements without quotes, as `println` does, so
  `("a, b", 1)` prints as `(a, b, 1)`.
- At run time a tuple is one heap object holding its elements, released like any
  other reference: reference elements are retained by the tuple and released when
  it is destroyed (see `08-memory-model.md`).

# PaykanLang — Tuples

> **Stable in v0.1** (issue #4): the syntax and semantics below are covered by the
> compatibility promise for the 0.1 series (see `01-language-basics.md`, "Stability").

A tuple is a fixed-size, ordered group of values that may have **different
types**: `(1, "a")` is an `(int, Str)`.  Tuples are the way to return more
than one value from a function.

---

## Quick Summary

- Type notation: `(T1, T2, ...)` with **at least two** element types, e.g.
  `(int, Str)`, `(int, (Str, bool))`, `(int, Str)[2]`.
- Literals: `(1, "a")`, `(x, y + 1)`.  A parenthesised single expression
  `(x)` is just `x`; there are no 1-tuples and no empty tuple.
- Element access: `t.0`, `t.1`, … — the index is a compile-time constant and
  is checked against the tuple's size.  Chain for nesting: `t.1.0`.
- Tuples are **immutable**: `t.0 = v` is a compile-time error.  Build a new
  tuple instead.  (Objects *inside* a tuple keep their own mutability.)
- Destructuring: `q, r = divmod(7, 2);` binds each element to a name; `_`
  skips an element; `q: int, r: int = ...` annotates the targets.
- Tuples are **values held in place**, like arrays: a tuple's elements sit in the
  variable, field or array element that holds it, and assigning or passing a tuple
  copies it.  A tuple is not an `Obj`.
- `==` / `!=` compare **element-wise**; `toString()` (and `println`) renders `(1, a)`.

---

## Types and Literals

```pkn
t: (int, Str) = (1, "a");
u = (2.5, True, 'c');                     // inferred: (float, bool, char)
n: (int, (Str, bool)) = (1, ("x", True)); // nesting
pairs: (int, Str)[2] = [(1, "one"), (2, "two")];
```

A literal's type is exactly the tuple of its element types — there is no
promotion inside a tuple literal, so `t: (float, int) = (1, 2)` is an error
(write `(1.0, 2)`).  Reference-typed elements are covariant: an `(int, Str)`
can be assigned to an `(int, Obj)`, and a nested tuple element by element
(`((1, "a"), 2)` to a `((int, Obj), int)`); value elements must match exactly.
Where the literal flows into a typed slot, its optional elements come from that
slot: `p: (Node?, int) = (None, 1)` and
`ps: (Str, int?)[2] = [("a", 1), ("b", None)]` work (see `10-optionals.md`).

An element cannot be a fixed-size array for now (`tuple element 0: a fixed-size array
('int[2]') can only be a local variable, a parameter, a result or a field for now`).

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

A tuple is a value, like an `int` or an array: it is held in place, in the
variable, the field or the array element that holds it, and assigning it,
passing it to a parameter without a mode or returning it copies it.  The copy
holds the same elements: a copy of a tuple holding an object names that same
object, as a copy of an array of objects does.  Assigning a new tuple to the
original leaves the copy as it was.  A tuple goes anywhere a type does — a
field, a parameter, a result, an array element (`(int, Str)[3]`, `[(0, "k");
3]`), a type argument (`Box<(Str, int)>`), a branch of a ternary:

```pkn
class Range {
  bounds: (int, int);                                   // a field
  view fn get() -> (int, int) { return self.bounds; }   // returned as a copy
}

impl Range {
  fn __init__(lo: int, hi: int) { self.bounds = (lo, hi); }
}

class Box<T> {
  v: T;
  view fn get() -> view T { return self.v; }
}

impl Box<T> {
  fn __init__(v: T) { self.v = v; }
}

fn swap(p: (int, Str)) -> (Str, int) { return (p.1, p.0); }  // parameter + return

fn main() -> int {
  r = Range(10, 25);
  lo, hi = r.get();
  t = (1, "one");
  m = t;                                 // a copy of t
  t = (2, "two");                        // m is still (1, one)
  keys: (int, Str)[3] = [(0, "k"); 3];   // three copies of (0, k)
  b = Box<(Str, int)>(swap(m));          // a tuple type argument
  println(Str(hi - lo) + " " + m.1 + " " + t.1 + " " + keys[2].1);
  println(b.get());
  return 0;
}
```

Output:

```
15 one two k
(one, 1)
```

Parameter modes work on tuples as on other types (`02-functions-and-calling.md`).
A parameter without a mode is the callee's own copy.  An `inout` tuple is the
caller's variable: assigning a new tuple to it replaces the caller's.  A `view`
tuple whose elements are all values (`(int, float)`) is copied when it is
assigned, like a `view int`; one that holds a reference (an object, a `Str`,
an optional of one) is a `view` of what it holds, so it is read or passed on
to a `view` parameter but never stored or returned (`'p' is a 'view' parameter;
it cannot be stored, only read or passed to a 'view' parameter`), as a `view
Str[]` is.  The same goes for a `-> view T` result with `T` a tuple and for a
tuple field returned from a `view fn`.

A tuple is not an `Obj`: `o: Obj = (1, 2)` is an error (`initializer of type
'(int, int)' does not match declared type 'Obj' for variable 'o'`).  Print it,
compare it and call `toString()` on it directly; where an `Obj` is needed, keep
the tuple in a field of a class.

---

## Equality and `toString`

```pkn
a = (1, "s");
b = (1, "s");
println(Str(a == b));               // True  — element-wise, Str by content
println(Str<bool>(a != (2, "s")));        // True
println(Str(((1, "x"), 2.5) == ((1, "x"), 2.5)));   // True — nested
println(a.toString());                  // (1, s)
println(a);                             // (1, s)
```

Both operands of `==` / `!=` must have the **same tuple type**.  Primitive
elements compare by value; reference elements use their own `equals` (so class
instances compare by identity unless the class overrides `equals`), and two
absent optionals are equal.  Ordering operators (`<`, …) are not defined for
tuples.  `toString()` is a tuple's one method: compare with `==`, not
`equals` (`t.equals(u)` is an error, `method call on non-class type '(int,
int)'`).

`print`, `println`, `printerr` and `printerrln` take a tuple as they take an
`Obj`, and print what its `toString()` returns: each element as `println`
shows it, `None` for an absent optional, an object through its own
`toString`.  A tuple meets a `Show` bound (`fn show<T: Show>(x: view T)`, see
`12-traits.md`), as `int` does.

Comparison is element-wise even when both sides are the same variable;
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
| Immutable | `t.0 = v` |
| Arity mismatch | `a, b, c = (1, 2)`; `f((1, 2, 3))` where `f` takes `(int, Str)` |
| Not a tuple | `a, b = 42` |
| Target type mismatch | `a: Str, b: int = (1, 2)` |
| Duplicate target | `a, a = (1, 2)` |
| No element promotion | `t: (float, int) = (1, 2)` |
| Mismatched equality | `(1, "a") == (1, 2)` |
| A tuple as an `Obj` | `o: Obj = (1, 2)`; passing a tuple to an `Obj` parameter |
| A method other than `toString` | `t.equals(u)`, `t.len()` |
| `void` element | `(nothing(), 1)` |
| Array element | `([1, 2], 1)` |

---

## Not Yet Supported

Tuples in `match` patterns, nested destructuring, named elements, mutation,
1-tuples, and spreading a tuple into call arguments. A `match` arm cannot name a
tuple type (`(int, int) { … }` is rejected with "match arm type must be a class
type"): a tuple is not an object, so no `match` subject ever holds one.  An
optional tuple (`(int, Str)?`) is rejected, as it could not be unwrapped (see
`10-optionals.md`).

---

## Notes

- `toString` prints the elements without quotes, as `println` does, so
  `("a, b", 1)` prints as `(a, b, 1)`.
- At run time a tuple is laid out in place like a C struct: its elements one
  after another at their natural alignment, a reference element as its box, an
  `int?` element inline (a presence flag, then the `int`), a nested tuple
  inline.  A tuple of `int`s allocates nothing.  Each copy of a tuple retains
  the references it holds; a variable's are released when its scope ends, a
  field's when its object is destroyed, an array element's with the array (see
  `08-memory-model.md`).

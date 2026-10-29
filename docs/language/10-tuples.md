# Tuples

> **Stable** (issue #4): the syntax and semantics below are covered by the
> compatibility promise for the 0.2 series (see [Stability](index.md#stability)).

A tuple is a fixed-size, ordered group of values that may have **different types**:
`(1, "a")` is an `(int, Str)`. Tuples are the way to return more than one value from a
function. This chapter shows how to build, read, return and take tuples apart; in it `tasks`
uses them to report statistics and to split lines of text. Then it gives the precise rules.
A tuple whose elements have names and can be changed is a struct, the subject of
[Structs](11-structs.md).

## Tuple types and values

A tuple type lists its element types in parentheses, `(int, Str)`, and a tuple value lists
its elements the same way. Elements are read with `.0`, `.1` and so on:

```pkn
import ::io;

fn main() -> int {
  pair: (int, Str) = (7, "seven");
  point = (2.5, -1.0, True);          // (float, float, bool)
  nested = ("a", (1, 2));             // (Str, (int, int))
  io::println(pair.1 + " is " + Str(pair.0));
  io::println(Str(point.0 + point.1));
  io::println(Str(nested.1.0 + nested.1.1));
  io::println(nested);
  return 0;
}
```

Output:

```
seven is 7
1.5
3
(a, (1, 2))
```

- A tuple has at least two elements; `(x)` is just `x` in parentheses.
- The index after the dot must be a number written in the source, and the compiler checks
  it: `pair.2` on a pair is an error.
- `io::println` prints a tuple as its elements in parentheses, strings without quotes.
- There is no implicit `int` to `float` promotion inside a tuple literal:
  `t: (float, int) = (1, 2)` is an error, so write `(1.0, 2)`.

Tuples are **immutable**: `pair.0 = 8` is an error. To change one, build a new tuple.
(Objects inside a tuple can still change, as objects always can.)

## Returning several values

A function returns a tuple by listing the values, and the caller takes it apart with
**destructuring**: several names on the left of `=`, separated by commas.

```pkn
import ::io;

fn divmod(a: int, b: int) -> (int, int) {
  return (a / b, a % b);
}

fn minMax(xs: view int[]) -> (int, int) {
  lo = xs[0];
  hi = xs[0];
  i = 1;
  while i < xs.len() {
    if xs[i] < lo { lo = xs[i]; }
    if xs[i] > hi { hi = xs[i]; }
    i = i + 1;
  }
  return (lo, hi);
}

fn main() -> int {
  q, r = divmod(17, 5);
  io::println("17 = 5 * " + Str(q) + " + " + Str(r));

  lo, hi = minMax([4, -2, 9, 3]);
  io::println("range " + Str(lo) + ".." + Str(hi));

  _, rem = divmod(100, 7);          // _ skips an element
  io::println("100 % 7 = " + Str(rem));

  q, r = divmod(9, 2);              // assigns the existing q and r
  a: float, b: int = (1, 2);        // annotated targets; 1 becomes a float
  io::println(Str(q) + " " + Str(r) + " " + Str(a) + " " + Str(b));
  return 0;
}
```

Output:

```
17 = 5 * 3 + 2
range -2..9
100 % 7 = 2
4 1 1 2
```

Each name follows the usual rules: a new name is declared with the element's type, an
existing one is assigned, and `name: Type` always declares a new variable (the full rules
are in [Multiple Return and Destructuring](#multiple-return-and-destructuring)).

## Storing and comparing tuples

A tuple is a value, like an `int` or an array: it can be stored in a variable, a field or an
array, and passed to and returned from functions, and each of these copies it. Its elements
are held right where the tuple is, so a tuple of numbers costs no allocation at all. A tuple
is not an `Obj`, though: `o: Obj = (1, 2)` is an error. `==` and `!=` compare tuples element
by element, with each element compared the way `==` compares it on its own (strings by
content):

```pkn
import ::io;

fn main() -> int {
  scores: (Str, int)[3] = [("ada", 92), ("alan", 85), ("grace", 97)];
  i = 0;
  while i < scores.len() {
    name, score = scores[i];
    io::println(name + ": " + Str(score));
    i = i + 1;
  }
  io::println(Str(scores[0] == ("ada", 92)) + " " + Str(scores[0] == ("ada", 91)));
  return 0;
}
```

Output:

```
ada: 92
alan: 85
grace: 97
True False
```

Tuples have no ordering operators, and `match` cannot take a tuple apart: destructure it
first.

## Project: statistics and splitting lines

`tasks` will soon store each task as one line of text, the title and the priority separated
by a `|`, such as `fix the leak|high`. A `splitOnce` function returns the two parts as a
tuple, and a `stats` function counts done and pending tasks in one pass:

```pkn
import ::io;

class Task {
  title: Str;
  done: bool;
}

impl Task {
  fn __init__(title: Str, done: bool) { self.title = title; self.done = done; }
}

// Splits `s` at the first `sep`: ("a", "b") for "a|b", (s, "") when there is none.
fn splitOnce(s: Str, sep: char) -> (Str, Str) {
  left = "";
  i = 0;
  while i < s.len() && s[i] != sep {
    left = left + Str(s[i]);
    i = i + 1;
  }
  right = "";
  i = i + 1;
  while i < s.len() {
    right = right + Str(s[i]);
    i = i + 1;
  }
  return (left, right);
}

// (done, pending)
fn stats(tasks: view Task[]) -> (int, int) {
  done = 0;
  i = 0;
  while i < tasks.len() {
    if tasks[i].done { done = done + 1; }
    i = i + 1;
  }
  return (done, tasks.len() - done);
}

fn main() -> int {
  title, priority = splitOnce("fix the leak|high", '|');
  io::println("title '" + title + "', priority '" + priority + "'");
  io::println(splitOnce("no separator", '|'));

  tasks = [Task("a", True), Task("b", False), Task("c", True)];
  done, pending = stats(tasks);
  io::println(Str(done) + " done, " + Str(pending) + " pending");
  return 0;
}
```

Output:

```
title 'fix the leak', priority 'high'
(no separator, )
2 done, 1 pending
```

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
- `==` / `!=` compare **element-wise**; `toString()` (and `io::println`) renders `(1, a)`.

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
`ps: (Str, int?)[2] = [("a", 1), ("b", None)]` work (see
[Optionals](12-optionals.md#literals-take-their-destinations-types)).

An element cannot be a fixed-size array for now (`tuple element 0: a fixed-size array
('int[2]') can only be a local variable, a parameter, a result or a class field for now`),
nor a slice (`variable 't' element 0 has type 'int[]': a slice can only be a 'view' or
'inout' parameter, and a tuple element cannot be an array yet`).

---

## Element Access

```pkn
t = (1, "a", 2.5);
x: int = t.0;
s: Str = t.1;
io::println(Str<float>(t.2));
io::println(n.1.0);          // "x" — chained access into a nested tuple
io::println(mk().0);         // works on any tuple-valued expression
```

`t.3` on a 3-tuple is a compile-time error:

```
error: tuple index '.3' is out of range for type '(int, Str, float)' (valid indices are .0 to .2)
```

`t.0` is lexed as a single token, so there is no clash with float literals:
`1.5` is still a float, and `t.0.1` is two element accesses.

---

## Multiple Return and Destructuring

A function returns several values as one tuple (`-> (int, int)`), and a statement with
several targets on the left of `=` takes it apart, as in
[Returning several values](#returning-several-values). Rules:

- The right-hand side must be a tuple with **exactly as many elements as
  targets**.
- A bare name follows the usual assignment rule: it is declared on first use
  and re-assigned (with the usual type check) if it is already in scope.
- An annotated name `a: T` is always a fresh declaration in the current scope;
  the element must be assignable to `T` (so `f: float, _ = (1, 2)` promotes).
- `_` discards an element.
- A name may appear only once per statement; nested patterns like
  `(a, (b, c)) = e` are not supported: destructure in two steps.

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
import ::io;

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
  io::println(Str(hi - lo) + " " + m.1 + " " + t.1 + " " + keys[2].1);
  io::println(b.get());
  return 0;
}
```

Output:

```
15 one two k
(one, 1)
```

[Parameter modes](03-functions.md#parameter-modes-view-and-inout) work on tuples as on
other types.
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
io::println(Str(a == b));               // True  — element-wise, Str by content
io::println(Str<bool>(a != (2, "s")));        // True
io::println(Str(((1, "x"), 2.5) == ((1, "x"), 2.5)));   // True — nested
io::println(a.toString());                  // (1, s)
io::println(a);                             // (1, s)
```

Both operands of `==` / `!=` must have the **same tuple type**.  Primitive
elements compare by value; reference elements use their own `equals` (so class
instances compare by identity unless the class overrides `equals`), and two
absent optionals are equal.  Ordering operators (`<`, …) are not defined for
tuples.  `toString()` is a tuple's one method: compare with `==`, not
`equals` (`t.equals(u)` is an error, `method call on non-class type '(int,
int)'`).

A tuple's `toString()` shows each element through its own `toString()`:
`None` for an absent optional, an object through its class's `toString`.  A
tuple meets a `Show` bound (`fn show<T: Show>(x: view T)`, see
[Traits](06-traits.md)), as `int` does, so `::io`'s `print`, `println`, `printerr`
and `printerrln` (each `fn println<T: Show>(value: view T)`) print one.  A tuple
with an element that does not implement `Show`, a function value, does not
either: `io::println((1, f))` and `(1, f).toString()` are errors naming the
element (`its element 1 has type '(int) -> int', which does not implement it`).
`Str(t)` is `t.toString()` (`Str((1, "a"))` is `(1, a)`), and is the same error
for such a tuple.

Comparison is element-wise even when both sides are the same variable;
there is no identity shortcut.  So `float` elements follow IEEE 754: a tuple
holding a NaN (directly, in a nested tuple, or in a `float?` element) is
unequal to every tuple, itself included, exactly as the NaN is unequal to
itself, while `-0.0` and `0.0` elements compare equal:

```pkn
import ::io;

fn main() -> int {
  nan = 0.0 / 0.0;
  t = (1, nan);
  io::println(Str(t == t));               // nan != nan
  io::println(Str(t != t));
  io::println(Str((1, 2.5) == (1, 2.5)));
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
| Index on non-tuple | `x.0` where `x` is not a tuple; on an optional tuple, the unwrap error of [Optionals](12-optionals.md#semantic-checks) |
| Immutable | `t.0 = v` |
| Arity mismatch | `a, b, c = (1, 2)`; `f((1, 2, 3))` where `f` takes `(int, Str)` |
| Not a tuple | `a, b = 42`; an optional tuple is the unwrap error, as for an index |
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
1-tuples, and spreading a tuple into call arguments. A tuple is not an object, so it
cannot be a `match` subject (`match subject must be a class type, got '(int, int)'`), and
an arm on an `Obj` subject cannot name a tuple type (`(int, int) { … }` is rejected with
"match arm type must be a class type").  An optional tuple (`(int, Str)?`) is unwrapped by
an arm naming the tuple type (`t: (int, Str) { … }`) in optional mode (see
[Optional Enums, Tuples and Structs](12-optionals.md#optional-enums-tuples-and-structs)).

---

## Notes

- `toString` prints the elements without quotes, as `io::println` does, so
  `("a, b", 1)` prints as `(a, b, 1)`.
- At run time a tuple is laid out in place like a C struct: its elements one
  after another at their natural alignment, a reference element as the object's address, an
  `int?` element inline (a presence flag, then the `int`), a nested tuple
  inline.  A tuple of `int`s allocates nothing.  Each copy of a tuple retains
  the references it holds; a variable's are released when its scope ends, a
  field's when its object is destroyed, an array element's with the array.  At
  a variable's last use an assignment, a store or a `return` moves them
  instead, and the variable releases none (see [Memory](16-memory.md),
  "Ownership transfers").

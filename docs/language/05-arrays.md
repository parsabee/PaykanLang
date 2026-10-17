# PaykanLang — Arrays

Arrays are ordered, dynamically-sized collections of a single element type.

---

## Quick Summary

- Array type notation: `T[]` (e.g. `int[]`, `Str[]`, `Point[]`).
- Multi-dimensional arrays: `T[][]` — an array of arrays.
- Array literals: `[e1, e2, e3]`.
- Empty array literal `[]` **requires a type annotation** on the variable; the compiler cannot
  infer the element type from an empty literal alone.
- Arrays are **`Obj` subtypes** — every array type extends `Obj` and carries a vtable pointer.
  They are reference-counted heap objects. Assignment copies the reference, not the data.
- Because arrays are `Obj` types, an array can be stored in an `Obj` variable and passed
  wherever an `Obj` is expected. The inherited `toString` and `equals` slots are overridden
  by the array's built-in implementations.
- Core methods: `.len()`, `.push(val)`, `.pop()`, `.toString()`, `.equals(other)`.

---

## Creation

### Literal Initialisation

```pkn
nums: int[] = [10, 20, 30];
words: Str[] = ["hello", "world"];
```

### Empty Array

```pkn
empty: int[] = [];
```

An empty literal requires a type annotation so the compiler knows the element type.

### Computed Initialisation with `push`

Build an array of any size at runtime using a loop:

```pkn
squares: int[] = [];
i: int = 0;
while (i < 5) {
  squares.push(i * i);
  i = i + 1;
}
// squares = [0, 1, 4, 9, 16]
```

---

## Length

Use the `.len()` method to get the number of elements:

```pkn
a: int[] = [1, 2, 3, 4];
println(Str<int>(a.len()));   // 4
```

---

## Subscript Read and Write

```pkn
a: int[] = [10, 20, 30];
println(Str(a[1]));   // 20

a[1] = 99;
println(Str<int>(a[1]));   // 99
```

Indices are zero-based. Every access is **bounds-checked at runtime**: an out-of-bounds
index aborts the program with a message such as `paykan: array index 10 out of bounds (len=3)`.
(A negative index is currently reported as its unsigned wrap-around value.)

---

## `push` and `pop`

| Method | Effect |
|--------|--------|
| `arr.push(val)` | Appends `val` to the end; increases `.len()` by 1. |
| `arr.pop()` | Removes and returns the last element; decreases `.len()` by 1. |

```pkn
stack: int[] = [];
stack.push(1);
stack.push(2);
stack.push(3);
top: int = stack.pop();          // top = 3
println(Str(stack.len()));    // 2
```

---

## `toString` and `equals`

```pkn
a: int[] = [1, 2, 3];
println(a.toString());           // Array@0x…[len=3]

b: int[] = [1, 2, 3];
println(Str<bool>(a.equals(b)));   // False  (reference identity, not deep equality)
```

`toString` currently prints an address-and-length form, `Array@<address>[len=N]` — rendering
the elements as `[e1, e2, …]` is planned but not shipped.  
`equals` compares by **reference identity** (same underlying array object), not element-by-element.
Because `==` lowers to `equals`, `a == b` is likewise identity comparison for arrays (`a == a` is
`True`; two arrays with equal contents are not), and `a != b` is its negation. Both operands of
`==` / `!=` must be arrays of the **same element type** — comparing `int[]` with `Str[]` is a
compile-time error.

---

## Iteration

Use `while` with an index variable:

```pkn
nums: int[] = [10, 20, 30, 40];
i: int = 0;
while (i < nums.len()) {
  println(Str(nums[i]));
  i = i + 1;
}
```

---

## Multi-Dimensional Arrays

A 2-D array is an array of arrays. Each inner array can have a different length.

```pkn
matrix: int[][] = [[1, 2, 3],
                   [4, 5, 6],
                   [7, 8, 9]];

println(Str<int>(matrix[1][2]));   // 6
```

### Building a 2-D Array Dynamically

```pkn
fn makeGrid(rows: int, cols: int) -> int[][] {
  grid: int[][] = [];
  r: int = 0;
  while (r < rows) {
    row: int[] = [];
    c: int = 0;
    while (c < cols) {
      row.push(r * cols + c);
      c = c + 1;
    }
    grid.push(row);
    r = r + 1;
  }
  return grid;
}
```

---

## Arrays of Class Instances

```pkn
class Point {
  x: int;
  y: int;
  view fn toString() -> Str { return "(" + Str(self.x) + "," + Str<int>(self.y) + ")"; }
}

impl Point {
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }
}

pts: Point[] = [Point(0,0), Point(1,2), Point(-3,4)];
i: int = 0;
while (i < pts.len()) {
  println(pts[i].toString());
  i = i + 1;
}
```

---

## Passing Arrays to Functions

Arrays are passed by reference. Mutations inside a function are visible to the caller.

```pkn
fn doubleAll(a: int[]) {
  i: int = 0;
  while (i < a.len()) {
    a[i] = a[i] * 2;
    i = i + 1;
  }
}

fn main() -> int {
  nums: int[] = [1, 2, 3];
  doubleAll(nums);
  println(Str(nums[0]) + " " + Str(nums[1]) + " " + Str(nums[2]));
  return 0;
}
```

Output:

```
2 4 6
```

---

## Fixed-size arrays

`T[n]` is an array of exactly `n` elements, stored inline: in the variable
itself, not on the heap. The length is an integer literal, at least 1. A
fixed-size array is a **value**: assigning or copying one copies its
elements, so the copy and the original change independently. It is not an
`Obj`, has no `push` or `pop`, and its one method is `len()`.

```pkn
fn main() -> int {
  xs: int[3] = [1, 2, 3];
  ys = xs;                 // a copy
  ys[0] = 10;
  xs = [xs[2], xs[0], xs[1]];
  println(Str(xs[0]) + " " + Str(ys[0]) + " " + Str(xs.len()));
  return 0;
}
```

Output:

```
3 10 3
```

- A literal for a `T[n]` slot has exactly `n` elements, each of type `T`
  (`array literal has 2 element(s), but 'int[3]' has 3`). A literal with no
  `T[n]` slot to flow into is still a growable `T[]`.
- In a variable declaration with an initializer, a length can be `_`: it is
  the initializer's. An array literal gives its element count, and its first
  element (a literal or any fixed-size array) the lengths of a nested `_`.
  Anything else gives the lengths of the fixed-size array it is. `x: int[_][_] = [[1, 2, 3], [4, 5, 6]];` is an
  `int[2][3]`, and with `fn fill<T>(v: T) -> T[3]`, `z: float[_] =
  fill<float>(1.5);` is a `float[3]`. An empty literal gives no length, and
  a `_` anywhere else is an error (`a length can be '_' only in a variable
  declaration with an initializer, which gives it`).
- An index is an `int`. A constant one is checked when compiling (`index 3 is
  out of bounds for 'int[3]' (its indexes are 0 to 2)`), any other when
  running: `paykan: array index 3 out of bounds (len=3)`.
- The elements are part of the value, so a `let` array's elements cannot be
  assigned, and neither can a `view`'s. An `inout` local names the array
  itself (`r: inout = xs;`).
- An array of arrays puts its first length on its first index, as in C:
  `int[2][3]` is two `int[3]`s, indexed `grid[i][j]` with `i < 2` and `j < 3`.
  A row is a value too: `row = grid[1];` copies it.
- An array holds at most 1048576 bytes (1 MiB).
- A parameter can be a fixed-size array in every mode. A plain one is the
  function's own copy, a `view` one reads the caller's array, and an `inout`
  one changes it. A result is returned as a copy (`-> int[3]`; a `view` or
  `inout` result of one is not supported yet).
- An element can be a scalar (`int`, `float`, `bool`, `char`), an optional
  of one (`int?`, held inline, `None` when zeroed), an enum, a reference (a
  class, `Str`, a growable array, a tuple, an optional of one) or another
  fixed-size array.
- An element of a reference type is a reference, as a variable of that type
  is: copying the array copies the references, so the copy and the original
  share their elements' objects, and an element of a `view` array is a
  `view` too.
- A field can be a fixed-size array: it is held inside its object, and
  `__init__` assigns it like any other field. Its elements change only where
  the object can (`self.cells[0][1] = 5;` is not allowed in a `view fn`).
- For now a fixed-size array is only ever a local variable, a parameter, a
  result or a field: it cannot be a tuple or growable array element, an
  optional or a type argument. And an element cannot be named by an `inout`
  local or passed to an `inout` parameter yet.

```pkn
fn bumped(xs: int[3]) -> int[3] {
  xs[0] = xs[0] + 100;     // the function's own copy
  return xs;
}

fn clear(xs: inout int[3]) {
  xs = [0, 0, 0];          // the caller's array
}

fn main() -> int {
  a: int[3] = [1, 2, 3];
  b = bumped(a);
  clear(a);
  println(Str(a[0]) + " " + Str(b[0]));
  return 0;
}
```

Output:

```
0 101
```

```pkn
class Counter { n: int; }

fn main() -> int {
  cs: Counter[2] = [Counter(1), Counter(2)];
  copy = cs;               // the same two counters
  copy[0].n = 10;
  names: Str[2] = ["ann", "bob"];
  names[1] = names[0] + "!";
  println(Str(cs[0].n) + " " + names[1]);
  return 0;
}
```

Output:

```
10 ann!
```

```pkn
class Board { cells: char[3][3]; }

fn main() -> int {
  b = Board([['.', '.', '.'], ['.', '.', '.'], ['.', '.', '.']]);
  b.cells[1][1] = 'x';
  println(Str(b.cells[1][1]) + " " + Str(b.cells.len()));
  return 0;
}
```

Output:

```
x 3
```

```pkn
fn main() -> int {
  let xs: int[2] = [1, 2];
  xs[0] = 5;
  return 0;
}
```

Error:

```
'xs' is declared with 'let'; its elements are part of its value and cannot be assigned
```

### Slices

A `view T[]` or `inout T[]` parameter is a **slice**. It takes a fixed-size
array of any length with element type `T`, and reaches the caller's elements
in place; `xs.len()` is that array's length. A `view` slice only reads. An
`inout` one assigns elements (`xs[i] = v;`), but not the whole slice, and it
has no `push` or `pop`.

```pkn
fn sum(xs: view int[]) -> int {
  t = 0;
  i = 0;
  while (i < xs.len()) {
    t = t + xs[i];
    i = i + 1;
  }
  return t;
}

fn scale(xs: inout int[], by: int) {
  i = 0;
  while (i < xs.len()) {
    xs[i] = xs[i] * by;
    i = i + 1;
  }
}

fn main() -> int {
  a: int[3] = [1, 2, 3];
  scale(a, 10);
  grid: int[2][2] = [[1, 2], [3, 4]];
  println(Str(sum(a)) + " " + Str(sum([5, 6])) + " " + Str(sum(grid[1])));
  return 0;
}
```

Output:

```
60 11 7
```

- An argument is a fixed-size array (a variable, a row such as `grid[1]`, a
  field), a literal, which becomes a fixed-size array of its own length, or a
  slice passed on. A growable array cannot be passed for one.
- The array's element type must be the slice's exactly: an `int[3]` is not a
  `float[]`.
- A slice can only be indexed, measured with `len()` and passed on. It cannot
  be stored, copied, returned or printed.
- A generic `xs: view T[]` takes `T` from the array's elements. A literal
  argument there needs a variable first (`xs: int[2] = [1, 2];`).

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Type mismatch on element | Literal `[1, "two"]` has mixed element types |
| Empty literal without annotation | `x = []` with no type annotation |
| Subscript on non-array type | `x[0]` where `x` is not an array |
| `.pop()` on empty array | Runtime abort with `paykan: pop on empty array` (checked at runtime, not statically) |
| Fixed-size array literal of the wrong length | `xs: int[3] = [1, 2]` |
| `_` length with nothing to give it | `fn f(x: int[_])`, `x: int[_] = []`, `x: int[_] = 3` |
| Fixed-size array index out of bounds | `xs[3]` for an `int[3]`: a constant index when compiling, any other at runtime (`paykan: array index 3 out of bounds (len=3)`) |

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
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }
  view fn toString() -> Str { return "(" + Str(self.x) + "," + Str<int>(self.y) + ")"; }
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

## Buffer

`Buffer<T>` is the same growable storage as a `T[]`, reached only through its methods:
it has no `[]` and no literal. It is the storage a container class keeps its elements in
(the standard library's `List<T>`).

| Method | |
|--------|---|
| `Buffer<T>()` | An empty buffer |
| `view fn len() -> int` | The number of elements |
| `view fn get(i: int) -> T` | Element `i` (a copy for the caller to keep; `i` out of range aborts, as `xs[i]` does) |
| `set(i: int, v: T)` | Replace element `i` |
| `push(v: T)`, `pop() -> T` | Add at the end; remove and return the last element (aborts when empty) |

```pkn
class Stack<T> {
  items: Buffer<T>;
  fn __init__() { self.items = Buffer<T>(); }
  fn push(v: T) { self.items.push(v); }
  fn pop() -> T { return self.items.pop(); }
  view fn top() -> T { return self.items.get(self.items.len() - 1); }
}
```

- A `Buffer<T>` is an object like an array: assigning one shares it, and it is an `Obj`.
  It is not a `T[]`, though: neither is assignable to the other, an array literal is not a
  buffer (`an array literal is not a 'Buffer<int>': make one with 'Buffer<int>()'`), and a
  generic `T[]` parameter does not bind to a buffer, nor `Buffer<T>` to an array.
- `b[i]` is an error: `a 'Buffer<int>' has no '[]': use its get(i) and set(i, v)
  methods`. Nothing borrows an element in place, so no `view` or `inout` reaches into a
  buffer: `get` returns a copy, `set` replaces.
- `len` and `get` are `view fn`s, so a `view Buffer<T>` can be read; `set`, `push` and
  `pop` change it. `set` and `push` keep their value, so it cannot be a `view`.
- A `match` arm cannot test for a buffer type (at run time a `Buffer<int>` is an array of
  the same elements): `a match arm cannot test for 'Buffer<int>'`.
- `Buffer` is a builtin name: no class, generic class or function can be declared with it.

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Type mismatch on element | Literal `[1, "two"]` has mixed element types |
| Empty literal without annotation | `x = []` with no type annotation |
| Subscript on non-array type | `x[0]` where `x` is not an array |
| `.pop()` on empty array | Runtime abort with `paykan: pop on empty array` (checked at runtime, not statically) |
| Subscript or literal for a buffer | `b[0]`, `b[0] = v`, `b: Buffer<int> = [1]` (see [Buffer](#buffer)) |
| Buffer type arguments | `Buffer()` (`'Buffer' needs its element type`), `Buffer<int, int>`, `Buffer<void>` |

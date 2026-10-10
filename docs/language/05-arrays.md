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

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Type mismatch on element | Literal `[1, "two"]` has mixed element types |
| Empty literal without annotation | `x = []` with no type annotation |
| Subscript on non-array type | `x[0]` where `x` is not an array |
| `.pop()` on empty array | Runtime abort with `paykan: pop on empty array` (checked at runtime, not statically) |

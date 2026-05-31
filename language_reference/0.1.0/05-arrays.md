# PaykanLang — Arrays

Arrays are **heap-allocated, dynamic-capacity** containers with a **dynamic length**.

---

## Quick Summary

- `T[]` — dynamic-capacity, heap-allocated array.
  length is dynamic and starts at the literal's element count (`0` for `[]`).
- `T[][]` — 2D (and higher) fixed arrays.
- All indexing is bounds-checked against the **logical length**. 

---

## Declaration and Initialisation

### Literal initialisation
```
a: int[] = [1, 2, 3, 4, 5];
b = [1.0, 2.0, 3.0];          // inferred float[]
```

### Empty initialisation — capacity reserved, length 0
```
buf:  Vec2D[] = [];           // can't be inferred
side: int[]   = [];           // can't be inferred
```

### Fill syntax — all elements the same value
```
zeros: float[] = [0.0; 1024];
flags = [False; 64];
```

The count after `;` is the initial length of the array.

### Type inference
```
c = [1, 2, 3];          // inferred as int[]
d = [0.0; 512];         // inferred as float[]
```

### 2D and higher
```
mat: float[][] = [[0.0; 4]; 4];       // 4×4 zero matrix
cube: int[][][] = [[[0; 4]; 3]; 2];  // rank-3
```

---

## Indexing

```
a: int[] = [10, 20, 30, 40, 50];
x: int = a[2];      // 30
a[0] = 99;
```

Multidimensional:
```
mat[1][2] = 3.14;
v: float = mat[0][0];
```

Indexing reads/writes `a[i]` for `0 <= i < len(a)`. 

### Bounds checking policy

| Index expression | Check |
|---|---|
| Compile-time constant index on a full literal-initialised `T[]` | Compile-time — error if out of range |
| General variable (`a[i]`) | Runtime — **panic** on OOB (against `len(a)`) |

---

## `len()`

```
n:   int = len(a);       // T[]   -> dynamic length
```

For multi-dimensional fixed arrays, use `len(arr[k])` to query the `k`-th dimension
size.
```
m:   int = len(mat[0]);  // T[M][N]   -> M
n:   int = len(mat[1]);  // T[M][N]   -> N
k:   int = len(mat[2]);  // T[M][N]  panic
```

---

## `for` Loop Integration

```
// By const reference (copy for builtins, const T& for class elements)
for x in a { process(x); }

// By mutable reference — requires the array to be mutable
for x in a { x = 0; }   // ERROR: &a is const; use a mutable borrow

// By index — bounds check elided (affine over 0..len(a))
for i in 0..len(a) { a[i] = a[i] * 2; }

```

Iteration walks the **live** prefix `0..len(a)`, never the reserved tail.

---

## Semantic Checks (Arrays)

| Error | Trigger |
|---|---|
| Out-of-bounds constant index | `a[5]` on `int[3]` (against capacity), or `a[3]` when `len == 3` (against length) |
| Fill type mismatch | `[True; 4]` assigned to `int[4]` |

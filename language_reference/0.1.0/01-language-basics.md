# PaykanLang — Language Basics

## What is Paykan?

Paykan (`.pkn`) is a statically-typed, ownership-aware, **high-performance computing (HPC)** language.
It compiles to LLVM and can be **JIT-executed or compiled to a native binary**. There is no garbage collector — memory lifetimes are determined via reference counting for objects.

### Design Pillars

| Pillar | Description |
|--------|-------------|
| **Zero-cost abstractions** | Functions, and arrays map directly to machine code with no overhead |
| **Classes with virtual dispatch** | classes with vtable-based method dispatch, inheritance, and method overriding. Classes are stored on the heap and carry meta data for reference counting|
| **Binary + JIT** | Programs can be JIT-executed for rapid development or compiled to standalone native binaries |

---

## Program Structure

- A Paykan program is a list of **top-level declarations**: functions, and classes.
  There are no top-level statements.
- Execution begins at `main`, which must return `int`.
- Functions must be **declared before use** (no forward declarations).
- **No function overloading** — each function name must be unique.
- Comments use `//` (single-line only).
- Modules are imported with `import path::to::module;`, `import path::to::module::{name1, name2};`,
  or `import ::system_module;`. See `06-modules.md` for full module system documentation.

```
import std;

fn greet() {
  std::print("hello!");
}

fn main() -> int {
  greet();
  return 0;
}
```

---

## Types

### Builtin Types (stack-allocated, no ownership qualifiers)

| Type     | LLVM Type | Description                        |
|---------|----------|---------------------------------|
| `char`  | `i8`     | 8-bit character                    |
| `int`   | `i64`    | 64-bit signed integer              |
| `float` | `double` | 64-bit IEEE 754 floating-point     |
| `bool`  | `i1`     | Boolean — literals `True` / `False`|
| `void`  | `void`   | Function return type only          |


- `int` -> `float` promotion is implicit when assigning or passing to a `float` parameter.

### Arrays

dynamic arrays stored on the heap.

```
a: int[] = [1, 2, 3, 4, 5];        
b: float[] = [0.0; 1024];         // fill syntax
mat: float[][] = [[0.0; 4]; 4];   // 2D
```
> **See `05-arrays.md`** for the full array system

### Class Types (vtable-bearing, on the heap, ref counting)

| Type     | Description                                              |
|----------|----------------------------------------------------------|
| `Obj`    | Root of the class hierarchy (vtable-based)     |

#### Root: `Obj`

`Obj` is implicit — every class extends it transitively. It defines the following vtable
slots (all virtual, all overridable):

| Slot | Signature | Default |
|------|-----------|-------------------------|
| `toString` | `fn toString() -> Str` | Returns the class name |
| `equals`   | `fn equals(other: Obj) -> bool` | Identity comparison |

User-defined classes inherit and may override these.
`None` is an `Obj` literal.

```
import std;

class MyType extends Obj {
  ...
}

a = None;
if (checkSomething()) {
  a = MyType();
}

match a {
  MyType { std::write(std::out(), a.toString()); }
  _      { std::write(std::err(), "error\n");}
}
```

---

## Variables & Declarations

Variables are declared with inferred type annotations:

```
name: Type = initializer;
```

Examples:
```
x: int = 42;
y = 31;
pi: float = 3.14;
flag: bool = True;
flag2 = False;
msg: Str = "hello";
msg2 = "world";
```
---

## Expressions & Operators

### Literals

| Literal         | Type    | Examples              |
|-----------------|---------|-----------------------|
| Char            | `char`  | `c`, `s`, `1`         |
| Integer         | `int`   | `0`, `42`, `-17`      |
| Floating-point  | `float` | `3.14`, `0.5`         |
| Boolean         | `bool`  | `True`, `False`       |
| Str             | `Str`   | `"hello"`, `""`       |

### Arithmetic

| Op  | Meaning        | Notes                                          |
|-----|----------------|------------------------------------------------|
| `+` | Addition       | Numeric Builtins only                          |
| `-` | Subtraction    | Numeric Builtins only                          |
| `*` | Multiplication | Numeric Builtins only                          |
| `/` | Division       | Numeric Builtins only                          |
| `%` | Modulo         | Numeric Builtins only                          |

Result type is `float` if either operand is `float`, otherwise `int`.

### Unary

| Op  | Meaning      | Type        |
|-----|-------------|-------------|
| `-` | Negation     | `int`/`float` |
| `!` | Logical NOT  | `bool`      |

### Relational & Equality

`<`, `>`, `<=`, `>=`, `==`, `!=` — result is `bool`.
Equality operators require compatible types.

### Logical

| Op   | Meaning      | Short-circuits |
|------|-------------|----------------|
| `&&` | Logical AND  | Yes (stops if left is `False`) |
| `\|\|` | Logical OR | Yes (stops if left is `True`)  |

### Ternary Expression

```
if <condition> then <expr> else <expr>
```

- Condition must be `bool`.
- Both branches must have a **common supertype** (LUB). For builtins this means equal types
  (modulo `int -> float` promotion). For class types it is the nearest common ancestor in the
  class hierarchy (e.g. `if c then dog else cat` -> `Animal`).
- Both branches must have the **same ownership mode**. Mixing modes is rejected — either both
  unique, both shared, or both `const T&`. To unify across modes, convert explicitly first.
- Right-associative; can be nested.

```
result: int = if x > 0 then x else 0 - x;
```

### Cast Expression

Cast expressions use C-style syntax: `(T)expr`.

```
n: int = 42;
f = (float)n;      // numeric cast — always succeeds, result type: float

a: Animal = Dog("rex");   // upcast Dog -> Animal (implicit, non-optional)
b = Cat("meow");
c = (Animal)b;            // upcast, fine
match a {                 // downcast using a match statement
  Dog   { // use a as a `Dog` in this block }
  Cat   { // a is a `Cat` }
  _     { // anything else }
}
```

**Upcast** (toward a base class): always succeeds; result type is `T` (non-optional).  
**Downcast** (toward a derived class): checked at runtime; must be checked using a match statement. 
**Numeric cast** (`(int)`, `(float)`, `(bool)`): value conversion, always succeeds, result type `T`.

Cast is an expression and has the **highest precedence** (level 1, prefix form `(T)`).  
Casting is **not an implicit coercion** — only explicit `(T)` syntax triggers a cast.

### Operator Precedence (highest -> lowest)

| Level | Operators                              | Associativity |
|-------|----------------------------------------|---------------|
| 1     | `(T)` (cast, prefix), postfix          | Left |
| 2     | `!` (logical not), `-` (unary), `&`    | Right (prefix)|
| 3     | `*`, `/`, `%`                          | Left          |
| 4     | `+`, `-`                               | Left          |
| 5     | `<`, `>`, `<=`, `>=`, `==`, `!=`       | Left          |
| 6     | `&&`                                   | Left          |
| 7     | `\|\|`                                 | Left          |
| 8     | `if...then...else` (ternary)           | Right         |

---

## Statements

```
expression;                         // expression statement
name: Type = expression;            // variable declaration
variable = expression;              // assignment
return expression;                  // return (or bare return; for void)
;                                   // empty statement
{ statements... }                   // block (new scope)
```

---

## Control Flow

### if / else

```
if (condition) {
  // then
}

if (condition) {
  // then
} else {
  // else
}

if (x > 100) {
  std::print("large");
} else if (x > 10) {
  std::print("medium");
} else {
  std::print("small");
}
```

Condition must be `bool`. Parentheses are required.

### while

```
i: int = 0;
while (i < 5) {
  i = i + 1;
}

while (True) {   // infinite loop
  // ...
}
```

### for / range

```
for i in 0..n {
  // i: int, takes values 0, 1, ..., n-1
}

for x in arr {
  // iterate by const reference (arr: T[])
  // x has type const T&
}
```

- `lo..hi` is a half-open `int` range. Both endpoints are `int`.
- The loop variable is implicitly declared, immutable, and scoped to the loop body.
- Ranges are zero-cost: they desugar to a `while` over an `int` counter.

### break

- `break` — exits the innermost `while` or `for` loop.
- `break` is a compile-time error outside a loop.

---

## Str Casts

Use the `(Str)` cast to convert builtin values to strings:

| Expression        | Input type | Result                        |
|-------------------|------------|-------------------------------|
| `(Str)n`       | `int`      | Integer -> string              |
| `(Str)f`       | `float`    | Float -> string                |
| `(Str)b`       | `bool`     | Boolean -> `"True"` / `"False"`|

```
s: Str = (Str)42;       // "42"
t: Str = (Str)3.14;    // "3.14"
u: Str = (Str)True;    // "True"
```

### File I/O

The `std` module exposes file primitives.

| Function | Description |
|----------|-------------|
| `std::open(path: Str, modes: std::Mode[]) -> Obj`          | Open a file with the given modes (e.g. `[std::Mode::Read]`). Returns `Obj`; downcast to `std::File` with a `match` statement — the match arm is skipped on failure. |
| `std::write(file: std::File, content: Str) -> bool`       | Write to a file. Returns `True` on success. |
| `std::read(file: std::File) -> Str`      | blocking read from a file. |
| `std::close(file: std::File)`               | Close file.

### Process arguments

| Function | Description |
|----------|-------------|
| `std::args() -> Str[]` | get the program's positional arguments (after the script path). |

```
import std;

// Synchronous round-trip
match std::open("input.txt", [std::Mode::Read]) {
  file: std::File {
    match std::open("output.txt", [std::Mode::Write]) {
      outfile: std::File { std::write(outfile, file.toString()); }
      _                  { std::printErr("failed to open output file");}
    }
    std::close(file);
  }
  _ { std::printErr("failed to open file"); }
}
```

### Numeric methods

Builtin numeric types expose a small intrinsic API that lifts cleanly through `T^`:

| Method | Description |
|---|---|
| `sqrt(float) -> float`     | IEEE square root |
| `abs(float) -> float`, `abs(int) -> int` | Absolute value |
| `floor(float) / ceil(float) / round() -> float` | Rounding intrinsics |

### Enums and pattern matching

Enums (`enum Op { Add, Sub }`, `enum Token { Num, Eof }`) and the `match`
statement are zero-cost tagged sums. `match` also accepts builtin scrutinees
(`int`, `float`, `bool`, `Str`) with literal-pattern arms and a mandatory `_`
catch-all (for unbounded value spaces) — the canonical way to write a multi-way
dispatch over a scalar. See **`03-enums.md`** for the full specification.

---

## Scoping Rules

- **Lexical scoping**: variables are resolved from innermost to outermost scope.
- `{ }` introduces a new scope.
- Each function body is its own scope with parameters pre-declared.
- **Shadowing**: inner scopes can shadow outer names via declaration-by-assignment.
- **Move tracking**: moved status propagates up the scope chain.

```
fn main() -> int {
  x: int = 1;
  {
    x = 2;         // modifies outer x
    y: int = 3;    // local to this block
  }
  // y is not visible here
  return 0;
}
```

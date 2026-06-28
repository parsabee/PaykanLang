# PaykanLang — Language Basics

## What is Paykan?

Paykan (`.pkn`) is a statically-typed clean and simple language.
It compiles to LLVM IR and is **JIT-executed** (ahead-of-time native compilation is planned for v0.1).
Memory lifetimes for objects are managed via automatic reference counting — there is no garbage
collector.

### Design Pillars

| Pillar | Description |
|--------|-------------|
| **Zero-cost abstractions** | Functions and arrays map directly to machine code with no overhead |
| **Classes with virtual dispatch** | vtable-based method dispatch, single inheritance, method overriding — class instances are heap-allocated with reference counting |
| **JIT execution** | Programs are JIT-executed for rapid development (standalone native binaries planned for v0.1) |

---

## Program Structure

- A Paykan program is a list of **top-level declarations**: imports, classes, and functions.
  There are no top-level statements.
- Execution begins at `main`, which must return `int`.
- Functions and classes must be **declared before use**.
- **No function overloading** — each function name must be unique.
- Comments use `//` (single-line only).
- Modules are imported with `import path::to::module;`. See `05-modules.md` for details.

```pkn
fn greet() {
  println("hello!");
}

fn main() -> int {
  greet();
  return 0;
}
```

---

## Types

### Builtin Types (stack-allocated)

| Type    | LLVM Type | Description                         |
|---------|-----------|-------------------------------------|
| `int`   | `i64`     | 64-bit signed integer               |
| `float` | `double`  | 64-bit IEEE 754 floating-point      |
| `bool`  | `i1`      | Boolean — literals `True` / `False` |
| `void`  | `void`    | Function return type only           |

- `int` -> `float` promotion is implicit when assigning or passing to a `float` parameter.

### String Type

`Str` is a built-in heap-allocated class type. String literals produce a `Str`. The `+`
operator concatenates two `Str` values and returns a new `Str`.

```pkn
s: Str = "hello";
t: Str = s + " world";   // "hello world"
```

### Arrays

Dynamic arrays stored on the heap. See `04-arrays.md` for the full array system.

```pkn
a: int[]    = [1, 2, 3, 4, 5];
mat: int[][] = [[1, 2], [3, 4]];   // 2D
```

### Class Types (vtable-bearing, heap-allocated, reference-counted)

`Obj` is the root of the class hierarchy. Every class transitively extends it. It defines:

| Slot       | Signature                       | Default              |
|------------|---------------------------------|----------------------|
| `toString` | `fn toString() -> Str`          | Returns class name   |
| `equals`   | `fn equals(other: Obj) -> bool` | Identity comparison  |
| `destroy`  | (compiler-generated)            | Destructor — final, runs on last release |

`None` is an `Obj` literal representing the absence of a value.

`File` is a built-in class type representing an open file handle. It extends `Obj` and provides:

| Slot        | Signature                       | Description                                      |
|-------------|---------------------------------|--------------------------------------------------|
| `write`     | `fn write(s: Str)`              | Writes `s` to the file                           |
| `readln`    | `fn readln() -> Obj`            | Returns the next line as `Str`, or `None` at EOF |
| `toString`  | `fn toString() -> Str`          | Returns a string description of the handle       |
| `equals`    | `fn equals(other: Obj) -> bool` | Reference identity comparison                    |

The file handle is closed automatically when the `File` object goes out of scope (ARC destroy).

`Error` is a built-in class type returned by operations that can fail (e.g. `open()`). It
extends `Obj` and provides:

| Slot        | Signature                       | Description                    |
|-------------|---------------------------------|--------------------------------|
| `toString`  | `fn toString() -> Str`          | Returns the error message      |
| `equals`    | `fn equals(other: Obj) -> bool` | Reference identity comparison  |

---

## Variables & Declarations

Variables are declared with a type annotation and initializer:

```pkn
name: Type = initializer;
```

The type annotation may be omitted when the type can be inferred from the initializer:

```pkn
x: int = 42;
y = 31;           // inferred int
pi: float = 3.14;
flag: bool = True;
msg: Str = "hello";
msg2 = "world";   // inferred Str
n: Obj = None;
```

---

## Expressions & Operators

### Literals

| Literal        | Type    | Examples          |
|----------------|---------|-------------------|
| Integer        | `int`   | `0`, `42`, `-17`  |
| Floating-point | `float` | `3.14`, `0.5`     |
| Boolean        | `bool`  | `True`, `False`   |
| String         | `Str`   | `"hello"`, `""`   |
| None           | `Obj`   | `None`            |

### Arithmetic

| Op  | Meaning        | Notes                              |
|-----|----------------|------------------------------------|
| `+` | Addition       | Numeric, or `Str` concatenation    |
| `-` | Subtraction    | Numeric only                       |
| `*` | Multiplication | Numeric only                       |
| `/` | Division       | Numeric only                       |
| `%` | Modulo         | Numeric only                       |

Result type is `float` if either operand is `float`, otherwise `int`.

### Unary

| Op  | Meaning     | Type          |
|-----|-------------|---------------|
| `-` | Negation    | `int`/`float` |
| `!` | Logical NOT | `bool`        |

### Relational & Equality

`<`, `>`, `<=`, `>=`, `==`, `!=` — result is `bool`.
Equality operators require compatible types.

### Logical

| Op     | Meaning     | Short-circuits |
|--------|-------------|----------------|
| `&&`   | Logical AND | Yes (stops if left is `False`) |
| `\|\|` | Logical OR  | Yes (stops if left is `True`)  |

### Ternary Expression

```pkn
if <condition> then <expr> else <expr>
```

- Condition must be `bool`.
- Both branches must have a compatible type (`int`/`float` promote; class types unify to their
  nearest common ancestor).
- Right-associative; can be nested.

```pkn
result: int = if x > 0 then x else 0 - x;
label: Str = if score >= 90 then "A" else if score >= 80 then "B" else "C";
```

### Operator Precedence (highest -> lowest)

| Level | Operators                              | Associativity  |
|-------|----------------------------------------|----------------|
| 1     | postfix: `.method()`, `.field`, `[i]`  | Left           |
| 2     | `!`, `-` (unary)                       | Right (prefix) |
| 3     | `*`, `/`, `%`                          | Left           |
| 4     | `+`, `-`                               | Left           |
| 5     | `<`, `>`, `<=`, `>=`, `==`, `!=`       | Left           |
| 6     | `&&`                                   | Left           |
| 7     | `\|\|`                                 | Left           |
| 8     | `if…then…else` (ternary)               | Right          |

---

## Statements

```pkn
expression;                          // expression statement
name: Type = expression;             // variable declaration with type
name = expression;                   // variable declaration, inferred type
variable = expression;               // assignment
obj.field = expression;              // field assignment
arr[i] = expression;                 // subscript assignment
return expression;                   // return (or bare return; for void)
;                                    // empty statement
{ statements... }                    // block (new scope)
```

---

## Control Flow

### if / else

```pkn
if (condition) {
  // then
}

if (condition) {
  // then
} else {
  // else
}

if (x > 100) {
  println("large");
} else if (x > 10) {
  println("medium");
} else {
  println("small");
}
```

Condition must be `bool`. Parentheses are required.

### while

```pkn
i: int = 0;
while (i < 5) {
  i = i + 1;
}

while (True) {   // infinite loop
  // ...
}
```

### break / continue

- `break` — exits the innermost `while` loop.
- `continue` — skips to the next iteration of the innermost `while` loop.
- Both are a compile-time error outside a loop.

```pkn
i: int = 0;
while (i < 10) {
  i = i + 1;
  if (i % 2 != 0) { continue; }   // skip odd
  if (i == 8)     { break; }       // stop at 8
  println(StrInt(i));              // prints 2 4 6
}
```

### match

`match` dispatches on the **runtime class** of a class-typed expression.

```pkn
match <expr> {
  TypeName          { /* no binding */ }
  binding: TypeName { /* bind matched object to name */ }
  _                 { /* catch-all wildcard */ }
}
```

- Each arm checks whether the object is exactly of the given type.
- An optional binding (`name: Type`) introduces a local variable in that arm.
- `_` matches anything; must be the last arm.
- `match` is a statement, not an expression.

```pkn
a: Animal = Dog();
match a {
  d: Dog { println(d.speak()); }
  Cat    { println("cat"); }
  _      { println("other"); }
}
```

See `03-classes.md` for more on `match` and class dispatch.

---

## Scoping Rules

- **Lexical scoping**: variables resolve from innermost to outermost scope.
- `{ }` introduces a new scope.
- Each function body is its own scope with parameters pre-declared.
- Inner scopes can shadow outer names via a new declaration.

```pkn
fn main() -> int {
  x: int = 1;
  {
    x = 2;        // modifies outer x
    y: int = 3;   // local to this block
  }
  // y is not visible here
  return 0;
}
```

---

## Built-in Functions

### Output

| Function            | Description                              |
|---------------------|------------------------------------------|
| `print(args…)`      | Print one or more values, no newline     |
| `println(args…)`    | Print one or more values, then newline   |
| `printerr(args…)`   | Print to stderr, no newline              |
| `printerrln(args…)` | Print to stderr, then newline            |

All print functions accept any number of arguments of any type.

```pkn
println("x =", StrInt(x));
print("a=", StrInt(a), " b=", StrInt(b));
```

### Type-to-String Conversion

| Function      | Input   | Output                  |
|---------------|---------|-------------------------|
| `StrInt(n)`   | `int`   | Integer as string       |
| `StrFloat(f)` | `float` | Float as string         |
| `StrBool(b)`  | `bool`  | `"True"` or `"False"`   |

```pkn
println(StrInt(42));      // "42"
println(StrFloat(3.14));  // "3.14"
println(StrBool(True));   // "True"
```

### File I/O

| Function           | Description                                                              |
|--------------------|--------------------------------------------------------------------------|
| `open(path, mode)` | Open a file. `mode` is `"r"`, `"w"`, or `"a"`. Returns `Obj` — either a `File` on success or an `Error` on failure. |

Because `open()` returns `Obj`, always use `match` to distinguish the two cases:

```pkn
match open("/tmp/data.txt", "r") {
  err: Error { printerrln("open failed: " + err.toString()); }
  f: File {
    while (True) {
      match f.readln() {
        line: Str { print(line); }
        _          { break; }   // None — EOF
      }
    }
  }
}
```

The `File` handle and its methods (`write`, `readln`, `toString`, `equals`) are described
in the **File built-in class type** section above.

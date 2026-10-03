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
- Execution begins at `main`, which must return `int`. Its return value becomes the process
  exit code. `main` may optionally take the command-line arguments as `fn main(args: Str[]) -> int`;
  `args[0]` is the source-file path and the remaining elements are the arguments after it.
- Declarations may appear in **any order** — a function or class may be used before the point
  in the file where it is declared (forward references are allowed).
- **No function overloading** — each function name must be unique.
- Comments use `//` (single-line only).
- Modules are imported with `import path::to::module;`. See `06-modules.md` for details.

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
| `char`  | `i8`      | Character — literals `'a'`, `'\n'`  |
| `void`  | `void`    | Function return type only           |

- `int` -> `float` promotion is implicit when assigning or passing to a `float` parameter.

### String Type

`Str` is a built-in heap-allocated class type. String literals produce a `Str`. The `+`
operator concatenates two `Str` values and returns a new `Str`. `s.len()` returns the
length as an `int`, and subscripting `s[i]` reads the `char` at index `i`.

```pkn
s: Str = "hello";
t: Str = s + " world";   // "hello world"
n: int = t.len();        // 11
c: char = t[0];          // 'h'
```

A string literal must close on the line it opens — a raw newline inside a string literal is a
compile-time error. Use the `\n` escape for line breaks.

### Arrays

Dynamic arrays stored on the heap. See `05-arrays.md` for the full array system.

```pkn
a: int[]    = [1, 2, 3, 4, 5];
mat: int[][] = [[1, 2], [3, 4]];   // 2D
```

### Class Types (vtable-bearing, heap-allocated, reference-counted)

`Obj` is the root of the class hierarchy. Every class transitively extends it. It defines:

| Slot       | Signature                       | Default              |
|------------|---------------------------------|----------------------|
| `toString` | `fn toString() -> Str`          | Returns `Object@<address>` (a per-class name is planned) |
| `equals`   | `fn equals(other: Obj) -> bool` | Identity comparison  |
| `destroy`  | (compiler-generated)            | Destructor — final, runs on last release |

`None` is an `Obj` literal representing the absence of a value.

### Optional Types (prototype)

Any reference type `T` (a class, `Str`, or an array) has an optional form `T?` that holds either
a `T` or `None`. A `T` converts to `T?` implicitly; a `T?` is unwrapped with `match`. See
`10-optionals.md`.

```pkn
n: Node? = None;
fn find(head: Node?, key: int) -> Node? { ... }
```

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

### Enum Types (distinct nominal value types)

An `enum` declares a new type whose values are a fixed, named set of variants. Each variant
is written `EnumName::Variant`, and variants take implicit ordinal values `0, 1, 2, …` in
declaration order. An enum is a **distinct nominal type**: it is *not* an `int` and does not
convert to or from one, even though it is represented as an integer at runtime. The only
operators defined on enums are `==` and `!=`, and both operands must belong to the **same**
enum. Enums are stack values (no heap allocation, no reference counting) and can be stored in
variables, passed and returned by functions, held in class fields, and collected in arrays.
The idiomatic way to branch on an enum is `match`, which can be made exhaustive over the
variants without a wildcard.

```pkn
enum Direction { North, East, South, West }

d: Direction = Direction::East;
if (d == Direction::East) { println("heading east"); }
```

See `03-enums.md` for the full treatment, including using `enum` with `match`.

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
| Character      | `char`  | `'a'`, `'\n'`, `'\t'` |
| String         | `Str`   | `"hello"`, `""`   |
| None           | `Obj`   | `None`            |

An integer literal must fit in a signed 64-bit integer; a larger one (above
`9223372036854775807`) is a compile-time error. `-17` is unary minus applied to the literal
`17`, so the most negative `int` cannot be written directly — compute it as
`-9223372036854775807 - 1`.

A float literal must name a finite `float`. One too large (`1e999` would be infinity) or too
small (`1e-400`: not zero, but rounds to zero) is a compile-time error. Subnormal values
such as `5e-324` are fine, as is zero written with any exponent (`0e-999`). Infinities and
NaN are computed, not written: `1.0e308 * 10.0`, `0.0 * (1.0e308 * 10.0)`.

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

For **reference types** (classes, `Str`, arrays), `==` dispatches to `equals`: `a == b` invokes
`a.equals(b)` through the vtable, and `a != b` is its negation. A class may override `equals` to
define what equality means for it; the default (inherited from `Obj`) compares object identity, so
two separately-constructed instances are unequal unless the class overrides `equals`. `Str`
overrides `equals` to compare by content, so `"ab" == "ab"` is `True`. Arrays do **not**
override it: array `==` compares reference identity, and both operands must be arrays of the
**same element type** (`int[] == Str[]` is a compile-time error; see `05-arrays.md`). For
**value types** (`int`, `float`, `bool`, `char`, enums) `==` compares the values directly.

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

### Move (`mov`)

`mov <expr>` transfers ownership of a value rather than copying/sharing it. It applies to a local
variable or a temporary. Moving a variable **consumes** it — the variable may not be used again
until it is re-assigned:

```pkn
a: int = 3;
b = mov a;        // b takes a's value; a is consumed
// c = a;         // error: use of moved variable 'a'

s: Str = "hi";
t = mov s;        // t takes ownership of the string with no extra retain/release
```

A member variable (`obj.field`) or array element (`arr[i]`) cannot be moved. `mov` binds like a
unary prefix operator. See `08-memory-model.md` for how moves interact with reference counting.

### Operator Precedence (highest -> lowest)

| Level | Operators                              | Associativity  |
|-------|----------------------------------------|----------------|
| 1     | postfix: `.method()`, `.field`, `[i]`  | Left           |
| 2     | `!`, `-` (unary), `mov`                | Right (prefix) |
| 3     | `*`, `/`, `%`                          | Left           |
| 4     | `+`, `-`                               | Left           |
| 5     | `<`, `>`, `<=`, `>=`, `==`, `!=`       | **Non-associative** |
| 6     | `&&`                                   | Left           |
| 7     | `\|\|`                                 | Left           |
| 8     | `if…then…else` (ternary)               | Right          |

Comparison and equality operators do **not** chain: `1 < 2 < 3` and `1 < 2 == True` are syntax
errors — parenthesize instead, e.g. `(1 < 2) == True`.

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
  println(Str<int>(i));              // prints 2 4 6
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

`match` has a second form that compares a builtin value against literal patterns, and it is
also the primary way to branch on an `enum`. See `07-match-statements.md` for the full
treatment and `03-enums.md` for matching on enums.

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

| Function          | Description                             |
|-------------------|-----------------------------------------|
| `print(o)`        | Print one value (via `toString`), no newline |
| `println(o)`      | Print one value, then a newline         |
| `printerr(o)`     | Print to stderr, no newline             |
| `printerrln(o)`   | Print to stderr, then a newline         |

Each takes **exactly one** argument — an `Obj`, printed via its `toString`.
Paykan has no variadic functions or overloading, so there is no multi-argument
form: compose pieces with `+` (string concatenation).

```pkn
println("x = " + Str<int>(x));
print("a=" + Str<int>(a) + " b=" + Str<int>(b));
```

### Conversions

Converting between the primitive types and `Str` uses a **conversion constructor**:
`Target<Source>(value)`. The target and the source are type names: the primitives `int`,
`float`, `bool` and `char`, and `Str`. You always write the source type, and the argument must
have exactly that type. There is no implicit `int` -> `float` promotion here, and an optional
must be unwrapped first.

| Conversion        | From → to          | Result                                                        |
|-------------------|--------------------|---------------------------------------------------------------|
| `Str<int>(n)`     | `int` → `Str`      | Decimal digits, with a `-` for negatives: `"42"`, `"-7"`      |
| `Str<float>(f)`   | `float` → `Str`    | `%g` formatting: `"3.14"`, `"2"`, `"1e+20"`, `"-0"`; `"nan"`, `"inf"`, `"-inf"` |
| `Str<bool>(b)`    | `bool` → `Str`     | `"True"` or `"False"`                                         |
| `Str<char>(c)`    | `char` → `Str`     | A one-character string                                        |
| `int<Str>(s)`     | `Str` → `int?`     | The parsed integer, or `None` (see below)                     |
| `float<Str>(s)`   | `Str` → `float?`   | The parsed float, or `None` (see below)                       |
| `int<float>(f)`   | `float` → `int`    | Truncates toward zero; panics on NaN, ±inf or out of range    |
| `float<int>(n)`   | `int` → `float`    | The nearest `float` (exact up to 2^53)                        |
| `int<bool>(b)`    | `bool` → `int`     | `1` for `True`, `0` for `False`                               |
| `bool<int>(n)`    | `int` → `bool`     | `n != 0`                                                      |
| `int<char>(c)`    | `char` → `int`     | The character's byte code, `0`..`255`                         |
| `char<int>(n)`    | `int` → `char`     | The character with byte code `n`; panics outside `0`..`255`   |

```pkn
println(Str<int>(42));            // 42
println(Str<float>(3.14));        // 3.14
println(Str<bool>(True));         // True
n: int = int<float>(-2.9);        // -2
f: float = float<int>(3) / 2.0;   // 1.5
code: int = int<char>('a');       // 97
c: char = char<int>(code + 1);    // 'b'
```

Any other pair is a compile-time error that lists the valid sources for that target:

```pkn
x = int<Node>(n);   // error: no conversion from 'Node' to 'int'; 'int<...>' converts from
                    //        'Str', 'float', 'bool' or 'char'
y = int<float>(3);  // error: argument of 'int<float>' has type 'int', expected 'float'
```

**Parsing.** `int<Str>(s)` and `float<Str>(s)` can fail, so they return an optional: `None`
when `s` is not a valid number. Unwrap the result with `match` (see `10-optionals.md`):

```pkn
match int<Str>(line) {
  n: int { total = total + n; }
  None   { printerrln("not a number: " + line); }
}
```

- `int<Str>` accepts an optional `+` or `-` followed by decimal digits. The digits must make
  up the whole string, with no surrounding whitespace, and the value must fit in an `int`
  (`"9223372036854775808"` is `None`).
- `float<Str>` accepts what C's `strtod` accepts, including exponents and `nan` / `inf`, but
  again the whole string with no leading or trailing whitespace. The range rule is the one
  for float literals: subnormal values (`"5e-324"`, `"1e-310"`) parse, but a value too
  large (`"1e999"`, which would be infinity) or too small (`"1e-400"`, not zero but rounding
  to zero) is `None`. `"inf"` and `"nan"` are spelled out, so they parse.

**Panics.** `int<float>` of NaN, of an infinity, or of a value outside the `int` range
(`-2^63` to just under `2^63`) stops the program. So does `char<int>` outside `0`..`255`.
Both print a message naming the value and abort, like an integer division by zero:

```
paykan: int<float>(inf): the value is NaN, infinite or outside the int range
paykan: char<int>(300): the value is outside the char range 0..255
```

**`char` is a byte.** A `char` is one byte of a `Str`, so a non-ASCII character in a UTF-8
string spans several `char`s (`"é"[0]` has code `195`). `int<char>` reads the byte as an
unsigned code `0`..`255`, and `char<int>` is its exact inverse over that range.

> The conversion builtins `StrInt`, `StrFloat`, `StrBool`, `StrChar`, `IntStr` and `FloatStr`
> were replaced by these constructors.

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

### Builtin Names Are Reserved

Top-level declarations — free functions, classes (a class name is also its constructor),
and enums — share a single namespace with the builtins, and a name identifies exactly one
entity. Declaring a function, class, or enum whose name is a builtin function (`print`,
`println`, `printerr`, `printerrln`, `Str`, `open`) or a builtin class (`Obj`, `Str`,
`Array`, `File`, `Error`, `Int`, `Float`, `Bool`, `Char`) is a compile-time error, reported at
the declaration:

```pkn
class print { fn __init__() {} }   // error: 'print' is a builtin function and cannot be redeclared
fn open(p: Str) -> int { ... }     // error: 'open' is a builtin function and cannot be redeclared
enum Error { NotFound }            // error: 'Error' is a builtin class and cannot be redeclared
```

The same rule applies between user declarations: `fn Point()` next to `class Point` is
rejected with `'Point' is already declared as a class`, and a function or class cannot
reuse an enum's name. Only top-level names are reserved — a field, method, parameter, or
local variable may be called `print` or `open` without affecting the builtin.

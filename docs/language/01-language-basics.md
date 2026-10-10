# PaykanLang — Language Basics

## What is Paykan?

Paykan (`.pkn`) is a statically-typed clean and simple language.
A program is checked, lowered to a backend-neutral IR (PIR, `docs/pir.md`) and compiled by a
backend: the **C backend** (emits C11 for the system C compiler) or the optional **LLVM backend**
(LLVM IR). `paykan run` (or just `paykan prog.pkn`) runs a program; the c backend is the default,
and `--backend=llvm` runs it through a JIT instead. `paykan build prog.pkn -o prog` writes a
native executable with either backend.
Memory lifetimes for objects are managed via automatic reference counting — there is no garbage
collector.

### Stability

Tuples (`09-tuples.md`), optional types (`10-optionals.md`, optional primitives and
optional-mode `match` included) and generics (`11-generics.md`) are **stable in v0.1**: the
syntax and semantics this reference documents for them are covered by the compatibility
promise for the 0.1 series. The limitations each chapter lists stay limitations; in
particular generics cannot be imported across modules until v0.2.0 ([#57](https://github.com/parsabee/PaykanLang/issues/57)), and a present
optional primitive is boxed ([#96](https://github.com/parsabee/PaykanLang/issues/96) changes that representation, not the semantics).

### Design Pillars

| Pillar | Description |
|--------|-------------|
| **Zero-cost abstractions** | Functions and arrays map directly to machine code with no overhead |
| **Classes with virtual dispatch** | vtable-based method dispatch, single inheritance, method overriding — class instances are heap-allocated with reference counting |
| **Run or build** | `paykan run` executes a program directly for rapid development; `paykan build` produces a standalone native executable |

---

## Program Structure

- A Paykan program is a list of **top-level declarations**: imports, classes, and functions.
  There are no top-level statements.
- Execution begins at `main`, which must return `int`. Its return value becomes the process
  exit code. `main` may optionally take the command-line arguments as `fn main(args: Str[]) -> int`;
  `args[0]` is the source-file path under `paykan run` (the executable's path for a program made
  by `paykan build`) and the remaining elements are the arguments after it. A program without
  one of these two `main`s (an empty file included) is a compile error; `paykan --check-only`
  also accepts a module without `main`, but still checks a `main` it declares.
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

| Type    | Description                         |
|---------|-------------------------------------|
| `int`   | 64-bit signed integer               |
| `float` | 64-bit IEEE 754 floating-point      |
| `bool`  | Boolean — literals `True` / `False` |
| `char`  | 8-bit character — literals `'a'`, `'\n'` |
| `void`  | Function return type only           |

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

### Optional Types

Any reference type `T` (a class, `Str`, or an array) and the primitives `int`, `float`, `bool`
and `char` have an optional form `T?` that holds either a `T` or `None`. A `T` converts to `T?` implicitly; a `T?` is unwrapped with `match`. See
`10-optionals.md`.

```pkn
n: Node? = None;
fn find(head: Node?, key: int) -> Node? { ... }
```

`File` is a built-in class type representing an open file handle. It extends `Obj` and provides:

| Slot        | Signature                       | Description                                      |
|-------------|---------------------------------|--------------------------------------------------|
| `write`     | `fn write(s: Str)`              | Writes `s` to the file                           |
| `readln`    | `fn readln() -> Obj`            | Returns the next line as `Str` (including its `\n`, if any), or `None` at EOF |
| `readbytes` | `fn readbytes(n: int) -> Obj`   | Returns the next `n` bytes (fewer at the end of the file) as `Str`, or `None` at EOF or when `n <= 0` |
| `read`      | `fn read() -> Obj`              | Returns everything left in the file as `Str`, or `None` when nothing is left |
| `toString`  | `fn toString() -> Str`          | Returns a string description of the handle       |
| `equals`    | `fn equals(other: Obj) -> bool` | Reference identity comparison                    |

The file handle is closed automatically when the `File` object goes out of scope (ARC destroy).
The read methods return `Obj`, so use `match` to tell a `Str` from `None` (see
[File I/O](#file-io)).

`Stdin` is a builtin `File` that reads the program's standard input. It needs no `open` and
offers the same methods (`readln`, `readbytes`, `read`):

```pkn
fn main() -> int {
  n: int = 0;
  while (True) {
    match Stdin.readln() {
      line: Str { n = n + 1; }
      _         { break; }       // None — end of input
    }
  }
  println(Str(n) + " lines");
  return 0;
}
```

Output (with nothing on standard input):

```
0 lines
```

`Stdin` cannot be reassigned: `Stdin = x` is an error ("'Stdin' is a type name and cannot be
used as a variable"). See `samples/codegen/23_stdin.pkn`.

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

A variable declared with `let` cannot be reassigned. It needs an initializer, and its type
annotation may be omitted like any other:

```pkn
let limit = 10;          // inferred int
let name: Str = "Ada";
limit = 11;              // error: 'limit' is declared with 'let' and cannot be reassigned
```

`let` applies to local declarations only: not to parameters, fields or destructuring
(`let a, b = t;` is a syntax error). It fixes the variable, not the object it refers to: the
fields of an object held in a `let` variable can still be changed, and an array's elements
too. A `let` variable cannot be passed to an `inout` parameter
([Parameter modes](02-functions-and-calling.md#parameter-modes-view-and-inout)), only to a
plain or `view` one. Like any variable, an inner block may declare its own variable with the
same name.

### Local borrows

A local can be declared with a mode, `view` or `inout`, written where the type goes, as for
a [parameter](02-functions-and-calling.md#parameter-modes-view-and-inout). The type may be
left off; it is the initializer's:

```pkn
fn main() -> int {
  k = 3;
  v: view = k + 1;     // a read-only local
  w: view float = k;
  println(Str(v) + " " + Str(w));
  return 0;
}
```

Output:

```
4 3
```

A `view` local is read-only: it cannot be assigned or be a destructuring target (`cannot
assign to 'view' local 'v'`), and it cannot be passed to an `inout` parameter. Its
initializer may be any expression. A `view` local of a value type holds a copy of it; one
of an object, string or array holds the same object, string or array, which cannot be
changed through it: no field or element write, and only a `view fn`
([Methods that don't change `self`](04-classes.md#methods-that-dont-change-self)), such as
`toString`, `equals`, `len` or `length`, can be called on it (`'v' is a 'view' local;
'tick' is not a 'view fn'`). A `view` local stays one: it can only be passed on to a `view` parameter,
and is never stored or returned
([A `view` stays a `view`](02-functions-and-calling.md#a-view-stays-a-view)).

An `inout` local, `x: inout = place;`, is another name for a variable or a field: reading
it reads there, and assigning to it writes there, of any type (an object, string or array is
replaced, as by an assignment to the variable). It can be passed on to an `inout`
parameter:

```pkn
class Account {
  balance: int;
  fn __init__() { self.balance = 0; }
}

fn main() -> int {
  a = Account();
  b: inout = a.balance;
  b = b + 10;
  names = ["x"];
  n: inout = names;
  n = ["y", "z"];
  println(Str(a.balance) + " " + Str(names.len()));
  return 0;
}
```

Output:

```
10 2
```

What it names follows the rules of an
[`inout` argument](02-functions-and-calling.md#inout-the-callers-storage): a variable or a
field, also of the object a `let` local holds, of exactly the local's type (`'inout' local
'f' has type 'float', but what it names has type 'int'`). An expression, an array element
(not yet), a string's character, `self`, a `let` local and anything reached through a
`view` are errors. An object whose field it names stays alive while the local can be used.

A local borrow of a variable (or of a field or element reached from one) is exclusive while
it is live, from its declaration to the last statement of its block that uses it; a loop
that uses it keeps it live for the whole loop:

- while an `inout` local is live, the variable it names cannot be used except through it:
  `'k' is borrowed by 'inout' local 'x' until 'x' is last used`;
- while a `view` local of a variable is live, the variable cannot change, as if it were a
  `view` itself: `'c' is viewed by 'view' local 'v' until 'v' is last used; 'tick' may
  change it`. A variable of a value type can still be read and passed on as a copy.

```pkn
fn main() -> int {
  k = 1;
  x: inout = k;
  x = 5;               // the last use of x
  println(Str(k));     // fine: x is no longer live
  return 0;
}
```

Output:

```
5
```

A borrow of a borrow (`y: inout = x;` with `x: inout = k;`) keeps `k` borrowed while `y` is
live. A `view` local of an expression (`v: view = k + 1;`) borrows nothing.

A local borrow always has an initializer, declares one variable (not a destructuring
target), and is never `let`: `let x: view = e;` is an error, `a local borrow cannot be
'let'`. Written before the name, `view x = e;`, the mode is a syntax error that shows the
fix: `'view' goes after the colon, before the type: write 'x: view = ...'`.

A `match` arm's binding can borrow the subject in the same way, `d: view Dog { … }` or
`d: inout Dog { … }` ([Borrowing the subject](07-match-statements.md#borrowing-the-subject)).

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

**Integer arithmetic** is 64-bit two's complement and behaves the same on every backend:

- `+`, `-`, `*` and unary `-` **wrap** on overflow: `9223372036854775807 + 1` is
  `-9223372036854775808` (`INT64_MAX + 1 == INT64_MIN`), and `-INT64_MIN` is `INT64_MIN`.
- `/` truncates toward zero and `%` takes the sign of the dividend: `-7 / 2` is `-3`,
  `-7 % 2` is `-1`, `7 % -2` is `1`, `7 / -2` is `-3`.
- `/` or `%` by zero **panics** with `integer division or modulo by zero`.
- `INT64_MIN / -1` (whose result does not fit) **panics** with `integer overflow in division`;
  `INT64_MIN % -1` is `0`.

A panic prints `paykan: <message>` to stderr and aborts the program (see the panic notes
under [Conversions](#conversions)).

**Float arithmetic** is IEEE 754 `double`: `%` is C's `fmod` (`7.5 % 2.0` is `1.5`,
`-7.5 % 2.0` is `-1.5`), and dividing by zero gives an infinity or NaN rather than panicking
(`1.0 / 0.0` is `inf`).

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

Float comparisons follow IEEE 754: NaN is unequal to everything, itself included (with `nan`
a NaN-valued `float` such as `0.0 * (1.0e308 * 10.0)`, `nan == nan` is `False`, `nan != nan` is `True`, and `<`, `>`, `<=`, `>=` with a NaN operand are `False`),
and `-0.0 == 0.0` is `True`. Tuples compare element by element with the same rule, so two
tuples holding a NaN (`(1, nan) == (1, nan)`) are unequal, and a tuple holding a NaN is
unequal to every tuple, itself included (`t == t` is `False`, `t != t` is `True`), consistent
with float semantics.

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

### `mov` (removed)

`mov` was removed in v0.2.0 and is a reserved word: write `t = s`, not `t = mov s`. Ownership
transfers are inferred by the compiler; see `08-memory-model.md`.

### Operator Precedence (highest -> lowest)

| Level | Operators                              | Associativity  |
|-------|----------------------------------------|----------------|
| 1     | postfix: `.method()`, `.field`, `[i]`  | Left           |
| 2     | `!`, `-` (unary)                       | Right (prefix) |
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
let name = expression;               // a variable that cannot be reassigned
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
println("x = " + Str(x));
print("a=" + Str<int>(a) + " b=" + Str(b));
```

`print` and `println` write to standard output, `printerr` and `printerrln` to standard
error; stdout is flushed before a runtime panic, so output printed before one is never lost.
See `samples/codegen/46_printerr.pkn`.

### Conversions

Converting between the primitive types, their boxes and `Str` uses a **conversion
constructor**: call the target type with the value, `Target(value)`.

```pkn
n = 42;
s: Str = Str(n);                  // "42"
half: float = float(n) / 2.0;     // 21
match int("17") {                 // a parse can fail: an int?
  v: int { println(Str(v + 1)); } // 18
  None   { }
}
```

The conversions are **specializations of the builtin types**. Each builtin target has a
fixed, closed set of them, one for each type it converts *from*. `Target(value)` picks the
specialization whose source is **exactly** the type of `value`: `Str(n)` with an `int` is
`Str<int>`, `int(f)` with a `float` is `int<float>`, `int(s)` with a `Str` is `int<Str>`.
Nothing is widened or unwrapped to find one. An `int` argument does not reach `float`'s
`Str` specialization, and an `int?` must be unwrapped with `match` first.

| Target  | Specializations (the source types)                      | Result                                   |
|---------|---------------------------------------------------------|------------------------------------------|
| `Str`   | `int`, `float`, `bool`, `char`, `Int`, `Float`, `Bool`, `Char` | `Str`                             |
| `int`   | `Str`; `float`, `bool`, `char`                          | `int?` from `Str`; `int` otherwise       |
| `Int`   | `Str`                                                   | `Int?`                                   |
| `float` | `Str`; `int`                                            | `float?` from `Str`; `float` from `int`  |
| `Float` | `Str`                                                   | `Float?`                                 |
| `bool`  | `int`; `Str`                                            | `bool` from `int`; `bool?` from `Str`    |
| `Bool`  | `Str`                                                   | `Bool?`                                  |
| `char`  | `int`                                                   | `char`                                   |

`Char` is not a target: it has no specializations, as there is no `char<Str>` parse to box.
`Int`, `Float` and `Bool` have no other constructor, so `Int(5)` is an error, not a box:
`Int`'s only specialization is the parse `Int(s)`. A present `int?` already is an `Int`.

**The explicit form.** You may also name the specialization: `Target<Source>(value)`, for
example `Str<int>(n)` or `int<Str>(s)`. It is the same conversion, and it documents the
source type at the call. The argument must then have exactly that type
(`Str<float>(3)` is an error, as `3` is an `int`).

What each specialization does:

| Conversion        | From → to          | Result                                                        |
|-------------------|--------------------|---------------------------------------------------------------|
| `Str<int>(n)`     | `int` → `Str`      | Decimal digits, with a `-` for negatives: `"42"`, `"-7"`      |
| `Str<float>(f)`   | `float` → `Str`    | `%g` formatting: `"3.14"`, `"2"`, `"1e+20"`, `"-0"`; `"nan"`, `"inf"`, `"-inf"` |
| `Str<bool>(b)`    | `bool` → `Str`     | `"True"` or `"False"`                                         |
| `Str<char>(c)`    | `char` → `Str`     | A one-character string                                        |
| `Str<Int>(x)` … `Str<Char>(x)` | box → `Str` | Exactly what the primitive form gives for the boxed value |
| `int<Str>(s)`     | `Str` → `int?`     | The parsed integer, or `None` (see below)                     |
| `float<Str>(s)`   | `Str` → `float?`   | The parsed float, or `None` (see below)                       |
| `bool<Str>(s)`    | `Str` → `bool?`    | `True` for `"True"`, `False` for `"False"`, else `None`       |
| `Int<Str>(s)`, `Float<Str>(s)`, `Bool<Str>(s)` | `Str` → `Int?` / `Float?` / `Bool?` | The same parse, as the optional box |
| `int<float>(f)`   | `float` → `int`    | Truncates toward zero; panics on NaN, ±inf or out of range    |
| `float<int>(n)`   | `int` → `float`    | The nearest `float` (exact up to 2^53)                        |
| `int<bool>(b)`    | `bool` → `int`     | `1` for `True`, `0` for `False`                               |
| `bool<int>(n)`    | `int` → `bool`     | `n != 0`                                                      |
| `int<char>(c)`    | `char` → `int`     | The character's byte code, `0`..`255`                         |
| `char<int>(n)`    | `int` → `char`     | The character with byte code `n`; panics outside `0`..`255`   |

```pkn
println(Str(42));                 // 42
println(Str<float>(3.14));        // 3.14
println(Str(True));               // True
n: int = int(-2.9);               // -2
f: float = float<int>(3) / 2.0;   // 1.5
code: int = int('a');             // 97
c: char = char<int>(code + 1);    // 'b'
match Int("7") {
  x: Int { println(Str(x)); }     // 7 (Str<Int>)
  None   { }
}
```

The boxed sources (`Str(x)` with `x: Int`, and the rest) take a present box: an `Int` is
never `None`, so an `Int?` is unwrapped with `match` first, as with any optional. The boxed
targets give the optional box rather than the optional primitive: `Int(s)` is an `Int?`,
holding the value `int(s)` would give.

**The set is closed.** When no specialization's source is the argument's type, a user class
included, the call is a compile-time error that lists the target's specializations. The
explicit form reports a type argument outside the set the same way. A class cannot add a
specialization; `toString()` is the way to turn an object into a `Str`:

```pkn
a = Point();
s = Str(a);         // error: no specialization of 'Str' for 'Point'; its specializations
                    //        are int, float, bool, char, Int, Float, Bool, Char
o = None;
t = Str(o);         // error: no specialization of 'Str' for 'Obj'; ...
x = float(2.5);     // error: no specialization of 'float' for 'float'; its specializations
                    //        are Str, int
y = int<float>(3);  // error: argument of 'int<float>' has type 'int', expected 'float'
```

`Str(s)` with a `Str` argument is not a conversion: it is the `Str` constructor, and still
returns the string.

**Parsing.** `int(s)`, `float(s)` and `bool(s)` with a `Str` (`int<Str>` & co., and their
boxed forms) can fail, so they return an optional: `None` when `s` is not a valid value. Unwrap the result
with `match` (see `10-optionals.md`):

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
- `bool<Str>` accepts exactly `"True"` and `"False"`, the spellings `Str<bool>` prints, so
  `bool<Str>(Str<bool>(b))` gives back `b`. Anything else (`"true"`, `"1"`, `" True"`) is
  `None`.

**Panics.** `int<float>` of NaN, of an infinity, or of a value outside the `int` range
(`-2^63` to just under `2^63`) stops the program. So does `char<int>` outside `0`..`255`.
Both print a message naming the value and abort, like an integer division by zero:

```
paykan: int<float>(inf): the value is NaN, infinite or outside the int range
paykan: char<int>(300): the value is outside the char range 0..255
```

A panic (these, an integer division by zero or `INT64_MIN / -1`, an index out of bounds, `.pop()` on an empty
array) flushes the output already printed to stdout, prints its message to stderr and
aborts the program with `SIGABRT`, so everything printed before the panic reaches a pipe or
a file too, ahead of the message. `paykan run` (and `paykan prog.pkn`) then exits with status
`134` (128 + `SIGABRT`) on every backend. An executable made by `paykan build` is the
program itself, so it dies by `SIGABRT` (a shell shows `$?` as `134` either way).

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

The `File` handle and its methods (`write`, `readln`, `readbytes`, `read`, `toString`,
`equals`) and the `Stdin` file are described in the **File built-in class type** section above.

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

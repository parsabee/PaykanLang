# Basics

This chapter covers the everyday core of PaykanLang: how a program is laid out, variables,
the built-in types, operators, statements, control flow and scoping. By the end of it you
can write small programs, and we write the first functions of `tasks`, the to-do list
manager that the following chapters build up.

## Program Structure

- A Paykan program is a list of **top-level declarations**: imports, classes, structs,
  enums, traits, `impl` blocks and functions (`native fn`s included). There are no top-level
  statements.
- Execution begins at `main`, which must return `int`. Its return value becomes the process
  exit code. `main` may optionally take the command-line arguments as
  `fn main(args: view Str[]) -> int`; `args[0]` is the source-file path under `paykan run`
  (the executable's path for a program made by `paykan build`) and the remaining elements
  are the arguments after it ([Command-line arguments](../stdlib/io.md#command-line-arguments)).
  A program without one of these two `main`s (an empty file included) is a compile error;
  `paykan --check-only` also accepts a module without `main`, but still checks a `main` it
  declares.
- Declarations may appear in **any order** — a function or class may be used before the point
  in the file where it is declared (forward references are allowed).
- **No function overloading** — each function name must be unique.
- Comments use `//` (single-line only).
- Modules are imported with `import path::to::module;`. See [Modules](15-modules.md) for
  details.

```pkn
import ::io;

fn greet() {
  io::println("hello!");
}

fn main() -> int {
  greet();
  return 0;
}
```

---

## Variables & Declarations

A variable is declared with a name, a colon, a type and an initial value:

```pkn
import ::io;

fn main() -> int {
  count: int = 3;
  price: float = 2.5;
  ready: bool = True;
  name: Str = "Ada";
  io::println(name + " ordered " + Str(count) + " items at " + Str(price));
  io::println(Str(ready));
  return 0;
}
```

Output:

```
Ada ordered 3 items at 2.5
True
```

Every variable needs an initial value: there are no uninitialised variables. When the type
is obvious from the value, leave it out and the compiler **infers** it from the initializer:

```pkn
import ::io;

fn main() -> int {
  count = 3;          // int
  price = 2.5;        // float
  name = "Ada";       // Str
  total = count * 4;  // int
  io::println(name + ": " + Str(total));
  return 0;
}
```

Output:

```
Ada: 12
```

So a declaration is `name: Type = initializer;` or `name = initializer;`. Once a variable
exists, `name = value` assigns a new value to it. The new value must have the variable's
type: a variable never changes type.

```pkn
fn main() -> int {
  count = 3;
  count = count + 1;
  count = "four";
  return 0;
}
```

Error:

```
error: cannot assign value of type 'Str' to variable 'count' of type 'int'
```

A name can be declared only once per block (`redeclaration of variable 'count'`), but an
inner block may declare its own variable with the same name
([Scoping Rules](#scoping-rules)).

A variable declared with `let` cannot be reassigned. It needs an initializer, and its type
annotation may be omitted like any other:

```pkn
import ::io;

fn main() -> int {
  let rate = 3;
  let label: Str = "rate";
  io::println(label + " = " + Str(rate));
  return 0;
}
```

Output:

```
rate = 3
```

```pkn
fn main() -> int {
  let rate = 3;
  rate = 4;
  return 0;
}
```

Error:

```
error: 'rate' is declared with 'let' and cannot be reassigned
```

`let` applies to local declarations only: not to parameters, fields or destructuring
(`let a, b = t;` is a syntax error, `'let' declares one variable; destructuring with 'let'
is not supported yet`). It fixes the variable, not the object a class-typed one refers to:
for a class `Point`, `let p = Point(1, 2);` can still be followed by `p.x = 5;`. An array's
elements and a struct's fields are part of its value, so those of a `let` array or struct
cannot be assigned (`'s' is declared with 'let'; its fields are part of its value and
cannot be assigned`, [Fields](11-structs.md#fields)). A `let` variable cannot be passed to
an `inout` parameter ([Parameter modes](03-functions.md#parameter-modes-view-and-inout)),
only to a plain or `view` one. Like any variable, an inner block may declare its own variable
with the same name.

A variable declared with `weak` refers to an object without keeping it alive. Its type is
`T?` for a class `T`, inferred from the initializer when left off, and it reads as `None`
once the object is gone:

```pkn
p = Point(1, 2);
weak w: Point? = p;      // p's object, while something holds it
weak u = p;              // inferred Point?
weak e: Point?;          // None until assigned
```

`weak` also declares a class field (`weak parent: Node?;`). See
[Weak references](16-memory.md#weak-references) for what a weak reference is for and how it
is read.

### Local borrows

A local can be declared with a mode, `view` or `inout`, written where the type goes, as for
a [parameter](03-functions.md#parameter-modes-view-and-inout). The type may be
left off; it is the initializer's:

```pkn
import ::io;

fn main() -> int {
  k = 3;
  v: view = k + 1;     // a read-only local
  w: view float = k;
  io::println(Str(v) + " " + Str(w));
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
([Methods that don't change `self`](05-classes.md#methods-that-dont-change-self)), such as
`toString`, `equals` or `len`, can be called on it (`'v' is a 'view' local;
'tick' is not a 'view fn'`). A `view` local stays one: it can only be passed on to a `view`
parameter, and is never stored or returned
([A `view` stays a `view`](03-functions.md#a-view-of-a-reference-stays-a-view)).

An `inout` local, `x: inout = place;`, is another name for a variable or a field: reading
it reads there, and assigning to it writes there, of any type (an object, string or array is
replaced, as by an assignment to the variable). It can be passed on to an `inout`
parameter:

```pkn
import ::io;

class Account {
  balance: int;
}

impl Account {
  fn __init__() { self.balance = 0; }
}

fn main() -> int {
  a = Account();
  b: inout = a.balance;
  b = b + 10;
  names = ["x", "y"];
  n: inout = names;
  n = [n[1], "z"];
  io::println(Str(a.balance) + " " + names[0] + names[1]);
  return 0;
}
```

Output:

```
10 yz
```

What it names follows the rules of an
[`inout` argument](03-functions.md#inout-the-callers-storage): a variable or a
field, also of the object a `let` local holds, of exactly the local's type (`'inout' local
'f' has type 'float', but what it names has type 'int'`). An expression, an array element
(not yet), a string's character, `self`, a `let` local and anything reached through a
`view` are errors. An object whose field it names stays alive while the local can be used,
and so does one holding a struct whose field it names (`o.h` for `x: inout = o.h.p.s;`).

A local borrow of a variable (or of a field or element reached from one) is exclusive while
it is live, from its declaration to the last statement of its block that uses it; a loop
that uses it keeps it live for the whole loop:

- while an `inout` local is live, the variable it names cannot be used except through it:
  `'k' is borrowed by 'inout' local 'x' until 'x' is last used`;
- while a `view` local of a variable is live, the variable cannot change, as if it were a
  `view` itself: `'c' is viewed by 'view' local 'v' until 'v' is last used; 'tick' is
  not a 'view fn'`. A variable of a value type can still be read and passed on as a copy.

```pkn
import ::io;

fn main() -> int {
  k = 1;
  x: inout = k;
  x = 5;               // the last use of x
  io::println(Str(k));     // fine: x is no longer live
  return 0;
}
```

Output:

```
5
```

A borrow of a borrow (`y: inout = x;` with `x: inout = k;`) keeps `k` borrowed while `y` is
live. A `view` local of an expression (`v: view = k + 1;`) borrows nothing.

A statement uses the names it holds: a call through a borrow of a function value (`v(3)`
with `v: view = f;`) uses `v`, and a lambda or a call through a function value
(`fs[0](5)`) uses only what it names, so neither keeps an unrelated borrow live. A lambda
that captures the borrowed variable uses it, and copies the reference it holds, so it is
an error while the borrow is live: `'k' is borrowed by 'inout' local 'x' until 'x' is last
used; a function cannot capture it`, and for a `view` local of an object, a string or an
array, `'h' is viewed by 'view' local 'v' until 'v' is last used; a function cannot
capture it (it could change it through its copy)`.

A use is of the name where it means the borrow. A binder of the same name further on is
another variable, so a statement that names only that one does not keep the borrow live: a
lambda's parameter (`let f = fn(x: int) => x * 2;` after `x = 5;`), a `for` loop's
variable, a `match` arm's binding, and a declaration in an inner block, from there to the
block's end (a use in the block before it, or in its initializer, is of the borrow).

A local borrow always has an initializer, declares one variable (not a destructuring
target), and is never `let`: `let x: view = e;` is an error, `a local borrow cannot be
'let'`. Written before the name, `view x = e;`, the mode is a syntax error that shows the
fix: `'view' goes after the colon, before the type: write 'x: view = ...'`.

A `match` arm's binding can borrow the subject in the same way, `d: view Dog { … }` or
`d: inout Dog { … }` ([Borrowing the subject](08-match.md#borrowing-the-subject)).

---

## Types

The built-in value types are the numbers, `bool` and `char`. Besides these, `Str` is the
string type, `void` is the "returns nothing" type of functions, and every class you declare
is a type. Arrays (`int[3]`), tuples (`(int, Str)`), structs, function types, optionals
(`int?`) and enums are introduced below and have chapters of their own, as do generic
types (`Box<int>`, [Generics](13-generics.md)).

### Builtin Types (stack-allocated)

| Type    | Description                         |
|---------|-------------------------------------|
| `int`   | 64-bit signed integer (also spelled `i64`) |
| `float` | 64-bit IEEE 754 floating-point (also spelled `f64`) |
| `unsigned` | 64-bit unsigned integer (also spelled `u64`) |
| `i8`, `i16`, `i32` | 8-, 16- and 32-bit signed integers |
| `u8`, `u16`, `u32` | 8-, 16- and 32-bit unsigned integers |
| `f16`, `f32` | 16- and 32-bit IEEE 754 floating-point |
| `bool`  | Boolean — literals `True` / `False` |
| `char`  | 8-bit character (a single byte) — literals `'a'`, `'\n'` |
| `void`  | Function return type only           |

`void` has no value: a call of a function that returns nothing is a statement, never the
value of a variable, a parameter, a field, a type argument, an element, a `match` subject
or a `return` (`variable 'x' cannot have type 'void' (the expression produces no value)`,
`type argument 1 of 'id' cannot be 'void': it has no value (it is a function's result type
only)`).

Note the capitals in `True` and `False`. An `int` converts to a `float` implicitly when
assigned or passed where a `float` is expected (`f: float = 3;` is fine), but nothing else
converts implicitly: no other numeric type converts, a `float` never silently becomes an
`int`, and neither ever becomes a `bool`.

`Str` is a built-in heap-allocated class type: string literals produce one, `+` joins two of
them, `s.len()` is the length and `s[i]` the `char` at index `i`. [Strings and
conversions](04-strings-and-conversions.md#string-type) covers it.

### Printing values

`io::print` and `io::println` take **one** argument of any type that can show itself as
text: a `Str`, a number, a `bool`, a `char`, an object, and the tuples, structs and
optionals of later chapters. To print several pieces on one line, convert the numbers to
strings with `Str(...)` and join the pieces with `+`:

```pkn
import ::io;

fn main() -> int {
  n = 6;
  io::println("n squared is " + Str(n * n));
  io::print("no newline, ");
  io::println("then a newline");
  return 0;
}
```

Output:

```
n squared is 36
no newline, then a newline
```

`io::println(n)` prints a number on its own; joining it to a string needs `Str(n)`, since
`+` joins two strings. `io::printerr` and `io::printerrln` work the same way but write to
standard error. [Strings and conversions](04-strings-and-conversions.md) covers `Str(...)`,
the other conversions and the printing functions in full.

### Sized numeric types

`i8`, `i16`, `i32`, `u8`, `u16`, `u32`, `unsigned`, `f16` and `f32` are numeric types with a
fixed width, next to `int` and `float` (whose other spellings `i64`, `f64` and `u64` name
the same three types: `int` and `i64` are one type, printed as `int`, as are `float` and
`f64`, and `unsigned` and `u64`, printed as `unsigned`).  Every one of them takes an 8-byte
slot in memory, like an `int`, so a struct, tuple or array of them has the layout it would
have with `int` and `float`; the width is the range of values the type holds.

```pkn
import ::io;

fn main() -> int {
  a: u8 = 250;
  a = a + 10;                        // wraps to 4: a u8 holds 0 to 255
  n: i8 = -128;
  big: unsigned = unsigned(-1);      // 18446744073709551615
  half: f32 = 0.1;
  io::println(Str(a) + " " + Str(n - 1) + " " + Str(big / 2) + " " + Str(half));
  return 0;
}
```

Output:

```
4 127 9223372036854775807 0.1
```

- **One type per operation.** The two operands of `+ - * / %` and of a comparison must
  have the same type; the one promotion is `int` with `float`.  `x + y` with `x: i32` and
  `y: int` is an error (`operator '+' is not defined for types 'i32' and 'int'`): write
  `x + i32(y)` or `int(x) + y`.  The result has the operands' type, and the integer result
  **wraps** to the type's width, as `int` wraps at 64 bits: `u8` arithmetic is modulo 256,
  `i8` arithmetic keeps the low 8 bits as a signed value.  `/` and `%` by zero panic as for
  `int`; `/`, `%` and the comparisons of an `unsigned` are unsigned (`unsigned(-1) / 2` is
  `9223372036854775807`, `unsigned(-1) > 1` is `True`).  Unary `-` is not defined on the
  unsigned types.
- **Literals take the type the context expects.**  An integer literal (and `-literal`)
  flowing into a slot of a sized integer type — a variable with a declared type, a
  parameter, a field, an element, a `match` arm, or the other operand of an operator — has
  that type when it fits it (`x: u8 = 255`, `x + 1`, `x == 0`), and is an error when it does
  not (`the literal 256 does not fit in 'u8' (0 to 255)`).  A float literal (or an integer
  literal) flowing into an `f32` or `f16` slot is that float: an integer literal rounded to
  the type once, as `f32(n)` is, and a float literal as its text is (see [Parsing a
  string](04-strings-and-conversions.md#parsing-a-string)).  One that rounds to infinity in
  the type is an error (`the literal 1e+39 does not fit in 'f32'`), the midpoint above the
  largest value included (`3.4028235677973366e38` for `f32`, `65520` for `f16`, which round
  to even, to infinity); one below the type's range is 0 or a subnormal of its sign, and no
  error (`h: f16 = 1e-9;` is `0`, `t: f32 = -1e-50;` is `-0`).  Only a literal adapts: `100 +
  100` is an `int` expression, so `q: i8 = 100 + 100;` is an error.  Without a context an
  integer literal is an `int` and a float literal a `float`.
- **`f16` and `f32` arithmetic** is computed in `float` precision and rounded to the type
  after each operation, which gives the correctly rounded `f32` or `f16` result;
  comparisons are IEEE as for `float`.  A value too large for the type is its infinity:
  `f16(70000.0)` is `inf`, and `65504` is the largest finite `f16`.
- **Conversions** between the numeric types use the conversion constructors, each type
  taking the sources of its family (`u8(300)` is `44`, `i8(200)` is `-56`,
  `i32(u32(unsigned(-1)))` is `-1`, `i32(-1.5)` is `-1`, `f16(0.1)` is `0.0999756`,
  `u8('a')` is `97`): see [Between numbers](04-strings-and-conversions.md#between-numbers).
  A signed type takes no `unsigned` (`u.toInt()` is its value as an `int?`) and an unsigned
  type no float (`u8(int(f))`).
  `Str(x)` prints any numeric value (an `unsigned` without a sign), and `s.toU8()`,
  `s.toUnsigned()`, `s.toF32()` … parse a `Str` into a `u8?`, `unsigned?`, `f32?` …, `None`
  when the text is not a number of that type (`"256".toU8()` and `"+1".toU8()` are `None`):
  see [Parsing a string](04-strings-and-conversions.md#parsing-a-string).  An `f32` or `f16`
  from a text with a fraction or an exponent, a literal's or a parse's, is the `float`
  nearest the text rounded to the type.
- **Methods and traits.**  Every numeric type has `toString()` and is `Show` and
  `ValueType`; the integer types have `hash()` and are `Hashable`, so a `u32` is a
  `HashMap` key; an `unsigned` also has `u.toInt()`, its value as an `int?` (`None` above
  9223372036854775807).  A sized type has an optional (`u8?`), is a `match` subject with literal
  arms, and is a type argument like any value type (`Box<u8>`, `List<f32>`).
- **Bitwise operators.**  The unsigned types have `&`, `|`, `^`, `~`, `<<` and `>>`; the
  signed and float types have none ([Bitwise operators](#bitwise-operators)).
- **Not yet:** literal suffixes (`1u8`) and implicit widening (`i32` to `int`).  The design
  is in [`design/sized-types.md`](../design/sized-types.md).

### Arrays

`T[n]` holds exactly `n` elements of type `T`, stored in the variable itself. An array
literal is an array of its own length. See [Arrays](09-arrays.md) for the full array system.

```pkn
a: int[5] = [1, 2, 3, 4, 5];
b = [1, 2, 3];                      // an int[3]
z: int[8] = [0; 8];                 // eight copies of 0
mat: int[2][2] = [[1, 2], [3, 4]];  // 2D
```

### Tuples

`(T1, T2, ...)` groups a fixed number of values of possibly different types. Like an array,
a tuple is a value held in its variable (or field, or array element) and copied on
assignment; it is immutable and is not an `Obj`. See [Tuples](10-tuples.md).

```pkn
p: (int, Str) = (1, "one");
q, name = p;                        // destructuring
first: int = p.0;                   // element access
```

### Structs

`struct Name { field: Type; ... }` declares a value type with named fields. Like a tuple,
a struct is held in its variable (or field, or array element) and copied on assignment;
unlike a tuple's elements, its fields are named and can be assigned in place. A struct has
no superclass and no vtable; its methods come from `impl` blocks. See
[Structs](11-structs.md).

```pkn
struct Point { x: int; y: int; }
p = Point(1, 2);                    // the fields in order
p.x = 5;                            // assigned in place
io::println(p);                     // Point(x: 5, y: 2)
```

### Function types

`(T1, T2) -> R` is the type of a function value: a lambda, `fn(x: int) -> int { return
x + 1; }`, or a named function used without calling it. A function value is a value, held
in place like a tuple: its code and its closure, the variables the lambda captured (by
copy). It is called like a function. See [Function types and
lambdas](14-function-types.md).

```pkn
square = fn(x: int) -> int { return x * x; };
apply: ((int) -> int, int) -> int = fn(f: (int) -> int, x: int) -> int { return f(x); };
n = apply(square, 3);               // 9
```

### Class Types (vtable-bearing, heap-allocated, reference-counted)

`Obj` is the root of the class hierarchy. Every class transitively extends it
([Classes](05-classes.md)). It defines:

| Slot       | Signature                       | Default              |
|------------|---------------------------------|----------------------|
| `toString` | `view fn toString() -> Str`     | Returns `Object@<address>` (a per-class name is planned) |
| `equals`   | `view fn equals(other: Obj) -> bool` | Identity comparison  |
| `destroy`  | (compiler-generated)            | Destructor — final, runs on last release |

`None` is an `Obj` literal representing the absence of a value.

### Optional Types

Any reference type `T` (a class or `Str`), the primitives `int`, `float`, `bool`, `char`
and the sized numeric types, and an enum, a tuple, a struct or a function value of at most
64 bytes have an optional form `T?` that holds either a `T` or `None`. A `T` converts to
`T?` implicitly; a `T?` is unwrapped with `match`. See [Optionals](12-optionals.md).

```pkn
n: Node? = None;
fn find(head: Node?, key: int) -> Node? { ... }
```

`Obj` and `Str` are the only builtin classes. Files, standard input and errors come from
the standard library: `io::File` is an open file, `io::stdin()` the program's standard
input, and `err::Error` (module `::error`) says what went wrong when an operation fails,
inside a `res::Result<T, err::Error>` (module `::result`) when the operation makes a value
(see [`::io`](../stdlib/io.md), [`::error`](../stdlib/error.md) and
[`::result`](../stdlib/result.md)). A read returns a `res::Result<Str, err::Error>`, an
[enum](07-enums.md#generic-enums) that is `Ok` with the line or `Err` at the end of the
input, so `match` on it to tell the two apart:

```pkn
import ::io;

fn main() -> int {
  n: int = 0;
  while True {
    match io::stdin().readln() {
      line: Ok { n = n + 1; }
      _        { break; }        // Err — the end of the input
    }
  }
  io::println(Str(n) + " lines");
  return 0;
}
```

Output (with nothing on standard input):

```
0 lines
```

See `samples/codegen/23_stdin.pkn`.

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
if d == Direction::East { io::println("heading east"); }
```

See [Enums](07-enums.md) for the full treatment, and [match](08-match.md) for matching on
enums.

---

## Expressions & Operators

### Literals

| Literal        | Type    | Examples          |
|----------------|---------|-------------------|
| Integer        | `int` (or the sized integer type the context expects) | `0`, `42`, `-17`, `0xFF`, `0b1010`, `0xFFFF_0000` |
| Floating-point | `float` (or `f32` / `f16` when expected) | `3.14`, `0.5`, `1e9` |
| Boolean        | `bool`  | `True`, `False`   |
| Character      | `char`  | `'a'`, `'\n'`, `'\t'` |
| String         | `Str`   | `"hello"`, `""`   |
| None           | `Obj`   | `None`            |

An integer literal is written in decimal (`255`), in hexadecimal after `0x` (`0xFF`, with
digits of either case) or in binary after `0b` (`0b1111_1111`). A `_` may stand between
two digits of a hexadecimal or binary literal, to group them (`0xFFFF_0000`). In every
base an integer literal is 0 to `18446744073709551615`; a larger one is a compile-time
error (`integer is out of range: ...`). Its type comes from the context, as described in
[Sized numeric types](#sized-numeric-types), and a literal above `9223372036854775807`
needs an unsigned or float context: `m: unsigned = 0xFFFF_FFFF_FFFF_FFFF` is fine, while in
an `int` slot, or with no context, it is an error that names the unsigned context. As the
argument of a conversion it is an `unsigned`: `u8(0xFFFF_FFFF_FFFF_FFFF)` keeps its low
bits, `255`. A hexadecimal literal is a value, not a bit pattern: `y: i8 = 0xFF` is `the
literal 255 does not fit in 'i8' (-128 to 127)`, and the reinterpretation is spelled
`i8(u8(0xFF))`, which is `-1`.

```pkn
import ::io;

fn main() -> int {
  let mask: u32 = 0xFFFF_0000;
  let bits: u8 = 0b1010_0101;
  let top: unsigned = 0xFFFF_FFFF_FFFF_FFFF;
  io::println(Str(mask) + " " + Str(bits) + " " + Str(top));
  io::println(0xFF.toString() + " " + Str(u8(0xFFFF_FFFF_FFFF_FFFF)) + " " + Str(i8(u8(0xFF))));
  return 0;
}
```

Output:

```
4294901760 165 18446744073709551615
255 255 -1
```

The prefixes are lower case (`0XFF` is an error that says so), and there are no octal
literals: `0o17` is an error, and `017` is the decimal `17`. A `_` in a decimal literal is
not a separator (`1_000` is an error), and a literal has no suffix: `255u8` is an error
that shows the context to write instead, `x: u8 = 255`. A hexadecimal or binary literal
never takes a `.`: `0xFF.toString()` calls a method of `255`, `0x10..0x20` is a range, and
`0x1.5` is an error.

`-17` is unary minus applied to the literal `17`, so the most negative `int` cannot be
written directly: `-9223372036854775808` is an error that says to compute it as
`-9223372036854775807 - 1`. A `match` pattern is the exception: there `-17` is one literal,
and `-9223372036854775808` is the most negative `int` ([Match](08-match.md#matching-values)).

A float literal must name a finite `float`. One too large (`1e999` would be infinity) or too
small (`1e-400`: not zero, but rounds to zero) is a compile-time error. Subnormal values
such as `5e-324` are fine, as is zero written with any exponent (`0e-999`). Infinities and
NaN are computed, not written: `1.0e308 * 10.0`, `0.0 * (1.0e308 * 10.0)`. In an `f32` or
`f16` slot the literal is then rounded to that type: one that rounds to infinity there is an
error, and one below its range is 0 or a subnormal, without one ([Sized numeric
types](#sized-numeric-types)).

### Arithmetic

| Op  | Meaning        | Notes                              |
|-----|----------------|------------------------------------|
| `+` | Addition       | Numeric, or `Str` concatenation    |
| `-` | Subtraction    | Numeric only                       |
| `*` | Multiplication | Numeric only                       |
| `/` | Division       | Numeric only                       |
| `%` | Modulo         | Numeric only                       |

Both operands have one numeric type, and the result has that type; the one exception is
`int` with `float`, computed as `float`.  The sized types (`u8`, `i32`, `unsigned`, `f32`,
…) wrap or round to their width ([Sized numeric types](#sized-numeric-types)). Integer
division truncates toward zero, and `%` takes the sign of the left operand:

```pkn
import ::io;

fn main() -> int {
  io::println(Str(7 / 2) + " " + Str(-7 / 2) + " " + Str(7 % 3) + " " + Str(-7 % 3));
  io::println(Str(7.0 / 2) + " " + Str(2 + 3 * 4) + " " + Str((2 + 3) * 4));
  io::println(Str(7.5 % 2.0));
  return 0;
}
```

Output:

```
3 -3 1 -1
3.5 14 20
1.5
```

**Integer arithmetic** is 64-bit two's complement and behaves the same on every backend:

- `+`, `-`, `*` and unary `-` **wrap** on overflow: `9223372036854775807 + 1` is
  `-9223372036854775808` (`INT64_MAX + 1 == INT64_MIN`), and `-INT64_MIN` is `INT64_MIN`.
- `/` truncates toward zero and `%` takes the sign of the dividend: `-7 / 2` is `-3`,
  `-7 % 2` is `-1`, `7 % -2` is `1`, `7 / -2` is `-3`.
- `/` or `%` by zero **panics** with `integer division or modulo by zero`.
- `INT64_MIN / -1` (whose result does not fit) **panics** with `integer overflow in division`;
  `INT64_MIN % -1` is `0`.

A panic prints `paykan: <message>` to stderr and aborts the program (see the panic notes
under [Conversions](04-strings-and-conversions.md#conversions)).

**Float arithmetic** is IEEE 754 `double`: `%` is C's `fmod` (`7.5 % 2.0` is `1.5`,
`-7.5 % 2.0` is `-1.5`), and dividing by zero gives an infinity or NaN rather than panicking
(`1.0 / 0.0` is `inf`).

### Bitwise operators

| Op | Meaning | Operands | Result |
|----|---------|----------|--------|
| `a & b` | bitwise and | one unsigned type | that type |
| `a \| b` | bitwise or | one unsigned type | that type |
| `a ^ b` | exclusive or | one unsigned type | that type |
| `~a` | complement within the type's width | an unsigned type | `a`'s type |
| `a << n` | shift left; the bits shifted out are lost | `a` unsigned, `n` any unsigned type | `a`'s type |
| `a >> n` | logical shift right: zeros come in | `a` unsigned, `n` any unsigned type | `a`'s type |

The bitwise operators are defined on the unsigned types only, `u8`, `u16`, `u32` and
`unsigned` (`u64`), and each has a compound assignment: `&=`, `|=`, `^=`, `<<=` and `>>=`
([Compound assignment](#compound-assignment)). `~` is the complement; `!` stays the `bool`
negation. Hexadecimal and binary literals ([Literals](#literals)) are the natural spelling
of their operands.

```pkn
import ::io;

fn main() -> int {
  flags: u8 = 0b0000_0101;
  flags |= 0b1000_0000;
  flags &= ~0b0000_0001;
  io::println(Str(flags));
  let word: u32 = 0x1234_5678;
  io::println(Str((word & 0xFF00) >> 8));
  let n: unsigned = 70;
  io::println(Str(unsigned(1) << n));
  io::println(Str(word & 0xF == 8));
  return 0;
}
```

Output:

```
132
86
0
True
```

**Operand types.** `&`, `|` and `^` take two operands of one type, as `+` does: `x & y` with
`x: u8` and `y: u16` is an error that says to convert one. A literal operand takes the other
operand's type (`x & 0xF0` with `x: u8` is a `u8`, and `x & 0x100` is `the literal 256 does
not fit in 'u8' (0 to 255)`). A shift's count may have any unsigned type, of any width, and
the result has the value's type: `x << n` with `x: u8` and `n: unsigned` is a `u8`. A
literal count is an `unsigned`, so `x << 3` needs no conversion.

A signed, `char`, `bool` or float operand is an error whose hint names the fix: the
unsigned type of the same width, `&&`, `||`, `!=` or `!` for a `bool`, `unsigned(...)` for a
shift count.

```pkn
fn main() -> int {
  let n = 6;
  let m = n & 3;
  return 0;
}
```

Error:

```
error: operator '&' is not defined for types 'int' and 'int' (the bitwise operators take unsigned operands: convert with 'unsigned(...)')
```

`i32 ^ i32` adds `'^' is exclusive or, not a power`, and `x << i` with `i: int` is `(a shift
count is unsigned: convert it with 'unsigned(...)')`. A shift's result has its value's type,
so a signed value names the unsigned type of its own width whatever the count: `i << n` with
`i: i32` and `n: u8` is `convert with 'u32(...)'`. A value goes back to `int` with `int(x)`
for a `u8`, `u16` or `u32`, and with `u.toInt()`, an `int?`, for an `unsigned` ([Sized
numeric types](#sized-numeric-types)).

**Literals.** Only a literal adapts to its context, so two literals are two `int`s, which
have no bitwise operator: `m: u32 = 1 << 5` is an error, and `u32(1) << 5` is the spelling.
The shifted value takes no type from the count either: `1 << n` with `n: u8` asks for the
value's type, `unsigned(1) << n`. `~` directly over a literal takes an unsigned context's
type, as `-5` takes a signed one's: `y: u8 = ~0` is 255, `z: unsigned = ~0` is
18446744073709551615, and `x & ~0x0F` with `x: u8` is `x & 240`. With no unsigned context,
`~5` is `~` of an `int`, an error.

**Shifting by the width or more gives 0.** `x << n` and `x >> n` are 0 for every count `n`
at or above the width of `x`'s type, up to 18446744073709551615, on every backend; nothing
panics, and the bits shifted out are lost as unsigned arithmetic wraps.

| Type | Width | 0 for counts |
|------|-------|--------------|
| `u8` | 8 | 8 to 18446744073709551615 |
| `u16` | 16 | 16 to 18446744073709551615 |
| `u32` | 32 | 32 to 18446744073709551615 |
| `unsigned` | 64 | 64 to 18446744073709551615 |

A literal count at or above the width is valid and always 0, so the compiler warns: `x << 8`
with `x: u8` is `warning: the shift count 8 is not less than the width of 'u8' (8 bits): the
result is always 0`. `>>` is logical: an unsigned value has no sign to copy, and there is no
`>>>`.

**Precedence.** The operators take Swift's precedence ([Operator
Precedence](#operator-precedence-highest---lowest)): `<<` and `>>` above `*`, `&` with `*`,
`|` and `^` with `+`, all above the comparisons. Shifts do not chain (`a << b << c` is an
error; write `(a << b) << c`). The grouping differs from C's:

| Source | Paykan | C |
|--------|--------|---|
| `a + b << 2` | `a + (b << 2)` | `(a + b) << 2` |
| `1 << n - 1` | `(1 << n) - 1` | `1 << (n - 1)` |
| `a & b * c` | `(a & b) * c` | `a & (b * c)` |
| `a + b & c` | `a + (b & c)` | `(a + b) & c` |
| `a \| b + c` | `(a \| b) + c` | `a \| (b + c)` |
| `a \| b ^ c` | `(a \| b) ^ c` | `a \| (b ^ c)` |
| `x & m == 0` | `(x & m) == 0` | `x & (m == 0)` |

**Idioms.** `x & (n - 1)` is `x % n` for a power of two `n`. A count masked with `& 63`
(`x << (n & 63)`) is always below 64, and the rotate `(x << (k & 63)) | (x >> ((0 - k) &
63))` of an `unsigned` compiles to one rotate instruction.

### Unary

| Op  | Meaning     | Type          |
|-----|-------------|---------------|
| `-` | Negation    | `int`, `float` and the signed sized types (not `unsigned`, `u8`, `u16`, `u32`) |
| `!` | Logical NOT | `bool`        |
| `~` | Complement  | `u8`, `u16`, `u32`, `unsigned` ([Bitwise operators](#bitwise-operators)) |

### Relational & Equality

`<`, `>`, `<=`, `>=`, `==`, `!=` — result is `bool`.
The two operands have one type, as for arithmetic, with the one promotion: an `int` and a
`float`, in either order, compare as floats (`2 == 2.0` and `2 <= 2.0` are `True`, `2 !=
2.5` is `True`).  Otherwise equality takes two classes when one is a subclass of the other,
and anything else is an error (`operands of '==' have mismatched types 'Str' and 'int'`),
two tuples whose elements differ so too (`(1, 2) == (1, 2.0)`).  A `char` compares with a
`char` only, by `<` as by `==`: `c < 100` is `operands of '<' have mismatched types 'char'
and 'int'`, so write `c < 'd'`, or `int(c) < 100` for its byte value.

For **reference types** (classes, `Str`), `==` dispatches to `equals`: `a == b` invokes
`a.equals(b)` through the vtable, and `a != b` is its negation. A class may override `equals` to
define what equality means for it; the default (inherited from `Obj`) compares object identity, so
two separately-constructed instances are unequal unless the class overrides `equals`. `Str`
overrides `equals` to compare by content, so `"ab" == "ab"` is `True`. For **value types**
(`int`, `float`, `bool`, `char`, the sized numeric types, enums) `==` compares the values
directly. Arrays have no `==` (see [Arrays](09-arrays.md)).

Float comparisons follow IEEE 754: NaN is unequal to everything, itself included (with `nan`
a NaN-valued `float` such as `0.0 * (1.0e308 * 10.0)`, `nan == nan` is `False`, `nan != nan`
is `True`, and `<`, `>`, `<=`, `>=` with a NaN operand are `False`), and `-0.0 == 0.0` is
`True`. Tuples compare element by element with the same rule, so two tuples holding a NaN
(`(1, nan) == (1, nan)`) are unequal, and a tuple holding a NaN is unequal to every tuple,
itself included (`t == t` is `False`, `t != t` is `True`), consistent with float semantics.

Comparisons do not chain: `1 < x < 10` is a syntax error, so write `1 < x && x < 10`
([Operator Precedence](#operator-precedence-highest---lowest)).

### Logical

| Op     | Meaning     | Short-circuits |
|--------|-------------|----------------|
| `&&`   | Logical AND | Yes (stops if left is `False`) |
| `\|\|` | Logical OR  | Yes (stops if left is `True`)  |

`&&` and `||` evaluate their right side only when it is needed:

```pkn
import ::io;

fn main() -> int {
  x = 7;
  inRange = 1 < x && x < 10;
  odd = x % 2 != 0;
  io::println(Str(inRange) + " " + Str(odd) + " " + Str(!odd || x > 100));
  return 0;
}
```

Output:

```
True True False
```

### Ternary Expression

The conditional expression `if c then a else b` evaluates to `a` when `c` is true and to `b`
otherwise. It chains naturally:

```pkn
import ::io;

fn main() -> int {
  score = 84;
  grade = if score >= 90 then "A" else if score >= 80 then "B" else "C";
  io::println("grade: " + grade);
  return 0;
}
```

Output:

```
grade: B
```

- Condition must be `bool`.
- Both branches must have the same type, with two exceptions: class types unify to their
  nearest common ancestor, and a `None` branch makes the result the other branch's optional
  (`if c then 1 else None` is an `int?`). `int` and `float` do not promote here:
  `if c then 1 else 2.5` is an error (`ternary branches have incompatible types 'int' and
  'float'`).
- Right-associative; can be nested. A nested one is written `else if`, not `elif` (`elif`
  belongs to the `if` statement, [if / elif / else](#if--elif--else)).
- It has the lowest precedence of all, so as an operand it needs parentheses:
  `bar + (if done then "#" else "-")`.

### `mov`

`mov` is a reserved word, not an operator: write `t = s`, not `t = mov s` (an error,
`'mov' was removed in v0.2.0; ownership transfers are inferred`). Ownership transfers are
inferred by the compiler; see [Ownership transfers](16-memory.md#ownership-transfers).

### Operator Precedence (highest -> lowest)

| Level | Operators                              | Associativity  |
|-------|----------------------------------------|----------------|
| 1     | postfix: `.method()`, `.field`, `f()`, `[i]`, `.0` | Left |
| 2     | `!`, `-`, `~` (unary)                  | Right (prefix) |
| 3     | `<<`, `>>`                             | **Non-associative** |
| 4     | `*`, `/`, `%`, `&`                     | Left           |
| 5     | `+`, `-`, `\|`, `^`                    | Left           |
| 6     | `<`, `>`, `<=`, `>=`, `==`, `!=`       | **Non-associative** |
| 7     | `&&`                                   | Left           |
| 8     | `\|\|`                                 | Left           |
| 9     | `if…then…else` (ternary)               | Right          |

Comparison and equality operators do **not** chain: `1 < 2 < 3` and `1 < 2 == True` are syntax
errors (`relational operators cannot be chained; parenthesise the comparison`) — parenthesize
instead, e.g. `(1 < 2) == True`. Shifts do not chain either: `a << b << c` is `shift operators
cannot be chained; parenthesise the shift`. The bitwise operators follow Swift, not C, so `x &
m == 0` is `(x & m) == 0` ([Bitwise operators](#bitwise-operators) lists where the two
differ). Parenthesise whenever in doubt.

### Evaluation order

An expression is evaluated left to right, whatever the precedence of its operators:

- a binary operator's left operand before its right one; `&&` and `||` evaluate their right
  one only when it is needed ([Logical](#logical));
- a call's arguments in order, after the function it calls (`pick()(a, b)` calls `pick`
  first), and a method's receiver before its arguments;
- a subscript's base before its index;
- the elements of a tuple or an array literal in order;
- in an assignment, the place assigned (the object whose field it is, or the array and the
  index of an element) before the value. A [compound assignment](#compound-assignment)
  evaluates its target twice.

A ternary evaluates its condition, then only the branch it chooses.

```pkn
import ::io;

fn tag(name: Str, v: int) -> int {
  io::println(name);
  return v;
}

fn main() -> int {
  xs = [10, 20, 30];
  n = tag("a", 1) + tag("b", 2) * tag("c", 3);
  xs[tag("index", 0)] = tag("value", n);
  io::println(Str(xs[0]));
  return 0;
}
```

Output:

```
a
b
c
index
value
7
```

A value read before a call is the one used, even when the call replaces what the variable
holds ([Exclusivity](03-functions.md#exclusivity)). That holds for a value held in memory too,
a struct, a tuple, a fixed-size array or a tagged union, and for the elements a slice
parameter names: passed to a parameter (plain, `view` or a slice, a slice parameter passed on
to another too), compared with `==` or `!=`, or the receiver of a struct's `view fn`, it is the
value it had where it was evaluated, whatever a later argument or the right operand writes to
where it came from: through an `inout` argument, a method that may change it, or a call given
the object it is in. The receiver of a struct's method that may change `self` is its storage,
as an `inout` argument is: the method sees what the arguments wrote to it. A `view` or slice
parameter and the receiver of a struct's `view fn` are borrows, which no later argument may
write through an `inout` argument or a method that may change it
([Exclusivity](03-functions.md#exclusivity)): what they are given is the value read when a
later call writes it through the object it is in, or through a function value.

```pkn
import ::io;

struct Point {
  x: int;
  y: int;
}

fn shift(p: inout Point) -> int {
  p = Point(p.x + 10, p.y);
  return 0;
}

fn show(p: Point, n: int) -> Str {
  return "(" + Str(p.x) + ", " + Str(p.y) + ")";
}

fn main() -> int {
  p = Point(1, 2);
  io::println(show(p, shift(p)));               // p as it was before shift
  io::println(Str(p == Point(shift(p) + 11, 2))); // (11, 2) == (11, 2)
  io::println(show(p, 0));
  return 0;
}
```

Output:

```
(1, 2)
True
(21, 2)
```

The value read is copied only where a later operand may write it. A struct, a tuple, a
fixed-size array or a tagged union is copied on the stack, as a plain parameter's value is.
The elements a slice parameter names are copied to the heap, as many as the slice has when
the program runs, each time the call runs. So the compiler warns where a slice passed on is
copied, naming the call in a later argument that may write its elements and the call it is
passed to:

```pkn
import ::io;

class Counts {
  xs: int[3];
}

fn total(xs: view int[], x: int) -> int => xs[0] + xs[1] + xs[2] + x;

fn reset(c: Counts) -> int {
  c.xs = [0, 0, 0];
  return 0;
}

fn sum(xs: view int[], c: Counts) -> int => total(xs, reset(c)); // xs as passed

fn main() -> int {
  let c = Counts([1, 2, 3]);
  io::println(Str(sum(c.xs, c)) + " " + Str(sum(c.xs, c)));
  return 0;
}
```

Output:

```
6 0
```

`paykan counts.pkn` prints this on standard error before the program runs, with the line
and the lines around it and a caret under `xs`, as for an error:

```
counts.pkn:14:51: warning: 'reset' in a later argument may write the elements of slice 'xs': they are copied to the heap before the call to 'total'
```

The calls are named as an error names them: a function, a method or a constructor by its
name (`'reset'`, and `'first<int>'` for an instantiation of a generic function), a variable
holding a function value by the variable's (`'f'`), an `==` whose `equals` may write as
`'=='`, and a call of another expression as `a function value`. In another module's generic
function, that module's functions are named as its file writes them, without the module,
and those of the modules it imports by their full names, as an error in the instantiation
names them (`'lib::sl::inner::wipe'` where the file writes `q::wipe`).

The warning comes once for each place a slice is copied, however many instantiations of a
generic function reach it, and no option turns it off. A call that need not see the
elements as they were can be made first (`let k = reset(c);`, then `total(xs, k)`), which
copies nothing and reads what `reset` wrote. The other copies are not warned about, and neither is
a slice that no later operand may write. The warning comes from compiling the program, not
from checking it: `--check-only` does not print it, and an imported module read from its
cache entry or from a prebuilt module file is not compiled again, so its warnings come only
when it is compiled from its source. An instantiation of another module's generic function
or class is compiled with the module that uses it, and warns when that module is compiled
([The paykan command](17-the-paykan-command.md)).

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
variable += expression;              // compound assignment: also -= *= /= %= &= |= ^= <<= >>=
return expression;                   // return (or bare return; for void)
;                                    // empty statement
{ statements... }                    // block (new scope)
```

`name = expression;` declares `name` when no variable of that name is in scope, and assigns
to it otherwise ([Scoping Rules](#scoping-rules)).

### Compound assignment

`x += e` is shorthand for `x = x + e`, and `-=`, `*=`, `/=` and `%=` likewise for `-`,
`*`, `/` and `%`, as `&=`, `|=`, `^=`, `<<=` and `>>=` are for the [bitwise
operators](#bitwise-operators) (`flags |= 0b100`, `x >>= 1`). The target is any target of
`=`: a variable, a field (`p.x += 1`) or an element (`xs[i] *= 2`). It is exactly the longer
assignment, so the same rules apply: the operator must be defined for the types (`s += "!"`
appends to a `Str`, `s -= "!"` is `operator '-' is not defined for types 'Str' and 'Str'`,
and `i >>= 1` with `i: int` is the `>>` error), a `let` variable or a `view` parameter cannot
be the target, and a sized integer wraps as `+` does. An error in the target is reported
once, though the target is read as well as written (`xs[y] += 1` with no `y` is one `use of
undeclared variable 'y'`), and the value's own errors still are (`xs[y] += z` with no `z`
is two). A target that names no variable or no field ends the check, as for `=`: `q.n += z`
with no `q` is only the error for `q`.

```pkn
import ::io;

fn main() -> int {
  total = 0;
  for i in 1..=4 { total += i; }
  k = 2;
  k *= k + 1;                        // k = k * (k + 1): the right side is one operand
  label = "total";
  label += ": " + Str(total);
  io::println(label + ", k = " + Str(k));
  return 0;
}
```

Output:

```
total: 10, k = 6
```

The target is written once but evaluated twice, once to read it and once to assign it:
`xs[next()] += 1` calls `next` twice. Name the index first (`i = next(); xs[i] += 1;`) when
that matters.

There are no increment operators (`++`): write `i += 1`.

---

## Control Flow

### if / elif / else

The condition must be a `bool`. It is written without parentheses; the body's `{` ends it
(parentheses around it are allowed, as around any expression). The branches are always
blocks in braces, and further conditions are tested with `elif`:

```pkn
import ::io;

fn main() -> int {
  temperature = 23;
  if temperature > 30 {
    io::println("hot");
  } elif temperature > 15 {
    io::println("pleasant");
  } else {
    io::println("cold");
  }
  return 0;
}
```

Output:

```
pleasant
```

A chain continues with `elif`, not `else if`; `else if` belongs to the conditional
expression (`if x > 0 then 1 else if x == 0 then 0 else -1`), and each form rejects the
other's spelling (`an if-statement spells 'else if' as 'elif'`). Both `elif` and `else` are
optional.

### while

`while` runs its body for as long as its condition, a `bool`, is true:

```pkn
i: int = 0;
while i < 5 {
  i = i + 1;
}

while True {   // infinite loop
  // ...
}
```

### for

`for` counts through a range of integers. `for i in a..b` gives `i` the values `a`,
`a + 1`, ..., `b - 1`; `a..=b` includes `b` too. `step` sets the distance between the
values, and a negative step counts down. Write `_` for the variable when the body does
not use it (`for _ in 0..n`):

```pkn
import ::io;

fn main() -> int {
  up = "";
  for i in 0..5 { up = up + Str(i) + " "; }
  evens = "";
  for i in 0..=10 step 2 { evens = evens + Str(i) + " "; }
  down = "";
  for i in 3..0 step -1 { down = down + Str(i) + " "; }
  io::println(up + "| " + evens + "| " + down + "|");

  bytes = 0;
  for _: u8 in 0..=255 { bytes = bytes + 1; }   // every u8, then the loop ends
  io::println(bytes);
  return 0;
}
```

Output:

```
0 1 2 3 4 | 0 2 4 6 8 10 | 3 2 1 |
256
```

What a loop does is fixed when it starts:

- **The start, the end and the step are evaluated once**, in that order, before the first
  iteration. The body can change the variables they read, and the loop's values stay the
  same: `for i in 0..n { n = 0; }` still runs `n` times.
- **The variable is read-only.** A new `i` is declared for the loop (it hides an outer `i`
  until the loop ends; `_` declares nothing), and the body cannot assign it (`'i' is the variable of a 'for' loop
  and cannot be assigned`) or pass it to an `inout` parameter, so the values are the
  range's. A lambda made in the body captures the value of that iteration.
- **The loop always ends.** The number of iterations is known before the first one, so a
  range reaching the end of its type stops there: `for b: u8 in 0..=255` runs 256 times,
  where `while b <= 255` would run forever (every `u8` is `<= 255`). Only `break` and
  `return` end it sooner.
- **A range can be empty**: when the start is past the end in the direction of the step,
  the body does not run (`for i in 5..0` runs zero times, and so does
  `for i in 0..5 step -1`). It is not an error.

The range is of integers: `int`, a [sized integer type](#sized-numeric-types) or
`unsigned`. Both ends have the same type, as the operands of `<` do: a literal takes the
other end's type (`for i in 0..n` counts in `unsigned` when `n` is one, as it does over a
list's `len()`), and a type written after the
variable (`for b: u8 in 0..=255`) is the type of both. A `float` range is an error (`a 'for'
loop's range is of integers, not 'float'`), as is one whose ends differ (`the ends of the
range have types 'unsigned' and 'int'; a 'for' loop's range has one type`).

The step is an `int`, whatever the type of the range. It may be any expression
(`step k / 2`). A step of `0` is an error when it is a constant (`a 'for' loop's step cannot
be 0`); a computed one that is `0` panics when the loop starts
(`the step of a 'for' loop is 0`). The last value is the last one the step reaches without
passing the end: `0..=10 step 3` gives 0, 3, 6 and 9. `in` and `step` mean something only
in a `for` header, so a variable may still be called `in` or `step`.

### break / continue

- `break` — exits the innermost loop, a `while` or a `for`.
- `continue` — skips to the next iteration of the innermost loop; in a `for` loop, to its
  next value.
- Both are a compile-time error outside a loop (`'break' outside of a loop`).

```pkn
import ::io;

fn main() -> int {
  i = 0;
  sum = 0;
  while True {
    i = i + 1;
    if i % 2 == 0 { continue; }  // skip even numbers
    if i > 9 { break; }
    sum = sum + i;
  }
  io::println("sum of odd numbers below 10: " + Str(sum));
  return 0;
}
```

Output:

```
sum of odd numbers below 10: 25
```

### match

`match` is the third way to branch. It inspects one subject and runs the first arm that
applies; how arms compare depends on the subject's static type: the **runtime class** of an
object, the variants of an `enum`, literal values of a builtin value (`int`, `Str`, …), or
whether an optional is present.

```pkn
match <expr> {
  TypeName          { /* no binding */ }
  binding: TypeName { /* bind matched object to name */ }
  _                 { /* catch-all wildcard */ }
}
```

- A type arm matches only an object whose class is exactly the given type (a subclass does
  not match).
- An optional binding (`name: Type`) introduces a local variable in that arm.
- `_` matches anything; must be the last arm.
- `match` is a statement, not an expression.

```pkn
a: Animal = Dog();
match a {
  d: Dog { io::println(d.speak()); }
  Cat    { io::println("cat"); }
  _      { io::println("other"); }
}
```

See [match](08-match.md) for the full treatment and [Enums](07-enums.md) for matching on
enums.

---

## Scoping Rules

- **Lexical scoping**: variables resolve from innermost to outermost scope.
- `{ }` introduces a new scope; a variable declared in a block is not visible after it.
- Each function body is its own scope with parameters pre-declared.
- Inner scopes can shadow outer names via a new declaration (with a type), which hides the
  outer variable until the block ends; assigning without a type assigns to the outer
  variable.
- A local never hides a function of the module: no variable, parameter (a method's or a
  lambda's included), `for` loop variable or `match` binding can take the name of one of
  the module's functions, a generic or a `native` one included, wherever the function is
  declared (`'func' is a function of this module and cannot be used as a variable`, `... as
  a parameter name` at a parameter, once, at the first binding, even when its value has an
  error of its own: the assignments that bind the name again are follow-ons of it). So a
  call by the name
  always means the function. A field may share a function's name (it is
  read as `obj.func`), and so may another module's function, which is always qualified
  (`io::println`).
- A local never hides a method's `self`: in a method's body no declaration, `for` loop
  variable, `match` binding or destructuring target can be named `self`, which always
  names what the method was called on (see [Classes](05-classes.md#declaration)).

```pkn
import ::io;

fn main() -> int {
  x = 1;
  {
    x: int = 10;  // a new x, local to this block
    io::println("inner x = " + Str(x));
  }
  {
    x = 2;        // assigns the outer x
  }
  io::println("outer x = " + Str(x));
  return 0;
}
```

Output:

```
inner x = 10
outer x = 2
```

---

## Builtin Names Are Reserved

Top-level declarations — free functions, classes (a class name is also its constructor),
and enums — share a single namespace with the builtins, and a name identifies exactly one
entity. Declaring a function, class, or enum whose name is a builtin class (`Obj`, `Str`;
`Str` is also the builtin conversion function) or a builtin type (`int`, `float`, `bool`,
`char`, `void` and the [sized numeric types](#sized-numeric-types), under any of their
spellings) is a compile-time error, reported at the declaration:

```pkn
class Obj {}                       // error: 'Obj' is a builtin class and cannot be redeclared
fn Str() -> int { ... }            // error: 'Str' is a builtin class and cannot be redeclared
struct u8 { v: int; }              // error: 'u8' is a builtin type and cannot be redeclared
fn float(x: int) -> int { ... }    // error: 'float' is a builtin type and cannot be redeclared
```

The names of the standard library (`print`, `println`, `open`, `File`, `Error`, ...) are
not builtins: a program may declare its own `fn print` or `class File`. Type names are
global across an import graph, though ([Modules](15-modules.md)), so a program that imports
`::io` cannot also declare a class, struct, enum, trait or function named `File`, `Error`
or `ErrorCode` (`class 'File' conflicts with an imported type of the same name`, `enum
'ErrorCode' ...`). A generic type of a module, such as `Result` or `List`, is named through
its module (`res::Result<int, Str>`) and leaves its name free: a program may declare its
own `Result`.

The same rule applies between user declarations: `fn Point()` next to `class Point` is
rejected with `'Point' is already declared as a class`, and a function or class cannot
reuse an enum's name. A builtin class or type name, or the name of a type the program
declares or imports, cannot name a field (a class's, a struct's or a variant's, whether or
not its type writes `__init__`, and a generic type's once, at its declaration, whether or
not it is instantiated), a parameter or a local variable either. It is reported at the
field, the parameter or the variable, as what it would name: `'Str' is a type name and
cannot be used as a variable` (`... as a parameter name`, `... as a field name`). Nor can a
function of the module name a parameter or a local variable
([Scoping Rules](#scoping-rules)).

---

## Project: the first pieces of `tasks`

Our to-do list manager will need to show how far along the list is. Here are its first two
functions: `percent` computes how many tasks are done, as a percentage, and `progressBar`
draws it with a loop. They take parameters, which `main` did not;
[Functions](03-functions.md) covers them in full.

```pkn
import ::io;

// Percentage of `done` out of `total`, rounded down; 0 for an empty list.
fn percent(done: int, total: int) -> int {
  if total == 0 { return 0; }
  return done * 100 / total;
}

// A bar of `width` cells, filled in proportion to done/total: [####------]
fn progressBar(done: int, total: int, width: int) -> Str {
  filled = if total == 0 then 0 else done * width / total;
  bar = "[";
  i = 0;
  while i < width {
    bar = bar + (if i < filled then "#" else "-");
    i = i + 1;
  }
  return bar + "]";
}

fn main() -> int {
  io::println(progressBar(0, 4, 10) + " " + Str(percent(0, 4)) + "%");
  io::println(progressBar(3, 4, 10) + " " + Str(percent(3, 4)) + "%");
  io::println(progressBar(4, 4, 10) + " " + Str(percent(4, 4)) + "%");
  return 0;
}
```

Output:

```
[----------] 0%
[#######---] 75%
[##########] 100%
```

Notice the parentheses around the conditional expression in `bar + (if ... else "-")`: the
`if` expression has the lowest precedence of all, so without them `bar + if ...` is a syntax
error ([Ternary Expression](#ternary-expression)).

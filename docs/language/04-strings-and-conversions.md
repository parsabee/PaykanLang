# Strings and conversions

Text is everywhere in real programs, and so are conversions between text and numbers. This
chapter covers the `Str` type and the `char` type, printing with `::io`, and the conversion
constructors that turn numbers into text, text into numbers and one numeric type into
another. It ends by adding some text helpers to `tasks`.

---

## String Type

`Str` is the string type: a built-in, heap-allocated, reference-counted class type (a
`final` one: `cannot inherit from 'Str': class is final`). A string literal produces a
`Str`. It is written in double quotes and must close on the line it opens: a raw newline
inside a literal is a compile-time error (`unterminated string literal (string literals
cannot span multiple lines; use \n)`). Special characters are written with escapes:

| Escape | Character        |
|--------|------------------|
| `\n`   | newline          |
| `\t`   | tab              |
| `\r`   | carriage return  |
| `\\`   | backslash        |
| `\"`   | double quote     |
| `\0`   | the zero byte    |

Any other backslash sequence is kept as written, so `"a\qb"` is the four characters `a\qb`.

String literals written next to each other, with only spaces, newlines or comments between
them, are **one** literal, joined when the program is compiled, as in C and C++. A long
string is split across lines this way, at no cost when the program runs: unlike `+`, the
join makes no new string.

```pkn
import ::io;

fn main() -> int {
  io::println("this message is far too long "
              "to fit on one line");
  usage = "usage: tool <file>\n"
          "  -o <file>   output\n"   // a comment between the pieces is fine
          "  -q          quiet\n";
  io::print(usage);
  io::println(Str("ab" "c".len()));
  return 0;
}
```

Output:

```
this message is far too long to fit on one line
usage: tool <file>
  -o <file>   output
  -q          quiet
3
```

Each piece's escapes are decoded on their own, and each piece still closes on its own line.
The joined literal is one value, so `"ab" "c".len()` is `3` and `"ab" "c"[2]` is `'c'`, and
it works as a `match` pattern too. A character literal is not a piece: `"a" 'b'` is an
error.

The operations on strings:

- `a + b` concatenates two `Str` values into a new `Str`. Both must be strings: a `Str` and a
  number are not added (`operator '+' is not defined for types 'Str' and 'int' (convert the
  'int' with 'Str(...)')`), so a number is converted first: `"n = " + Str(n)`;
- `s.len()` is the length, in bytes, as an `int`;
- `s[i]` is the `char` at index `i` (zero-based, checked: an index out of range panics);
- `a == b` and `a != b` compare **contents**: `"ab" == "a" + "b"` is `True`.

```pkn
import ::io;

fn main() -> int {
  first = "Paykan";
  full = first + "Lang";
  io::println(full + " has " + Str(full.len()) + " characters");
  io::println("it starts with " + Str(full[0]) + " and ends with " + Str(full[full.len() - 1]));
  io::println(Str(full == "Paykan" + "Lang"));
  io::println("tab:\t|  quote: \"  backslash: \\");
  return 0;
}
```

Output:

```
PaykanLang has 10 characters
it starts with P and ends with g
True
tab:	|  quote: "  backslash: \
```

A string's characters cannot be assigned: `s[0] = 'x'` is an error (`subscript assignment
on non-array type 'Str'`). To change a string, build a new one with `+`. The ordering
operators (`<`, `>`, `<=`, `>=`) are not defined on strings (`operator '<' is not defined
for types 'Str' and 'Str'`); compare characters instead.

Besides `len()`, a string has the methods every object has, `toString()` (the string
itself) and `equals(o)`, and `hash()` (a `Str` is `Hashable`, so it can be a `HashMap`
key; see [Traits](06-traits.md)). One more method, `s.concat(t)`, appends `t` to `s` in
place. A string is an object, shared by every variable, field or parameter that holds it,
so the change is seen through all of them, while `+` leaves its operands alone; `concat`
cannot be called through a `view` (`'s' is a 'view' parameter; 'concat' is not a 'view
fn'`):

```pkn
import ::io;

fn main() -> int {
  s = "to";
  t = s;              // t and s name one string
  s.concat("do");     // changes that string in place
  u = s + "!";        // a new string; s is unchanged
  io::println(s + " " + t + " " + u);
  return 0;
}
```

Output:

```
todo todo todo!
```

There are no built-in methods for searching, slicing or splitting yet, and the standard
library has none either, but they are short loops over `len()` and `[]`, as
[the project below](#project-text-helpers-for-tasks) shows. How strings are shared and
freed is in [Memory](16-memory.md#strings-arrays-and-tuples).

### Characters and bytes

A `char` is one byte, written in single quotes: `'a'`, `'\n'`, `'\''`, `'\\'`. Characters
compare with `==`, `!=`, `<`, `<=`, `>` and `>=` by their byte value, `0` to `255` as
`int(c)` reads it, which makes range checks such as "is this a digit?" easy. Both sides are
chars: `c < 100` is an error, and `int(c) < 100` compares the byte value:

```pkn
import ::io;

fn isDigit(c: char) -> bool {
  return c >= '0' && c <= '9';
}

fn countDigits(s: Str) -> int {
  n = 0;
  i = 0;
  while i < s.len() {
    if isDigit(s[i]) { n = n + 1; }
    i = i + 1;
  }
  return n;
}

fn main() -> int {
  io::println(Str(countDigits("call 555-0199 before 9pm")));
  return 0;
}
```

Output:

```
8
```

Because a `char` is one byte of a `Str`, a character outside ASCII spans several `char`s in
a UTF-8 string: `"é".len()` is `2`, and `"é"[0]` has code `195`. `int(c)` reads the byte as
an unsigned code `0` to `255`, and `char(n)` is its exact inverse over that range.

---

## Output

Printing is not built in: it is the standard library's [`::io`](../stdlib/io.md) module,
imported with `import ::io;`.

| Function             | Description                             |
|----------------------|-----------------------------------------|
| `io::print(v)`       | Print one value (via `toString`), no newline |
| `io::println(v)`     | Print one value, then a newline         |
| `io::printerr(v)`    | Print to stderr, no newline             |
| `io::printerrln(v)`  | Print to stderr, then a newline         |

Each is `fn print<T: Show>(value: view T)`: it takes **exactly one** argument of any type
that implements `Show` (an object, a `Str`, a number, a `bool`, a `char`, a tuple, a struct
or an optional of one) and writes its `toString()`. Paykan has no variadic functions or
overloading, so there is no multi-argument form: `io::println(n)` prints a number on its
own, and to print several pieces on one line, convert the numbers with `Str(...)` and join
the pieces with `+` (string concatenation).

```pkn
io::println("x = " + Str(x));
io::print("a=" + Str<int>(a) + " b=" + Str(b));
io::println(5);                   // 5
```

`print` and `println` write to standard output, `printerr` and `printerrln` to standard
error; stdout is flushed before a runtime panic, so output printed before one is never lost.
See `samples/codegen/46_printerr.pkn`.

---

## Conversions

To convert a value to another type, call the target type like a function with the value,
`Target(value)`: `Str(42)`, `float(n)`, `int(x)`. These are called **conversion
constructors**.

### To a string

`Str(x)` works for every value that has a text: every type that implements the built-in
trait `Show` ([Traits](06-traits.md)). It gives the text `x.toString()` gives, which is
also what `io::println(x)` prints. A number, a `bool` and a `char` give theirs:

```pkn
import ::io;

fn main() -> int {
  io::println(Str(42) + " " + Str(-7));
  io::println(Str(3.14) + " " + Str(2.0) + " " + Str(1.0 / 3.0) + " " + Str(1e20));
  io::println(Str(True) + " " + Str('z'));
  return 0;
}
```

Output:

```
42 -7
3.14 2 0.333333 1e+20
True z
```

An integer is its decimal digits, with a `-` for negatives (an `unsigned` has no sign). A
`float` is formatted like C's `%g`: at most six significant digits, no trailing zeros, and
an exponent for very large or small numbers; the special values print as `-0`, `nan`,
`inf` and `-inf`. A `bool` is `True` or `False`, and a `char` a one-character string.

Every other value with a text converts the same way:

| Value | Its text |
|-------|----------|
| an optional (`int?`, `Str?`, `Point?`) | `None`, or the value's text |
| `None` | `None` |
| a tuple | its elements in parentheses, a string unquoted: `(1, a, 2.5)` |
| a struct | its name and fields: `Size(w: 2, h: 3)` |
| an enum | its variant, then the variant's fields: `North`, `Circle(r: 1.5)` |
| an object, an `Obj` | its `toString()`, the one of its own class ([Classes](05-classes.md)); a class without one prints `Name@0x...`, its address |
| a `Str` | the string itself |

```pkn
import ::io;

class Point {
  x: int;
  y: int;
  view fn toString() -> Str { return "Point(" + Str(self.x) + ", " + Str(self.y) + ")"; }
}

struct Size { w: int; h: int; }

enum Shape { Circle(r: float), Square(side: int) }

fn main() -> int {
  o: int? = None;
  s: Str? = "hi";
  p = Point(1, 2);
  io::println(Str(o) + " " + Str(s) + " " + Str(p) + " " + Str(None));
  io::println(Str((1, "a", 2.5)) + " " + Str(Size(2, 3)) + " " + Str(Shape::Circle(1.5)));
  io::println(Str(p) == p.toString());
  return 0;
}
```

Output:

```
None hi Point(1, 2) None
(1, a, 2.5) Size(w: 2, h: 3) Circle(r: 1.5)
True
```

`Str(s)` of a `Str` is the string itself, the same object, and so is `Str<Str>(s)`. Being
the same object, which `concat` can change, it takes no `view` of a string
([A `view` stays a `view`](03-functions.md#a-view-of-a-reference-stays-a-view)): with `v`
a `view Str` parameter, `Str(v)` is an error (`'v' is a 'view' parameter; it can only be
passed to a 'view' parameter, and parameter 1 of 'Str' is not one`), as `return v` is.
`v.toString()` is a copy of the string, and takes a `view`. In a generic function,
`Str(x)` with `x: T` works for every `T` it is instantiated with that has a text, except
that a `view T` at `T` = `Str` is that error; `x.toString()` works for every `T`.

A function value has no text, nor has a fixed-size array or a slice (print one element at
a time), nor a tuple, struct or enum holding one. `Str` of such a value is an error that
names the part without a text:

```pkn
struct F {
  n: int;
  f: (int) -> int;
}

fn main() -> int {
  s = Str(F(1, fn(x: int) => x * 2));
  return 0;
}
```

Error:

```
'Str' cannot convert a value of type 'F', which does not implement trait 'Show': its field 'f' has type '(int) -> int', which does not implement it
```

`+` joins two strings, so a value is converted first: `"x = " + Str(p)`. A `+` of a `Str`
and another value with a text is an error that says so (`operator '+' is not defined for
types 'Str' and 'Point' (convert the 'Point' with 'Str(...)')`).

### Between numbers

The numbers come in three **families**: the signed integers `int` (also spelled `i64`),
`i8`, `i16` and `i32`; the unsigned integers `unsigned` (also `u64`), `u8`, `u16` and `u32`;
and the floats `float` (also `f64`), `f16` and `f32`. A type converts from the types its
family takes, and from itself (`int(n)` with an `int` is `n`):

| Target                       | Converts from                                                  |
|------------------------------|----------------------------------------------------------------|
| `int`, `i8`, `i16`, `i32`    | every signed type, `u8`, `u16`, `u32`, every float type, `bool`, `char` |
| `unsigned`, `u8`, `u16`, `u32` | every integer type, `bool`, `char`                           |
| `float`, `f16`, `f32`        | every integer type, every float type                           |
| `bool`                       | every integer type                                             |
| `char`                       | every integer type                                             |

What a conversion gives:

| Conversion        | Result                                                                  |
|-------------------|-------------------------------------------------------------------------|
| integer → integer | the value when it fits, else the low bits of its two's-complement form: `u8(300)` is `44`, `i8(200)` is `-56`, `unsigned(-1)` is `18446744073709551615`, `u8(i8(-1))` is `255` |
| float → integer   | truncated toward zero; panics on NaN, an infinity or a value outside the target's range (`int(-2.9)` is `-2`, `i8(128.0)` panics) |
| integer → float   | the nearest value of the target, rounded once: `float(n)` is exact up to 2^53, `f32(16777217)` is `16777216`, `f16(70000)` is `inf` |
| float → float     | rounded to the target's precision; too large is its infinity (`f16(0.1)` is `0.0999756`) |
| `bool` → integer  | `1` for `True`, `0` for `False`                                         |
| `char` → integer  | the byte value, `0` to `255`, then the low bits: `u8('a')` is `97`; an `i8` reads the byte as signed |
| integer → `bool`  | `n != 0`                                                                |
| integer → `char`  | the `char` with that byte value; panics outside `0` to `255` (a `u8` never does) |

```pkn
import ::io;

fn main() -> int {
  io::println(Str(int(2.9)) + " " + Str(int(-2.9)));
  io::println(Str(float(7) / 2.0));
  io::println(Str(int('A')) + " " + Str(char(int('A') + 2)));
  io::println(Str(bool(0)) + " " + Str(int(True)));
  return 0;
}
```

Output:

```
2 -2
3.5
65 C
False 1
```

The sized numeric types (`u8`, `i32`, `unsigned`, `f32`, …) convert the same way: `u8(n)`
keeps the low 8 bits of an `int` (`u8(300)` is `44`), `int(b)` widens a `u8` back, `i32(f)`
truncates a `float` and panics outside the `i32` range, and `f32(f)` rounds a `float` to
single precision. Two values of different numeric types never meet in an operation without
one of these conversions (see [Sized numeric types](02-basics.md#sized-numeric-types)).

```pkn
import ::io;

fn main() -> int {
  b: u8 = 200;
  io::println(Str(u8(300)) + " " + Str(int(b) + 100) + " " + Str(b + 100));
  io::println(Str(unsigned(-1)) + " " + Str(f32(0.1)));
  io::println(Str(u8('a')) + " " + Str(bool(b)) + " " + Str(char(u8(65))));
  return 0;
}
```

Output:

```
44 300 44
18446744073709551615 0.1
97 True A
```

Three kinds of pair are left out, and the error of each names the way to write it:

- **An `unsigned` into a signed type.** An `unsigned` can be larger than any `int`.
  `u.toInt()` gives its value as an `int?`, `None` above `9223372036854775807`; `i32(u32(u))`
  keeps its low 32 bits; or keep the arithmetic `unsigned`.
- **A float into an unsigned type.** Convert it to an `int` first: `u8(int(f))` truncates
  toward zero, then keeps the low 8 bits. Check the range first, as it wraps a value out of
  range rather than stopping.
- **A `bool` or a `char` into a float type.** Convert it to an `int` first: `float(int(c))`.

```pkn
fn main() -> int {
  u: unsigned = 7;
  n = int(u);
  return 0;
}
```

Error:

```
no specialization of 'int' for 'unsigned': an 'unsigned' can be larger than any 'int'; its method 'toInt()' returns 'int?', None above 9223372036854775807
```

```pkn
import ::io;

fn main() -> int {
  u = unsigned(-1);
  io::println(unsigned(42).toInt());
  io::println(u.toInt());
  io::println(Str(i32(u32(u))) + " " + Str(u8(int(255.9))));
  return 0;
}
```

Output:

```
42
None
-1 255
```

### Parsing a string

A string's methods parse it into a number or a `bool`: `s.toInt()`, `s.toI8()`, `s.toI16()`,
`s.toI32()`, `s.toUnsigned()`, `s.toU8()`, `s.toU16()`, `s.toU32()`, `s.toFloat()`,
`s.toF16()`, `s.toF32()` and `s.toBool()`. Parsing can fail (`"12abc"` is not a number), so
each one does not return its type but the *optional* of it: `s.toInt()` is an `int?`, either
a number or `None`. You get the number out with `match`:

```pkn
import ::io;

fn main() -> int {
  inputs = ["42", "-8", "12abc", " 7", "9223372036854775808"];
  i = 0;
  while i < inputs.len() {
    match inputs[i].toInt() {
      n: int { io::println("'" + inputs[i] + "' is the number " + Str(n)); }
      None   { io::println("'" + inputs[i] + "' is not a number"); }
    }
    i = i + 1;
  }
  return 0;
}
```

Output:

```
'42' is the number 42
'-8' is the number -8
'12abc' is not a number
' 7' is not a number
'9223372036854775808' is not a number
```

The first arm runs when the parse succeeded and binds the number to `n`; the `None` arm
runs when it failed. [Optionals](12-optionals.md#unwrapping-with-match) explains `match` on
optionals in full.

The parses are strict. The whole string must be the value, with no surrounding whitespace,
and the empty string is never one. A text parses exactly when it is a valid literal of the
type, and gives that literal's value; on top of that, a signed or a float type takes a
leading `+` or `-`, and `toFloat()` takes the other spellings of C's `strtod`.

| Method | Gives | Accepts | `None` when |
|---|---|---|---|
| `toInt()` | `int?` | an optional `+` or `-`, then decimal digits | outside `-9223372036854775808` to `9223372036854775807` |
| `toI8()`, `toI16()`, `toI32()` | `i8?`, `i16?`, `i32?` | the same | outside the type's range (`"-129".toI8()`) |
| `toUnsigned()` | `unsigned?` | decimal digits, no sign | it has a sign (`"+1"`, `"-0"`), or is above `18446744073709551615` |
| `toU8()`, `toU16()`, `toU32()` | `u8?`, `u16?`, `u32?` | the same | it has a sign (`"+1".toU8()`), or is above `255`, `65535`, `4294967295` |
| `toFloat()` | `float?` | what C's `strtod` accepts: digits with an optional fraction and exponent (`"2.5"`, `".5"`, `"1e3"`), hexadecimal (`"0x10"`), `inf`, `infinity` and `nan` in any case, with an optional sign | the value is too large, which would be infinity (`"1e999"`), or too small, not zero but rounding to zero (`"1e-400"`): the range rule of float literals. Subnormal values (`"5e-324"`) parse, and `"inf"` and `"nan"` are spelled out, so they parse |
| `toF32()`, `toF16()` | `f32?`, `f16?` | the same | as for `toFloat()`, and when a finite text rounds to infinity in the type (`"1e39".toF32()`, `"70000".toF16()`), as its literal is an error. A text below the type's range is 0 or a subnormal, as its literal is (`"1e-9".toF16()` is `0`) |
| `toBool()` | `bool?` | exactly `"True"` and `"False"`, the spellings `Str(b)` prints | anything else (`"true"`, `"1"`, `" True"`) |

So the text a number or a `bool` prints parses back: `Str(n).toInt()` gives `n` for every
`int`, and `Str(b).toBool()` gives `b`. A float prints six significant digits
(`Str(0.1234567)` is `"0.123457"`), so its text gives back the value only when those digits
are exact (`0.5`, `65504`, `inf`). There is one method per type, under the type's name:
`"7".toI64()` is an error that names `toInt()`, since an `i64` is an `int`.

A `float` text becomes the `float` (`f64`) nearest to it. An `f32` or `f16` from a text with
a fraction or an exponent — a literal in an `f32` or `f16` slot, or `s.toF32()` and
`s.toF16()` — is that `float` rounded to the type, as C's `(float)strtod(s)` is. That is two
roundings. It can give the other neighbour than a single rounding would, but only for a
text with many significant digits (13 or more) lying very close to the point halfway
between two neighbouring values of the type. `x: f32 = 1.0000000596046448;` and
`"1.0000000596046448".toF32()` are both `1`, where rounding the text once gives
`1.0000001`. A literal and the parse of the same text always give the same value. An
integer text becomes an `f32` or `f16` in one rounding.

A type does not parse a string by being called with it: `int(s)` and `int<Str>(s)` are an
error that names the method, and so are `u8(s)`, `float(s)`, `bool(s)` and the others.

```pkn
fn main() -> int {
  s = "12";
  n = int(s);
  return 0;
}
```

Error:

```
no specialization of 'int' for 'Str': parse a 'Str' with its method 'toInt()', which returns 'int?'
```

### The explicit form

Each conversion has an explicit spelling that names the source type too:
`Target<Source>(value)`, for example `Str<int>(n)`, `int<float>(f)`, `bool<int>(n)`. It is
the same conversion; the explicit form documents what is being converted and makes the
compiler check it:

```pkn
import ::io;

fn main() -> int {
  n = 5;
  io::println(Str<int>(n) + " " + Str<float>(2.5) + " " + Str<bool>(n > 3));
  return 0;
}
```

Output:

```
5 2.5 True
```

The argument must then have exactly that type: `Str<float>(3)` is an error, as `3` is an
`int` (`argument of 'Str<float>' has type 'int', expected 'float'`).

### Specializations

The conversions are **specializations of the builtin types**. Each primitive target has a
fixed, closed set of them, one for each type it converts *from*; `Str` has one for every
type with a text ([To a string](#to-a-string)). `Target(value)` picks the
specialization whose source is **exactly** the type of `value`: `Str(n)` with an `int` is
`Str<int>`, and `int(f)` with a `float` is `int<float>`. Nothing is widened or unwrapped to
find one. An `int` argument does not reach `float`'s `Str` specialization, and an `int?`
must be unwrapped with `match` first. Each primitive has the specialization of itself, the
value unchanged: `float(2.5)` is `float<float>(2.5)`.

| Target  | Specializations (the source types) | Result                                   |
|---------|------------------------------------|------------------------------------------|
| `Str`   | every type with a text: the primitives, `Str`, optionals, tuples, structs, enums, classes, `Obj` | `Str` |
| `int`, `i8`, `i16`, `i32` | `int`, `float`, `i8`, `i16`, `i32`, `u8`, `u16`, `u32`, `f16`, `f32`, `bool`, `char` | `T` |
| `unsigned`, `u8`, `u16`, `u32` | `int`, `i8`, `i16`, `i32`, `u8`, `u16`, `u32`, `unsigned`, `bool`, `char` | `T` |
| `float`, `f16`, `f32` | `int`, `float`, `i8`, `i16`, `i32`, `u8`, `u16`, `u32`, `unsigned`, `f16`, `f32` | `T` |
| `bool`  | `int`, `i8`, `i16`, `i32`, `u8`, `u16`, `u32`, `unsigned`, `bool` | `bool`  |
| `char`  | `int`, `i8`, `i16`, `i32`, `u8`, `u16`, `u32`, `unsigned`, `char` | `char`  |

No type converts *from* a `Str`: a string is parsed with its methods ([Parsing a
string](#parsing-a-string)), each of which returns an optional.

The numeric types are `int`, `float`, `i8`, `i16`, `i32`, `u8`, `u16`, `u32`, `unsigned`,
`f16` and `f32` (`i64`, `f64` and `u64` are other spellings of the first two and of
`unsigned`, so `u64(x)` and `unsigned(x)` are one conversion).

What each specialization does:

| Conversion        | From → to          | Result                                                        |
|-------------------|--------------------|---------------------------------------------------------------|
| `Str<int>(n)`     | `int` → `Str`      | Decimal digits, with a `-` for negatives: `"42"`, `"-7"`      |
| `Str<float>(f)`   | `float` → `Str`    | `%g` formatting: `"3.14"`, `"2"`, `"1e+20"`, `"-0"`; `"nan"`, `"inf"`, `"-inf"` |
| `Str<bool>(b)`    | `bool` → `Str`     | `"True"` or `"False"`                                         |
| `Str<char>(c)`    | `char` → `Str`     | A one-character string                                        |
| `int<float>(f)`   | `float` → `int`    | Truncates toward zero; panics on NaN, ±inf or out of range    |
| `float<int>(n)`   | `int` → `float`    | The nearest `float` (exact up to 2^53)                        |
| `int<bool>(b)`, `u8<bool>(b)`, … | `bool` → integer | `1` for `True`, `0` for `False`                    |
| `bool<int>(n)`, `bool<u8>(b)`, … | integer → `bool` | `n != 0`                                           |
| `int<char>(c)`, `u8<char>(c)`, … | `char` → integer | The character's byte code, `0`..`255`, then the low bits (`i8<char>` reads it as signed) |
| `char<int>(n)`, `char<u16>(h)`, … | integer → `char` | The character with byte code `n`; panics outside `0`..`255` (never from a `u8`) |
| `int<int>(n)`, `f32<f32>(x)`, … | a type → itself | The value                                          |
| `u8<int>(n)`, `i32<u32>(w)`, `int<u8>(b)`, … | integer → integer | The low bits of the two's-complement value: `u8(300)` is `44`, `i8(200)` is `-56`, `unsigned(-1)` is `18446744073709551615`, `i32(u32(4294967295))` is `-1` |
| `i8<float>(f)`, `int<f32>(x)`, … | float → signed integer | Truncates toward zero; panics on NaN, ±inf or a value outside the target's range (`i8(-128.9)` is `-128`, `i8(128.0)` panics) |
| `f32<int>(n)`, `float<u8>(b)`, … | integer → float | The nearest value of the target, rounded once (`f32(16777217)` is `16777216`) |
| `f16<float>(f)`, `float<f32>(x)`, … | float → float | Rounded to the target's precision; too large is its infinity (`f16(0.1)` is `0.0999756`, `f16(70000.0)` is `inf`) |
| `Str<u8>(b)`, `Str<f32>(x)`, … | sized → `Str` | As `Str<int>` / `Str<float>`; `Str<unsigned>` prints the unsigned value |
| `Str<Point>(p)`, `Str<int?>(o)`, `Str<(int, Str)>(t)`, … | any other type with a text → `Str` | `x.toString()`: an object's own, an optional's `None` or its value's, a tuple's, struct's or enum's text |
| `Str<Str>(s)`     | `Str` → `Str`      | The string itself                                             |

**The set is closed** for every target but `Str`, whose sources are the types with a
text. When no specialization's source is the argument's type, a user class included, the
call is a compile-time error that lists the target's specializations. For a pair a family
leaves out ([Between numbers](#between-numbers)) it names the conversion to write instead,
and for a `Str` argument and a number or `bool` target, the method that parses the string.
The explicit form reports a type argument outside the set the same way. A class cannot add
a specialization to a primitive; its `toString()` is what `Str(x)` gives:

```pkn
a = Point();
n = int(a);         // error: no specialization of 'int' for 'Point'; its specializations
                    //        are int, float, i8, i16, i32, u8, u16, u32, f16, f32, bool,
                    //        char
s = Str(a);         // the text a.toString() gives
x = bool(2.5);      // error: no specialization of 'bool' for 'float'; its specializations
                    //        are int, i8, i16, i32, u8, u16, u32, unsigned, bool
w = u8(2.5);        // error: no specialization of 'u8' for 'float': convert the 'float' to
                    //        an 'int' first: 'u8(int(...))' truncates toward zero, then
                    //        keeps the low 8 bits
y = int<float>(3);  // error: argument of 'int<float>' has type 'int', expected 'float'
n = u8("7");        // error: no specialization of 'u8' for 'Str': parse a 'Str' with its
                    //        method 'toU8()', which returns 'u8?'
```

### Panics

`int<float>` of NaN, of an infinity, or of a value outside the `int` range (`-2^63` to just
under `2^63`) stops the program, and so does a float-to-integer conversion to a sized type
outside that type's range (`i8(128.0)`). So does `char(n)` of an integer outside
`0`..`255`, from any integer type but `u8`. Each prints a message naming the conversion and
the value and aborts, like an integer division by zero; an unsigned value prints unsigned:

```
paykan: int<float>(inf): the value is NaN, infinite or outside the int range
paykan: i8<float>(128): the value is NaN, infinite or outside the i8 range
paykan: char<int>(300): the value is outside the char range 0..255
paykan: char<i32>(300): the value is outside the char range 0..255
paykan: char<unsigned>(18446744073709551615): the value is outside the char range 0..255
```

A panic (these, an integer division by zero or `INT64_MIN / -1`, an index out of bounds)
flushes the output already printed to stdout, prints its message to stderr and
aborts the program with `SIGABRT`, so everything printed before the panic reaches a pipe or
a file too, ahead of the message. `paykan run` (and `paykan prog.pkn`) then exits with status
`134` (128 + `SIGABRT`) on every backend. An executable made by `paykan build` is the
program itself, so it dies by `SIGABRT` (a shell shows `$?` as `134` either way).

---

## Project: text helpers for `tasks`

The task list will print its tasks in aligned columns and accept titles typed by the user.
Three helpers will do: `padRight` pads a string with spaces to a width, `trim` removes
spaces and line breaks from both ends (lines read from a file end in `\n`), and
`capitalize` upper-cases the first letter, using the fact that the ASCII lowercase letters
are 32 above the uppercase ones:

```pkn
import ::io;

fn padRight(s: Str, width: int) -> Str {
  out = s;
  while out.len() < width {
    out = out + " ";
  }
  return out;
}

fn isSpace(c: char) -> bool {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// The characters of `s` from index `from` up to (not including) `to`.
fn slice(s: Str, from: int, to: int) -> Str {
  out = "";
  i = from;
  while i < to {
    out = out + Str(s[i]);
    i = i + 1;
  }
  return out;
}

fn trim(s: Str) -> Str {
  start = 0;
  end = s.len();
  while start < end && isSpace(s[start]) { start = start + 1; }
  while end > start && isSpace(s[end - 1]) { end = end - 1; }
  return slice(s, start, end);
}

fn capitalize(s: Str) -> Str {
  if s.len() == 0 { return s; }
  c = s[0];
  if c >= 'a' && c <= 'z' {
    c = char(int(c) - 32);
  }
  return Str(c) + slice(s, 1, s.len());
}

fn main() -> int {
  io::println("|" + padRight("buy milk", 12) + "|");
  io::println("|" + trim("  water the plants \n") + "|");
  io::println(capitalize("call the plumber"));
  return 0;
}
```

Output:

```
|buy milk    |
|water the plants|
Call the plumber
```

`padRight` builds its result with `+`, not `concat`: `out = s` makes `out` the caller's
string, and `out.concat(" ")` would change that string too.

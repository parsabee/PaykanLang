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
`int(c)` reads it, which makes range checks such as "is this a digit?" easy:

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

`Str(x)` works for `int`, `float`, `bool`, `char` and the sized numeric types:

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

`Str(...)` does not work on objects of your own classes; give the class a `toString()`
method instead ([Classes](05-classes.md)). `Str(s)` with a `Str` argument is not a
conversion: it is the `Str` constructor, and returns the string itself.

### Between numbers

| Conversion   | Result                                                      |
|--------------|-------------------------------------------------------------|
| `float(n)`   | the `int` `n` as a `float`: the nearest one (exact up to 2^53) |
| `int(f)`     | the `float` `f` truncated toward zero; panics on NaN, an infinity or a value out of range |
| `int(b)`     | `1` for `True`, `0` for `False`                             |
| `bool(n)`    | `n != 0`                                                    |
| `int(c)`     | the byte value of the `char` `c`, `0` to `255`              |
| `char(n)`    | the `char` with byte value `n`; panics outside `0` to `255` |

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

The sized numeric types (`u8`, `i32`, `unsigned`, `f32`, …) convert the same way, from and
to every other numeric type: `u8(n)` keeps the low 8 bits of an `int` (`u8(300)` is `44`),
`int(b)` widens a `u8` back, `i32(f)` truncates a `float` and panics outside the `i32`
range, and `f32(f)` rounds a `float` to single precision.  Two values of different numeric
types never meet in an operation without one of these conversions (see [Sized numeric
types](02-basics.md#sized-numeric-types)).

```pkn
import ::io;

fn main() -> int {
  b: u8 = 200;
  io::println(Str(u8(300)) + " " + Str(int(b) + 100) + " " + Str(b + 100));
  io::println(Str(unsigned(-1)) + " " + Str(f32(0.1)));
  return 0;
}
```

Output:

```
44 300 44
18446744073709551615 0.1
```

### From a string: parsing

`int(s)`, `float(s)` and `bool(s)` parse a string. Parsing can fail (`"12abc"` is not a
number), so they do not return an `int` but an `int?`, an *optional* `int` that is either a
number or `None`. You get the number out with `match`:

```pkn
import ::io;

fn main() -> int {
  inputs = ["42", "-8", "12abc", " 7", "9223372036854775808"];
  i = 0;
  while i < inputs.len() {
    match int(inputs[i]) {
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
optionals in full. The parsers are strict: the whole string must be the value, with no
surrounding whitespace.

- `int(s)` accepts an optional `+` or `-` followed by decimal digits, and the value must
  fit in an `int` (`"9223372036854775808"` is `None`).
- `float(s)` accepts what C's `strtod` accepts, including exponents and `nan` / `inf`
  (`"2.5"`, `"1e3"`, `"inf"`). The range rule is the one for float literals: subnormal
  values (`"5e-324"`, `"1e-310"`) parse, but a value too large (`"1e999"`, which would be
  infinity) or too small (`"1e-400"`, not zero but rounding to zero) is `None`. `"inf"`
  and `"nan"` are spelled out, so they parse.
- `bool(s)` accepts exactly `"True"` and `"False"`, the spellings `Str(b)` prints, so
  `bool(Str(b))` gives back `b`. Anything else (`"true"`, `"1"`, `" True"`) is `None`.
- The sized types parse the same way into an optional of their own (`u8(s)` is a `u8?`,
  `unsigned(s)` an `unsigned?`, `f32(s)` an `f32?`), `None` when the text is not a number
  of the type: outside its range (`u8("256")`), or signed for an `unsigned`
  (`unsigned("-1")`).

### The explicit form

Each conversion has an explicit spelling that names the source type too:
`Target<Source>(value)`, for example `Str<int>(n)`, `int<float>(f)`, `int<Str>(s)`. It is
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

The conversions are **specializations of the builtin types**. Each builtin target has a
fixed, closed set of them, one for each type it converts *from*. `Target(value)` picks the
specialization whose source is **exactly** the type of `value`: `Str(n)` with an `int` is
`Str<int>`, `int(f)` with a `float` is `int<float>`, `int(s)` with a `Str` is `int<Str>`.
Nothing is widened or unwrapped to find one. An `int` argument does not reach `float`'s
`Str` specialization, and an `int?` must be unwrapped with `match` first. A conversion
between a type and itself, such as `float(2.5)`, does not exist either.

| Target  | Specializations (the source types) | Result                                   |
|---------|------------------------------------|------------------------------------------|
| `Str`   | `int`, `float`, `bool`, `char`, and every sized numeric type | `Str`           |
| `int`   | `Str`; `float`, `bool`, `char`, and every sized numeric type | `int?` from `Str`; `int` otherwise |
| `float` | `Str`; `int`, and every sized numeric type | `float?` from `Str`; `float` otherwise |
| `i8` … `u32`, `unsigned`, `f16`, `f32` | `Str`; every other numeric type | `T?` from `Str`; `T` otherwise |
| `bool`  | `int`; `Str`                       | `bool` from `int`; `bool?` from `Str`    |
| `char`  | `int`                              | `char`                                   |

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
| `int<Str>(s)`     | `Str` → `int?`     | The parsed integer, or `None` (see [parsing](#from-a-string-parsing)) |
| `float<Str>(s)`   | `Str` → `float?`   | The parsed float, or `None` (see [parsing](#from-a-string-parsing)) |
| `bool<Str>(s)`    | `Str` → `bool?`    | `True` for `"True"`, `False` for `"False"`, else `None`       |
| `int<float>(f)`   | `float` → `int`    | Truncates toward zero; panics on NaN, ±inf or out of range    |
| `float<int>(n)`   | `int` → `float`    | The nearest `float` (exact up to 2^53)                        |
| `int<bool>(b)`    | `bool` → `int`     | `1` for `True`, `0` for `False`                               |
| `bool<int>(n)`    | `int` → `bool`     | `n != 0`                                                      |
| `int<char>(c)`    | `char` → `int`     | The character's byte code, `0`..`255`                         |
| `char<int>(n)`    | `int` → `char`     | The character with byte code `n`; panics outside `0`..`255`   |
| `u8<int>(n)`, `i32<unsigned>(u)`, `int<u8>(b)`, … | integer → integer | The low bits of the two's-complement value: `u8(300)` is `44`, `i8(200)` is `-56`, `unsigned(-1)` is `18446744073709551615`, `int(unsigned(-1))` is `-1` |
| `i8<float>(f)`, `unsigned<f32>(x)`, … | float → integer | Truncates toward zero; panics on NaN, ±inf or a value outside the target's range (`i8(-128.9)` is `-128`, `i8(128.0)` panics) |
| `f32<int>(n)`, `float<u8>(b)`, … | integer → float | The nearest value of the target (`f32(16777217)` is `16777216`) |
| `f16<float>(f)`, `float<f32>(x)`, … | float → float | Rounded to the target's precision; too large is its infinity (`f16(0.1)` is `0.0999756`, `f16(70000.0)` is `inf`) |
| `Str<u8>(b)`, `Str<f32>(x)`, … | sized → `Str` | As `Str<int>` / `Str<float>`; `Str<unsigned>` prints the unsigned value |
| `u8<Str>(s)`, `unsigned<Str>(s)`, `f32<Str>(s)`, … | `Str` → `T?` | The parsed value, or `None` when the text is not a number of the type: outside its range (`u8("256")`), or signed for an `unsigned` (`unsigned("-1")`) |

**The set is closed.** When no specialization's source is the argument's type, a user class
included, the call is a compile-time error that lists the target's specializations. The
explicit form reports a type argument outside the set the same way. A class cannot add a
specialization; `toString()` is the way to turn an object into a `Str`:

```pkn
a = Point();
s = Str(a);         // error: no specialization of 'Str' for 'Point'; its specializations
                    //        are int, float, bool, char, i8, i16, i32, u8, u16, u32,
                    //        unsigned, f16, f32
o = None;
t = Str(o);         // error: no specialization of 'Str' for 'Obj'; ...
x = float(2.5);     // error: no specialization of 'float' for 'float'; its specializations
                    //        are Str, int, i8, i16, i32, u8, u16, u32, unsigned, f16, f32
y = int<float>(3);  // error: argument of 'int<float>' has type 'int', expected 'float'
```

### Panics

`int<float>` of NaN, of an infinity, or of a value outside the `int` range (`-2^63` to just
under `2^63`) stops the program, and so does a float-to-integer conversion to a sized type
outside that type's range (`i8(128.0)`). So does `char<int>` outside `0`..`255`. Each
prints a message naming the value and aborts, like an integer division by zero:

```
paykan: int<float>(inf): the value is NaN, infinite or outside the int range
paykan: i8<float>(128): the value is NaN, infinite or outside the i8 range
paykan: char<int>(300): the value is outside the char range 0..255
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

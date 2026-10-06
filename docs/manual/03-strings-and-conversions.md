# Strings and conversions

Text is everywhere in real programs, and so are conversions between text and numbers. This
chapter covers the `Str` type, the `char` type and the conversion constructors, and adds
some text helpers to `tasks`.

## Strings

`Str` is the string type. A string literal is written in double quotes and must end on the
line it starts; use the escapes for special characters:

| Escape | Character        |
|--------|------------------|
| `\n`   | newline          |
| `\t`   | tab              |
| `\r`   | carriage return  |
| `\\`   | backslash        |
| `\"`   | double quote     |
| `\0`   | the zero byte    |

Any other backslash sequence is kept as written, so `"a\qb"` is the four characters `a\qb`.

The operations on strings:

- `a + b` concatenates two strings into a new one;
- `s.len()` is the length, in bytes;
- `s[i]` is the `char` at index `i` (zero-based, checked: an index out of range panics);
- `a == b` and `a != b` compare **contents**: `"ab" == "a" + "b"` is `True`.

```pkn
fn main() -> int {
  first = "Paykan";
  full = first + "Lang";
  println(full + " has " + Str(full.len()) + " characters");
  println("it starts with " + Str(full[0]) + " and ends with " + Str(full[full.len() - 1]));
  println(Str(full == "Paykan" + "Lang"));
  println("tab:\t|  quote: \"  backslash: \\");
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

Strings are immutable from the program's point of view: `s[0] = 'x'` is an error
(`subscript assignment on non-array type 'Str'`). To change a string, build a new one. The
ordering operators (`<`, `>`) are not defined on strings; compare characters instead.

There are no built-in methods for searching, slicing or splitting yet (a standard library is
planned), but they are short loops over `len()` and `[]`, as we will see below.

### Characters and bytes

A `char` is one byte, written in single quotes: `'a'`, `'\n'`, `'\''`, `'\\'`. Characters
compare with `==`, `!=`, `<`, `<=`, `>` and `>=` by their byte value, which makes
range checks such as "is this a digit?" easy:

```pkn
fn isDigit(c: char) -> bool {
  return c >= '0' && c <= '9';
}

fn countDigits(s: Str) -> int {
  n = 0;
  i = 0;
  while (i < s.len()) {
    if (isDigit(s[i])) { n = n + 1; }
    i = i + 1;
  }
  return n;
}

fn main() -> int {
  println(Str(countDigits("call 555-0199 before 9pm")));
  return 0;
}
```

Output:

```
8
```

Because a `char` is a byte, a character outside ASCII takes several `char`s in a UTF-8
string: `"é".len()` is `2`.

## Conversions

To convert a value to another type, call the target type like a function with the value:
`Str(42)`, `float(n)`, `int(x)`. These are called **conversion constructors**.

### To a string

`Str(x)` works for `int`, `float`, `bool` and `char`:

```pkn
fn main() -> int {
  println(Str(42) + " " + Str(-7));
  println(Str(3.14) + " " + Str(2.0) + " " + Str(1.0 / 3.0) + " " + Str(1e20));
  println(Str(True) + " " + Str('z'));
  return 0;
}
```

Output:

```
42 -7
3.14 2 0.333333 1e+20
True z
```

A `float` is formatted like C's `%g`: at most six significant digits, no trailing zeros, and
an exponent for very large or small numbers.

`Str(...)` does not work on objects of your own classes; give the class a `toString()`
method instead ([Classes](04-classes.md)).

### Between numbers

| Conversion   | Result                                                      |
|--------------|-------------------------------------------------------------|
| `float(n)`   | the `int` `n` as a `float`                                  |
| `int(f)`     | the `float` `f` truncated toward zero; panics on NaN, an infinity or a value out of range |
| `int(b)`     | `1` for `True`, `0` for `False`                             |
| `bool(n)`    | `n != 0`                                                    |
| `int(c)`     | the byte value of the `char` `c`, `0` to `255`              |
| `char(n)`    | the `char` with byte value `n`; panics outside `0` to `255` |

```pkn
fn main() -> int {
  println(Str(int(2.9)) + " " + Str(int(-2.9)));
  println(Str(float(7) / 2.0));
  println(Str(int('A')) + " " + Str(char(int('A') + 2)));
  println(Str(bool(0)) + " " + Str(int(True)));
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

### From a string: parsing

`int(s)`, `float(s)` and `bool(s)` parse a string. Parsing can fail (`"12abc"` is not a
number), so they do not return an `int` but an `int?`, an *optional* `int` that is either a
number or `None`. You get the number out with `match`:

```pkn
fn main() -> int {
  inputs = ["42", "-8", "12abc", " 7", "9223372036854775808"];
  i = 0;
  while (i < inputs.len()) {
    match int(inputs[i]) {
      n: int { println("'" + inputs[i] + "' is the number " + Str(n)); }
      None   { println("'" + inputs[i] + "' is not a number"); }
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
runs when it failed. [Optionals](08-optionals.md) explains `match` on optionals in full.
The parsers are strict: the whole string must be the value, with no surrounding spaces, and
an `int` must fit in 64 bits. `float(s)` accepts what C's `strtod` does (`"2.5"`, `"1e3"`,
`"inf"`), and `bool(s)` accepts exactly `"True"` and `"False"`.

### The explicit form

Each conversion has an explicit spelling that names the source type too: `Str<int>(n)`,
`int<float>(f)`, `int<Str>(s)`. It is the same conversion; the explicit form documents what
is being converted and makes the compiler check it:

```pkn
fn main() -> int {
  n = 5;
  println(Str<int>(n) + " " + Str<float>(2.5) + " " + Str<bool>(n > 3));
  return 0;
}
```

Output:

```
5 2.5 True
```

The set of conversions is fixed and exact: the argument's type must be exactly one of the
sources the target accepts. `Str<float>(3)` is an error because `3` is an `int`, and a
conversion between a type and itself, such as `float(2.5)`, is an error too. The
[reference](../language/01-language-basics.md#conversions) lists every conversion.

## Project: text helpers for `tasks`

The task list will print its tasks in aligned columns and accept titles typed by the user.
Three helpers will do: `padRight` pads a string with spaces to a width, `trim` removes
spaces and line breaks from both ends (lines read from a file end in `\n`), and
`capitalize` upper-cases the first letter, using the fact that the ASCII lowercase letters
are 32 above the uppercase ones:

```pkn
fn padRight(s: Str, width: int) -> Str {
  out = s;
  while (out.len() < width) {
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
  while (i < to) {
    out = out + Str(s[i]);
    i = i + 1;
  }
  return out;
}

fn trim(s: Str) -> Str {
  start = 0;
  end = s.len();
  while (start < end && isSpace(s[start])) { start = start + 1; }
  while (end > start && isSpace(s[end - 1])) { end = end - 1; }
  return slice(s, start, end);
}

fn capitalize(s: Str) -> Str {
  if (s.len() == 0) { return s; }
  c = s[0];
  if (c >= 'a' && c <= 'z') {
    c = char(int(c) - 32);
  }
  return Str(c) + slice(s, 1, s.len());
}

fn main() -> int {
  println("|" + padRight("buy milk", 12) + "|");
  println("|" + trim("  water the plants \n") + "|");
  println(capitalize("call the plumber"));
  return 0;
}
```

Output:

```
|buy milk    |
|water the plants|
Call the plumber
```

Next: [Classes and inheritance](04-classes.md), where tasks become objects.

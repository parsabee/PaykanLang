# PaykanLang — Standard Library (`std`)

This document covers the built-in `std` sub-module tree. The standard library is
organised into focused sub-modules; each can be imported individually or you can import
the umbrella `std` to get everything at once.

```
import std::io;        // File I/O and standard streams
import std::math;      // Math functions and constants
import std::string;    // String utilities
import std::process;   // args, exit, getenv

import std;            // umbrella — re-exports all four sub-modules
```

After `import std;` every symbol is accessible as `std::io::File`,
`std::math::sin`, etc. After `import std::math;` the same symbols are accessible as
`math::sin`, `math::pi`, etc.

---

## `std::io`

```
import std::io;
```

Provides file handles, open/close/read/write, the three standard streams, and
convenience output helpers.

### Quick Reference

| Symbol | Kind | Description |
|--------|------|-------------|
| `io::File` | class | Open file handle (extends `Obj`) |
| `io::Mode` | enum | Open modes: `Read`, `Write`, `Append` |
| `io::open(path, modes)` | fn | Open a file; returns `Obj`, downcast to `io::File` with `match` |
| `io::close(file)` | fn | Flush and close a handle |
| `io::read(file)` | fn | Blocking line read |
| `io::write(file, s)` | fn | Write a string |
| `io::split(s, sep)` | fn | Split a string by separator |
| `io::in()` | fn | stdin stream |
| `io::out()` | fn | stdout stream |
| `io::err()` | fn | stderr stream |
| `io::print(s)` | fn | Write line to stdout |
| `io::printErr(s)` | fn | Write line to stderr |

### `io::File`

`io::File` is a heap-allocated, reference-counted class (extends `Obj`) wrapping an OS
file descriptor. The handle is flushed and closed automatically when its reference count
drops to zero, but explicit `io::close` is strongly preferred.

**Methods**

| Method | Signature | Description |
|--------|-----------|-------------|
| `ok` | `fn ok() -> bool` | `True` while the handle is valid and open |
| `toString` | `fn toString() -> Str` | Read and return all remaining content; advances to EOF |

### `io::Mode`

| Variant | Description |
|---------|-------------|
| `io::Mode::Read` | Read-only; file must exist |
| `io::Mode::Write` | Write; creates or truncates |
| `io::Mode::Append` | Write at end; creates if absent, never truncates |

### Functions

#### `io::open`
```
fn io::open(path: Str, modes: io::Mode[]) -> Obj
```
Opens `path` with the given modes. Returns `Obj`; use a `match` statement to downcast
to `io::File`. The `io::File` arm is entered only when the open succeeds — no separate
error-check needed.

#### `io::close`
```
fn io::close(file: io::File)
```
Flushes and releases the descriptor. A no-op on an already-closed handle.

#### `io::read`
```
fn io::read(file: io::File) -> Str
```
Blocking read; returns the next line (newline stripped). Returns `""` at EOF.

#### `io::write`
```
fn io::write(file: io::File, content: Str) -> bool
```
Writes `content`. Returns `True` on success.

#### `io::split`
```
fn io::split(s: Str, sep: Str) -> Str[]
```
Splits `s` on every occurrence of `sep`. Consecutive separators produce empty elements.

### Standard Streams

`io::in()`, `io::out()`, and `io::err()` return the three POSIX standard streams as
`io::File`. They are always open; **do not close them**.

```
fn io::in()  -> io::File   // stdin
fn io::out() -> io::File   // stdout
fn io::err() -> io::File   // stderr (unbuffered)
```

`io::print` / `io::printErr` are convenience wrappers that append `"\n"`:

```
fn io::print(s: Str)    // io::write(io::out(), s + "\n")
fn io::printErr(s: Str)    // io::write(io::err(), s + "\n")
```

### Example

```
import std::io;

fn main() -> int {
  io::print("enter a filename:");
  name: Str = io::read(io::in());

  match io::open(name, [io::Mode::Read]) {
    io::File f {
      io::write(io::out(), f.toString());
      io::close(f);
    }
    _ { io::printErr("cannot open: " + name); return 1; }
  }
  return 0;
}
```

### Extending `io::File` — Drivers and Sockets

`io::File` methods are all virtual. Subclass it to build custom I/O abstractions —
device drivers, sockets, pipes — that work anywhere `io::File` is accepted.

**Subclassing contract**

| Method | Required? | Notes |
|--------|-----------|-------|
| `__init__` | Yes | Call `__super__()` first; open the resource here |
| `ok() -> bool` | Yes | `True` while the resource is live |
| `toString() -> Str` | Yes | Return buffered / available content |
| `__del__` | Recommended | Release the resource; base chained automatically |

```
import std::io;

class CharDevice extends io::File {
  _handle: io::File;
  _open:   bool;

  fn __init__(path: Str) {
    __super__();
    self._open = False;
    match io::open(path, [io::Mode::Read]) {
      io::File h { self._handle = h; self._open = True; }
      _          {}
    }
  }

  fn ok() -> bool     { return self._open; }
  fn toString() -> Str {
    if (!self._open) { return ""; }
    return self._handle.toString();
  }
  fn __del__() {
    if (self._open) {
      io::close(self._handle);
      self._open = False;
    }
  }
}
```

Polymorphic I/O works because both `CharDevice` and a plain `io::File` share the same
static type in call sites:

```
fn copyTo(src: io::File, dst: io::File) {
  if (!src.ok() || !dst.ok()) { return; }
  io::write(dst, src.toString());
}
```

---

## `std::math`

```
import std::math;
```

Floating-point math functions and constants. All functions operate on `float`; cast
`int` inputs explicitly with `(float)n`.

### Quick Reference

| Symbol | Kind | Description |
|--------|------|-------------|
| `math::pi` | float | π ≈ 3.14159265358979 |
| `math::e` | float | e ≈ 2.71828182845905 |
| `math::pow(b, e)` | fn | bᵉ |
| `math::log(x)` | fn | Natural log |
| `math::log2(x)` | fn | Base-2 log |
| `math::log10(x)` | fn | Base-10 log |
| `math::min(a, b)` | fn | Minimum (`int` or `float`) |
| `math::max(a, b)` | fn | Maximum (`int` or `float`) |
| `math::sin(x)` | fn | Sine (radians) |
| `math::cos(x)` | fn | Cosine (radians) |
| `math::tan(x)` | fn | Tangent (radians) |
| `math::asin(x)` | fn | Arc-sine → radians |
| `math::acos(x)` | fn | Arc-cosine → radians |
| `math::atan(x)` | fn | Arc-tangent → radians |
| `math::atan2(y, x)` | fn | Two-argument arc-tangent |

### Constants

```
math::pi   // float  π ≈ 3.14159265358979323846
math::e    // float  e ≈ 2.71828182845904523536
```

### Power and Logarithm

```
fn math::pow(base: float, exp: float) -> float
fn math::log(x: float)   -> float     // natural log (base e)
fn math::log2(x: float)  -> float
fn math::log10(x: float) -> float
```

### Min / Max

`math::min` and `math::max` are overloaded for `int` and `float` — the only overloaded
names in the standard library:

```
fn math::min(a: int,   b: int)   -> int
fn math::min(a: float, b: float) -> float
fn math::max(a: int,   b: int)   -> int
fn math::max(a: float, b: float) -> float
```

```
clamp: int = math::max(0, math::min(n, 255));
```

### Trigonometry

All angles are in **radians**.

```
fn math::sin(x: float)           -> float
fn math::cos(x: float)           -> float
fn math::tan(x: float)           -> float
fn math::asin(x: float)          -> float
fn math::acos(x: float)          -> float
fn math::atan(x: float)          -> float
fn math::atan2(y: float, x: float) -> float
```

### Example

```
import std::math;
import std::io;

fn main() -> int {
  io::print((Str)math::pow(2.0, 10.0));             // 1024.0
  io::print((Str)math::sin(math::pi / 2.0));        // 1.0
  angle: float = math::atan2(1.0, 1.0);             // π/4
  io::print((Str)angle);
  return 0;
}
```

---

## `std::string`

```
import std::string;
```

Pure string utilities — every function returns a new `Str` without modifying the input.

### Quick Reference

| Symbol | Description |
|--------|-------------|
| `string::trim(s)` | Strip leading/trailing whitespace |
| `string::contains(s, sub)` | Test for substring |
| `string::startsWith(s, prefix)` | Test prefix |
| `string::endsWith(s, suffix)` | Test suffix |
| `string::indexOf(s, sub)` | First index of `sub`, or `-1` |
| `string::slice(s, lo, hi)` | Substring `s[lo..hi)` |
| `string::replace(s, from, to)` | Replace all occurrences |
| `string::toUpper(s)` | ASCII upper-case |
| `string::toLower(s)` | ASCII lower-case |
| `string::isNumeric(s)` | `True` if valid int or float literal |
| `string::parseInt(s)` | Parse to `int`; `0` on failure |
| `string::parseFloat(s)` | Parse to `float`; `0.0` on failure |

### Functions

#### `string::trim`
```
fn string::trim(s: Str) -> Str
```
Strips leading and trailing ASCII whitespace (space, tab, `\r`, `\n`).

```
line: Str = string::trim(io::read(io::in()));
```

#### `string::contains` / `string::startsWith` / `string::endsWith`
```
fn string::contains(s: Str, sub: Str)    -> bool
fn string::startsWith(s: Str, pre: Str)  -> bool
fn string::endsWith(s: Str, suf: Str)    -> bool
```

```
if (string::startsWith(line, "//")) { continue; }
```

#### `string::indexOf`
```
fn string::indexOf(s: Str, sub: Str) -> int
```
Returns the first zero-based index of `sub`, or `-1`.

```
pos: int = string::indexOf(line, "=");
if (pos >= 0) {
  key: Str = string::slice(line, 0, pos);
  val: Str = string::slice(line, pos + 1, len(line));
}
```

#### `string::slice`
```
fn string::slice(s: Str, lo: int, hi: int) -> Str
```
Half-open range `s[lo..hi)`. Both bounds must be within `0..=len(s)`.

```
first3: Str = string::slice(s, 0, 3);
```

#### `string::replace`
```
fn string::replace(s: Str, from: Str, to: Str) -> Str
```
Replaces all non-overlapping occurrences of `from` with `to`.

```
clean: Str = string::replace(raw, "\t", "  ");
```

#### `string::toUpper` / `string::toLower`
```
fn string::toUpper(s: Str) -> Str
fn string::toLower(s: Str) -> Str
```
ASCII-only case conversion; non-ASCII bytes are passed through unchanged.

#### `string::isNumeric` / `string::parseInt` / `string::parseFloat`
```
fn string::isNumeric(s: Str)   -> bool
fn string::parseInt(s: Str)    -> int
fn string::parseFloat(s: Str)  -> float
```
`isNumeric` validates before parsing — the bare `(int)s` / `(float)s` casts have
undefined behaviour on malformed input.

```
if (!string::isNumeric(tok)) {
  io::printErr("expected a number, got: " + tok);
  process::exit(1);
}
n: int = string::parseInt(tok);
```

---

## `std::process`

```
import std::process;
```

Process-level utilities: CLI arguments, controlled termination, and environment access.

### Quick Reference

| Symbol | Description |
|--------|-------------|
| `process::args()` | CLI arguments as `Str[]` (excludes program path) |
| `process::exit(code)` | Terminate with exit code; never returns |
| `process::getenv(name)` | Read environment variable; `""` if unset |

### Functions

#### `process::args`
```
fn process::args() -> Str[]
```
Returns command-line arguments **after** the program path. Empty array if none given.

```
import std::process;
import std::io;

fn main() -> int {
  argv: Str[] = process::args();
  if (len(argv) == 0) {
    io::printErr("usage: prog <name>");
    return 1;
  }
  io::print("hello, " + argv[0]);
  return 0;
}
```

#### `process::exit`
```
fn process::exit(code: int)
```
Terminates immediately. All open handles are flushed. Never returns.

```
fn assert(cond: bool, msg: Str) {
  if (!cond) {
    io::printErr("assertion failed: " + msg);
    process::exit(1);
  }
}
```

#### `process::getenv`
```
fn process::getenv(name: Str) -> Str
```
Returns the environment variable value, or `""` if not set.

```
home: Str = process::getenv("HOME");
if (len(home) == 0) { io::printErr("HOME is not set"); }
```

---

## Complete Example — File Copy

```
import std::io;
import std::process;

fn main() -> int {
  argv: Str[] = process::args();
  if (len(argv) < 2) {
    io::printErr("usage: copy <src> <dst>");
    return 1;
  }

  match io::open(argv[0], [io::Mode::Read]) {
    src: io::File {
      match io::open(argv[1], [io::Mode::Write]) {
        dst: io::File {
          ok: bool = io::write(dst, src.toString());
          io::close(dst);
          if (!ok) { 
            io::printErr("write failed"); 
            io::close(src); 
            return 1; 
          }
        }
        _ { 
            io::printErr("cannot open destination: " + argv[1]); 
            io::close(src); 
            return 1; 
          }
      }
      io::close(src);
    }
    _ { io::printErr("cannot open source: " + argv[0]); return 1; }
  }
  return 0;
}
```


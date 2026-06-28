# PaykanLang

PaykanLang (`.pkn`) is a statically-typed, object-oriented language. It compiles to LLVM IR and is **JIT-executed**.
Memory lifetimes are managed via automatic reference counting — there is no garbage collector.

> **Status: v0.0 — JIT only.** Programs are run via the LLVM JIT (`paykan program.pkn`). Ahead-of-time
> compilation to a standalone native binary is planned for v0.1 and is not yet implemented.

---

## Features

- **Static typing** — every variable, parameter, and return value has a compile-time type
- **Single-inheritance classes** with vtable-based virtual dispatch, automatic construction and destruction
- **Automatic reference counting (ARC)** — no GC pauses; objects are freed deterministically when the last reference drops
- **Match statements** — safe runtime type dispatch on class hierarchies and arrays
- **Arrays** — heap-allocated, dynamically-sized, element-typed (`int[]`, `Str[]`, `Point[]`, …)
- **Modules** — file-based module system with selective and aliased imports
- **Built-in I/O** — `open()` / `File` / `Error` for file I/O; `println` / `printerrln` for console output
- **LLVM backend** — programs are compiled to LLVM IR and JIT-executed (ahead-of-time native compilation planned for v0.1)

---

## Quick Tour

### Hello, World

```pkn
fn main() -> int {
  println("Hello, world!");
  return 0;
}
```

### Variables & Types

```pkn
x: int     = 42;
pi: float  = 3.14;
ok: bool   = True;
msg: Str   = "paykan";
inferred   = 100;          // type inferred as int
```

### Functions

```pkn
fn add(a: int, b: int) -> int {
  return a + b;
}

fn greet(name: Str) -> Str {
  return "Hello, " + name + "!";
}
```

- No overloading — each function name is unique.
- Every control-flow path in a non-void function must `return` a value.

### Classes

```pkn
class Animal {
  name: Str;
  fn __init__(n: Str) { self.name = n; }
  fn sound() -> Str   { return "..."; }
}

class Dog : Animal {
  fn __init__(n: Str) { __super__(n); }
  fn sound() -> Str   { return "woof"; }
}

a: Animal = Dog("Rex");
println(a.sound());   // woof  — virtual dispatch
```

- `__init__` is the constructor; `destroy` is the destructor (called automatically by ARC).
- `__super__(args)` calls the parent constructor and is required when the parent has a
  parameterised `__init__`.
- All methods are virtual.

### Match

`match` dispatches on the **runtime class** of a value. It works on any `Obj`-typed
expression, including arrays and the return value of `open()`.

```pkn
a: Obj = Dog("Rex");
match a {
  d: Dog { println(d.sound()); }   // binding + narrowed type
  Cat    { println("cat"); }
  _      { println("other"); }     // wildcard — must be last
}
```

### Arrays

```pkn
nums: int[]  = [1, 2, 3, 4, 5];
strs: Str[]  = ["hello", "world"];

println(StrInt(nums.len()));   // 5
nums.push(6);
nums.pop();

i: int = 0;
while (i < nums.len()) {
  println(StrInt(nums[i]));
  i = i + 1;
}
```

Arrays are `Obj` subtypes — they can be stored in `Obj` variables and matched with array-type arms.

### File I/O

`open()` returns `Obj` — either a `File` on success or an `Error` on failure.
Always use `match` to handle both cases.

```pkn
fn copyLines(src: Str, dst: Str) -> int {
  match open(src, "r") {
    err: Error { printerrln("cannot open: " + err.toString()); return 1; }
    inFile: File {
      match open(dst, "w") {
        err: Error { printerrln("cannot create: " + err.toString()); return 1; }
        outFile: File {
          while (True) {
            match inFile.readln() {
              line: Str { outFile.write(line); }
              _          { break; }   // None — EOF
            }
          }
        }
      }
    }
  }
  return 0;
}
```

`File` handles are closed automatically when they go out of scope.

### Modules

```pkn
// geometry/point.pkn
class Point {
  x: int; y: int;
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }
}

// main.pkn
import geometry::point;

fn main() -> int {
  p: point::Point = point::Point(3, 4);
  println(p.toString());
  return 0;
}
```

---

## Built-in Types

| Type    | Description                                                      |
|---------|------------------------------------------------------------------|
| `int`   | 64-bit signed integer                                            |
| `float` | 64-bit IEEE 754 double                                           |
| `bool`  | Boolean — literals `True` / `False`                             |
| `Str`   | Immutable heap-allocated string; `+` concatenates               |
| `T[]`   | Dynamic array of element type `T`                                |
| `Obj`   | Root of the class hierarchy; all classes extend `Obj`           |
| `File`  | Open file handle returned by `open()`; extends `Obj`            |
| `Error` | Error value returned by `open()` on failure; extends `Obj`      |
| `None`  | The absence of a value; an `Obj` literal                        |

---

## Built-in Functions

| Function            | Description                                              |
|---------------------|----------------------------------------------------------|
| `print(args…)`      | Print values to stdout, no newline                       |
| `println(args…)`    | Print values to stdout, then newline                     |
| `printerr(args…)`   | Print values to stderr, no newline                       |
| `printerrln(args…)` | Print values to stderr, then newline                     |
| `StrInt(n)`         | Convert `int` to `Str`                                   |
| `StrFloat(f)`       | Convert `float` to `Str`                                 |
| `StrBool(b)`        | Convert `bool` to `Str` (`"True"` / `"False"`)           |
| `open(path, mode)`  | Open a file (`"r"`, `"w"`, `"a"`); returns `File` or `Error` |

---

## Installing

### Homebrew (macOS)

```sh
brew tap parsabee/paykanlang https://github.com/parsabee/PaykanLang
brew install parsabee/paykanlang/paykanlang
paykan --version
```

---

## Building

PaykanLang uses CMake and requires LLVM (pre-built, vendored under `third-party/llvm`),
Bison, and Flex.

```sh
mkdir build && cd build
cmake ..
make -j$(nproc) paykan
```

The `paykan` binary is placed at `build/bin/paykan`.

To install the compiler to a prefix (the binary statically links the runtime, so it is
self-contained for JIT execution):

```sh
cmake --install build --prefix /usr/local
```

---

## Running Programs

```sh
# JIT-execute a source file
./build/bin/paykan program.pkn

# Emit LLVM IR (for inspection)
./build/bin/paykan --emit-llvm program.pkn

# Print the compiler and LLVM versions
./build/bin/paykan --version
```

---

## Running Tests

```sh
cd build
ctest --output-on-failure
```

---

## Development Tooling

Quality gates are enforced in CI and available locally via Python helpers in
`scripts/` (they use the vendored LLVM 17 tools so results match CI):

```sh
# Format check / apply
python3 scripts/clang_format.py            # check only (prints diffs)
python3 scripts/clang_format.py --apply    # reformat in place

# Bug-focused clang-tidy gate (needs a configured build/)
python3 scripts/run_clang_tidy.py --build-dir build

# Coverage report + floor gate (needs an instrumented tree)
cmake -B build-cov -DPAYKAN_COVERAGE=ON \
  -DCMAKE_C_COMPILER=build/third-party/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=build/third-party/llvm/bin/clang++
cmake --build build-cov --parallel
python3 scripts/coverage.py --build-dir build-cov
```

Install the pre-commit hooks (clang-format on commit, clang-tidy on push):

```sh
pip install pre-commit        # or: brew install pre-commit
pre-commit install
pre-commit install --hook-type pre-push
```

---

## Language Reference

Detailed documentation lives in `language_reference/`:

| File | Contents |
|------|----------|
| `01-language-basics.md`       | Types, variables, expressions, operators, control flow, built-ins |
| `02-functions-and-calling.md` | Function declarations, parameters, return values, ARC semantics   |
| `03-classes.md`               | Classes, fields, methods, inheritance, `__init__`, `destroy`      |
| `04-arrays.md`                | Array literals, subscript, `.len()`, `.push()`, `.pop()`, 2D arrays |
| `05-modules.md`               | Import forms, module paths, selective and aliased imports          |

---

## Known Limitations

v0.0 is an honest preview. The following are known gaps; fixes are planned for v0.1
(see [CHANGELOG.md](CHANGELOG.md) for the full list):

- **`Obj.equals()` on user-class instances is unreliable** — it may return `False`
  even for the same object (`b.equals(b)` is `False` while `b == b` is `True`).
  Use `==` for object identity.
- **`toString()` formatting is incomplete** — arrays print `Array@<addr>[len=N]`
  rather than `[1, 2, 3]`, and the default object `toString()` prints
  `Object@<addr>` rather than a class-name-based format.
- **JIT only** — ahead-of-time native compilation is planned for v0.1.

---

## License

See [LICENSE](LICENSE).

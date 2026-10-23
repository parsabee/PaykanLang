# PaykanLang

<p align="center">
  <img src="assets/images/paykan-logo.png" alt="PaykanLang logo" width="480">
</p>

PaykanLang (source files end in `.pkn`) is a statically typed language that compiles to native
code. It has type inference, classes with single inheritance and virtual methods, enums,
`match`, fixed-size arrays, tuples, structs, function types and lambdas, optional types,
generics, traits, sized numeric types, weak references, a file-based module system and a
standard library.
Memory is managed by automatic reference counting: there is no garbage collector and no
manual `free`, and objects are destroyed the moment their last reference goes away.

A program is type-checked, lowered to a small backend-neutral IR and compiled by a backend.
The built-in backend takes it through MLIR to LLVM: it runs a program straight from source
through a JIT, builds a standalone executable, and can also print the program as C11.

PaykanLang is young: the current release is an early one that lays the foundation. The road
ahead (thread-safe concurrency, offloading work to GPUs and other accelerators, C interop
through modules) is described in the
[README](https://github.com/parsabee/PaykanLang#readme) and the
[CHANGELOG](https://github.com/parsabee/PaykanLang/blob/develop/CHANGELOG.md).

## A taste

```pkn
import ::io;

enum Size { Small, Large }

fn describe(n: int) -> (Size, Str) {
  size = if n < 100 then Size::Small else Size::Large;
  return (size, "n=" + Str(n));
}

fn main() -> int {
  nums = [7, 250, 42];
  for i in 0..nums.len() {
    size, label = describe(nums[i]);
    match size {
      Small { io::println(label + " is small"); }
      Large { io::println(label + " is large"); }
    }
  }
  return 0;
}
```

Output:

```
n=7 is small
n=250 is large
n=42 is small
```

## Install

Each [GitHub release](https://github.com/parsabee/PaykanLang/releases) carries a tarball
for Linux (x86_64 and ARM64) and macOS (Apple Silicon), and a `.deb` package for Debian and Ubuntu,
which also install from the [apt repository](language/01-getting-started.md#with-apt-debian-and-ubuntu).
On macOS you can also use Homebrew:

```sh
brew install parsabee/paykanlang/paykanlang
```

`paykan build` links executables with the system C compiler, so a C compiler (`cc`) must
be installed to build them. [Getting started](language/01-getting-started.md) covers every way to install,
including building from source, and runs your first program.

## Documentation

- **[The language reference](language/index.md)** teaches the language from the ground
  up, a chapter per feature, and specifies each one precisely: every rule, limitation and
  compile-time error. Start with [Getting started](language/01-getting-started.md).
- **[The standard library](stdlib/index.md)** documents every module a program can import:
  `::io`, `::error`, `::result`, `::collections`, `::memory` and `::math`.
- **[Implementation and plugins](internals.md)** describes the compiler's IR, its backend
  and C output, and how to write your own frontend or backend as a plugin.

Every program with its output shown in this documentation is run by the project's test suite, so the
output shown is what the current compiler prints.

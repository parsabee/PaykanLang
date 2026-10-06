# PaykanLang

PaykanLang (source files end in `.pkn`) is a statically typed language that compiles to native
code. It has type inference, classes with single inheritance and virtual methods, enums,
`match`, dynamic arrays, tuples, optional types, generics and a file-based module system.
Memory is managed by automatic reference counting: there is no garbage collector and no
manual `free`, and objects are destroyed the moment their last reference goes away.

A program is type-checked, lowered to a small backend-neutral IR and compiled by a backend.
The default backend emits C and builds it with your system's C compiler; an optional LLVM
backend runs programs through a JIT. Either one can run a program straight from source or
build a standalone executable.

PaykanLang is young: the current release is an early one that lays the foundation. The road
ahead (thread-safe concurrency, offloading work to GPUs and other accelerators, C interop
through modules) is described in the
[README](https://github.com/parsabee/PaykanLang#readme) and the
[CHANGELOG](https://github.com/parsabee/PaykanLang/blob/develop/CHANGELOG.md).

## A taste

```pkn
enum Size { Small, Large }

fn describe(n: int) -> (Size, Str) {
  size = if n < 100 then Size::Small else Size::Large;
  return (size, "n=" + Str(n));
}

fn main() -> int {
  nums = [7, 250, 42];
  i = 0;
  while (i < nums.len()) {
    size, label = describe(nums[i]);
    match size {
      Small { println(label + " is small"); }
      Large { println(label + " is large"); }
    }
    i = i + 1;
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
for Linux (x86_64) and macOS (Apple Silicon), and a `.deb` package for Debian and Ubuntu. On
macOS you can also use Homebrew:

```sh
brew tap parsabee/paykanlang https://github.com/parsabee/PaykanLang
brew install parsabee/paykanlang/paykanlang
```

PaykanLang compiles programs with the system C compiler, so a C compiler (`cc`) must be
installed. [Getting started](manual/01-getting-started.md) covers every way to install,
including building from source, and runs your first program.

## Documentation

- **[The manual](manual/index.md)** teaches the language from the ground up, chapter by
  chapter, for programmers who are new to PaykanLang. Start here.
- **[The language reference](language/index.md)** specifies each feature precisely: every
  rule, limitation and compile-time error.
- **[Implementation and plugins](internals.md)** describes the compiler's IR, the C
  backend, and how to write your own frontend or backend as a plugin.

Every program in the manual and on this page is run by the project's test suite, so the
output shown is what the current compiler prints.

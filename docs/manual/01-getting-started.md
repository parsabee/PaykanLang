# Getting started

This chapter installs PaykanLang, runs a first program and builds it into an executable.

## What you need

PaykanLang compiles your programs with the **C compiler of your system**: the default
backend translates a program to C and hands it to `cc` (or to the compiler named by the
`CC` environment variable). So whichever way you install PaykanLang, make sure a C compiler
is installed:

- on Debian or Ubuntu, `sudo apt install build-essential` (or just `gcc`);
- on macOS, `xcode-select --install` (the Command Line Tools provide `cc`).

## Installing

### From a release tarball

Every [GitHub release](https://github.com/parsabee/PaykanLang/releases) carries a tarball
per platform, `paykanlang-<version>-linux-x86_64.tar.gz` and
`paykanlang-<version>-macos-arm64.tar.gz`, each with a `.sha256` file next to it. A tarball
unpacks into a `paykanlang/` directory holding the `paykan` compiler in `bin/` and the
runtime that compiled programs link against in `lib/` and `include/`. Unpack it anywhere
and put its `bin/` on your `PATH`:

```sh
$ sha256sum -c paykanlang-<version>-linux-x86_64.tar.gz.sha256   # macOS: shasum -a 256 -c
$ tar -xzf paykanlang-<version>-linux-x86_64.tar.gz
$ export PATH="$PWD/paykanlang/bin:$PATH"
$ paykan --version
```

`paykan` finds its runtime relative to its own location, so keep the directory together.

### With apt (Debian and Ubuntu)

On Debian, Ubuntu and their derivatives (x86_64), add the PaykanLang apt repository once,
then install with `apt`:

```sh
$ curl -fsSL https://parsabee.github.io/PaykanLang/apt/paykanlang.gpg \
  | sudo tee /usr/share/keyrings/paykanlang.gpg > /dev/null
$ echo "deb [arch=amd64 signed-by=/usr/share/keyrings/paykanlang.gpg] https://parsabee.github.io/PaykanLang/apt stable main" \
  | sudo tee /etc/apt/sources.list.d/paykanlang.list
$ sudo apt update
$ sudo apt install paykanlang
```

`sudo apt upgrade` picks up new releases. Each release also carries the `.deb` itself, which
`sudo apt install ./paykanlang_<version>_amd64.deb` installs directly.

### With Homebrew (macOS)

The repository doubles as a Homebrew tap:

```sh
$ brew tap parsabee/paykanlang https://github.com/parsabee/PaykanLang
$ brew install parsabee/paykanlang/paykanlang
$ paykan --version
```

The release tarballs and the Homebrew formula contain the core compiler: the default
frontend and the C backend.

### From source

Building PaykanLang needs CMake 3.24 or newer and a C++20 compiler (and, as always, a C
compiler to compile programs with):

```sh
$ git clone https://github.com/parsabee/PaykanLang.git
$ cd PaykanLang
$ cmake -B build
$ cmake --build build --parallel
$ build/bin/paykan --version
```

The default build needs nothing else. It also builds the test suite, which needs GoogleTest
and downloads it when CMake does not find one installed; configure with
`-DPAYKAN_BUILD_TESTS=OFF` to skip the tests. To install the compiler and its runtime under a
prefix:

```sh
$ cmake --install build --prefix /usr/local
```

A source build can also include the optional **LLVM backend**, which can run programs
in-process through a JIT. It is opt-in because it needs LLVM, which the configure step
downloads:

```sh
$ cmake -B build "-DPAYKAN_BACKENDS=llvm;c"
```

[The `paykan` command](13-the-paykan-command.md) explains how to choose a backend.

### Checking the installation

`paykan --version` prints the version and lists the frontend and backends the compiler
has, with the plugin directories it searches:

```text
$ paykan --version
PaykanLang <version>
accepts plugins built with PaykanLang <version>
frontend recursive-descent (built with PaykanLang <version>, compatible)
backend c (built with PaykanLang <version>, compatible)
plugin API 1
plugin directories: ...
```

## Hello, world

Create a file called `hello.pkn`:

```pkn
fn main() -> int {
  println("Hello, world!");
  return 0;
}
```

Output:

```
Hello, world!
```

and run it:

```sh
$ paykan hello.pkn
Hello, world!
```

A few things to notice:

- Every program has a function called `main`, where execution starts. `main` returns an
  `int`, which becomes the program's **exit status**: return `0` for success.
- `fn` declares a function; its return type comes after `->`.
- Statements end with a semicolon, and blocks use braces.
- `println` prints one value followed by a newline. (`print` leaves out the newline.)
- Comments start with `//` and run to the end of the line. There are no block comments.

## Running and building

`paykan hello.pkn` is short for `paykan run hello.pkn`: it compiles the program and runs it
right away. Anything after the file name is passed to the program as its command-line
arguments (chapter 11 shows how to read them):

```sh
$ paykan run hello.pkn
Hello, world!
```

`paykan build` compiles the program into a standalone native executable instead. `-o`
names it; without `-o`, the executable is named after the source file, without the `.pkn`:

```sh
$ paykan build hello.pkn -o hello
$ ./hello
Hello, world!
$ echo $?
0
```

The executable has the PaykanLang runtime linked in, so it runs without `paykan` or the
`.pkn` source.

After the first run you will find a `.paykan_cache/` directory next to `hello.pkn`. It holds
the generated C and compiled objects, so that the next run only recompiles what changed.
It is safe to delete at any time; chapter 13 says more.

## When something is wrong

PaykanLang checks the whole program before it runs any of it. A type error stops
compilation with a message that points at the problem:

```pkn
fn main() -> int {
  count: int = "three";
  return 0;
}
```

Error:

```
main.pkn:2:3: error: initializer of type 'Str' does not match declared type 'int' for variable 'count'
```

The message gives the file, line and column, then shows the offending line with the
expression underlined. `paykan` exits with status 1 when a program does not compile.

Some errors can only be found while the program runs, such as an array index out of bounds
or a division by zero. These **panics** print a message starting with `paykan:` to standard
error and stop the program:

```text
$ paykan crash.pkn
paykan: integer division or modulo by zero
$ echo $?
134
```

Output printed before a panic is never lost: standard output is flushed first.

## Source files

PaykanLang source files use the `.pkn` extension (`.pk` is accepted too). A file contains
only declarations (imports, functions, classes and enums), in any order: a function may call
another one declared further down the file. There are no statements at the top level, so all
code lives in functions.

Next, [Basics](02-basics.md) covers the core of the language.

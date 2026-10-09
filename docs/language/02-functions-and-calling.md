# PaykanLang — Functions & Argument Passing

---

## Function Declaration

```pkn
fn name(params) -> ReturnType {
  body
}

fn name(params) {
  // void return — no -> annotation needed
}
```

- No function overloading — each name must be unique in the program.
- Functions may be declared in any order — calling a function that is declared later in the
  file is fine (forward references are allowed).
- `main` must return `int`; its return value becomes the process exit code. `main` may
  optionally take the command-line arguments:

```pkn
fn main(args: Str[]) -> int {
  // args[0] is the source-file path; args[1..] are the arguments after it
  println(args[0]);
  return 0;
}
```

---

## Parameters

Each parameter has the form:

```pkn
paramName: TypeName
```

### Builtin type parameters (`i: int`, `f: float`, `b: bool`)

Passed by value — a copy is made at the call site.

### Class / Str / Array parameters (`s: Str`, `a: int[]`, `p: Point`)

The reference count of the object is incremented on entry and decremented on return.
The caller's variable is unaffected.

---

## Return Values

```pkn
fn add(a: int, b: int) -> int {
  return a + b;
}

fn nothing() {
  return;   // bare return for void
}
```

- Non-void functions must return a value matching the declared return type.
- `return;` in a non-void function is a compile-time error.
- Every control-flow path in a non-void function must end with a `return`.

---

## Examples

```pkn
fn writeLine(s: Str) {
  println(s);
}

fn greet(name: Str) -> Str {
  return "Hello, " + name + "!";
}

fn clamp(v: int, lo: int, hi: int) -> int {
  return if v < lo then lo else if v > hi then hi else v;
}

fn main() -> int {
  writeLine(greet("world"));
  println(Str<int>(clamp(150, 0, 100)));  // 100
  return 0;
}
```

---

## Recursive Functions

```pkn
fn factorial(n: int) -> int {
  if (n <= 1) { return 1; }
  return n * factorial(n - 1);
}

fn fib(n: int) -> int {
  if (n <= 1) { return n; }
  return fib(n - 1) + fib(n - 2);
}
```

---

## Native Functions

A `native fn` declares a function whose body is a C function, written against
the Paykan runtime (`Runtime.h`).  It is how a library implements what Paykan
cannot express itself, such as the standard library's file I/O (#198):

```pkn
native fn __io_write(fd: int, s: Str) -> int = "paykan_io_write";
```

The string is the C symbol.  The rest of the program calls the function like
any other.  Only these types can cross into C, with the runtime's own
convention:

| Paykan | C | Notes |
|--------|---|-------|
| `int`, `float`, `bool`, `char` | `int64_t`, `double`, `bool`, `int8_t` | by value |
| `Str`, `Obj` parameter | `PaykanObject *` | borrowed for the call; the C code must not keep it without a retain |
| `Str`, `Obj` result | `PaykanShared *` | owned (+1); never `NULL` |
| `Str?`, `Obj?` result | `PaykanShared *` | owned (+1); `NULL` is `None` |

A native function cannot be generic, cannot be a method, and cannot be `main`.
Its parameters are copies: `view` and `inout` don't apply to it yet (#21).

`paykan` does not build native code: you compile it however you like (any
compiler, flags or language that produces C-ABI symbols) and pass the objects
or archives with `--object`, a comma-separated list:

```sh
cc -std=c11 -fPIC -I<paykan include dir> -c text.c -o text.o
paykan --object=text.o,more.o main.pkn          # run (JIT or C backend)
paykan build --object=text.o main.pkn -o main   # an executable
```

Every backend links the objects; the llvm backend's `run` loads them into the
JIT, so build them position-independent (`-fPIC`).  There, the C code may call
the C library and the runtime functions compiled code uses
(`PaykanString_new`, `PaykanShared_new`, `Paykan_retain`, `Paykan_release`,
...); a built executable can call any runtime function.  A `PaykanString`'s
`data` and `len` may be read directly.  See `samples/imports/14_native`.

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Undeclared function | Calling a function that is not declared anywhere in the program |
| Function redefinition | Two functions with the same name |
| Builtin name reused | A function named like a builtin function or class (`fn print(...)`, `fn Str()`) — see *Builtin Names Are Reserved* in `01-language-basics.md` |
| Class / enum name reused | A function named like a class (its constructor) or an enum declared in the same program |
| Argument count mismatch | Wrong number of arguments |
| Argument type mismatch | Argument type incompatible with parameter type |
| Non-void return without value | `return;` in non-void function |
| Return type mismatch | Returned expression type ≠ declared return type |

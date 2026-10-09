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

## Parameter modes: view and inout

A parameter of a value type (`int`, `float`, `bool`, `char` or an enum) may have a mode,
`view` or `inout`, written where the type goes: `n: inout int`. A parameter without one is
passed by value, as above.

```pkn
fn bump(n: inout int) {
  n = n + 1;
}

fn scale(x: inout float, by: view float) {
  x = x * by;
}

fn main() -> int {
  k = 1;
  bump(k);              // no marking at the call: just the variable
  x = 1.5;
  scale(x, 2);
  println(Str(k) + " " + Str(x));
  return 0;
}
```

Output:

```
2 3
```

Modes work on functions, methods and constructors (`__init__`). A method keeps its
implicit `self`, which never takes a mode.

### `view`: read-only

A `view` parameter cannot be changed: it cannot be assigned or be a destructuring target
(`cannot assign to 'view' parameter 'x'`), and it cannot be passed to an `inout`
parameter (`'view' parameter 'x' cannot be passed to 'inout' parameter 'n'`). Reading it
gives a copy, which may be stored or returned freely. It is passed by value, so any
argument of its type will do: a literal, an expression, a `let` local.

### `inout`: the caller's storage

An `inout` parameter refers to its caller's storage: every write to it changes the
caller's variable or field at once, not when the call returns. The argument must be one
of:

- a local variable (not a `let` one: `'k' is declared with 'let' and cannot be passed to
  'inout' parameter 'n'`);
- a plain (by-value) parameter;
- another `inout` parameter, whose address is passed on;
- a field of an object: `obj.f`, `self.f`, or a chain such as `a.b.f`.

It must have exactly the parameter's type: an `int` variable does not go to an
`x: inout float` (`argument 1 of 'scale' has type 'int', but 'inout' parameter 'x' has
type 'float'`). Anything else is an error:

| Argument | Error |
|---|---|
| a literal or another expression (`bump(3)`, `bump(k + 1)`) | `argument 1 of 'bump' must be a variable or a field: parameter 'n' is 'inout'` |
| a `view` parameter | `'view' parameter 'v' cannot be passed to 'inout' parameter 'n'` |
| an array element (`bump(xs[0])`) | `an array element cannot be passed to 'inout' parameter 'n' yet` |
| a character of a string (`next(s[0])`) | `a character of a string cannot be passed to 'inout' parameter 'c'` |

A field is passed by its real address, never copied in and out, so the callee's writes
are visible through every reference to the object during the call. The caller keeps the
object alive for the duration of the call (objects never move), so the address stays
valid even if the callee drops the object from where it was reached:

```pkn
class Counter {
  hits: int;
  fn __init__() { self.hits = 0; }
}

fn hit(n: inout int, c: Counter) {
  n = n + 1;
  println("seen " + Str(c.hits));  // the write is already visible
}

fn main() -> int {
  c = Counter();
  hit(c.hits, c);
  println(Str(c.hits));
  return 0;
}
```

Output:

```
seen 1
1
```

### Exclusivity

One call cannot pass the same storage to two `inout` parameters: the same variable, or
the same chain of fields on the same variable (`a.x` and `a.x`, `self.n` and `self.n`).

```pkn
fn swap(a: inout int, b: inout int) {
  t = a;
  a = b;
  b = t;
}

fn main() -> int {
  k = 1;
  swap(k, k);
  return 0;
}
```

Error:

```
error: 'k' is passed to two 'inout' parameters ('a' and 'b')
```

The check is conservative: it compares how the arguments are written. Two different
paths that reach the same object at run time (`a.n` and `b.n` after `b = a`) are
accepted, and the callee then sees one storage through both parameters.

### Where modes apply

- Only to `int`, `float`, `bool`, `char` and enum parameters: anything else is an error
  naming the type, `'inout' applies only to int, float, bool, char and enum parameters;
  'Counter' is not a value type`. Strings, arrays, tuples, optionals, objects and `Obj`
  are references already (see above).
- Not to a type parameter, for now: `fn f<T>(x: inout T)` is an error (`'inout' does not
  apply to type parameter 'T' yet`). A generic function's other parameters may have
  modes.
- An override keeps every parameter's mode (`override of 'add' must keep 'inout' on
  parameter 'to'`, `override of 'add' must not add 'view' to parameter 'k'`).
- Across modules: a module's exported functions, constructors and methods keep their
  modes, whether the module is imported from source or from a `.pkm` file, and calls into
  it are checked the same way.
- Functions are not values in Paykan (only calls name them), so a mode is always known
  where the function is called.

`view` and `inout` are reserved words and may appear only before a parameter's type.
Written before the name, as in `inout n: int`, they are a syntax error that shows the
fix: `'inout' goes after the colon, before the type: write 'n: inout int'`.

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
Building and linking the C file next to a module is a separate step (#198).

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
| Parameter mode on a non-value type | `view`/`inout` on a parameter that is not `int`, `float`, `bool`, `char` or an enum, or on a type parameter |
| Write to a `view` parameter | Assigning or destructuring into it |
| Bad `inout` argument | Not a variable or field, not exactly the parameter's type, a `view` parameter or `let` local, an array element, a string's character, or the same place twice in one call |
| Override changes a mode | An override that drops, adds or changes a parameter's `view`/`inout` |

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

A parameter of any type may have a mode, `view` or `inout`, written where the type goes:
`n: inout int`, `c: view Counter`. A parameter without one is passed as above.

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

A `view` parameter cannot be changed, and neither can anything reached through it. It
cannot be assigned or be a destructuring target (`cannot assign to 'view' parameter 'x'`),
and it cannot be passed to an `inout` parameter (`'x' is a 'view' parameter; it cannot be
passed to 'inout' parameter 'n'`). It is passed like a parameter without a mode, so any
argument of its type will do: a literal, an expression, a `let` local, an object of a
subclass.

A `view` parameter of a value type is a copy, which may be assigned to a variable or
returned. One of an object, string, array, tuple or optional shares what the caller
passed, but the function cannot change it through the parameter:

- no assignment to a field or an element (`'c' is a 'view' parameter; cannot assign to
  its field 'n'`, `'xs' is a 'view' parameter; cannot assign to its elements`);
- no field or element passed to an `inout` parameter;
- only `view fn` methods may be called on it (`'c' is a 'view' parameter; 'tick' is not a
  'view fn'`): the class's own [`view fn`s](04-classes.md#methods-that-dont-change-self)
  and the builtin ones that only read (`toString`, `equals`, `len`, `length`; not `push`
  or `concat`).

```pkn
class Counter {
  n: int;
  fn __init__() { self.n = 0; }
  fn tick() { self.n = self.n + 1; }
  view fn get() -> int { return self.n; }
}

fn show(c: view Counter, label: view Str) {
  println(label + " " + Str(c.get()));
}

fn main() -> int {
  c = Counter();
  c.tick();
  show(c, "ticks");
  return 0;
}
```

Output:

```
ticks 1
```

#### A `view` stays a `view`

A `view` can only be passed on to another `view` parameter, whatever its type, so no
function it reaches can change it. With `fn report(c: Counter)`, `report(c)` is an error:
`'c' is a 'view' parameter; it can only be passed to a 'view' parameter, and parameter 1 of
'report' is not one`. The fix is to make that parameter `view` too. The builtin functions
only read (`println(c)`, `Str(n)`), and so do the builtin methods, except `push`.

A `view` of an object, string, array, tuple or optional shares what it holds, so it is
never stored or returned, where another name could change it later. Assigning it to a
variable, a field or an element, putting it in an array or tuple, `push` and `return` are
errors: `'c' is a 'view' parameter; it cannot be stored, only read or passed to a 'view'
parameter`, `'c' is a 'view' parameter; it cannot be returned`. A `view` of a value type is
copied when it is assigned (`m = n`), and the copy is the variable's own.

A `match` arm's name for a `view` is a `view` too: in `match c { k: Fast { k.tick(); } }`,
`k.tick()` is an error, `'k' is bound to a 'view'; 'tick' is not a 'view fn'`.

`self` in a `view fn` follows the same rules, except that a method may return part of
`self`. Its caller does not yet see that result as a `view`; a `-> view T` result is
planned for it.

### `inout`: the caller's storage

An `inout` parameter refers to its caller's storage: every write to it changes the
caller's variable or field at once, not when the call returns. For an object, string,
array, tuple or optional, the parameter is the caller's variable itself, not another
reference to what it holds: passing it shares nothing, and assigning to it replaces what
the caller's variable holds.

```pkn
fn swap(a: inout Str, b: inout Str) {
  t = a;
  a = b;
  b = t;
}

fn main() -> int {
  x = "left";
  y = "right";
  swap(x, y);
  println(x + " " + y);
  return 0;
}
```

Output:

```
right left
```

The argument must be one of:

- a local variable (not a `let` one: `'k' is 'let'; it cannot be passed to 'inout'
  parameter 'n'`);
- a plain (by-value) parameter;
- another `inout` parameter, whose address is passed on;
- a field of an object: `obj.f`, `self.f`, or a chain such as `a.b.f`, including one of
  the object a `let` local holds (not one reached through a `view` parameter, or through
  `self` in a `view fn`).

It must have exactly the parameter's type: an `int` variable does not go to an
`x: inout float` (`argument 1 of 'scale' has type 'int', but 'inout' parameter 'x' has
type 'float'`), and a `Fast` variable does not go to a `c: inout Counter` even when `Fast`
is a subclass of `Counter`, since the function could store another kind of `Counter` in
it. Anything else is an error:

| Argument | Error |
|---|---|
| a literal or another expression (`bump(3)`, `bump(k + 1)`) | `argument 1 of 'bump' must be a variable or a field: parameter 'n' is 'inout'` |
| a `view` parameter | `'v' is a 'view' parameter; it cannot be passed to 'inout' parameter 'n'` |
| an array element (`bump(xs[0])`) | `an array element cannot be passed to 'inout' parameter 'n' yet` |
| a character of a string (`next(s[0])`) | `a character of a string cannot be passed to 'inout' parameter 'c'` |
| `self` (`reset(self)`) | `'self' cannot be passed to 'inout' parameter 'c': the method would no longer know its object` |

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

- To a parameter of any type: value types, strings, arrays, tuples, optionals, objects,
  `Obj`, and type parameters (`fn swap<T>(a: inout T, b: inout T)`).
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
| Write to a `view` parameter | Assigning or destructuring into it, assigning to a field or element of what it holds, or calling a method on it that is not a `view fn` |
| A `view` passed on or kept | Passing it to a parameter that is not `view`; storing or returning one that shares what it holds |
| Bad `inout` argument | Not a variable or field, not exactly the parameter's type, `self`, a `let` local, something reached through a `view` parameter, an array element, a string's character, or the same place twice in one call |
| Override changes a mode | An override that drops, adds or changes a parameter's `view`/`inout` |

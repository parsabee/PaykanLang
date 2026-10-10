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

A `view` parameter cannot be changed: it cannot be assigned or be a destructuring target
(`cannot assign to 'view' parameter 'x'`), and it cannot be passed to an `inout`
parameter (`'x' is a 'view' parameter; it cannot be passed to 'inout' parameter 'n'`).
It is passed like a parameter without a mode, so any argument of its type will do: a
literal, an expression, a `let` local, an object of a subclass.

A `view` parameter of a value type is a copy, which may be assigned to a variable or
returned. One of an object, string, array, tuple or optional shares what the caller
passed, but the function cannot change it through the parameter: no assignment to a field
or an element (`'c' is a 'view' parameter; cannot assign to its field 'n'`), no field or
element passed to an `inout` parameter, and only a `view fn` can be called on it
(`toString`, `equals`, `len`, `length`, or a method of its class declared `view fn`, see
[Methods that don't change `self`](04-classes.md#methods-that-dont-change-self); `'c' is a
'view' parameter; 'tick' is not a 'view fn'`).

```pkn
class Counter {
  n: int;
  fn __init__() { self.n = 0; }
  fn tick() { self.n = self.n + 1; }
}

fn show(c: view Counter, label: view Str) {
  println(label + " " + Str(c.n));
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

A `view`, a parameter or a [local](01-language-basics.md#local-borrows), can only be passed
on to another `view` parameter, whatever its type, so no function it reaches can change
it. With `fn twice(n: int)`, `twice(v)` is an error: `'v' is a 'view' local; it can only
be passed to a 'view' parameter, and parameter 1 of 'twice' is not one`. The fix is to make
that parameter `view` too. The builtin functions only read (`println(v)`), and so do the
builtin methods, except `push`. `Str(s)` with a `Str` returns `s` itself, so it takes no
`view`.

A `view` of an object, string, array, tuple or optional shares what it holds, so it is
never stored or returned, where another name could change it later. Assigning it to a
variable, a field or an element, putting it in an array or tuple, `push` and `return` are
errors: `'v' is a 'view' local; it cannot be stored, only read or passed to a 'view'
parameter`, `'v' is a 'view' local; it cannot be returned`. A `view` of a value type is
copied when it is assigned (`m = n`), and the copy is the variable's own.

A `match` arm's name for a `view` is a `view` too: in `match v { k: Fast { k.tick(); } }`,
`k.tick()` is an error, `'k' is bound to a 'view'; 'tick' is not a 'view fn'`.

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

- a local variable (not a `let` one: `'k' is declared with 'let' and cannot be passed to
  'inout' parameter 'n'`);
- a plain (by-value) parameter;
- another `inout` parameter, whose address is passed on;
- a field of an object: `obj.f`, `self.f`, or a chain such as `a.b.f`.

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

### Borrowed results

A function or method can return a borrow instead of a copy, with the mode where the result's
type goes: `-> view T` or `-> inout T`. The result is part of what the function was given as
a borrow, so it needs no annotation saying where it comes from:

```pkn
class Counter {
  n: int;
  fn __init__(n: int) { self.n = n; }
  view fn get() -> int { return self.n; }
}

class Box {
  c: Counter;
  fn __init__() { self.c = Counter(4); }
  view fn peek() -> view Counter { return self.c; }
}

fn longer(a: view Str, b: view Str) -> view Str {
  return if a.len() > b.len() then a else b;
}

fn main() -> int {
  b = Box();
  v: view = b.peek();
  println(longer("hello", "hi") + " " + Str(v.get()));
  return 0;
}
```

Output:

```
hello 4
```

- What a `-> view T` function returns must be part of `self` or of a `view` or `inout`
  parameter: the parameter itself, a field or an element reached from it, or the borrowed
  result of another call that borrows only those. A new value, a copy parameter or a local
  is an error: `'d' is a copy: a 'view' result must be part of 'self' or of a 'view' or
  'inout' parameter`.
- The caller sees the result as a `view` ([A `view` stays a `view`](#a-view-stays-a-view)):
  it can be read, passed on to a `view` parameter or bound to a `view` local, and a value
  type can be copied out (`k = b.num();`); nothing changes through it and one that shares
  what it holds is never stored (`the result of 'peek' is a 'view'; 'tick' is not a 'view
  fn'`).
- A borrowed result borrows what its call borrowed: the receiver and every argument passed
  to a `view` or `inout` parameter. While a `view` local of it (or a `match` arm's
  `n: view T`) is live, those variables cannot change, as for any
  [local borrow](01-language-basics.md#local-borrows): with `w: view = longer(s, t);`,
  `t = "bye";` is `'t' is viewed by 'view' local 'w' until 'w' is last used; it cannot be
  assigned`. Both arguments are borrowed, though only one comes back; a value type can be
  copied out instead.
- How a function returns its result is part of a module's interface, imported from source
  or from a `.pkm` file.
- `main` returns a copy. An override returns its result the way the method it overrides
  does.

`-> inout T` returns the storage of part of `self` or of an `inout` parameter, as an `inout`
local names it: the caller writes through it.

```pkn
class Box {
  k: int;
  fn __init__() { self.k = 3; }
  fn num() -> inout int { return self.k; }
}

fn pick(a: inout int, b: inout int, first: bool) -> inout int {
  return if first then a else b;
}

fn bump(n: inout int) { n = n + 1; }

fn main() -> int {
  b = Box();
  x: inout = b.num();
  x = x + 10;      // writes b.k
  bump(b.num());   // passed on as b.k's storage
  i = 1;
  j = 2;
  p: inout = pick(i, j, False);
  p = 20;          // writes j
  println(Str(b.k) + " " + Str(i) + " " + Str(j));
  return 0;
}
```

Output:

```
14 1 20
```

- What it returns is a place an `inout` argument could be: part of `self` (not `self`
  itself) or of an `inout` parameter, not a `view`, a copy or a local, not an array element
  yet, of exactly the result's type (`an 'inout' result has type 'Animal', but what it
  returns has type 'Dog'`). A `view fn` cannot return one.
- The caller binds it to an `inout` local, passes it on to an `inout` parameter, calls a
  method on it or reads it (`y = b.num() + 1;` copies). It borrows the receiver and the
  arguments to the call's `inout` parameters, as an `inout` local of them would: while a
  local of it is live, they cannot be used (`'b' is borrowed by 'inout' local 'x' until 'x'
  is last used`).
- The receiver of a method returning `inout` is a variable or a field, which outlives the
  call: `Box().num()` is an error.

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
| Write to a `view` parameter | Assigning or destructuring into it |
| A `view` changed, passed on or kept | Writing a field or element of what it holds, calling a method on it that is not a `view fn`, passing it to a parameter that is not `view`, or storing or returning one that shares what it holds |
| Bad `inout` argument | Not a variable or field, not exactly the parameter's type, a `view` parameter or `let` local, an array element, a string's character, or the same place twice in one call |
| Override changes a mode | An override that drops, adds or changes a parameter's `view`/`inout`, or returns a borrow where the method it overrides returns a copy (or the other way) |
| A `view` result of something else | A `-> view T` function returns a new value, a copy parameter or a local, not part of `self` or of a `view` / `inout` parameter |
| An `inout` result of something else | A `-> inout T` function returns what an `inout` argument could not be, or what is not part of `self` or of an `inout` parameter, or another type; a `view fn` returns `inout`; the receiver of a method returning `inout` is not a variable or a field |

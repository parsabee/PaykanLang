# Functions

This chapter covers declaring and calling functions: parameters and how each kind of
argument is passed, the `view` and `inout` parameter modes, return values and borrowed
results, recursion, and native functions written in C. Using a function as a value (function
types and lambdas) has its own chapter, [Function types](14-function-types.md).

---

## Function Declaration

A function declares its parameters with their types, and its return type after `->`. A
function without `->` returns nothing (`void`), and may use a bare `return;` to leave early:

```pkn
import ::io;

fn add(a: int, b: int) -> int {
  return a + b;
}

fn greet(name: Str) {
  if name == "" {
    return;
  }
  io::println("Hello, " + name + "!");
}

fn main() -> int {
  greet("Grace");
  greet("");
  io::println(Str(add(2, 3)));
  return 0;
}
```

Output:

```
Hello, Grace!
5
```

The general form is:

```pkn
fn name(params) -> ReturnType {
  body
}

fn name(params) {
  // void return — no -> annotation needed
}
```

- Functions may be declared in any order: calling a function that is declared later in the
  file is fine (forward references are allowed), and a function may call itself
  ([Recursive Functions](#recursive-functions)).
- No function overloading: each name is declared once in the program (`redefinition of
  function 'f'`). A function cannot take the name of a builtin type or class (`int`,
  `float`, `bool`, `char`, `void`, `Str`, `Obj`, the sized numeric types), nor that of a
  class or enum of the program; see [Builtin Names Are
  Reserved](02-basics.md#builtin-names-are-reserved). The standard library's names, such as
  `print` or `open`, are not reserved: they live in their module (`io::print`), and a
  program may declare its own `fn print`.
- A function is a value too: `f = square;` holds it, and `fn(x: int) -> int { ... }` is a
  function without a name (a lambda). Its type is `(int) -> int`; see
  [Function types](14-function-types.md).
- `main` must return `int`, and its return value becomes the process exit code. A non-zero
  status tells the shell that something went wrong, which matters for scripts:
  `paykan check.pkn && echo ok` prints `ok` only when `main` returned 0. Any other
  signature is an error (`the program's entry point must be 'fn main() -> int' or 'fn
  main(args: view Str[]) -> int'`). `main` may optionally take the command-line arguments
  (see [`::io`](../stdlib/io.md)):

```pkn
import ::io;

fn main(args: view Str[]) -> int {
  // args[0] is the source-file path; args[1..] are the arguments after it
  io::println(args[0]);
  return 0;
}
```

---

## Parameters

Each parameter has the form:

```pkn
paramName: TypeName
```

A free function may also leave the type out (see [Untyped Parameters](#untyped-parameters)).

How an argument is passed depends on its type, and a [mode](#parameter-modes-view-and-inout)
can change it. Without a mode, the call passes a copy of a value type and a reference to an
object or a string:

```pkn
import ::io;

class P {
  x: int;
}

impl P {
  fn __init__() { self.x = 0; }
}

fn f(n: int, s: Str, p: P) {
  n = 5;        // the function's own copy
  p.x = 7;      // the caller's object
  p = P();      // the function's own reference, now to another object
  p.x = 9;
  s = "other";
}

fn main() -> int {
  n = 1;
  s = "mine";
  p = P();
  f(n, s, p);
  io::println(Str(n) + " " + s + " " + Str(p.x));
  return 0;
}
```

Output:

```
1 mine 7
```

### Builtin type parameters (`i: int`, `f: float`, `b: bool`)

Passed by value — a copy is made at the call site. The same holds for `char` and the
[sized numeric types](02-basics.md#sized-numeric-types): assigning to the parameter changes
the function's copy only.

### Class / Str parameters (`s: Str`, `p: Point`)

Strings and objects are passed by reference: the function receives the same object as its
caller, so a field it assigns or a method that changes the object is seen by the caller.
A caller that keeps the object alive for the whole call lends it, and the function changes
no reference count unless it keeps the object (see
[Borrowed parameters](16-memory.md#borrowed-parameters)).
Assigning to the parameter makes it refer to another object; the caller's variable is
unaffected. More in [Classes](05-classes.md) and [Memory](16-memory.md).

### Array parameters (`a: int[3]`, `xs: view int[]`)

A fixed-size array is a value: a plain `a: int[3]` parameter is the function's own copy.
A `view int[]` or `inout int[]` parameter is a slice, which reaches the caller's array of
any length in place (see [Slices](09-arrays.md#slices)).

---

## Parameter modes: view and inout

A parameter of any type may have a mode, `view` or `inout`, written where the type goes:
`n: inout int`, `c: view Counter`. An `inout` parameter is the caller's variable (or a field
of an object): the function changes it in place, and assigning an object to it gives the
caller that object. A `view` parameter is read-only: the function cannot assign to it, nor
change the object, string or array it holds. A parameter without a mode is passed as
[above](#parameters). The call looks the same either way:

```pkn
import ::io;

fn addTax(price: inout float, rate: view float) {
  price = price + price * rate;
}

fn main() -> int {
  price = 20.0;
  addTax(price, 0.5);
  io::println("price = " + Str(price));
  return 0;
}
```

Output:

```
price = 30
```

An `inout` argument is written as just the variable, and a `view` parameter takes any
argument of its type, a literal included:

```pkn
import ::io;

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
  io::println(Str(k) + " " + Str(x));
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
(`toString`, `equals`, `len`, or a method of its class declared `view fn`, see
[Methods that don't change `self`](05-classes.md#methods-that-dont-change-self); `'c' is a
'view' parameter; 'tick' is not a 'view fn'`). A conditional's object is what one of its
branches holds, so it cannot change where either branch's could not: with `d` a `Counter`
that may change, `(if k == 0 then c else d).n = 2` and `(if k == 0 then c else d).tick()`
are the same errors.

```pkn
import ::io;

class Counter {
  n: int;
  fn tick() { self.n = self.n + 1; }
}

impl Counter {
  fn __init__() { self.n = 0; }
}

fn show(c: view Counter, label: view Str) {
  io::println(label + " " + Str(c.n));
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

#### A `view` of a reference stays a `view`

A `view` of a reference -- an object, a string, an optional of one, or an array, tuple or
struct holding one -- can only be passed on to another `view` parameter, a parameter or a
[local](02-basics.md#local-borrows) alike, so no function it reaches can change or
keep what it shares. With `fn keep(c: Counter)` and `v: view = Counter()`, `keep(v)` is an
error: `'v' is a 'view' local; it can only be passed to a 'view' parameter, and parameter 1
of 'keep' is not one`. The fix is to make that parameter `view` too. The builtin methods
only read, and so do the standard library's print functions (`io::println(v)` takes a
`view T`). `Str(s)` with a `Str`, like `Str<Str>(s)`, returns `s`
itself, so it takes no `view`.

A value type read through a `view` -- an `int` parameter `n: view int`, a local `k: view =
3`, a field `v.n` of a viewed object, an element `xs[i]` of a `view int[]`, a `view int`
result, or a struct, tuple or fixed-size array of value types -- is passed to a by-value
parameter as a copy, which cannot change what it was read from, just as assigning it
(`m = n`) copies it. So `twice(n)`, `f(xs[i])` with `f: (int) -> int` and, in a `view fn`,
`g(self.x)` with `x: int` are fine; `g(self.name)` with `name: Str` is not (`'self' is
read-only in 'view fn m'; it can only be passed to a 'view' parameter, and parameter 1 of
'g' is not one`).

A `view` of an object, string or optional, or of an array or tuple holding one, shares what
it holds, so it is never stored or returned, where another name could change it later.
Assigning it to a variable, a field or an element and `return` are errors: `'v' is a 'view'
local; it cannot be stored, only read or passed to a 'view' parameter`, `'v' is a 'view'
local; it cannot be returned`. A `view` of a value type, or of an array or tuple of them
(`int[3]`, `(int, float)`), is copied when it is assigned (`m = n`), and the copy is the
variable's own. An array or tuple literal holds its elements, so it stores them too: a
`view` of a value type can be one of its elements, as a copy, and one that shares what it
holds cannot, as a copy of it in the literal could change it later (`[v]`, `(v, 1)` and
`[v; 3]` with `v: view Node` are `'v' is a 'view' parameter; it cannot be stored, only read
or passed to a 'view' parameter`, and so is `[self]` in a `view fn`). That holds wherever
the literal goes, a `view` parameter included: with `l` a `List<(Str, Node)>`,
`l.contains(("ab", l.at(0).1))` is `the result of 'at' is a 'view'; it cannot be stored,
only read or passed to a 'view' parameter`. A copying accessor gives the literal an element
of its own: `l.contains(("ab", l.get(0).1))`.

A `match` arm's name for a `view` is a `view` too: in `match v { k: Fast { k.tick(); } }`,
`k.tick()` is an error, `'k' is bound to a 'view'; 'tick' is not a 'view fn'`.

### `inout`: the caller's storage

An `inout` parameter refers to its caller's storage: every write to it changes the
caller's variable or field at once, not when the call returns. For an object, string,
array, tuple or optional, the parameter is the caller's variable itself, not another
reference to what it holds: passing it shares nothing, and assigning to it replaces what
the caller's variable holds.

```pkn
import ::io;

fn swap(a: inout Str, b: inout Str) {
  t = a;
  a = b;
  b = t;
}

fn main() -> int {
  x = "left";
  y = "right";
  swap(x, y);
  io::println(x + " " + y);
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
import ::io;

class Counter {
  hits: int;
}

impl Counter {
  fn __init__() { self.hits = 0; }
}

fn hit(n: inout int, c: Counter) {
  n = n + 1;
  io::println("seen " + Str(c.hits));  // the write is already visible
}

fn main() -> int {
  c = Counter();
  hit(c.hits, c);
  io::println(Str(c.hits));
  return 0;
}
```

Output:

```
seen 1
1
```

### Exclusivity

What one call passes to an `inout` parameter shares no storage with anything else the
call borrows: another `inout` argument, a `view` argument, or the receiver, when the
method may change it or is a `view fn`. A variable and a chain of fields on it share
storage (`p` and `p.x`, `self` and `self.n`); two fields of one variable don't (`p.x` and
`p.y`), and an element shares its array's.

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

The other overlaps are errors the same way:

| Call | Error |
|---|---|
| `both(k, k)` with `fn both(a: view int, b: inout int)` | `'k' is passed to 'view' parameter 'a'; it cannot also be passed to 'inout' parameter 'b'` |
| `two(p, p.x)` with `fn two(a: inout P, b: inout int)` | `'p.x' is part of 'p', passed to 'inout' parameter 'a'; it cannot also be passed to 'inout' parameter 'b'` |
| `p.take(p.x)`, `take` a method that may change `self` | `'p.x' is part of 'p', the receiver of 'take' (not a 'view fn'); it cannot also be passed to 'inout' parameter 'm'` |
| `p.look(p.x)`, `look` a `view fn` | `'p.x' is part of 'p', the receiver of 'view fn look'; it cannot also be passed to 'inout' parameter 'm'` |
| `p.merge(p)`, `merge` a method that may change `self` and takes `o: view P` | `'p' is the receiver of 'merge' (not a 'view fn'); it cannot also be passed to 'view' parameter 'o'` |

A plain parameter is a copy, so it may name the same storage as an `inout` one:
`three(k, j, k)` with `fn three(a: inout int, b: inout int, c: int)` is fine.

The check is conservative: it compares how the arguments are written. Two different
paths that reach the same object at run time (`a.n` and `b.n` after `b = a`) are
accepted, and the callee then sees one storage through both parameters.

Exclusivity is a rule of one call. An expression may read a variable and also pass it to
an `inout` parameter of a call inside it. Its operands are evaluated left to right, and a
value read before the call is the one used, even when the call replaces what the variable
holds:

```pkn
import ::io;

fn shout(s: inout Str) -> Str {
  s = s + "!";
  return s;
}

fn main() -> int {
  s = "hi";
  t = s + shout(s); // the s read before shout changes it
  io::println(t + " " + s);
  return 0;
}
```

Output:

```
hihi! hi!
```

The same holds for what is read through an object, which any call given the object may change:
a field's value read before the call is the one used, the method of `a.b.m(...)` runs on the
object that was in `a.b`, and `a.b.f = ...` assigns a field of that object, whatever the call
assigns to `a.b`.

```pkn
import ::io;

class Box {
  label: Str;
}

class Shelf {
  box: Box;
}

fn swap(s: Shelf) -> Str {
  s.box = Box("new");
  return "!";
}

fn main() -> int {
  shelf = Shelf(Box("old"));
  t = shelf.box.label + swap(shelf); // the label read before swap replaces the box
  io::println(t + " " + shelf.box.label);
  return 0;
}
```

Output:

```
old! new
```

### Where modes apply

- To a parameter of any type: value types, strings, arrays, tuples, optionals, objects,
  `Obj`, and type parameters (`fn swap<T>(a: inout T, b: inout T)`).
- An override keeps every parameter's mode (`override of 'add' must keep 'inout' on
  parameter 'to'`, `override of 'add' must not add 'view' to parameter 'k'`).
- Across modules: a module's exported functions, constructors and methods keep their
  modes, whether the module is imported from source or from a `.pkm` file, and calls into
  it are checked the same way.
- A function value's type carries its parameters' modes (`(inout int) -> void`), so a
  mode is always known where a function is called, by name or through a value
  ([Function types](14-function-types.md)).

`view` and `inout` are reserved words. They go where a type goes: a parameter's, a
[local borrow's](02-basics.md#local-borrows), a `match` arm's or a result's
([Borrowed results](#borrowed-results)). Written before the name, as in `inout n: int`,
they are a syntax error that shows the fix: `'inout' goes after the colon, before the
type: write 'n: inout int'`.

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

- Non-void functions must return a value matching the declared return type (`return
  value of type 'Str' does not match function return type 'int'`).
- `return;` in a non-void function is a compile-time error (`non-void function must
  return a value`).
- The compiler checks that every control-flow path in a non-void function ends with a
  `return` (`non-void function 'g' does not always return a value`). A `while True` loop
  with no `break` out of it never falls through, so a function it ends needs no `return`
  after it; a `break` in a loop nested inside it leaves only that loop.

### Borrowed results

A function or method can return a borrow instead of a copy, with the mode where the result's
type goes: `-> view T` or `-> inout T`. The result is part of what the function was given as
a borrow, so it needs no annotation saying where it comes from:

```pkn
import ::io;

class Counter {
  n: int;
  view fn get() -> int { return self.n; }
}

impl Counter {
  fn __init__(n: int) { self.n = n; }
}

class Box {
  c: Counter;
  view fn peek() -> view Counter { return self.c; }
}

impl Box {
  fn __init__() { self.c = Counter(4); }
}

fn longer(a: view Str, b: view Str) -> view Str {
  return if a.len() > b.len() then a else b;
}

fn main() -> int {
  b = Box();
  v: view = b.peek();
  io::println(longer("hello", "hi") + " " + Str(v.get()));
  return 0;
}
```

Output:

```
hello 4
```

- What a `-> view T` function returns must be part of `self` or of a `view` or `inout`
  parameter: the parameter itself, a field or an element reached from it, a `match` arm's
  name for a `view` of one (`match self.head { n: Node { return n.v; } ... }`), or the
  borrowed result of another call that borrows only those. A new value, a copy parameter or a local
  is an error: `'d' is a copy: a 'view' result must be part of 'self' or of a 'view' or
  'inout' parameter`.
- The caller sees the result as a `view` ([A `view` stays a `view`](#a-view-of-a-reference-stays-a-view)):
  it can be read, passed on to a `view` parameter or bound to a `view` local, and a value
  type can be copied out (`k = b.num();`); nothing changes through it and one that shares
  what it holds is never stored (`the result of 'peek' is a 'view'; 'tick' is not a 'view
  fn'`).
- In a `view fn`, part of `self` that shares what it holds is a `view`, so it is returned
  as one: `view fn name() -> view Str { return self.name; }`. As a copy it is an error,
  `'self' is read-only in 'view fn name', so part of it can only be returned as a 'view':
  write '-> view Str'` ([Methods that don't change
  self](05-classes.md#methods-that-dont-change-self)).
- A borrowed result borrows what its call borrowed: the receiver and every argument passed
  to a `view` or `inout` parameter. While a `view` local of it (or a `match` arm's
  `n: view T`) is live, those variables cannot change, as for any
  [local borrow](02-basics.md#local-borrows): with `w: view = longer(s, t);`,
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
import ::io;

class Box {
  k: int;
  fn num() -> inout int { return self.k; }
}

impl Box {
  fn __init__() { self.k = 3; }
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
  io::println(Str(b.k) + " " + Str(i) + " " + Str(j));
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
  call: `Box().num()` is an error (`the receiver of 'num' must be a variable or a field:
  its 'inout' result is part of it`).
- While the result is used, its caller keeps alive what it is part of: the receiver, the
  objects holding the `inout` arguments, and the objects those arguments hold
  (`return b.k;` with `b: inout B`). So the result must be in that storage, as the next
  section says.

#### Where an `inout` result can point

An `inout` result can only point into storage its caller keeps alive while the result is
used:

- Into `self`: a field of a class's `self`, or a field of a struct or an element of a tuple
  held in it, at any depth (`return self.k;`, `return self.pos.x;`). A struct's `self` is
  the receiver's storage, as an `inout` parameter's is.
- Into what an `inout` parameter names: its storage (`return n;`, `return s.x;`), and the
  object a reference held there refers to: the object of a class-typed one (`return n.k;`
  with `n: inout Node`), or one that a struct, a tuple or a fixed-size array holds
  (`return s.b.k;` with `s: inout S` and `b` a class-typed field, `return a[5].k;` with
  `a: inout B[6]`). Not an object in a slice (`a: inout B[]`): its caller keeps none of
  them alive.
- Wherever another call's `inout` result can point, when all of it is allowed here: the
  storage of the call's receiver and of each of its `inout` arguments, and the object a
  reference held there refers to when that object can hold the result (the receiver's
  object, for a class's method): `return self.key();`, `return pick(n);` with
  `n: inout Node`.

A reference reached past that leads to a separate object. Another reference could replace
it while the result is in use, freeing the storage the result names, so following one is an
error:

```pkn
class C { k: Str; }

class H {
  c: C;
  fn e() -> inout Str { return self.c.k; }
}

fn main() -> int { return 0; }
```

Error:

```
error: 'e' returns an 'inout' reference into 'self.c', a separate object: an 'inout' result can only point into 'self' or into an 'inout' parameter's storage; return a copy, or call an accessor on 'self.c' directly
```

So are `return self.c.p.x;`, `return self.arr[0].k;` when the elements are objects,
`return n.c.k;` with `n: inout Node`, and a call's result in such an object: `return
self.c.slot();` is `'e' returns the 'inout' result of 'slot', which can point into
'self.c', a separate object: ...`. A reference followed from a call's result is named by
the call: `return deep(n).k;`, with `deep(n: inout Node) -> inout C` and `Node` holding a
`C`, is `'f' returns an 'inout' reference into a separate object, the one referred to by the
'inout' result of 'deep', which can point into 'n': ...`.

A call's result counts as pointing wherever its signature lets it point, whatever its body
returns. Past the storage it borrowed, that is an object held there that can hold a place
of the result's type: one with a field of that type, or of a struct, a tuple or a
fixed-size array holding one, or with a method whose `inout` result is or holds one (an
override in a subclass can return a field of its own). With `struct S { c: C; n: int; }`,
`return self.s.num();` for a method `num() -> inout int` of `S` is an error when `C` has an
`int` field, even if `num` returns `self.n` (`return self.s.n;` is not). When `C` holds no
`C`, `return self.s.ref();` for `ref() -> inout C` is in `self.s`, and `return s.ref().k;`
with `s: inout S` is in the object `s` holds, as `return s.c.k;` is.

Return a copy instead (`-> Str`), or let the caller call the accessor of the object itself:
its receiver is then what the caller keeps alive.

```pkn
import ::io;

class Counter {
  n: int;
  fn count() -> inout int { return self.n; }
}

class Panel {
  c: Counter;
  hits: int;
  fn total() -> inout int { return self.hits; }
  view fn seen() -> int { return self.c.n; }   // a copy of a separate object's field
}

fn countOf(c: inout Counter) -> inout int { return c.n; }

fn bump(n: inout int) { n = n + 1; }

fn main() -> int {
  p = Panel(Counter(1), 10);
  bump(p.total());        // in p
  bump(p.c.count());      // in p.c, the receiver
  bump(countOf(p.c));     // in the object the argument refers to
  io::println(Str(p.hits) + " " + Str(p.seen()));
  return 0;
}
```

Output:

```
11 3
```

The rule is checked where the function is defined, in every module, and for a generic one
in each instantiation, with its type arguments: `self.v.x` in a `-> inout int` method of
`Wrap<T>` is part of `self` in `Wrap<P>` for a struct `P`, and in a separate object in
`Wrap<D>` for a class `D`.

A `-> view T` result needs no such rule: its caller keeps what it names, an object or a
string with a reference of its own and a value as a copy (with a reference of its own to
each object in it), so replacing the field it was read from frees nothing the result uses.

---

## Expression Bodies

A function whose body only returns an expression can be written with `=>` and the
expression, ending with `;`: `fn double(x: int) -> int => x * 2;` is
`fn double(x: int) -> int { return x * 2; }`. Leave the result type out and it is the
expression's: `fn double(x: int) => x * 2;` returns an `int`, `fn greet(name: view Str) =>
"hi " + name;` a `Str`.

```pkn
import ::io;

fn area(w: int, h: int) => w * h;
fn describe(w: int, h: int) => "area " + Str(area(w, h)) + ", half " + Str(half(w * h));
fn half(n: int) => n / 2;                  // used above, declared after
fn twice<T>(x: T) => x + x;                // inferred for each type it is used at

struct Size { w: int; h: int; }
impl Size {
  static fn square(n: int) => Size(n, n);
  view fn area() -> int => self.w * self.h;  // a method writes its result type
}

fn main() -> int {
  io::println(describe(3, 4));
  io::println(Str(twice(21)) + " " + twice("ab") + " " + Str(Size::square(5).area()));
  return 0;
}
```

Output:

```
area 12, half 6
42 abab 25
```

- An inferred result type comes from the expression alone. A function may be used before
  its declaration, and one that uses another inferred function infers that one first.
- A generic function's result type is inferred for each instantiation: `twice(21)` is an
  `int`, `twice("ab")` a `Str`.
- A function whose expression depends on its own result type cannot infer it: `fn fact(n:
  int) => if n <= 1 then 1 else n * fact(n - 1);` is an error (`the result type of 'fact' is
  inferred from its expression, which uses 'fact' itself: write the result type, ...`).
  Write it, `fn fact(n: int) -> int => ...`, and the recursion is fine. The same goes for
  two functions that use each other.
- The expression must have a value: `fn say() => io::println("x");` is an error (`the result
  of 'say' is the value of its expression, which has none`); write a body in braces.
- A method takes the `=>` form with its result type written (`view fn area() -> int =>
  self.w * self.h;`): its signature is part of its class before any body is checked, so
  `fn area() => ...` in a class or an `impl` block is an error (`the result type of method
  'area' is not inferred: write it`). A `static fn` is a function, and infers.
- Everything else is a body's: the expression is the one `return`, checked as one, and a
  public function's inferred result must be a type an importer can name, as a written one.
- Lambdas take the same forms ([Expression-bodied lambdas](14-function-types.md#expression-bodied-lambdas)).

## Untyped Parameters

A free function may leave out a parameter's type. The parameter then has a type parameter of
its own, so `fn area(w, h) => w * h;` is the generic function `fn area<W, H>(w: W, h: H) => w *
h;`, and each call instantiates it for its arguments' types:

```pkn
import ::io;

fn area(w, h) => w * h;
fn describe(label: Str, value) => label + " " + Str(value);
fn larger(a, b) => if a > b then a else b;

fn main() -> int {
  io::println(describe("area", area(3, 4)) + ", " + describe("float", area(1.5, 2.5)));
  io::println(Str(larger(3, 9)) + " " + Str(larger(2.5, 0.5)));
  return 0;
}
```

Output:

```
area 12, float 3.75
9 2.5
```

- Each untyped parameter has its own type: `area(2, 1.5)` multiplies an `int` and a `float`.
  The type parameters an untyped parameter adds have names no program can write
  (`typeof(w)`), so they never clash with a type.
- The function is a [generic function](13-generics.md#generic-functions), and everything
  about one applies. Its body is checked for each instantiation, and an error says which one
  needed it: `area("ab", 2)` reports `operator '*' is not defined for types 'Str' and
  'int'` with `in instantiation of 'area<Str, int>'`. It cannot be a function value yet,
  and it can be used from another module, which instantiates it for its own calls.
- Typed parameters and written type parameters mix with untyped ones. The added type
  parameters come after the written ones, so explicit type arguments give all of them:
  `fn larger<T>(a: T, b: T, tag) -> T` is called `larger<float, Str>(1.5, 2.5, "x")`.
- To return a value of an untyped parameter's type, use an expression body: its result type
  is [inferred](#expression-bodies) for each instantiation. A block body cannot name that
  type, so `fn larger(a, b) { return a; }` is a `void` function, and its `return` is an
  error. Name the type parameter instead: `fn larger<T>(a: T, b: T) -> T { ... }`.
- An untyped parameter is passed by value. To give a parameter a mode, write its type
  (`x: view T`, with `T` a written type parameter).
- Only a free function's parameters may leave out their types. A lambda's, a method's, a
  `static fn`'s and a trait method's need theirs (`parameter 'x' needs a type ('x: int'):
  only a free function's parameters can leave it out`), as do a native function's
  (`parameter 'x' needs a type ('x: int'): a native function's parameters have types`)
  and `main`'s.

---

## Examples

```pkn
import ::io;

fn writeLine(s: Str) {
  io::println(s);
}

fn greet(name: Str) -> Str {
  return "Hello, " + name + "!";
}

fn clamp(v: int, lo: int, hi: int) -> int {
  return if v < lo then lo else if v > hi then hi else v;
}

fn main() -> int {
  writeLine(greet("world"));
  io::println(Str<int>(clamp(150, 0, 100)));  // 100
  return 0;
}
```

Output:

```
Hello, world!
100
```

---

## Recursive Functions

A function may call itself, directly or through another function (declaration order does
not matter, so two functions can call each other):

```pkn
import ::io;

fn fib(n: int) -> int {
  if n < 2 { return n; }
  return fib(n - 1) + fib(n - 2);
}

fn main() -> int {
  io::println("fib(20) = " + Str(fib(20)));
  return 0;
}
```

Output:

```
fib(20) = 6765
```

---

## Native Functions

A `native fn` declares a function whose body is a C function, written against
the Paykan runtime (`Runtime.h`).  It is how a library implements what Paykan
cannot express itself, such as the standard library's file I/O (#198,
`stdlib/io.pkn`):

```pkn
// from: stdlib/io.pkn
native fn _write(file: view Obj, s: view Str) -> int = "Paykan_io_write";
```

The string is the C symbol.  The rest of the program calls the function like
any other.

A C symbol names one C function, with one C signature: the C types the
table below gives the native function's parameters and result.  Native
functions may bind a symbol again only with that signature (`int` and `u32`
both cross as an `int64_t`, so they give the same one).  Two in one module
that do not agree are an error (`native function 'g' binds C symbol 'llabs',
which 'f' already binds with a different signature`), and so are two in
different modules of a program, once it is put together, whether the other
module is built from source or loaded from its `.pkm` (`native functions of
modules 'main' and 'na' bind C symbol 'llabs' with different signatures`).
A native function bound to a C function that compiled code calls itself
must have that function's signature (`native function 'cat' binds C symbol
'PaykanString_concat', a function of the runtime, with a different
signature`): one of the runtime's (`PaykanString_concat`, `Paykan_malloc`,
...), one an executable's entry code calls (`Paykan_heap_reset`,
`Paykan_args_free`, ...) or one of the C library's (`fmod` for a float's
`%`, and `memcpy`, `memmove` and `getenv`, which the C of `--emit-c`
calls).  No native function has the signature of one that takes a C `int`
(`Paykan_heap_set_tracking`, `Paykan_args_new`, `memset`), so none can
bind it.

Only `int`, `float`, `bool`, `char`, a sized numeric type and an enum
without fields cross into C by value.  Any other parameter is a `view`,
which C borrows for the call, or an `inout`, which C changes in place: a
call never shares a reference with C, so it costs no reference counting
(`parameter 's' must be 'view' or 'inout' ('s: view Str'); only int, float,
bool, char, a sized numeric type and an enum without fields cross into C by
value`).  Only these
types cross, with the runtime's own convention (an enum whose variants have
fields, [a tagged union](07-enums.md#variants-with-fields), does not):

| Paykan | C | Notes |
|--------|---|-------|
| `int`, `float`, `bool`, `char` | `int64_t`, `double`, `bool`, `int8_t` | by value, or a `view` |
| an enum without fields | `int64_t` | its variant's ordinal (`0` for the first), by value: `native fn f(d: Dir) -> Dir` |
| a sized numeric type (`u8`, `i32`, `unsigned`, `f32`, …) | `int64_t` (an integer, in its canonical sign- or zero-extended form; an `unsigned` as its bits), `double` (a float) | as `int` and `float`: the 8-byte slot of [sized numeric types](02-basics.md#sized-numeric-types) |
| `inout int`, ... | `int64_t *`, `double *`, `bool *`, `int8_t *` | the caller's variable |
| `view Str`, `view Obj` | `PaykanObject *` | lent for the call only: C must not retain it or keep it past the call (see below) |
| `inout Str`, `inout Obj` | `PaykanShared **` | the caller's box: to replace it, release it and store an owned box (never `NULL`) |
| `Str`, `Obj` result | `PaykanShared *` | owned (+1); never `NULL` |
| `Str?`, `Obj?` result | `PaykanShared *` | owned (+1); `NULL` is `None` |

Nothing C is given outlives the call.  A `view` argument is lent for the call
only: C must not retain it, store it, or keep any pointer into it, and once the
call returns it must assume the value has been deallocated.  An `inout`
argument is the caller's storage for the call only.  So a native function keeps
no Paykan value between calls: what it hands back is its owned result.  The
compiler relies on this: a native call given no `inout` argument cannot free
anything its caller holds ([PIR §7](../pir.md)).

Any other type, a tuple or a struct for one, crosses only as a type parameter's
argument of a [generic native function](#generic-native-functions): `native fn
f(t: view (int, int))` is an error (`parameter 't' of type '(int, int)' cannot
cross into C`), and so is `native fn g(p: view Point)` for a struct `Point`.

The result can be a `view` of a `view` or `inout` parameter (`-> view T`);
it is returned owned, like a copy.  A native function cannot be a method,
cannot return `inout`, and cannot be `main`.

A call in the module that declares the function calls C directly, lending
the caller's own references (a temporary argument is released after the
call).  A call from another module goes through the function's Paykan body,
which only reads its parameters: it is lent what the caller keeps, and
releases after the call only what it was handed (a temporary, a variable
at its last use; see [Borrowed parameters](16-memory.md#borrowed-parameters)).
The module's interface says the function is native, so the caller knows
the call changes nothing but its `inout` arguments, and lends it a field
read that nothing else in the call may replace, as it would a variable.

### Generic native functions

A native function can be generic.  Each instance calls the same C function,
which is passed a `PaykanType` describing each type argument first.  What the
function types by a type parameter crosses by address, so a value of any type
and size crosses (a tuple as the address of its elements):

```pkn
native fn pick<T>(first: bool, a: view T, b: view T) -> T = "sample_pick";
native fn swap<T>(a: inout T, b: inout T) = "sample_swap";
native fn get<T>(b: view Obj, i: int) -> view T = "sample_get";
```

```c
void sample_pick(const PaykanType *t, void *out, bool first, const void *a,
                 const void *b);
void sample_swap(const PaykanType *t, void *a, void *b);
const void *sample_get(const PaykanType *t, PaykanObject *b, int64_t i);
```

| Paykan | C |
|--------|---|
| a type argument | `const PaykanType *`, ahead of the arguments |
| `x: view T` | `const void *`: the value's address, lent for the call only (C must not keep it or the references in it) |
| `x: inout T` | `void *`: the caller's storage, which C may change (`PaykanType_drop` the old value, then store an owned one) |
| `-> T` | `void *out`, after the `PaykanType`s: storage C fills, owning the references it writes |
| `-> view T` | `const void *` returned: the address of a value in a `view` argument, which the caller copies |
| anything else | as in a non-generic native function |

A `PaykanType` holds a value's `size` and `align`, its `kind`
(`PaykanTypeKind`: int, float, bool, char, object, optional or tuple), and where it
holds references: `nrefs` words follow it, each a `PaykanShared *`'s byte
offset `<< 1`, `| 1` if it may be `NULL` (`PaykanType_ref`).  After them come
`nunset` words, the byte offsets of the 8-byte words that are all 0 only in
storage never set (`PaykanType_unset`; `PaykanType_isUnset` tests them): a
reference that may not be `NULL`, a function value's code word, and a tagged
union's tag with its first variant's words.  A type whose zero bytes are a
value, an `int` or an optional, has none.  An `int` or a
`float` is 8 bytes, a `bool` or a `char` 1, an `int?` or a `float?` 16 and a
`bool?` or a `char?` 2 (a `bool` that is true when it is present, then its
payload at the payload's alignment), and a reference an 8-byte
`PaykanShared *` box (`NULL` for an optional's `None`).  A tuple is its
elements in place, laid out as a C struct of them (kind `PAYKAN_KIND_TUPLE`,
each reference it holds, nested tuples' included, at its offset).
`PaykanType_copy` copies a value, retaining its references, and
`PaykanType_drop` releases them.  A parameter typed by a type parameter is a
`view` or an `inout` (`x: T` is rejected whatever `T` will be).  See
`samples/imports/15_native_generic`.

`paykan` does not build native code: you compile it however you like (any
compiler, flags or language that produces C-ABI symbols) and pass the objects
or archives with `--object`, a comma-separated list:

```sh
cc -std=c11 -fPIC -I<paykan include dir> -c text.c -o text.o
paykan --object=text.o,more.o main.pkn          # run (the JIT loads the objects)
paykan build --object=text.o main.pkn -o main   # an executable
```

`run` loads the objects into the JIT, so build them position-independent
(`-fPIC`).  There, the C code may call the C library and the runtime functions
compiled code uses (`PaykanString_new`, `PaykanShared_new`, `Paykan_retain`,
`Paykan_release`, ...); `build` links the objects into the executable, which
can call any runtime function.  `Runtime.h`'s `static inline`
`Paykan_retain_inline`, `Paykan_release_inline` and `PaykanShared_get_inline`
do what `Paykan_retain`, `Paykan_release` and `PaykanShared_get` do, and call
them only at the count's limit or to destroy the object.  A
`PaykanString`'s `data` and `len` may be read directly.  See
`samples/imports/14_native`.

A native function can also bind a C library function directly, with the C
types the table above gives its signature: `native fn llabs(x: int) -> int =
"llabs";` calls the C library's `llabs` on every path.  The C of `--emit-c`
declares each native function from that signature, not from the C
headers' declaration of the symbol (`long long llabs(long long)`, or a macro
for it), so it compiles wherever `run` and `build` work ([The C
backend](../c-backend.md)).

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Undeclared function | Calling a function that is not declared anywhere in the program |
| Function redefinition | Two functions with the same name |
| Builtin name reused | A function named like a builtin class or type (`fn Str()`, `fn float(...)`) — see [Builtin Names Are Reserved](02-basics.md#builtin-names-are-reserved) |
| Class / enum name reused | A function named like a class (its constructor) or an enum declared in the same program |
| Duplicate parameter | Two parameters of one function, method, trait requirement or lambda with the same name |
| Function name reused by a local | A variable, a parameter (a method's or a lambda's too), a `for` loop variable or a `match` binding named like a function of the module (`'func' is a function of this module and cannot be used as a variable`); see [Scoping Rules](02-basics.md#scoping-rules) |
| Untyped parameter outside a free function | A lambda's, method's, `static fn`'s, trait method's, native function's or `main`'s parameter without a type (see [Untyped Parameters](#untyped-parameters)) |
| Argument count mismatch | Wrong number of arguments |
| Argument type mismatch | Argument type incompatible with parameter type |
| Non-void return without value | `return;` in non-void function |
| Return type mismatch | Returned expression type ≠ declared return type |
| Missing return | A path through a non-void function that does not end in a `return` |
| Bad entry point | `main` that does not return `int`, or takes anything but `args: view Str[]` |
| Write to a `view` parameter | Assigning or destructuring into it |
| A `view` changed, passed on or kept | Writing a field or element of what it holds, calling a method on it that is not a `view fn`, passing it to a parameter that is not `view`, or storing or returning one that shares what it holds |
| Bad `inout` argument | Not a variable or field, not exactly the parameter's type, a `view` parameter or `let` local, an array element, a string's character, or the same place twice in one call |
| Override changes a mode | An override that drops, adds or changes a parameter's `view`/`inout`, or returns a borrow where the method it overrides returns a copy (or the other way) |
| A `view` result of something else | A `-> view T` function returns a new value, a copy parameter or a local, not part of `self` or of a `view` / `inout` parameter |
| An `inout` result of something else | A `-> inout T` function returns what an `inout` argument could not be, or what is not part of `self` or of an `inout` parameter, or another type; it returns storage in a separate object, reached through a field of `self` or past the object an `inout` parameter refers to (see [Where an `inout` result can point](#where-an-inout-result-can-point)); a `view fn` returns `inout`; the receiver of a method returning `inout` is not a variable or a field |
| A function value misused | A builtin, a constructor, a generic function or a function returning a borrow used as a value; a call through a variable that is not a function; wrong arguments to a function value; a lambda capturing a borrow or assigning a captured name (see [Function types](14-function-types.md)) |

# Generics

> **Stable** (issue #2): the syntax and semantics below are covered by the
> compatibility promise for the 0.2 series (see [Stability](index.md#stability)).

Generic classes and functions are written once for any type. A `Stack<T>` works as a
`Stack<int>`, a `Stack<Str>` or a `Stack<Task>`, each fully type-checked. This chapter
shows how to declare and use them, gives `tasks` an undo history built on a generic stack,
and then states the rules: type arguments, inference, how generic bodies are checked, and
generics across modules. A type parameter's bounds (`<T: Show>`) are covered in
[Traits](06-traits.md); variance is not supported.

## Generic classes

Type parameters go in angle brackets after the class name, and stand for a type
everywhere in the class:

```pkn
import ::io;

class Pair<A, B> {
  first: A;
  second: B;
  fn swap() -> Pair<B, A> { return Pair<B, A>(self.second, self.first); }
}

impl Pair<A, B> {
  fn __init__(a: A, b: B) { self.first = a; self.second = b; }
}

fn main() -> int {
  p = Pair<Str, int>("answer", 42);
  q = p.swap();                     // Pair<int, Str>
  io::println(q.second + " = " + Str(q.first));

  r = Pair(2.5, True);              // Pair<float, bool>, inferred
  io::println(Str(r.first) + " " + Str(r.second));
  return 0;
}
```

Output:

```
answer = 42
2.5 True
```

Each distinct list of type arguments makes a separate, ordinary class: `Pair<Str, int>` and
`Pair<int, Str>` are different types, and so are `Box<Dog>` and `Box<Animal>` even when
`Dog` extends `Animal` (there is no variance). When the type arguments can be worked out
from the constructor's arguments, as in `Pair(2.5, True)`, you can leave them out.

## Generic functions

A free function can have type parameters too. They are usually inferred from the arguments;
when they cannot be (for example when the parameter appears only in the return type), write
them explicitly:

```pkn
import ::io;

fn lastOf<T>(xs: view T[]) -> view T {
  return xs[xs.len() - 1];
}

fn indexOf<T>(xs: view T[], x: view T) -> int? {
  i = 0;
  while i < xs.len() {
    if xs[i] == x { return i; }
    i = i + 1;
  }
  return None;
}

fn three<T>(x: T) -> T[3] {
  return [x; 3];
}

fn main() -> int {
  io::println(Str(lastOf([3, 1, 4])));
  io::println(lastOf(["x", "y"]));
  io::println(indexOf(["a", "b", "c"], "c"));
  io::println(indexOf([1, 2], 5));
  words = three<Str>("ab");          // a Str[3]
  io::println(Str(words.len()) + " " + lastOf(words));
  io::println(Str(three(7)[1] + 1 == 8));
  return 0;
}
```

Output:

```
4
y
2
None
3 ab
True
```

A slice parameter (`view T[]`, see [Slices](09-arrays.md#slices)) takes `T` from the
array's elements, so `lastOf` works on an array of any type and length. `lastOf` returns a
`view` of the caller's element, and `indexOf` an `int?` that is `None` when the value is
not there. [Inference](#inference) below gives the exact rules.

## How generic code is checked

A type parameter can name the traits its type argument must implement, as a **bound**:
`fn show<T: Show>(x: T)` accepts only types with a `toString()`, and `io::println` is
declared just so. A trait such as `Show` or `Hashable` is built in, and a program declares
its own with `trait` (see [Traits](06-traits.md)). A call with a type that lacks the trait
is an error at the call (`'Task' does not implement trait 'Named', which 'f' requires of
'T'`).

Bounds or not, the body of a generic class or function is type-checked again for each set
of type arguments it is used with. If an operation does not work for one of them, the error
points at the generic code and says which instantiation needed it:

```pkn
class Doubler<T> {
  v: T;
  fn twice() -> T { return self.v + self.v; }
}

impl Doubler<T> {
  fn __init__(v: T) { self.v = v; }
}

fn main() -> int {
  a = Doubler<int>(21);     // fine: int has +
  s = Doubler<Str>("ab");   // fine: Str has + too
  b = Doubler<bool>(True);  // bool does not
  return 0;
}
```

Error:

```
error: operator '+' is not defined for types 'bool' and 'bool'
note: in instantiation of 'Doubler<bool>' requested here
```

## Project: undo with a generic stack

`tasks` remembers the tasks it removes, so the last removal can be undone. The history is a
`Stack<Task>`, and since nothing about a stack depends on what it holds, `Stack<T>` is
generic. It keeps its items in a `T?[8]` with a count, so its empty slots are `None`.
`push` returns `False` when the stack is full, and `pop` returns `T?`, `None` when the stack
is empty:

```pkn
import ::io;

class Stack<T> {
  items: T?[8];
  count: int;

  fn push(x: T) -> bool {
    if self.count == self.items.len() { return False; }
    self.items[self.count] = x;
    self.count = self.count + 1;
    return True;
  }

  fn pop() -> T? {
    if self.count == 0 { return None; }
    self.count = self.count - 1;
    top = self.items[self.count];
    self.items[self.count] = None;
    return top;
  }

  view fn size() -> int { return self.count; }
}

impl Stack<T> {
  fn __init__() {
    self.items = [None; 8];
    self.count = 0;
  }
}

class Task {
  title: Str;
}

impl Task {
  fn __init__(title: Str) { self.title = title; }
}

class TaskList {
  tasks: Task[8];
  count: int;
  removed: Stack<Task>;

  fn add(t: Task) -> bool {
    if self.count == self.tasks.len() { return False; }
    self.tasks[self.count] = t;
    self.count = self.count + 1;
    return True;
  }

  // Removes task number `n`, moving the ones after it down.
  fn remove(n: int) {
    if n < 1 || n > self.count { return; }
    self.removed.push(self.tasks[n - 1]);
    i = n;
    while i < self.count {
      self.tasks[i - 1] = self.tasks[i];
      i = i + 1;
    }
    self.count = self.count - 1;
  }

  fn undo() {
    match self.removed.pop() {
      t: Task {
        if self.add(t) { io::println("restored '" + t.title + "'"); }
        else { io::println("no room for '" + t.title + "'"); }
      }
      None    { io::println("nothing to undo"); }
    }
  }

  view fn titles() -> Str {
    out = "";
    i = 0;
    while i < self.count {
      out = out + (if i > 0 then ", " else "") + self.tasks[i].title;
      i = i + 1;
    }
    return out;
  }
}

impl TaskList {
  fn __init__() {
    self.tasks = [Task(""); 8];
    self.count = 0;
    self.removed = Stack<Task>();
  }
}

fn main() -> int {
  list = TaskList();
  list.add(Task("buy milk"));
  list.add(Task("fix the leak"));
  list.add(Task("call mum"));
  list.remove(1);
  list.remove(1);
  io::println(list.titles());
  list.undo();
  io::println(list.titles());
  list.undo();
  list.undo();

  numbers = Stack<int>();       // the same Stack, for ints
  numbers.push(1);
  numbers.push(2);
  size = numbers.size();
  match numbers.pop() {
    top: int { io::println(Str(size) + " numbers, top " + Str(top)); }
    None     { }
  }
  return 0;
}
```

Output:

```
call mum
restored 'fix the leak'
call mum, fix the leak
restored 'buy milk'
nothing to undo
2 numbers, top 2
```

In [Modules and imports](15-modules.md), `tasks` is split into several files.

## Declaring a Generic Class

```pkn
class Box<T> {
  v: T;
  fn get() -> T      { return self.v; }
  fn set(v: T)       { self.v = v; }
}

impl Box<T> {
  fn __init__(v: T) { self.v = v; }
}

class Pair<K, V> {
  k: K;
  v: V;
  fn key() -> K   { return self.k; }
  fn value() -> V { return self.v; }
}

impl Pair<K, V> {
  fn __init__(k: K, v: V) { self.k = k; self.v = v; }
}
```

- Type parameter names go in `<…>` right after the class name. Any identifier
  works; single capital letters are the convention.
- A parameter may not be repeated (`class P<T, T>`) and may not reuse the name
  of a type in scope (`class Bar<Foo>` when `Foo` is a class, `fn f<Str>`).
- A generic class may extend a **concrete** class: `class Wrap<T> : Base { … }`.
  The superclass itself cannot be generic: `class Full : Half<int>` and
  `class Full<T> : Half<T>` are syntax errors at the `<`.
- A generic class may mention its own instantiation in signatures
  (`fn link(n: Node<T>)`).

### `impl` Blocks

A generic class's `impl` block names its type parameters after the class's name, and
each instantiation gets the block's methods with the type arguments put in. The names
are the block's own: they need not be the ones the class uses.

```pkn
impl Box<T> {
  view fn isSame(other: view Box<T>) -> bool { return self.v == other.v; }
}

trait Keyed<K> { fn key() -> K; }
impl Keyed<K> for Pair<K, V> {}   // met by Pair's own key()
```

- The block names as many type parameters as the class has (`impl Pair<K, V>`), and
  `impl Box` without them is an error.
- Its methods are checked when the class is instantiated, like the class body's: a
  method name already in the class is reported for `Box<int>`, the instantiation.
- `impl Keyed<K> for Pair<K, V>` makes every instantiation implement the trait:
  `Pair<Str, int>` implements `Keyed<Str>`, and meets a `T: Keyed<Str>` bound.
- The class's `__init__` goes in one (it cannot be in the class body, see
  [Classes](05-classes.md)): `Box(3)` infers `T` from it.
- A generic class's `impl` block can have `static fn`s; one is called on an
  instantiation, `Box<int>::of(3)` (or `m::Box<int>::of(3)` through an import), and is that
  instantiation's function.

A struct can be generic in the same way (`struct Pair<A, B> { a: A; b: B; }`, with
`impl Pair<A, B>` blocks): see [Generic structs](11-structs.md#generic-structs). So can an
enum, whose variants' fields name its type parameters (`enum Maybe<T> { Just(value: T),
Nothing }`): see [Generic Enums](07-enums.md#generic-enums).

## Declaring a Generic Function

```pkn
fn first<T>(xs: view T[]) -> view T { return xs[0]; }

fn unbox<T>(b: Box<T>) -> T { return b.get(); }

fn pick<A, B>(a: A, b: B) -> B { return b; }
```

Only free functions can be generic: a method, in a class body or an `impl`
block, and a `static fn` cannot declare their own type parameters (a generic
class's or struct's methods use its parameters). `fn map<U>(x: U) -> U` in a
class body is a syntax error, `a method cannot have type parameters of its own:
only a free function is generic (the methods of a generic class or struct use
its type parameters)`; a `static fn`'s says `a 'static fn' cannot ...`.

A free function's parameter written without its type has a type parameter of
its own, after the written ones: `fn pick(a, b) => b;` is generic in both
parameters, as `fn pick<A, B>(a: A, b: B) => b;` is (see [Untyped
Parameters](03-functions.md#untyped-parameters)).

## Using Type Arguments

Type arguments are accepted wherever a type is written:

```pkn
b: Box<int> = Box<int>(3);            // variable annotation and constructor
p: Pair<Str, int> = Pair<Str, int>("age", 42);
nested: Box<Box<int>> = Box<Box<int>>(b);
arr: Box<int>[2] = [Box<int>(1), Box<int>(2)];   // array of instantiations

class Holder {
  b: Box<int>;                         // field
  fn get() -> Box<int> { return self.b; }        // return type
}

impl Holder {
  fn __init__() { self.b = Box<int>(1); }
}

fn take(b: Box<Str>) -> int { … }      // parameter

match o {
  x: Box<int> { … }                    // match arm (with binding)
  Box<Str>    { … }
  _           { … }
}
```

`Box<int>` is a real class: it has fields, virtual methods, a constructor and
a destructor, and it takes part in `match`, `==`/`equals`, arrays and module
export exactly like a hand-written class.

## Inference

The type arguments of a generic **function** are inferred from the argument
types when they are not written:

```pkn
first([10, 20]);          // T = int
first(["a", "b"]);        // T = Str
unbox(Box<int>(4));       // T = int, found through Box<T>
first<Str>(words);        // explicit — always allowed
```

Inference looks through slices (`view T[]`, against an array's elements),
tuples (`(A, B)`, element by element), optionals (`T?` against a `U?` or a
plain `U`), function types (`(T) -> U`, parameter by parameter and the
result, see [Function types and lambdas](14-function-types.md)) and instantiations
(`Box<T>`, or a generic struct's `Pair<A, B>`). A bare `None` argument is typed `Obj` and
carries no information about `T`: `unwrapOr(None, Node(3))` infers `T = Node` from the
second argument, but `wrap(None)` alone cannot be inferred and needs
`wrap<Node>(None)`. It is exact: a parameter that appears twice must be
deduced as the same type, and no promotion or subtyping is applied. When inference is
impossible or ambiguous the call is an error with a hint to write the arguments explicitly:

```pkn
fn mk<T>() -> int { return 1; }
mk();                     // error: cannot infer type parameter 'T' of 'mk' from the call arguments
mk<int>();                // ok

fn same<T>(a: T, b: T) -> T { return a; }
same(1, 2.0);             // error: cannot infer type parameter 'T' of 'same': deduced as both 'int' and 'float'
same<float>(1, 2.0);      // ok (1 promotes to float)
```

A type parameter the arguments leave open is taken from the type the call's value flows
into, when the function's result type names it: a declared variable, a parameter, a
`return`, a field, an array element, the other side of `==` or `!=` or the other arm of a
conditional expression (in either order: `res::ok(1) == r` and `r == res::ok(1)`), as for a
generic enum's variant ([Generic Enums](07-enums.md#generic-enums)). The arguments come
first, and what they determine stays theirs:

```pkn
fn none<T>() -> T? { return None; }
s: Str? = none();                       // T = Str, from the variable
r: res::Result<int, err::Error> = res::ok(1);   // T = int from 1, E from the variable
x = none();               // error: cannot infer type parameter 'T' of 'none' from the call arguments
```

A generic **class** may be constructed without type arguments when they can be
inferred from its `__init__` parameters, or its fields' types when it has the
default `__init__` ([The default `__init__`](05-classes.md#the-default-__init__)):
`Box(3)` is `Box<int>(3)`, also for a `class Box<T> { v: T; }` with no `impl` block.

## Type Parameters Are Types

Inside a generic declaration `T` stands for the type argument and nothing
else. It can be used in annotations, `T[3]`, `view T[]`, `Box<T>`, tuple types
`(T, int)`, optional types `T?`, and `match` arms, but `y = T;` and `T()` are
errors ("type parameter 'T' cannot be used as a value"). Like any type's name,
it names no field, parameter or local of the declaration: `class D<T> { T: int;
v: T; }` is `'T' is a type name and cannot be used as a field name`, at the field
(once, whether or not `D` is instantiated), and `fn g<T>(T: int, y: T)` is `... as a
parameter name` (`T = 3;` in a body `... as a variable`), for each instantiation. An
`impl` block's type parameters are the names it gives them: in `impl Box<U> { … }`,
`U` is the type and `T` an ordinary name.

Tuples and optionals work inside templates and as type arguments:

```pkn
class Pair<A, B> {
  p: (A, B);
  fn swap() -> (B, A) { return (self.p.1, self.p.0); }
}

impl Pair<A, B> {
  fn __init__(a: A, b: B) { self.p = (a, b); }
}
fn orElse<T>(x: T?, d: T) -> T {
  match x {
    v: T { return v; }
    None { return d; }
  }
}
b = Box<(int, Str)>((1, "a"));     // tuple type argument
m = ident<Node?>(n);               // optional type argument
```

A tuple type argument is a value like any tuple: `Box<(int, Str)>`'s field
holds the tuple's elements in place, a `T` parameter is a copy of one, and a
tuple meets a `T: Show` bound when its elements do (`(int, Str)` does, a tuple
holding a function value does not).

Because each instantiation is checked separately, a `T?` in a template is
an optional of whatever `T` is: `Slot<int>` below holds an `int?` (an
optional primitive, see [Optional Primitives](12-optionals.md#optional-primitives)),
`Slot<Color>` a `Color?` and `Slot<(int, Str)>` an `(int, Str)?`, and `orElse(x, 0)` with
`x: int?` works as written above. When `T` is itself optional, `T?` is `T`: a
template never makes a nested optional, so `Slot<int?>` holds an `int?`
(a written `int??` is still an error). A `match` arm naming `T` on a `T?` then matches
what `T` wraps, the present case, and binds it as a `T`, like the template's other values of
type `T`: `orElse<int?>(y, None)` binds `y`'s present value to `v`, an `int?`. So a template
that unwraps a `T?` works for an optional `T` as for any other, whether it returns the
binding, compares it with another `T`, or assigns it one through `v: inout T` (`None`
included, which empties the subject).

```pkn
import ::io;

enum Color { Red, Green }

class Slot<T> {
  v: T?;
}

impl Slot<T> {
  fn __init__() { self.v = None; }
  fn put(x: T) { self.v = x; }
}

fn main() -> int {
  a = Slot<int>();
  a.put(3);
  b = Slot<(int, Str)>();
  b.put((1, "one"));
  c = Slot<Color>();
  c.put(Color::Green);
  d = Slot<int?>();                // its v is an int?, not an int??
  io::println(a.v.toString() + " " + b.v.toString() + " " + d.v.toString());
  match c.v {
    k: Color { io::println(k == Color::Green); }
    None     { io::println("empty"); }
  }
  return 0;
}
```

Output:

```
3 (1, one) None
True
```

## How Errors Are Reported

A generic body is checked once per instantiation. An error is reported at the
template's source and says which instantiation was being checked, as the `Doubler<bool>`
example in [How generic code is checked](#how-generic-code-is-checked) shows: the error
points at `self.v + self.v`, and a note points at `Doubler<bool>(True)`.

A generic declaration that is never instantiated is only parsed, not checked.
Nested instantiation is bounded: a class that instantiates itself with an
ever-growing argument (`inner: Bad<Bad<T>>`) stops with "exceeds the maximum
instantiation depth (16)".

A type parameter used as a `match` arm (`T { … }`) is substituted first and then
follows the ordinary arm rules, so in an instantiation with `T = int` the arm is
rejected like any `int` arm.

## Modules

A module's generic classes and functions are exported with the rest of it,
and an importer instantiates them under the module's qualifier, in every
position a local template may be used: `gen::Box<int>(1)`, `gen::Box("s")`
(inferred), `b: gen::Box<Str>`, `gen::first(xs)`, and inside the importer's
own templates (`gen::Box<T>`).

```pkn
// lib/gen.pkn
import lib::util;
class Box<T> {
  v: T;
  view fn get() -> view T { return self.v; }
  view fn tag() -> Str { return util::bracket(Str(_serial())); }
}

impl Box<T> {
  fn __init__(v: T) { self.v = v; }
}
fn _serial() -> int { return 7; }
fn mk() -> Box<int> { return Box<int>(41); }

// main.pkn
import lib::gen;
b = gen::Box<Str>("s");   // instantiated here
c = gen::mk();            // the module's own Box<int>
d = gen::Box<int>(1);     // the same class as c's
```

- **Names in a body are the module's.** A template's body is resolved where
  it is declared: `_serial`, `Box` and `util::bracket` above are what
  `lib::gen` declares and imports, whatever the importer calls those names
  (its own `_serial`, its own `util`). So are the traits its bounds name: an
  importer's own trait of the same name never satisfies one. A template may
  use the module's [private](15-modules.md#private-to-the-module) names: the
  importer runs that code, it just cannot write the names.
- **Checked per instantiation, as the module would check it.** An error in
  the body is reported in the template's file, with the instantiation note
  at the request. A bound by the module's own trait is the exception: the
  importer cannot instantiate such a template yet, which is reported at the
  request ([Traits](06-traits.md#not-supported-yet)).
- **One class per instantiation per program.** Class names are global
  across imports, so `Box<int>` is one class whoever instantiates it: the
  template's module, the importer, or another importer. Values pass between
  them, and inference sees through them (a `fn peek<T>(b: Box<T>)` of
  `lib::gen` binds `T = int` from a `Box<int>` any of them made). An
  instantiation is compiled once per program.
- Declaring your own `class Box<T>` and instantiating `Box<int>` in a module
  that also imports `lib::gen`'s `Box<int>`, or instantiating both your
  `Box<Str>` and `gen::Box<Str>`, is an error: type names are global across
  the import graph. It is one error, at your module's own instantiation
  (`instantiation 'Box<Str>' of 'Box' conflicts with the instantiation
  'Box<Str>' of 'lib::gen::Box'`), also when the other is made inside
  another module's template, with notes at the other template's declaration
  and at where it is instantiated; for an instantiation an import brings in,
  at that import. This holds for a library's private templates too
  ([the standard library](../stdlib/index.md)).
- A generic class's or struct's `impl` blocks go with it, `impl Trait for`
  blocks too, and each instantiation checks their methods as the module
  would check its own, whichever module makes it: an importer's
  `gen::Box<float>` is an error when `Box<float>` is one in `lib::gen`, even
  for a method the importer never calls. `impl Hashable for Box<T>` makes
  every instantiation `Hashable`. A block of the module's own trait gives an
  importer's instantiation its methods, not the trait: an importer cannot
  name another module's traits yet
  ([#147](https://github.com/parsabee/PaykanLang/issues/147),
  [Traits](06-traits.md#not-supported-yet)).
- A module's `.pkm` holds its templates: an importer instantiates them from
  a prebuilt or cached module as from its source (the standard library's
  `::collections` is one). Each module compiles its own copy of the
  instantiations of other modules' templates it uses, so its `.pkm` is the
  same in every program; a program keeps one copy of each
  ([`docs/design/pkm.md`](../design/pkm.md) §4.8). Editing a template's body
  recompiles the modules that import its module, directly or not
  ([The compilation cache](15-modules.md#the-compilation-cache)).

## Syntax Notes

- `a < b` stays a comparison everywhere; `f<int>(x)` is a generic call because
  the `>` is followed by `(`, and `Maybe<int>::Nothing` a generic enum's variant
  because it is followed by `::` and a name. The one ambiguous shape is `g(a < b, c > (d))`,
  which reads as a generic call — parenthesise a comparison to get the other
  meaning.
- `>=` is a single token: write `x: Box<int> = …`, not `x: Box<int>=…`.

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Wrong number of type arguments | `Box<int, Str>` for a one-parameter class, `f<int, int>(…)` |
| Unknown generic | `Nope<int>` with no such class |
| Type arguments on a non-generic | `f<int>(1)` where `f` is ordinary; `Str<int>` in a type position (`t: Str<int>`) — as an expression, `Str<int>(n)` is a conversion constructor ([Conversions](04-strings-and-conversions.md#conversions)) |
| Missing type arguments | `b: Box = …` for a generic `Box` |
| Type parameter used as a value | `y = T;`, `T()` inside a generic body |
| Type parameter as a name | A field, a parameter (of a function, a method, a `static fn` or a lambda), a local or a `for` variable named like a type parameter of its declaration: `class D<T> { T: int; … }`, `fn g<T>(T: int, y: T)`, `T = 3;`: `'T' is a type name and cannot be used as a field name` (`... as a parameter name`, `... as a variable`) |
| Duplicate / shadowing type parameter | `class P<T, T>`, `class Bar<Foo>` when `Foo` is a type, `impl Box<Foo>` |
| `impl` type parameters | `impl Box` or `impl Box<T, U>` for a one-parameter `Box`, `impl A<T>` for a non-generic `A` |
| Generic superclass | `class Full : Half<int> { … }`, `class Full<T> : Half<T> { … }` (a syntax error, at the `<`; the class body is still parsed): `a superclass cannot be generic: a class, generic or not, extends a class that has no type parameters` |
| Generic method | A method or a `static fn` declares type parameters of its own, `fn map<U>(x: U) -> U` in a class body or an `impl` block (a syntax error): `a method cannot have type parameters of its own: only a free function is generic ...`, `a 'static fn' cannot have type parameters of its own: ...` |
| Name reuse | a class or function named like a generic declaration |
| Error inside an instantiation | any ordinary error in a body, reported with the instantiation note |
| Inference failure / ambiguity | see [Inference](#inference) |
| Instantiation depth exceeded | self-instantiation with a growing argument |
| Instantiation name clash | this module's `Box<Str>` and an imported template's (also one a library's private template makes, `::collections`' `_Node<int>`), or an imported class `Box<int>`: one error, at this module's own instantiation, with notes at the other template's declaration and instantiation, or at the import |

## Not Supported Yet

Variance, default type arguments, explicit specialisation, generic methods (a method's or a
`static fn`'s own type parameters) and generic superclasses. Each instantiation is compiled on its
own, also over class-type arguments; sharing one compiled copy among those would be an
optimisation a program cannot observe.

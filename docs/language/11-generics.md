# PaykanLang — Generics

> **Stable in v0.1** (issue #2): the syntax and semantics below are covered by the
> compatibility promise for the 0.1 series (see `01-language-basics.md`, "Stability").

Generic classes and functions are parameterised by types. A type parameter's
bounds (`<T: Show>`) are in `12-traits.md`; variance is not supported in v0.1.

---

## Quick Summary

- Declare type parameters after the name: `class Box<T> { … }`,
  `class Pair<K, V> { … }`, `fn first<T>(xs: view T[]) -> view T { … }`.
- Use a generic class by giving it type arguments: `Box<int>`, `Pair<Str, int>`,
  `Box<int>[2]`, `Box<Box<int>>`. Each distinct argument list is its own
  ordinary class named after it (`Box<int>` and `Box<Str>` are unrelated, and
  so are `Box<Dog>` and `Box<Animal>`: there is no variance).
- Construct with the type arguments — `Box<int>(3)` — or let the compiler
  infer them from the `__init__` arguments: `Box(3)`. Without an `__init__`, the
  default one takes the fields (see `04-classes.md`), so `class Box<T> { v: T; }`
  infers them too.
- Call a generic function with explicit arguments, `first<int>(xs)`, or let
  them be inferred from the argument types, `first(xs)`.
- Type parameters name types only; `T` cannot be read as a value or constructed.
- Bodies are checked per instantiation: `Adder<int>` may be fine while
  `Adder<bool>` reports an error that names the instantiation.
- A module's generic declarations are exported with it: an importer writes
  `mod::Box<int>` or `mod::first(xs)` (see *Modules*).

---

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
  The superclass itself cannot be generic in v0.1.
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
  `04-classes.md`): `Box(3)` infers `T` from it.
- A generic class's `impl` block can have `static fn`s; one is called on an
  instantiation, `Box<int>::of(3)` (or `m::Box<int>::of(3)` through an import), and is that
  instantiation's function.

A struct can be generic in the same way (`struct Pair<A, B> { a: A; b: B; }`, with
`impl Pair<A, B>` blocks): see [Generic structs](14-structs.md#generic-structs).

## Declaring a Generic Function

```pkn
fn first<T>(xs: view T[]) -> view T { return xs[0]; }

fn unbox<T>(b: Box<T>) -> T { return b.get(); }

fn pick<A, B>(a: A, b: B) -> B { return b; }
```

Only free functions can be generic; methods of a non-generic class cannot
declare their own type parameters (a generic class's methods use the class's
parameters).

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
result, see `15-function-types.md`) and instantiations (`Box<T>`, or a
generic struct's `Pair<A, B>`). A bare `None` argument is typed `Obj` and carries no information
about `T`: `unwrapOr(None, Node(3))` infers `T = Node` from the second
argument, but `wrap(None)` alone cannot be inferred and needs
`wrap<Node>(None)`. It is exact: a parameter that appears twice must be
deduced as the same type, and no promotion or subtyping is applied. When inference is impossible or ambiguous
the call is an error with a hint to write the arguments explicitly:

```pkn
fn mk<T>() -> int { return 1; }
mk();                     // error: cannot infer type parameter 'T' of 'mk'
mk<int>();                // ok

fn same<T>(a: T, b: T) -> T { return a; }
same(1, 2.0);             // error: cannot infer type parameter 'T' of 'same': deduced as both 'int' and 'float'
same<float>(1, 2.0);      // ok (1 promotes to float)
```

A generic **class** may be constructed without type arguments when they can be
inferred from its `__init__` parameters, or its fields' types when it has the
default `__init__`: `Box(3)` is `Box<int>(3)`.

## Type Parameters Are Types

Inside a generic declaration `T` stands for the type argument and nothing
else. It can be used in annotations, `T[3]`, `view T[]`, `Box<T>`, tuple types
`(T, int)`, optional types `T?`, and `match` arms, but `y = T;` and `T()` are
errors ("type parameter 'T' cannot be used as a value").

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
tuple meets a `T: Show` bound.

Because each instantiation is checked separately, a `T?` in a template is
an optional of whatever `T` is: `Slot<int>` below holds an `int?` (an
optional primitive, see `10-optionals.md`), `Slot<Color>` a `Color?` and
`Slot<(int, Str)>` an `(int, Str)?`, and `orElse(x, 0)` with `x: int?`
works as written above. When `T` is itself optional, `T?` is `T`: a
template never makes a nested optional, so `Slot<int?>` holds an `int?`
(a written `int??` is still an error).

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
template's source and says which instantiation was being checked:

```
class Adder<T> {
  v: T;
  fn twice() -> T { return self.v + self.v; }
}
impl Adder<T> {
  fn __init__(v: T) { self.v = v; }
}
a = Adder<int>(1);        // fine
b = Adder<bool>(True);    // error: operator '+' is not defined for types 'bool' and 'bool'
                          // note: in instantiation of 'Adder<bool>' requested here
```

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
  (its own `_serial`, its own `util`).
- **Checked per instantiation, as the module would check it.** An error in
  the body is reported in the template's file, with the instantiation note
  at the request.
- **One class per instantiation per program.** Class names are global
  across imports, so `Box<int>` is one class whoever instantiates it: the
  template's module, the importer, or another importer. Values pass between
  them, and inference sees through them (a `fn peek<T>(b: Box<T>)` of
  `lib::gen` binds `T = int` from a `Box<int>` any of them made). An
  instantiation is compiled once per program.
- Declaring your own `class Box<T>` and instantiating `Box<int>` in a module
  that also imports `lib::gen`'s `Box<int>`, or instantiating both your
  `Box<Str>` and `gen::Box<Str>`, is an error: type names are global across
  the import graph.
- A generic class's `impl` blocks go with it, except `impl Trait for` blocks:
  an importer cannot name another module's traits yet (`12-traits.md`).
- A `.pkm` does not hold templates yet (#160): a module that declares them,
  instantiates another module's, or uses such an instantiation, is built
  from its source every time (`docs/design/pkm.md` §4.8).

## Syntax Notes

- `a < b` stays a comparison everywhere; `f<int>(x)` is a generic call because
  the `>` is followed by `(`. The one ambiguous shape is `g(a < b, c > (d))`,
  which reads as a generic call — parenthesise a comparison to get the other
  meaning.
- `>=` is a single token: write `x: Box<int> = …`, not `x: Box<int>=…`.

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Wrong number of type arguments | `Box<int, Str>` for a one-parameter class, `f<int, int>(…)` |
| Unknown generic | `Nope<int>` with no such class |
| Type arguments on a non-generic | `f<int>(1)` where `f` is ordinary; `Str<int>` in a type position (`t: Str<int>`) — as an expression, `Str<int>(n)` is a conversion constructor (`01-language-basics.md`) |
| Missing type arguments | `b: Box = …` for a generic `Box` |
| Type parameter used as a value | `y = T;`, `T()` inside a generic body |
| Duplicate / shadowing type parameter | `class P<T, T>`, `class Bar<Foo>` when `Foo` is a type, `impl Box<Foo>` |
| `impl` type parameters | `impl Box` or `impl Box<T, U>` for a one-parameter `Box`, `impl A<T>` for a non-generic `A` |
| Name reuse | a class or function named like a generic declaration |
| Error inside an instantiation | any ordinary error in a body, reported with the instantiation note |
| Inference failure / ambiguity | see *Inference* |
| Instantiation depth exceeded | self-instantiation with a growing argument |
| Instantiation name clash | this module's `Box<Str>` and an imported template's, or an imported class `Box<int>` |

## Not Supported in v0.1

Variance, default type arguments, explicit specialisation, generic methods on non-generic classes,
generic superclasses, and templates in a prebuilt `.pkm` (v0.2.0, [#160](https://github.com/parsabee/PaykanLang/issues/160)).

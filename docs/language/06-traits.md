# Traits

A trait names the methods a type must have. `impl Trait for Type` declares that a class (or
a struct) implements it, and a type parameter's bounds (`<T: Show>`) say which traits its
type argument must implement. This chapter shows how to declare and use a trait, then gives
the rules, including the built-in traits `Show`, `Hashable` and `ValueType`.

## Declaring and using a trait

A trait lists method signatures, each ending with `;`. A class implements it with an
`impl ... for` block, which either supplies the methods or is empty when the class has them
already. A generic function whose type parameter is bounded by the trait can then call those
methods on any type that implements it:

```pkn
import ::io;

trait Named { view fn name() -> Str; }

class Dog {
  view fn name() -> Str { return "Rex"; }
}

class Planet {
  title: Str;
}

impl Named for Dog {}
impl Named for Planet {
  view fn name() -> Str { return "planet " + self.title; }
}

fn greet<T: Named>(x: view T) -> Str { return "hello, " + x.name(); }

fn main() -> int {
  io::println(greet(Dog()));
  io::println(greet(Planet("Mars")));
  io::println(Planet("Venus").name());
  return 0;
}
```

Output:

```
hello, Rex
hello, planet Mars
planet Venus
```

`Dog` already has a `name` method, so its `impl` block is empty; `Planet`'s supplies one,
which becomes an ordinary method of `Planet`. A type that does not implement the trait is
rejected where the generic function is called, and a trait cannot be used as a type:

```pkn
trait Named { view fn name() -> Str; }

fn greet<T: Named>(x: view T) -> Str { return "hello, " + x.name(); }

fn main() -> int {
  s = greet("world");
  n: Named = "x";
  return 0;
}
```

Error:

```
'Str' does not implement trait 'Named', which 'greet' requires of 'T'
variable 'n' names trait 'Named', which is not a type (use a type parameter bounded by it: '<T: Named>')
```

You use a built-in trait every time you print: `io::println` is declared
`io::println<T: Show>`, and every class, every number and every string implements `Show`.
[Generics](13-generics.md) has more on how bounded generic code is checked.

---

## Quick Summary

- `trait Name { view fn f() -> T; ... }` lists method signatures, each ending with `;`.
  A trait can be generic (`trait Into<T>`), and `Self` in its signatures is the type
  that implements it.
- `impl Name for A { ... }` declares that class `A` implements `Name`. The block's
  methods join `A` like the methods of any `impl` block (see
  [`impl` Blocks](05-classes.md#impl-blocks)), and a method that `A` already has meets a
  requirement too.
- A class implements every trait its superclasses implement. Every class implements the
  built-in trait `Show` (`view fn toString() -> Str`) through `Obj`, and so do `int`,
  `float`, `bool`, `char` and every sized numeric type (`n.toString()` is `Str(n)`), tuples (`(1, "a").toString()`
  is `(1, a)`), structs (`Point(x: 1, y: 2)`), enums (the variant's name, `North`, and a
  variant's fields after it, `Rect(w: 2, h: 3)`) and the optional of a `Show` type
  (`"None"` or the value's text). A tuple, struct or enum implements it only when every
  element, field and variant field does: a function value does not, so `struct S { f: (int)
  -> int; }` is not `Show` (`'S' does not implement trait 'Show', which 'io::println'
  requires of 'T': its field 'f' has type '(int) -> int', which does not implement it`, and
  `s.toString()` is an error), nor is `(1, f)` or an enum with such a variant. The
  standard library's print functions (`io::println<T: Show>`) take any `Show` value, and
  `Str(x)` gives the text `x.toString()` gives for every `Show` value `x` (see
  [To a string](04-strings-and-conversions.md#to-a-string): `Str(s)` of a `Str` is `s`
  itself, so it takes no `view` of one).
- The built-in trait `Hashable` (`view fn hash() -> int`) is implemented by `int`, `bool`,
  `char` and `Str`, every sized integer type, and by a class or a struct with an
  `impl Hashable for` block. The standard library's hash tables take `Hashable` keys (see
  [Hashable](#hashable)).
- The built-in trait `ValueType` has no methods. It is implemented by the types held in
  place and copied whole: `int`, `float`, `bool`, `char`, every sized numeric type, every
  enum, every tuple, every struct and every function type (see [ValueType](#valuetype)).
- `impl Name for S { ... }` declares that struct `S` implements `Name` too, with the same
  rules (see [Structs](11-structs.md)).
- `fn f<T: Show + Into<int>>(...)` and `class Box<T: Named>` bound their type
  parameters. Each instantiation checks its type arguments against the bounds.
- A trait is not a type: `x: Named` is an error.

---

## Declaring and Implementing

```pkn
trait Named { view fn name() -> Str; }
trait Into<T> { view fn into() -> T; }
trait Same { view fn same(other: view Self) -> bool; }

class A {
  n: int;
  view fn name() -> Str { return "A" + Str(self.n); }   // meets Named's requirement
}

impl A {
  fn __init__(n: int) { self.n = n; }
}

impl Named for A {}
impl Into<int> for A { view fn into() -> int { return self.n; } }
impl Same for A {
  view fn same(other: view Self) -> bool { return self.n == other.n; }
}
```

A requirement is met by the method of the same name. Its signature must be the trait's,
with `Self` and the trait's type parameters replaced. That covers the parameter and
result types, the `view` / `inout` modes, and whether it is a `view fn`. An `impl ... for`
block holds only the methods its trait requires, and no `static fn`.

`Self` also names the enclosing class in a class's own methods, those of its body and of
its `impl` blocks; not in a `static fn`, where it is an unknown type.

A generic class implements a trait with `impl Named for Box<T> { … }`: each
instantiation (`Box<int>`) implements it, checked when it is instantiated (see
[Generics](13-generics.md)).

## Bounds

```pkn
fn greet<T: Named>(x: view T) -> Str { return "hi " + x.name(); }
fn show<T: Show>(x: view T) -> Str { return x.toString(); }   // show(42), show(a), show("s")

class Holder<T: Named> {
  v: T;
}

impl Holder<T> {
  fn __init__(v: T) { self.v = v; }
}

greet(A(1));          // A implements Named
Holder<A>(A(2));
greet("s");           // error: 'Str' does not implement trait 'Named', which 'greet' requires of 'T'
```

Bounds are checked where a template is instantiated. The body is still checked once per
instantiation (see [Generics](13-generics.md)), so it may call what the bound requires. A
bound may name another type parameter: `<T, C: Into<T>>`.

## Hashable

`Hashable` is a built-in trait, like `Show`:

```pkn
trait Hashable { view fn hash() -> int; }
```

`int`, `bool`, `char`, `Str` and the sized integer types (`i8` … `unsigned`) implement it.
Their `hash()` is the same for equal values,
in every run and on every backend, and its bits are well mixed: a `bool` hashes as `0` or
`1` does, and a `char` as its code does. A `float` does not implement it, and neither does
an enum, a tuple or an optional. A class or a struct implements it with an `impl Hashable
for` block. Values that are `==` must have the same hash, so a class that overrides
`equals` hashes the same fields that `equals` compares, and a struct (whose `==` is field
by field) hashes its fields:

```pkn
import ::io;

class Point {
  x: int;
  y: int;
  view fn equals(other: Obj) -> bool {
    match other {
      p: Point { return p.x == self.x && p.y == self.y; }
      _ { return False; }
    }
  }
}

impl Hashable for Point {
  view fn hash() -> int { return (self.x * 31 + self.y).hash(); }
}

fn sameHash<T: Hashable>(a: view T, b: view T) -> bool { return a.hash() == b.hash(); }

fn main() -> int {
  io::println(Str(sameHash(Point(1, 2), Point(1, 2))) + " " + Str(sameHash("ab", "ab")) +
          " " + Str(sameHash(7, 8)));
  return 0;
}
```

Output:

```
True True False
```

The standard library's `HashMap<K, V>` and `HashSet<K>` require `K: Hashable` (see
[Collections](../stdlib/collections.md)). Declaring a trait named `Hashable` is an
error, as for `Show`.

A conformance crosses modules with its type. A struct or a class of module `keys` with an
`impl Hashable for` block is `Hashable` in every module that imports `keys`, and wherever
the type is reached through another module (a function of `mid` that returns a
`keys::Key`), whether `keys` is compiled from its source or loaded from a `.pkm` file
([Prebuilt modules](15-modules.md#prebuilt-modules)):

```pkn
// file: keys.pkn
struct Key { a: int; b: int; }

impl Hashable for Key {
  view fn hash() -> int { return (self.a * 31 + self.b).hash(); }
}
```

```pkn
import ::io;

import ::collections;
import keys;

fn main() -> int {
  counts = collections::HashMap<keys::Key, int>();
  counts.put(keys::Key(1, 2), 5);
  counts.put(keys::Key(1, 2), 6);
  io::println(Str(counts.len()) + " " + Str(counts.get(keys::Key(1, 2))));
  return 0;
}
```

Output:

```
1 6
```

The `impl` block stays in the type's module: an importer can neither write `impl Hashable
for keys::Key` nor repeat it for a subclass of an imported `Hashable` class, which is
`Hashable` through its superclass. A generic type's `impl Hashable for Box<T>` block goes
with its template, so every instantiation is `Hashable`, also one an importer makes
([Generics](13-generics.md#modules)).

## ValueType

`ValueType` is a built-in trait with no methods. It is implemented by the types whose
values are held in place and copied whole by an assignment: `int`, `float`, `bool`,
`char`, the sized numeric types (`u8`, `i32`, `unsigned`, `f32`, …), every enum, every
tuple, every struct and every function type (see [Memory](16-memory.md)).
A fixed-size array is one too, though it cannot be a type argument yet. A class, `Str` and
an optional are not, and `impl ValueType for` a class is an error: a class is shared by
reference. A struct implements it already, so `impl ValueType for` a struct is an error
too (`struct 'P' already implements 'ValueType'`).

```pkn
import ::io;

enum Suit { Hearts, Spades }

fn twice<T: ValueType>(x: T) -> (T, T) { return (x, x); }

fn main() -> int {
  p = twice((1, "one"));
  s = twice(Suit::Spades);
  io::println(Str(twice(2).1) + " " + p.1.1 + " " + Str(s.0 == Suit::Spades));
  return 0;
}
```

Output:

```
2 one True
```

The standard library's `memory::Box<T: ValueType>` holds one in a shared object (see
[Memory utilities](../stdlib/memory.md)).

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Trait name taken | A trait is named like a class, enum, function or another trait, or a built-in trait (`Show`, `Hashable`, `ValueType`) |
| Bad requirement | A requirement names an unknown type, repeats a method's name, has two parameters of one name or one named like a type (`fn m(u8: int)`) or `self`, or is `__init__` / `destroy` |
| Missing or mismatched method | The class has no method a requirement names, or its signature differs |
| Extra method | An `impl ... for` block has a method, or a `static fn`, its trait does not require |
| Implemented twice | The class, or a superclass (also one of another module), already implements the trait (every class implements `Show`) |
| Not a trait | An `impl ... for` or a bound names a class, a built-in type or an unknown name, or gives the wrong number of type arguments |
| Unmet bound | A type argument does not implement a trait its parameter is bounded by |
| Trait of another module | A template of another module is bounded, directly or through a template it uses, by that module's own trait ([#147](https://github.com/parsabee/PaykanLang/issues/147); see below) |
| Aggregate without a text | `io::println(s)`, `s.toString()` or `impl Show for S {}` for a tuple, struct, enum or optional holding a function value (the message names the part) |
| Trait as a type | A trait is used where a type is expected |
| Trait as a superclass | `class C : T` for a trait `T`: `superclass 'T' of class 'C' is a trait, not a class: a class implements a trait with 'impl T for C { ... }'` |

## Not Supported Yet

Traits of another module. An importer cannot name one, in a bound or in an `impl ... for`,
and a type's conformance to one is not in the module interface. A template of that module
bounded by one, directly or through a template it uses (an unbounded `fn wrap<T>` calling
`greet<T: Named>`, or a method of a generic class that does), cannot be instantiated from
the importer ([#147](https://github.com/parsabee/PaykanLang/issues/147)). The error is
reported where the importer asks for the instantiation, with a note at the use:

```pkn
// file: lib.pkn
trait Named { view fn name() -> Str; }

class Dog { view fn name() -> Str { return "Rex"; } }

impl Named for Dog {}

fn greet<T: Named>(x: view T) -> Str { return "hello, " + x.name(); }

fn wrap<T>(x: T) -> Str { return greet(Dog()); }
```

```pkn
import ::io;

import lib;

fn main() -> int {
  io::println(lib::wrap(1));
  return 0;
}
```

Error:

```
main.pkn:6:15: error: 'lib::wrap<int>' cannot be instantiated here: it uses 'lib::greet', which requires 'T' to implement trait 'Named' of module 'lib', and another module's traits are not supported yet (#147)
lib.pkn:10:34: note: 'lib::greet' is used here
```

Calling `lib::greet` itself is reported at the call: `'lib::greet' requires 'T' to
implement trait 'Named' of module 'lib', ...`. So is a use two templates deep or more (a
generic function of `lib` calling one that calls `greet`, or a template of a third module
calling `lib::wrap`), at the use in `lib`'s file, with the note of the nearest request
only: the compiler keeps no request further out for an instantiation it checks later, so
no line of the error may be in the importer's file. A trait the importer declares under
the same name is another trait, and never satisfies such a bound, of a function, a class,
a struct or an enum. An `impl Named for Box<T>` block of the module gives an importer's
`lib::Box<Str>` its methods, not the trait.

Also not supported yet: `impl ... for` a built-in type, an enum or a class of another
module, default methods, `static fn` requirements, and trait objects.

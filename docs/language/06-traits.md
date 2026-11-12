# Traits

A trait names the methods a type must have. `impl Trait for Type` declares that a class (or
a struct) implements it, and a type parameter's bounds (`<T: Showable>`) say which traits its
type argument must implement. This chapter shows how to declare and use a trait, then gives
the rules, including the built-in traits `Showable`, `Hashable`, `ValueType` and `Copyable`.

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
  let s = greet("world");
  let n: Named = "x";
  return 0;
}
```

Error:

```
'Str' does not implement trait 'Named', which 'greet' requires of 'T'
variable 'n' names trait 'Named', which is not a type (use a type parameter bounded by it: '<T: Named>')
```

You use a built-in trait every time you print: `io::println` is declared
`io::println<T: Showable>`, and every class, every number and every string implements `Showable`.
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
  built-in trait `Showable` (`view fn toString() -> Str`; named `Show` through 0.3, which is
  now an error: `'Show' is now 'Showable'`) through `Obj`, and so do `int`,
  `float`, `bool`, `char` and every sized numeric type (`n.toString()` is `Str(n)`), tuples (`(1, "a").toString()`
  is `(1, a)`), structs (`Point(x: 1, y: 2)`), enums (the variant's name, `North`, and a
  variant's fields after it, `Rect(w: 2, h: 3)`) and the optional of a `Showable` type
  (`"None"` or the value's text). A tuple, struct or enum implements it only when every
  element, field and variant field does: a function value does not, so `struct S { f: (int)
  -> int; }` is not `Showable` (`'S' does not implement trait 'Showable', which 'io::println'
  requires of 'T': its field 'f' has type '(int) -> int', which does not implement it`, and
  `s.toString()` is an error), nor is `(1, f)` or an enum with such a variant. The
  standard library's print functions (`io::println<T: Showable>`) take any `Showable` value, and
  `Str(x)` gives the text `x.toString()` gives for every `Showable` value `x` (see
  [To a string](04-strings-and-conversions.md#to-a-string): `Str(s)` of a `Str` is a new
  string with `s`'s text, and takes no `view` of one).
- The built-in trait `Hashable` (`view fn hash() -> int`) is implemented by `int`, `bool`,
  `char` and `Str`, every sized integer type, and by a class or a struct with an
  `impl Hashable for` block. The standard library's hash tables take `Hashable` keys (see
  [Hashable](#hashable)).
- The built-in trait `ValueType` has no methods. It is implemented by the types held in
  place rather than on the heap: `int`, `float`, `bool`, `char`, every sized numeric type,
  every enum, every tuple, every struct and every function type (see
  [ValueType](#valuetype)).
- The built-in trait `Copyable` has no methods and is never implemented: a type is
  Copyable by what it holds. A number, `bool`, `char`, a `shared` reference (`shared T` or
  `shared mut T`) and an `atomic`, `atomic shared` or `locked` handle are, and so are a
  struct, a tuple, a fixed-size array, an optional and an enum of such parts, and a
  function value (its copies share its closure); a unique reference, a `Str`, a container
  and a task are not. `T: Copyable`
  bounds a template (see [Copyable](#copyable)).
- The built-in trait `Cloneable` (`view fn clone() -> Self`) is the explicit copy of a
  value. Every Copyable type implements it, and so do `Str`, a struct, a tuple, a
  fixed-size array and an optional of Cloneable parts, and a class with an `impl Cloneable
  for` block, which may leave the method to the compiler (see [Cloneable](#cloneable)).
- `impl Name for S { ... }` declares that struct `S` implements `Name` too, with the same
  rules (see [Structs](11-structs.md)).
- `fn f<T: Showable + Into<int>>(...)` and `class Box<T: Named>` bound their type
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
fn show<T: Showable>(x: view T) -> Str { return x.toString(); }   // show(42), show(a), show("s")

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

`Hashable` is a built-in trait, like `Showable`:

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
error, as for `Showable`.

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
values are held in place rather than on the heap: `int`, `float`, `bool`, `char`, the
sized numeric types (`u8`, `i32`, `unsigned`, `f32`, …), every enum, every tuple, every
struct and every function type (see [Memory](16-memory.md)). It says where a value lives,
not that it copies: a tuple holding a `Str` is a `ValueType` and is move-only, so a
template that copies its value bounds it by [`Copyable`](#copyable) or, as `twice` below,
copies it with `clone()`.
A fixed-size array is one too, though it cannot be a type argument yet. A class, `Str` and
an optional are not, and `impl ValueType for` a class is an error: a class is shared by
reference. A struct implements it already, so `impl ValueType for` a struct is an error
too (`struct 'P' already implements 'ValueType'`).

```pkn
import ::io;

enum Suit { Hearts, Spades }

fn twice<T: ValueType + Cloneable>(x: T) -> (T, T) { return (x.clone(), move x); }

fn main() -> int {
  let p = twice((1, "one"));
  let s = twice(Suit::Spades);
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

## Copyable

`Copyable` is a built-in trait with no methods, of the types a program may copy freely
([Ownership transfers](16-memory.md#ownership-transfers)). It is structural: a type is
Copyable by what it holds, and no `impl` block gives it. A number, `bool`, `char`, a
`shared` reference (`shared T` or `shared mut T`) and an `atomic`, `atomic shared` or
`locked` handle are Copyable, and so are a struct, a tuple, a fixed-size array and an
optional whose every part is (a struct's `shared` field counts), and an enum whose every
variant's fields are (`enum Color { Red, Green }` is; `enum Shape { Named(s: Str), Empty }`
is not, `it holds move-only field 's' ('Str')`), and a function value is, its copies
sharing its closure. A unique reference, a `Str`, a container and a task are not: they are
move-only, and reach another owner by
[`move`](19-concurrency.md#move-an-explicit-transfer), or a copy of them is made with
[`clone()`](#cloneable). `T: Copyable` bounds a type parameter that copies its value:

```pkn
import ::io;

struct Point {
  x: int;
  y: int;
}

class Node {
  id: int;
}

struct Ref {
  n: shared mut Node;
}

fn twice<T: Copyable>(v: T) -> (T, T) { return (v, v); }

fn main() -> int {
  let p = twice(Point(1, 2));
  let n = twice(3);
  let s: shared mut Node = Node(7);
  let both = twice(s);                // twice<shared mut Node>: a shared reference
  let pair = twice(Ref(s));           // a struct of a shared reference is Copyable
  s.id = 8;                           // the one node, seen through every copy
  io::println(Str(p.0.x + p.1.y) + " " + Str(n.0 + n.1));
  io::println(Str(both.1.id) + " " + Str(pair.1.n.id));
  return 0;
}
```

Output:

```
3 6
8 8
```

`impl Copyable for X` is an error for any `X`, and a move-only type argument fails the
bound:

```pkn
class Node {
  id: int;
}

impl Copyable for Node {}

fn twice<T: Copyable>(v: T) -> (T, T) { return (v, v); }

fn main() -> int {
  let d = twice(Node(2));
  let e = twice("text");
  return 0;
}
```

Error:

```
'Copyable' is built in: a type is Copyable by what it holds, and cannot be implemented
'Node' does not implement trait 'Copyable', which 'twice' requires of 'T': a reference, a 'Str' or a task is move-only
'Str' does not implement trait 'Copyable', which 'twice' requires of 'T': a reference, a 'Str' or a task is move-only
```

A qualifier is part of a type argument ([Qualifiers in a
type](19-concurrency.md#qualifiers-in-a-type)), so `twice(s)` with `s: shared mut Node`
binds `T` to `shared mut Node`, which is Copyable: its copies are the same node, as the
copies of `Ref` are. A value that is not Copyable flows into an owning slot only by `move`,
by `clone()` or to a borrow ([Ownership transfers](16-memory.md#ownership-transfers)).
A template that copies its `T` without the bound is reported at the template, even if it
is only ever used with `int`: `'T' is not 'Copyable': declare 'T: Copyable' to copy it`
([The archetype check](13-generics.md#the-archetype-check)).

## Cloneable

`Cloneable` is a built-in trait, the explicit copy of a value that does not copy by itself:

```pkn
trait Cloneable { view fn clone() -> Self; }
```

`clone` is a `view fn`, so it is called through every form of a value: a plain or `mut`
one, a `shared` one, a frozen `atomic shared` one and a `view`. It returns a new value with
one owner, which a `mut` variable takes as it takes a temporary (`m: mut = n.clone();`).

- Every Copyable type is Cloneable: its clone is the copy. So a template bounded by
  `T: Cloneable` takes numbers and `shared` references as well as `Str`: a `T` whose
  argument is `shared Node` clones to the same node. A variable declared `shared Node` is
  cloned by its class, into a new unique object
  ([Qualifiers in a type](19-concurrency.md#qualifiers-in-a-type)).
- `Str` is Cloneable: `s.clone()` is a new string with the same text.
- A struct, a tuple, a fixed-size array and an optional are Cloneable when every part is,
  and the clone clones each part (a struct's `shared` or `weak` field is the same object
  in the clone). One that holds a part that is not is `'Pair' is not Cloneable: its field
  'a' ('Node') is not Cloneable`. Such a struct may write its own, `impl Cloneable for Pair
  { view fn clone() -> Pair { ... } }`; one whose every field is Cloneable has its clone
  built in, and the block is an error (`struct 'Q' already implements 'Cloneable': each of
  its fields is Cloneable, so its 'clone' is built in`).
- An enum is Cloneable when it is Copyable. One whose payloads are not is not Cloneable yet
  (`'Item' is not Cloneable: an enum whose payloads are not Copyable is not Cloneable yet`).
- A function value is Cloneable, being Copyable: its clone is a copy, sharing its closure.
  A task is never Cloneable.
- A class is Cloneable only when it says so, with an `impl Cloneable for` block, since an
  object may have an identity or hold a resource that a copy should not have. An empty
  block, `impl Cloneable for Note {}`, has the compiler write `clone`: a new object whose
  unique fields are clones of the original's, whose Copyable fields are copies, whose
  `shared` and `weak` fields refer to the same objects, and whose `atomic` fields are new
  cells holding the original's current values. A block may write `clone` instead,
  `impl Cloneable for C { view fn clone() -> C { ... } }`, with that signature.

```pkn
import ::io;

class Tag {
  label: Str;
}

impl Cloneable for Tag {}

class Note {
  text: Str;
  tag: Tag;
  owner: shared mut Tag;
}

impl Cloneable for Note {}

fn twoOf<T: Cloneable>(v: view T) -> (T, T) { return (v.clone(), v.clone()); }

fn main() -> int {
  let s = "text";
  let t = s.clone();                      // a new string: s is still s
  let pair = twoOf((1, "one"));           // a tuple of Cloneable parts
  let owner: shared mut Tag = Tag("me");
  let a: mut = Note("a", Tag("todo"), owner);
  let b = a.clone();                      // the derived clone
  a.tag.label = "done";                   // b's tag is its own
  owner.label = "us";                     // b's owner is a's
  io::println(s + " " + t + " " + pair.1.1);
  io::println(a.tag.label + " " + b.tag.label + " " + b.owner.label);
  return 0;
}
```

Output:

```
text text one
done todo us
```

A derived `clone` clones every unique field, so each field's type must be Cloneable, a class
field's by a block of its own. `clone` is virtual: called through a reference to a
superclass, it runs the clone of the object's own class. So a subclass of a Cloneable class
has a clone of its own, derived by an empty block of its own or written there, and returns
its own class (`view fn clone() -> Circle`); one without is an error, rather than a clone
that would make an object of the superclass. The error that rejects a copy of a Cloneable
value offers the clone (`cannot copy 's' (a unique 'Str'); write 'move s' to transfer it,
's.clone()' to copy it, or declare it 'shared Str'`).

```pkn
class Node {
  id: int;
}

class Holder {
  node: Node;
}

impl Cloneable for Holder {}

class Shape {
  name: Str;
}

impl Cloneable for Shape {}

class Circle : Shape {
  r: int;
}

fn main() -> int {
  let n = Node(1);
  let m = n.clone();
  return 0;
}
```

Error:

```
'impl Cloneable for Holder {}' cannot derive 'clone': its field 'node' ('Node') has no clone: a class is Cloneable only with an 'impl Cloneable for Node' block; write the 'clone' instead
class 'Circle' inherits 'Cloneable' from 'Shape', whose 'clone' makes a 'Shape': write 'impl Cloneable for Circle {}' to derive its own, or a 'clone' of its own
class 'Node' is not Cloneable: write 'impl Cloneable for Node {}' to derive its 'clone', or write one
```

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Trait name taken | A trait is named like a class, enum, function or another trait, or a built-in trait (`Showable`, `Hashable`, `ValueType`, `Copyable`, `Cloneable`: `'Copyable' is a builtin trait and cannot be redeclared`) |
| Bad requirement | A requirement names an unknown type, repeats a method's name, has two parameters of one name or one named like a type (`fn m(u8: int)`) or `self`, or is `__init__` / `destroy` |
| Missing or mismatched method | The class has no method a requirement names, or its signature differs |
| Extra method | An `impl ... for` block has a method, or a `static fn`, its trait does not require |
| Implemented twice | The class, or a superclass (also one of another module), already implements the trait (every class implements `Showable`) |
| Not a trait | An `impl ... for` or a bound names a class, a built-in type or an unknown name, or gives the wrong number of type arguments |
| `impl Copyable` | `impl Copyable for X`, for a class or a struct: `'Copyable' is built in: a type is Copyable by what it holds, and cannot be implemented` (see [Copyable](#copyable)) |
| Not Cloneable | `x.clone()` of a class with no `impl Cloneable for` block, of a struct, tuple or array with a part that is not Cloneable, or of an enum whose payloads are not Copyable; a type argument that fails `T: Cloneable` (see [Cloneable](#cloneable)) |
| `clone` not derived | `impl Cloneable for C {}` for a class with a unique field whose type is not Cloneable: `'impl Cloneable for C {}' cannot derive 'clone': its field ...` |
| Inherited `clone` | A subclass of a Cloneable class with no `impl Cloneable for` block of its own: `class 'D' inherits 'Cloneable' from 'C', whose 'clone' makes a 'C': ...`; a `clone` returning another class: `'view fn clone() -> C' of class 'D' does not match 'view fn clone() -> D' of trait 'Cloneable'` |
| Unmet bound | A type argument does not implement a trait its parameter is bounded by; the instantiation's body, the bounds of the templates it uses and the bodies of the instantiations only it uses are not checked, so it is the one error |
| Trait of another module | A template of another module is bounded, directly or through a template it uses, by that module's own trait ([#147](https://github.com/parsabee/PaykanLang/issues/147); see below) |
| Aggregate without a text | `io::println(s)`, `s.toString()` or `impl Showable for S {}` for a tuple, struct, enum or optional holding a function value (the message names the part) |
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
module, default methods and `static fn` requirements.

There are no trait objects, by design: a trait is a bound on a type parameter, never a
type of its own, and a class hierarchy is what dispatches at run time (see
[Inheritance and virtual methods](05-classes.md#inheritance-and-virtual-methods)).

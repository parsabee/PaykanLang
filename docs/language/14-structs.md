# PaykanLang — Structs

A struct is a value with named fields: `struct Point { x: int; y: int; }` declares a
type whose values hold an `x` and a `y` side by side, like a tuple `(int, int)` whose
elements have names and can be assigned.  A struct is held in place and copied by
assignment, as a tuple is; it has no superclass, no vtable and no identity, and its
methods come from `impl` blocks.

---

## Quick Summary

- Declaration: `struct Name { field: Type; ... }`, at the top level, with at least one
  field.  The body holds fields only; methods go in an `impl Name { ... }` block.
- Construction: `Point(1, 2)` takes the fields in declaration order, or the parameters
  of an `__init__` declared in an `impl` block, which must assign every field.
- Fields: `p.x` reads a field, `p.x = v` assigns it **in place**, in the variable, field
  or array element that holds the struct.  Chains work: `line.a.x = 0`, `shape.at.y`,
  `ps[1].x = 2`.
- A struct is a **value held in place**, like a tuple: `q = p` copies it, and passing
  or returning one copies it.  A copy of a struct holding an object names the same
  object.  A struct is not an `Obj`.
- Nominal: two structs with the same fields are different types, and a struct is not the
  tuple of its fields.
- Methods: `impl Point { fn shift(d: int) { self.x = self.x + d; } }`.  In a method
  `self` **is the receiver's storage**: `p.shift(1)` changes `p`.  A `view fn` reads
  `self` only; a `static fn` is called as `Point::origin()`.
- `==` / `!=` compare **field by field**; `toString()` (and `io::println`) renders
  `Point(x: 1, y: 2)`.  Every struct implements `Show` and `ValueType`;
  `impl Hashable for` and user traits work as for classes.
- Parameter modes work as for tuples: a plain parameter is a copy, `view` reads the
  caller's struct, `inout` is the caller's struct.  A field of a struct variable or field
  is an `inout` place (`bump(p.x)`).
- Generic: `struct Pair<A, B> { a: A; b: B; }` is a template, instantiated by its type
  arguments (`Pair<int, Str>`) or inferred from a constructor call (`Pair(1, "a")`),
  with `impl Pair<A, B>` blocks, as a generic class is (`11-generics.md`).
- A struct has an optional (`Point?`), held in place like the tuple `(bool, Point)` and
  unwrapped with `match p { q: Point { … } None { … } }` (`10-optionals.md`).
- Not yet: `match` on a struct itself, a fixed-size array field, a struct that contains
  itself.

---

## Declaration

```pkn
struct Point {
  x: int;
  y: int;
}

struct Item {
  name: Str;            // a reference field
  node: Node?;          // an optional reference
  pos: (int, int);      // a tuple
  at: Point;            // another struct, held in place
}

class Node { n: int; }
```

A struct is declared at the top level, before or after its uses.  Its name is a type
name, like a class's or an enum's, and must not be taken by one of them, by a function or
by a trait (`class 'A' conflicts with a struct of the same name`, `'B' is already declared
as a struct`).  The body is a list of fields, each `name: Type;`, with no duplicates and at
least one field (`struct 'X' has no fields: a struct has at least one`).  A field can have
any type but `void` and a fixed-size array (`field 'xs' in struct 'A' has type 'int[3]': a
struct field cannot be a fixed-size array yet`), and a struct cannot hold one of its own,
directly, through another struct or through a tuple, since it is held in place and would
have no size (`struct 'A' contains itself (through 'B'); a struct is held in place, so it
cannot hold one of its own (hold it in a class instead)`).  A class may hold a struct that
holds the class: a reference is not containment.

A struct has no superclass: `struct X : Base { ... }` is a syntax error (`a struct has no
superclass: 'struct X { ... }' (a struct is a value, not an object; use a class to
inherit)`), and so is a method in the body (`a struct body holds fields only ('name:
Type;'); a method goes in an 'impl' block: 'impl X { fn ... }'`).  A struct can be
generic (`struct Pair<A, B> { a: A; b: B; }`, see [Generic structs](#generic-structs)).

---

## Construction

A struct is constructed by calling its name.  Without an `__init__`, the constructor takes
the fields in declaration order (`function 'Point' expects 2 argument(s), got 1 (it has no
'__init__', so it takes x: int, y: int)`).  An `__init__` in an `impl` block replaces it:
its parameters are the constructor's, and it must assign every field on every path (`field
'y' of struct 'A' is not assigned on every path through '__init__'`), as a class's does.

```pkn
import ::io;

struct Point { x: int; y: int; }

struct Line {
  a: Point;
  b: Point;
  label: Str;
}

impl Line {
  fn __init__(a: Point, b: Point) {
    self.a = a;
    self.b = b;
    self.label = "line";
  }
}

fn main() -> int {
  p = Point(1, 2);                 // the fields in order
  l = Line(p, Point(3, 4));        // through __init__
  io::println(p);
  io::println(l);
  return 0;
}
```

Output:

```
Point(x: 1, y: 2)
Line(a: Point(x: 1, y: 2), b: Point(x: 3, y: 4), label: line)
```

`__init__` cannot call `__super__` (`'__super__' called in struct 'A', which has no
superclass (a struct does not inherit)`), cannot be a `view fn`, and cannot be called by
name.

---

## Fields

`p.x` reads a field and `p.x = v` assigns it, in the storage that holds the struct: the
variable, the field of an object or of another struct, or the array element.  A chain of
fields, tuple elements and array elements is read and assigned through to the place:

```pkn
import ::io;

struct Point { x: int; y: int; }
struct Line { a: Point; b: Point; }
class Shape { at: Point; }

fn main() -> int {
  p = Point(1, 2);
  p.x = 5;                         // in place
  l = Line(p, Point(3, 4));
  l.a.y = 0;                       // a field of a field
  s = Shape(Point(0, 0));
  s.at.x = 7;                      // a struct field of an object
  ps: Point[2] = [p, l.b];
  ps[1].x = 9;                     // an element's field
  t = (p, "p");
  io::println(Str(p.x + l.a.y + s.at.x + ps[1].x + t.0.x));
  return 0;
}
```

Output:

```
26
```

The field is assigned where the struct is **changeable**:

- not in a `let` variable, whose fields are part of its value (`'a' is declared with
  'let'; its fields are part of its value and cannot be assigned`), nor in a `let`
  variable's struct field or array element;
- not through a `view` (`'a' is a 'view' parameter; cannot assign to its field 'x'`),
  nor in a `view fn`'s `self` (`'self' is read-only in 'view fn bad'; cannot assign to
  its field 'x'`), nor while a `view` local of the variable is live (`'c' is viewed by
  'view' local 'v' until 'v' is last used; cannot assign to its field 'x'`);
- not in a tuple: a struct held in a tuple is part of the tuple's value, which is
  immutable (`cannot assign to field 'x' of a struct in element '.1' of type '(int, A)':
  tuples are immutable; build a new tuple instead`).  Read it, or build a new tuple.

A struct's fields are named, so `p.0` is an error (`tuple index '.0' applied to struct
type 'A': a struct's fields are named (x, y)`), a struct cannot be destructured (`cannot
destructure a value of type 'A'; only tuples can be destructured`), and `p.z` names no
field (`no field 'z' in struct 'A'`).

---

### Private fields and methods

A field or a method whose name starts with two underscores is private to the struct, exactly
as for a class ([Classes](04-classes.md#private-fields-and-methods)): `self.__n` is read and
assigned only in the struct's own methods, in its body or its `impl` blocks and trait impls;
`p.__n` elsewhere is `'__n' is private to 'P': it is reachable only from its own methods`.
The default constructor still takes every field, private ones included. A field or method
with one underscore (`_n`) is private to the module instead, as for a class: any code of the
module reaches it, an importer does not. A struct's initializer may be written `___init__`
as well, private to the module as a class's: the module constructs the struct, an importer
does not (`'m::P' cannot be constructed here: its initializer '___init__' is private to
module 'm'`).

## Structs as Values

A struct is a value, like a tuple or an `int`: it is held in place and assigning it,
passing it to a parameter without a mode or returning it copies it.  The copy holds the
same references: a copy of a struct holding an object names that same object, as a copy of
a tuple does.  Changing a field of the copy leaves the original as it was; changing the
shared object is seen through both.

```pkn
import ::io;

class Node { n: int; view fn toString() -> Str { return "Node" + Str(self.n); } }
struct Item { name: Str; node: Node; }

fn rename(it: Item) -> Item { it.name = it.name + "'"; return it; }   // its own copy

fn main() -> int {
  a = Item("a", Node(1));
  b = a;                           // a copy: the same Node
  b.name = "b";
  b.node.n = 2;                    // the shared object
  c = rename(a);
  io::println(a);
  io::println(b);
  io::println(c.name + " " + Str(a == b) + " " + Str(a == Item("a", a.node)));
  return 0;
}
```

Output:

```
Item(name: a, node: Node2)
Item(name: b, node: Node2)
a' False True
```

A struct is nominal: two struct types are the same only when they are the same
declaration, so `b: B = a` with `a` an `A` of the same fields is an error (`initializer
of type 'A' does not match declared type 'B'`), as are an `A` where a `(int, int)` is
expected and the reverse, an `A` argument to a `B` parameter, and `a == b` across the two
(`operands of '==' have mismatched types 'A' and 'B'`).  A struct is not an `Obj`
(`initializer of type 'A' does not match declared type 'Obj'`): where an `Obj` is needed,
keep the struct in a field of a class.

A struct goes anywhere a type does: a field of a class or of another struct, a parameter,
a result, an array element (`Point[3]`), a tuple element (`(Point, Str)`), a type argument
(`Box<Point>`, `List<Point>`) and a `view` or `inout` local.

### Parameter modes and places

Parameter modes work on a struct as on a tuple (`02-functions-and-calling.md`): a plain
parameter is the callee's own copy, a `view` parameter reads the caller's struct, and an
`inout` parameter is the caller's variable or field, which the callee changes in place or
replaces whole.  A `view` of a struct of value types is copied when it is assigned, like a
`view int`; one holding a reference is a `view` of what it holds, so it is read or passed
on to a `view` parameter but never stored or returned except as a `view`.

```pkn
import ::io;

struct Point { x: int; y: int; }

fn sum(p: view Point) -> int { return p.x + p.y; }
fn shift(p: inout Point, d: int) { p.x = p.x + d; p.y = p.y + d; }
fn reset(p: inout Point) { p = Point(0, 0); }
fn bump(n: inout int) { n = n + 1; }

fn main() -> int {
  p = Point(1, 2);
  shift(p, 10);
  bump(p.x);                       // a field is an inout place
  k: inout int = p.y;              // another name for p.y
  k = k + 100;
  io::println(Str(sum(p)) + " " + Str(p.x) + " " + Str(p.y));
  reset(p);
  io::println(p);
  return 0;
}
```

Output:

```
124 12 112
Point(x: 0, y: 0)
```

A field of a struct is an `inout` place when the struct is one: a variable, a field (of an
object or of another struct), an `inout` parameter or result.  A field of a temporary is
not (`a field of a struct that is not a variable or a field (a temporary, which dies with
the statement) cannot be passed to 'inout' parameter 'n'`), and neither is a field of an
array element yet (`an array element cannot be passed to 'inout' parameter 'n' yet`) or
of a struct in a tuple.

---

## Methods

An `impl` block gives a struct methods, an `__init__` and `static fn`s, with the rules of a
class's `impl` block (`04-classes.md`, "`impl` Blocks"): a struct may have several, in
its own module only (`an 'impl' block for a struct of another module is not supported
yet`), and a method name is declared once (`struct 'A' already has a method 'm'`).  A
struct has no vtable: every call is resolved at compile time.

In a method `self` is the **receiver's storage**, not a copy and not an object: `p.m()`
reads and writes `p` itself, as if `p` were passed to an `inout` parameter.  So a method
assigns `self.x` to change the receiver, and may pass `self` or a field of `self` on to an
`inout` parameter.  `self` cannot be assigned whole (`cannot assign to 'self': assign its
fields instead`).  A `view fn` reads `self` and cannot change it, call a method on it that
is not a `view fn`, or pass it to an `inout` parameter; a `view fn` may copy a `self` of
value types out (`q = self`, `return self`), while one holding a reference is a `view` of
what it holds, so it is returned, as are its reference fields, only as a `view` (`'self'
is read-only in 'view fn name', so part of it can only be returned as a 'view': write
'-> view Str'`).  A method that never changes `self` and is not a
`view fn` gets the warning a class method gets (`'get' never changes 'self': make it a
'view fn' so 'view' parameters can call it`).

```pkn
import ::io;

struct Counter {
  n: int;
  name: Str;
}

impl Counter {
  fn __init__(name: Str) { self.n = 0; self.name = name; }
  fn tick() { self.n = self.n + 1; }
  fn tickTwice() { self.tick(); self.tick(); }
  view fn get() -> int { return self.n; }
  view fn label() -> view Str { return self.name; }
  view fn same(other: view Counter) -> bool { return self.n == other.n; }
  fn slot() -> inout int { return self.n; }
  static fn named(s: Str) -> Counter { return Counter(s); }
}

class Box { c: Counter; }

fn bump(n: inout int) { n = n + 1; }

fn main() -> int {
  c = Counter::named("a");         // a static fn
  c.tick();
  c.tickTwice();                   // a method calling methods on self
  bump(c.slot());                  // an inout result: c.n
  b = Box(c);                      // a copy in the field
  b.c.tick();                      // the field's struct
  d = c;                           // a copy
  d.n = 9;
  v: view = c;
  io::println(Str(c.get()) + " " + Str(b.c.get()) + " " + Str(d.get()) + " " +
          Str(c.same(d)) + " " + v.label());
  Counter::named("tmp").tick();    // a temporary: changed, then dropped
  return 0;
}
```

Output:

```
4 5 9 False a
```

The receiver of a method call is any expression of the struct type: a variable, a field, an
array element, a call's result or a temporary (a temporary that a method changes is dropped
with the statement).  `__init__`, `static fn`s and the method names are the struct's;
`toString` is built in and cannot be declared (`struct 'A' already has a method
'toString'`), and `equals` is not a method of a struct (`no method 'equals' on type 'A'`:
compare with `==`).  A method's parameter `other: view Self` names the struct itself.

---

## Equality, `toString` and Traits

`==` and `!=` compare two structs of the same type field by field: value fields by value,
reference fields through their own `equals` (identity unless the class overrides it), two
absent optionals equal, nested structs and tuples element-wise; a `float` field follows
IEEE 754, so a struct holding a NaN is unequal to itself.  `toString()` renders the
struct's name and each field's name and value as its own `toString()` shows it:
`Point(x: 1, y: 2)`, `Item(name: a, node: Node2, pos: (7, 8))`.  `::io`'s `print`,
`println`, `printerr` and `printerrln` take a struct as they take a tuple (it is `Show`).

Every struct implements the built-in traits `Show` (its `toString`) and `ValueType` (it
is held in place), so it meets `T: Show` and `T: ValueType` bounds (`12-traits.md`), and
`impl Show for A {}` or `impl ValueType for A {}` is an error (`struct 'A' already
implements 'Show'`).  `Hashable` and user traits are implemented with `impl Trait for
Name` blocks, as for a class:

```pkn
import ::io;

struct Point { x: int; y: int; }

impl Hashable for Point {
  view fn hash() -> int { return (self.x * 31 + self.y).hash(); }
}

trait Named { view fn name() -> Str; }
impl Named for Point { view fn name() -> Str { return "Point"; } }

fn show<T: Show>(x: view T) -> Str { return x.toString(); }
fn twice<T: ValueType>(x: T) -> (T, T) { return (x, x); }
fn greet<T: Named>(x: view T) -> Str { return x.name(); }
fn sameHash<T: Hashable>(a: view T, b: view T) -> bool { return a.hash() == b.hash(); }

fn main() -> int {
  p = Point(1, 2);
  pair = twice(p);
  io::println(show(p) + " " + greet(p) + " " + Str(pair.1.y) + " " +
          Str(sameHash(p, Point(1, 2))) + " " + Str(p == Point(1, 2)) + " " +
          Str(p != pair.0));
  return 0;
}
```

Output:

```
Point(x: 1, y: 2) Point 2 True True False
```

Values that are `==` must hash the same, so `hash` reads the fields `==` compares.  A
`Hashable` struct is a key of the standard library's `HashMap<K, V>` and `HashSet<K>`
(`13-standard-library.md`), and any struct is an element of `List<T>` and the other
collections:

```pkn
import ::io;

import ::collections;

struct Point { x: int; y: int; }
impl Hashable for Point {
  view fn hash() -> int { return (self.x * 31 + self.y).hash(); }
}

fn main() -> int {
  names = collections::HashMap<Point, Str>();
  names.put(Point(0, 0), "origin");
  names.put(Point(1, 0), "east");
  ps = collections::List<Point>();
  ps.push(Point(1, 0));
  ps.push(Point(2, 2));
  io::println(names.get(Point(1, 0)) + " " + Str(names.containsKey(Point(2, 2))) + " " +
          Str(ps.contains(Point(2, 2))) + " " + Str(ps.get(0).x));
  return 0;
}
```

Output:

```
east False True 1
```

---

## Generic structs

A struct can have type parameters, like a class (`11-generics.md`): `struct Pair<A, B> {
a: A; b: B; }` is a template, and each use with distinct type arguments instantiates it
into a struct of its own, `Pair<int, Str>`, which has everything a written struct has.
An instantiation is written with its arguments (`p: Pair<int, Str>`,
`Pair<int, Str>(1, "a")`) or inferred from a constructor call: `Pair(1, "a")` unifies the
fields, or the parameters of the `__init__` of an `impl` block, with the arguments.  Its
`impl` blocks name the type parameters after the struct's name (`impl Pair<A, B>`; the
names are the block's own), and every instantiation gets their methods; `impl Trait for
Pair<A, B>` makes every instantiation implement the trait; a bound (`struct Keyed<K:
Hashable>`) is checked at each instantiation.

```pkn
import ::io;

struct Pair<A, B> {
  a: A;
  b: B;
}

impl Pair<A, B> {
  view fn first() -> view A { return self.a; }
  fn swapWith(other: inout Pair<A, B>) { t = self.a; self.a = other.a; other.a = t; }
  fn flipped() -> Pair<B, A> { return Pair<B, A>(self.b, self.a); }
}

struct Box<T> { v: T; }

impl Box<T> {
  fn __init__(v: T) { self.v = v; }
  view fn get() -> view T { return self.v; }
}

struct Keyed<K: Hashable> { key: K; n: int; }   // a bound, checked per instantiation

impl Hashable for Keyed<K> {
  view fn hash() -> int { return self.key.hash(); }
}

fn sumInts(p: view Pair<int, int>) -> int { return p.a + p.b; }
fn firstOf<A, B>(p: view Pair<A, B>) -> view A { return p.a; }   // A, B inferred
fn sameHash<T: Hashable>(a: view T, b: view T) -> bool { return a.hash() == b.hash(); }

fn main() -> int {
  p = Pair(1, "one");                    // Pair<int, Str>, inferred
  q: Pair<int, Str> = Pair<int, Str>(2, "two");
  p.swapWith(q);
  f = p.flipped();                       // Pair<Str, int>
  b = Box(Pair(3, 4));                   // Box<Pair<int, int>>, through __init__
  bs: Box<int>[2] = [Box(1), Box(2)];
  bs[1].v = 7;
  io::println(p);
  io::println(f);
  io::println(Str(sumInts(b.get())) + " " + firstOf(f) + " " + Str(bs[0].v + bs[1].v) + " " +
          Str(sameHash(Keyed("k", 1), Keyed("k", 2))) + " " + Str(p == q));
  return 0;
}
```

Output:

```
Pair<int, Str>(a: 2, b: one)
Pair<Str, int>(a: one, b: 2)
7 one 8 True False
```

The rules of generic classes hold: a type parameter is a type inside the template, the
template itself is not checked until it is instantiated, an error in an instantiation is
reported in the template's body with a note naming the instantiation (`in instantiation
of 'Box<Str>' requested here`), instantiations are cached by their canonical name (`Pair<int,
Str>` is one struct wherever it is used), each instantiation is a distinct nominal type
(`Pair<int, Str>` is not `Pair<Str, int>`), a template name is global with the other type
names (`'Pair' is already declared as a generic struct`), and the instantiation depth is
bounded.  `toString` renders the instantiation's name.  An instantiation of another
module's generic struct (`gen::Pair<int, Str>`, `gen::Pair(1, "a")`) is made in the
importer as for a generic class, and is one struct whoever makes it; a module that
declares or instantiates generic structs is built from its source each time, as a `.pkm`
holds no templates yet.

What a struct cannot hold, an instantiation cannot either: `struct Node<T> { v: T; next:
Node<T>; }` is reported when it is instantiated (`struct 'Node<int>' contains itself:
field 'next' holds a 'Node<int>'`), as is a fixed-size array type argument.  A `static fn`
in a generic struct's `impl` block is the instantiation's, called as
`Pair<int, Str>::make(1, "a")`, like a generic class's.

---

## Modules

A struct declared in a module is used from an importer as `mod::Name`: its constructor
`mod::Point(1, 2)`, its static functions `mod::Point::origin()`, and its fields and methods
on a value as usual.  A prebuilt module (`.pkm`, see `06-modules.md`) keeps a struct's
fields, methods and constructor.  An `impl` block for a struct of another module is not
supported yet.

```pkn
// file: geo.pkn
struct Point { x: int; y: int; }
impl Point {
  static fn origin() -> Point { return Point(0, 0); }
  view fn sum() -> int { return self.x + self.y; }
  fn shift(d: int) { self.x = self.x + d; }
}
fn make(n: int) -> Point { return Point(n, n); }
```

```pkn
import ::io;

import geo;

fn main() -> int {
  p = geo::Point(1, 2);
  p.shift(3);
  o: geo::Point = geo::Point::origin();
  io::println(p.toString() + " " + Str(p.sum()) + " " + Str(o == geo::make(0)));
  return 0;
}
```

Output:

```
Point(x: 4, y: 2) 6 True
```

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| No fields | `struct A { }` |
| Private member used outside the struct | `p.__n` or `p.__m()` outside the struct's methods: `'__n' is private to 'P': it is reachable only from its own methods` |
| Superclass | `struct A : B { ... }` |
| Method in the body | `struct A { x: int; fn f() {} }` |
| Duplicate field | `struct A { x: int; x: int; }` |
| Taken name | `struct A` next to a class, enum, function or trait `A`; `struct Str` |
| Contains itself | `struct A { b: B; } struct B { a: A; }`; `struct C { c: (int, C); }` |
| Array field | `struct A { xs: int[3]; }` |
| Generic struct misuse | `Pair<int>` for two parameters; `p: Pair` without arguments; `A<int>` for a plain struct; `impl Pair { }`; `Pair::make(1, "a")` without the type arguments; `Same(1, "a")` for `Same<T> { a: T; b: T; }` |
| Nominal mismatch | `b: B = a`; `t: (int, int) = a`; `a == b` across types |
| Not an `Obj` | `o: Obj = a` |
| Tuple index | `a.0` |
| Destructuring | `x, y = a` |
| Unknown field | `a.z` |
| `match` | `match a { ... }` |
| Wrong constructor arguments | `A(1)` for two fields; `A("s", 1)` |
| `__init__` leaves a field unassigned | `fn __init__(x: int) { self.x = x; }` with a field `y` |
| `__super__` | in a struct's `__init__` |
| Assigning `self` | `self = A(1)` in a method |
| Duplicate method | two `fn m()` across `impl` blocks; `fn toString()` |
| `let` field assignment | `let a = A(1); a.x = 2;` |
| `view` field assignment | `fn f(a: view A) { a.x = 1; }`; in a `view fn` |
| Field of a struct in a tuple | `t.1.x = 3` |
| Field of a temporary as `inout` | `bump(mk().x)` |
| Built-in trait | `impl Show for A {}`; `impl ValueType for A {}` |
| Another module's struct | `impl other::A { ... }` |
| Crossing into C | `native fn f(a: view A)` |

---

## Not Yet Supported

`match` on a struct (other than unwrapping a `Point?`), a fixed-size array field, an `impl`
block for a struct of another module, a `static fn` in a generic struct's `impl` block, a
field of an array element as an `inout` place, and a struct crossing into C other than as
a type argument of a generic native function (`02-functions-and-calling.md`, "Generic
native functions").  Like a tuple, a struct has no `equals` method and cannot be ordered
with `<`.

---

## Notes

- At run time a struct is laid out in place like a C struct of its fields, exactly as the
  tuple of its field types is (`09-tuples.md`, "Notes"): a struct of `int`s allocates
  nothing, a reference field is its box, retained once per copy and released when the
  variable goes out of scope, the holding object is destroyed or the array goes.  A
  method takes the receiver's address as its first argument, and a `view fn` the same;
  a constructor writes its result into the caller's storage.
- `toString` and `==` go through the struct's layout, constant data the compiler emits
  for each struct type that is printed or compared (`docs/pir.md`): the field count,
  the struct's name, then each field's name and the words a tuple element has.

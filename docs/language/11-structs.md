# Structs

A struct is a value with named fields: `struct Point { x: int; y: int; }` declares a
type whose values hold an `x` and a `y` side by side, like a tuple `(int, int)` whose
elements have names and can be assigned.  A struct is held in place and copied by
assignment, as a tuple is; it has no superclass, no vtable and no identity, and its
methods come from `impl` blocks.  This chapter introduces structs with the statistics of
the `tasks` project, then gives the precise rules.

## Structs: named fields

When a group of values has a shape worth naming, declare a **struct**. `tasks` keeps its
statistics in one:

```pkn
import ::io;

struct Stats {
  done: int;
  pending: int;
}

impl Stats {
  view fn total() -> int { return self.done + self.pending; }
  fn count(isDone: bool) {
    if isDone { self.done = self.done + 1; } else { self.pending = self.pending + 1; }
  }
}

fn main() -> int {
  s = Stats(0, 0);                  // the fields, in order
  s.count(True);
  s.count(False);
  s.count(True);
  s.pending = s.pending + 10;       // a field is assigned in place
  t = s;                            // a copy
  t.done = 0;
  io::println(s);
  io::println(Str(s.total()) + " " + Str(t.total()) + " " + Str(s == t));
  return 0;
}
```

Output:

```
Stats(done: 2, pending: 11)
13 11 False
```

`Stats(0, 0)` builds a struct from its fields in order, and the `impl` block holds its
methods, as a class's does. `t = s` copies the struct, so `t.done = 0` leaves `s` alone;
unlike a tuple's elements, its fields have names and can be assigned, and in a method `self`
is the variable the method was called on, so `s.count(True)` changes `s`. `==` compares two
structs field by field, and `io::println` prints the struct's name with each field.

Use a class when values must share one object that several names refer to, or when
subclasses and `match` are needed; use a struct for a small group of fields that is
copied around like a number. A struct can be generic, like a class
([Generic structs](#generic-structs)): `struct Pair<A, B> { a: A; b: B; }`, used as
`Pair<int, Str>` or inferred from `Pair(1, "one")`.

---

## Quick Summary

- Declaration: `struct Name { field: Type; ... }`, at the top level, with at least one
  field.  The body holds fields only; methods go in an `impl Name { ... }` block.
- Construction: `Point(1, 2)` takes the fields in declaration order, or the parameters
  of an `__init__` declared in an `impl` block, which must assign every field.
- Fields: `p.x` reads a field, `p.x = v` assigns it **in place**, in the variable, field
  or array element that holds the struct (the field of an object through a `mut` reference
  to it).  Chains work: `line.a.x = 0`, `shape.at.y`, `ps[1].x = 2`.
- A struct is a **value held in place**, like a tuple: `q = p` copies it, and passing
  or returning one copies it, when its fields are Copyable; one holding a `Str` or a
  unique object moves, and `clone()` copies it.  A copy of a struct holding a `shared`
  object names the same object.  A struct is not an `Obj`.
- Nominal: two structs with the same fields are different types, and a struct is not the
  tuple of its fields.
- Methods: `impl Point { fn shift(d: int) { self.x = self.x + d; } }`.  In a method
  `self` **is the receiver's storage**: `p.shift(1)` changes `p`.  A `view fn` reads
  `self` only; a `static fn` is called as `Point::origin()`.
- `fn __deinit__() { ... }` in an `impl` block runs once when a value of the struct is
  destroyed ([below](#running-code-when-a-value-is-destroyed-__deinit__)).  A struct that
  declares one is **move-only**, whatever its fields are.
- `==` / `!=` compare **field by field**; `toString()` (and `io::println`) renders
  `Point(x: 1, y: 2)`.  Every struct implements `ValueType`, and `Showable` when each field
  does (not one holding a function value);
  `impl Hashable for` and user traits work as for classes; a struct's `Hashable` crosses
  modules with it (a user trait's does not yet, #147).
- Parameter modes work as for tuples: a plain parameter is a copy, `view` reads the
  caller's struct, `inout` is the caller's struct.  A field of a struct variable or field
  is an `inout` place (`bump(p.x)`).
- Generic: `struct Pair<A, B> { a: A; b: B; }` is a template, instantiated by its type
  arguments (`Pair<int, Str>`) or inferred from a constructor call (`Pair(1, "a")`),
  with `impl Pair<A, B>` blocks, as a generic class is ([Generics](13-generics.md)).
- A struct of at most 64 bytes has an optional (`Point?`), held in place like the tuple
  `(bool, Point)` and unwrapped with `match p { q: Point { … } None { … } }` ([Optionals](12-optionals.md#optional-enums-tuples-and-structs)).
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
  let p = Point(1, 2);             // the fields in order
  let l = Line(p, Point(3, 4));    // through __init__
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

Until every field is assigned, `self` is only the place they are assigned in, as in a
class's `__init__` ([Every field before `self`](05-classes.md#every-field-before-self)):
`field 'y' is read before it is assigned`, `'self' is used before every field is
assigned: 'y' is not assigned yet`. `__init__` cannot call `__super__` (`'__super__'
called in struct 'A', which has no superclass (a struct does not inherit)`), cannot be a
`view fn`, has no result type, and cannot be called by name.

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
  let s: mut = Shape(Point(0, 0));
  s.at.x = 7;                      // a struct field of a mut object
  ps: Point[2] = [p, l.b];
  ps[1].x = 9;                     // an element's field
  let t = (p, "p");
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
- not in an object the reference to it only reads: a struct field of an object is assigned
  through a `mut` reference, as `s` is a `mut Shape` above (`'s' is immutable (a 'Shape';
  write 'mut Shape' to change it); cannot assign to its field 'x'`);
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

### Private fields and methods

A field or a method whose name starts with two underscores is private to the struct, exactly
as for a class ([Classes](05-classes.md#private-fields-and-methods)): `self.__n` is read and
assigned only in the struct's own methods, in its body or its `impl` blocks and trait impls;
`p.__n` elsewhere is `'__n' is private to 'P': it is reachable only from its own methods`.
The default constructor still takes every field, private ones included. A field or method
with one underscore (`_n`) is private to the module instead, as for a class: any code of the
module reaches it, an importer does not. A struct's initializer may be written `___init__`
as well, private to the module as a class's: the module constructs the struct, an importer
does not (`'m::P' cannot be constructed here: its initializer '___init__' is private to
module 'm'`).

---

## Structs as Values

A struct is a value, like a tuple or an `int`: it is held in place, and assigning it,
passing it to a parameter without a mode or returning it copies it when its fields are
Copyable.  A struct with a `Str` or a unique object among its fields is move-only, as a
tuple holding one is: it moves (`b = move a;`), and `clone()` copies it when its fields are
[Cloneable](06-traits.md#cloneable).  The copy holds the same `shared` references: a copy
of a struct holding a `shared` object names that same object.  Changing a field of the copy
leaves the original as it was; changing the shared object is seen through both.

```pkn
import ::io;

class Node { n: int; view fn toString() -> Str { return "Node" + Str(self.n); } }
struct Item { name: Str; node: shared mut Node; }

fn rename(it: Item) -> Item { it.name = it.name + "'"; return it; }   // its own copy

fn main() -> int {
  let a = Item("a", Node(1));
  b = a.clone();                   // a copy: its own name, the same Node
  b.name = "b";
  b.node.n = 2;                    // the shared object
  let c = rename(a.clone());
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

[Parameter modes](03-functions.md#parameter-modes-view-and-inout) work on a struct as on a
tuple ([Tuples as Values](10-tuples.md#tuples-as-values)): a plain
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
object or of another struct), an element of a fixed-size array (`ps[1].n`), an `inout`
parameter or result.  A field of a temporary is not (`a field of a struct that is not a
variable or a field (a temporary, which dies with the statement) cannot be passed to
'inout' parameter 'n'`), and neither is a field of a struct in a tuple.

---

## Methods

An `impl` block gives a struct methods, an `__init__` and `static fn`s, with the rules of a
class's `impl` block ([`impl` Blocks](05-classes.md#impl-blocks)): a struct may have several, in
its own module only (`an 'impl' block for a struct of another module is not supported
yet`), and a method name is declared once (`struct 'A' already has a method 'm'`).  A
struct has no vtable: every call is resolved at compile time.

In a method `self` is the **receiver's storage**, not a copy and not an object: `p.m()`
reads and writes `p` itself, as if `p` were passed to an `inout` parameter.  So a method
assigns `self.x` to change the receiver, and may pass `self` or a field of `self` on to an
`inout` parameter.  `self` cannot be assigned whole (`cannot assign to 'self': assign its
fields instead`), nor can a variable of the method take its name, as in a class's method.
A `view fn` reads `self` and cannot change it, call a method on it that is not a `view fn`,
or pass it to an `inout` parameter; a `view fn` may copy a `self` of value types out
(`q = self`, `return self`), while one holding a reference is a `view` of
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
  fn __init__(name: Str) { self.n = 0; self.name = move name; }
  fn tick() { self.n = self.n + 1; }
  fn tickTwice() { self.tick(); self.tick(); }
  view fn get() -> int { return self.n; }
  view fn label() -> view Str { return self.name; }
  view fn same(other: view Counter) -> bool { return self.n == other.n; }
  fn slot() -> inout int { return self.n; }
  static fn named(s: Str) -> Counter { return Counter(move s); }
}

class Box { c: Counter; }

fn bump(n: inout int) { n = n + 1; }

fn main() -> int {
  c = Counter::named("a");         // a static fn
  c.tick();
  c.tickTwice();                   // a method calling methods on self
  bump(c.slot());                  // an inout result: c.n
  let b: mut = Box(c.clone());     // a copy in the field
  b.c.tick();                      // the field's struct, through a mut Box
  d = c.clone();                   // a copy
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

The receiver of a method call is any expression of the struct type: a variable, a field (a
field of an object through a `mut` reference, for a method that changes it: `b.c.tick()`
above), an array element, a call's result or a temporary (a temporary that a method changes
is dropped with the statement).  `__init__`, `static fn`s and the method names are the
struct's; `toString` is built in and cannot be declared (`struct 'A' already has a method
'toString'`), and `equals` is not a method of a struct (`no method 'equals' on type 'A'`:
compare with `==`).  A method's parameter `other: view Self` names the struct itself.

A struct's methods are all `impl` methods, called directly, so any of them, and any
`static fn`, may have type parameters of its own: `fn add<U>(by: U, weigh: (U) -> int)`,
called as `tally.add("abc", f)` or `tally.add<int>(4, f)`, and
`Tally::of<Str>("xyz", f)` ([Generic Methods](13-generics.md#generic-methods)).  A generic
method that changes `self` needs a receiver it can change, like any method.

---

## Running code when a value is destroyed: `__deinit__`

A struct that has something to do when a value of it goes away, such as giving back what it
took, declares `fn __deinit__()` in an `impl` block. It takes nothing and returns nothing,
as a class's does ([Classes](05-classes.md#deinitializers)), and runs once for each value,
where the value's storage ends, while its fields are still alive.

```pkn
import ::io;
import ::memory as mem;

struct Lock { name: Str; }

impl Lock {
  fn __deinit__() { io::println("unlock " + self.name); }
}

class Session { held: Lock; user: Str; }

fn hold(l: Lock) { io::println("holding " + l.name); }

fn main() -> int {
  let a = Lock("a");
  {
    let b = Lock("b");
    io::println("inner");
  }                                      // b's scope ends
  hold(move a);                          // a moves into hold, and ends there
  c = Lock("c");
  c = Lock("d");                         // the value assigned over
  let s: mut = Session(move c, "ann");   // d moves into the object
  let old = mem::replace(s.held, Lock("e"));
  io::println("done");
  return 0;                              // old (d), then s and the lock it holds (e)
}
```

Output:

```
inner
unlock b
holding a
unlock a
unlock c
done
unlock d
unlock e
```

**One value.** A struct with a `__deinit__` is move-only, whatever its fields are: a copy
would be a second value to destroy. `q = p` is an error (`cannot copy 'p' (a 'Lock'); write
'move p' to transfer it`), and so is passing it by copy, or capturing it (`cannot capture 'p'
(a unique 'Lock'): a function captures by copy`). `move`, a `return` of
the variable and a `mem::replace` hand the one value on, and it is destroyed where it ends
up, not where it was. The struct is not `Copyable`, and it is `Cloneable` only with a
`clone` of its own, since only its own code can say what a second value is:

```pkn
struct Slot { id: int; }

impl Slot {
  fn __deinit__() { }
}

impl Cloneable for Slot {
  view fn clone() -> Self { return Slot(self.id + 1); }
}

fn main() -> int {
  let a = Slot(1);
  let b = a.clone();
  return b.id - a.id - 1;
}
```

A struct, a tuple, a fixed-size array, an optional or an enum that holds one is move-only
with it (`cannot copy 'p': 'Pair' holds unique field 'g' ('Guard')`), and each but the enum
is Cloneable when it is: its `clone` calls the struct's.

**Where it runs.** A value is destroyed where its storage ends:

- a variable's, when its scope ends: a scope's variables go in the reverse of the order they
  were declared in, objects and values alike;
- a parameter's, when the function returns (a `view` or `inout` parameter is the caller's
  value, and ends nothing);
- a temporary's, once the expression that made it has used it. A part read out of a
  temporary into a variable, a field or an element leaves it (`let l = pair().first;`): the
  part is the variable's, and the rest of the temporary goes;
- the value an assignment replaces, once the new one is in its place (`c = Lock("d")`
  above): a `__deinit__` that reaches the storage its value was in finds the new value
  there;
- a field's, an element's or a payload's, when the object or the value that holds it is
  destroyed, or the optional or enum holding it is assigned another value.

The struct's own `__deinit__` runs first, and its fields are released after it, in order: a
field that is a struct with a `__deinit__` runs its own then. An object's `__deinit__`s run
before those of the values in its fields. A value that was moved runs nothing where it was.
As with a class's, a panic aborts the program and runs none.

**In `__deinit__`**, `self` is the value's storage, as in any method: the code reads and
changes the fields, calls the struct's other methods, and lends `self` to a `view`
parameter and its fields to `inout` ones. It cannot hand the value on (`cannot copy 'self'
(a 'Lock'); a method borrows its receiver`), `await` (`'await' cannot be used in
'__deinit__': a value is destroyed where its storage ends, which waits for no task`), or be
called by name (`'__deinit__' cannot be called directly; it runs when the value is
destroyed`).

What a method of such a struct is called on stays the value it is: no method of it lends
`self` whole to an `inout` parameter or an `inout` local, or returns it `inout`, which
could put another value there (`'self' cannot be passed to 'inout' parameter 'slot': a
'Lock' has a '__deinit__', and what a method is called on stays the value it is (lend its
fields)`). In `__deinit__`
that would hand on the value being destroyed, and leave one that is never destroyed.

**While a `match` binding reads it.** A binding of a value in a variable or a field
(`match o { l: Lock { ... } None { } }`) looks at the value where it is. Nothing counts a
value, so what the binding reads is not replaced until its arm ends: `o = None` in that arm
is `'o' cannot be assigned here: 'match' binding 'l' reads a value of it that runs a
'__deinit__' where it is replaced, until its arm ends`, and so is writing the field,
lending it `inout`, or calling a method that changes the object holding it (`s.release()`
while a binding reads `s.held`). Another field of the holder is written as usual. `match
move o` binds the value itself, and an `inout` binding has it for the arm.

What a `match` only looks at is destroyed after it: a subject read from a temporary (an
object a call made, the value a `view` result is in) keeps the temporary until the match
ends, and a part matched out of a temporary of its own (`match make().o { ... }`) is the
match's until then. Two looks are refused, since the temporary would end first: a `view`
local of a part of a temporary (`'v' cannot be a 'view' of 'make(...).o': it is a part of
a temporary, which is destroyed with the statement, and a 'Lock?' runs a '__deinit__'
there; bind the value itself ('let v = ...')`), and a branch of a conditional subject that
reads a temporary it does not leave.

A `__deinit__` runs on the whole value, so a field does not leave a struct that has one by
`move` (`cannot move 'g.name' out of 'g': a 'Guard' has a '__deinit__', which runs on the
whole value; write 'mem::replace(g.name, v)'`).

A generic struct's `__deinit__` goes in a block of every instance, `impl Holder<T>`: one in
a conditional block is an error (`'__deinit__' cannot be in a conditional 'impl' block:
every 'Holder<T>' is destroyed the same way; declare it in 'impl Holder<T>'`), also when
the block's bounds only repeat the struct's own. An enum has none of its own; it runs those
of the structs in its payloads.

A struct with a `__deinit__` implements the built-in trait `Deinitializable`
([Traits](06-traits.md#deinitializable)).

**Not yet:**

- A `mem::Buffer<T>` runs no `__deinit__` for its elements, so a buffer, and a container
  built on one (`cl::List<Lock>`), cannot hold such a value (`a buffer cannot hold a 'Lock'
  yet: its elements run no '__deinit__' when the buffer lets them go; hold each in a
  'mem::Box<Lock>'`). A `mem::Box<Lock>` is an object holding the value, which a buffer
  holds as any object. `cl::LinkedList<Lock>` and `cl::HashMap<K, Lock>` hold their
  elements in objects and work, unless the struct is `Cloneable`: their `toList()`,
  `keys()` and `values()` then make a `cl::List` of it, and the same error is reported
  inside `::collections`.
- A value has no count. The compiler refuses to replace a value that a `match` binding or
  a `view` argument reads where it can see it (above; a call that reads and writes one
  variable's parts is refused as for any value). It cannot see code that reaches the object
  holding the value through another `shared mut` reference: such code can replace the value
  while it is read, and the reader then reads it after its `__deinit__` ran. What the value
  holds is still alive (the reader's own references keep it), but what the `__deinit__`
  gave up is gone.

---

## Equality, `toString` and Traits

`==` and `!=` compare two structs of the same type field by field: value fields by value,
reference fields through their own `equals` (identity unless the class overrides it), two
absent optionals equal, nested structs and tuples element-wise; a `float` field follows
IEEE 754, so a struct holding a NaN is unequal to itself.  `toString()` renders the
struct's name and each field's name and value as its own `toString()` shows it:
`Point(x: 1, y: 2)`, `Item(name: a, node: Node2, pos: (7, 8))`.  `::io`'s `print`,
`println`, `printerr` and `printerrln` take a struct as they take a tuple (it is `Showable`).

Every struct implements the built-in traits `Showable` (its `toString`) and `ValueType` (it
is held in place), so it meets `T: Showable` and `T: ValueType` bounds ([Traits](06-traits.md)), and
`impl Showable for A {}` or `impl ValueType for A {}` is an error (`struct 'A' already
implements 'Showable'`).  A struct with a field that does not implement `Showable`, a function
value or an aggregate holding one, does not either, and cannot, since its `toString` is
built in: `io::println(s)` and `s.toString()` are errors naming the field (`its field 'f'
has type '(int) -> int', which does not implement it`), and so is `impl Showable for S {}`.
A `ValueType` is not always Copyable (a struct holding a unique object moves), so a template
that copies one declares `T: Copyable` too, as `twice` below does
([The archetype check](13-generics.md#the-archetype-check)). `Hashable` and user traits are
implemented with `impl Trait for Name` blocks, as for a class:

```pkn
import ::io;

struct Point { x: int; y: int; }

impl Hashable for Point {
  view fn hash() -> int { return (self.x * 31 + self.y).hash(); }
}

trait Named { view fn name() -> Str; }
impl Named for Point { view fn name() -> Str { return "Point"; } }

fn show<T: Showable>(x: view T) -> Str { return x.toString(); }
fn twice<T: ValueType + Copyable>(x: T) -> (T, T) { return (x, x); }
fn greet<T: Named>(x: view T) -> Str { return x.name(); }
fn sameHash<T: Hashable>(a: view T, b: view T) -> bool { return a.hash() == b.hash(); }

fn main() -> int {
  let p = Point(1, 2);
  let pair = twice(p);
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
`Hashable` struct, of this module or an imported one, is a key of the standard library's
`HashMap<K, V>` and `HashSet<K>` ([Collections](../stdlib/collections.md)), and any struct
is an element of `List<T>` and the other collections:

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
          Str(ps.contains(Point(2, 2))) + " " + Str(ps.at(0).x));
  return 0;
}
```

Output:

```
east False True 1
```

---

## Generic structs

A struct can have type parameters, like a class ([Generics](13-generics.md)): `struct Pair<A, B> {
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
import ::memory as mem;

struct Pair<A, B> {
  a: A;
  b: B;
}

impl Pair<A, B> {
  view fn first() -> view A { return self.a; }
  fn swapWith(other: inout Pair<A, B>) { mem::swap(self.a, other.a); }
  view fn flipped() -> Pair<B, A> { return Pair<B, A>(self.b.clone(), self.a.clone()); }
}

struct Box<T> { v: T; }

impl Box<T> {
  fn __init__(v: T) { self.v = move v; }
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
  let f = p.flipped();                   // Pair<Str, int>
  let b = Box(Pair(3, 4));               // Box<Pair<int, int>>, through __init__
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
bounded, as is the name of an instantiation made inside an instantiation of its own
template with its type arguments grown ([Generics](13-generics.md#how-errors-are-reported)).  `toString` renders the instantiation's name.  An instantiation of another
module's generic struct (`gen::Pair<int, Str>`, `gen::Pair(1, "a")`) is made in the
importer as for a generic class, and is one struct whoever makes it; a module's `.pkm`
holds its generic structs for its importers to instantiate, as it does its generic classes
([Generics](13-generics.md#modules)).

What a struct cannot hold, an instantiation cannot either: `struct Node<T> { v: T; next:
Node<T>; }` is reported when it is instantiated (`struct 'Node<int>' contains itself:
field 'next' holds a 'Node<int>'`), as is a fixed-size array type argument.  A `static fn`
in a generic struct's `impl` block is the instantiation's, called as
`Pair<int, Str>::make(1, "a")`, like a generic class's.  A method or a `static fn` of the
block may add type parameters of its own to the struct's (`view fn fold<U>(seed: U, f: (U,
view A) -> U) -> U`, `Pair<int, Str>::keyed<K>(...)`), bounded like a free function's
(`K: Hashable`): see [Generic Methods](13-generics.md#generic-methods).

---

## Modules

A struct declared in a module is used from an importer as `mod::Name`: its constructor
`mod::Point(1, 2)`, its static functions `mod::Point::origin()`, and its fields and methods
on a value as usual.  A prebuilt module (`.pkm`, see [Prebuilt modules](15-modules.md#prebuilt-modules)) keeps a struct's
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
  let o: geo::Point = geo::Point::origin();
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
| Superclass | `struct A : B { ... }`; a struct named as a class's superclass, `class C : A {}`: `superclass 'A' of class 'C' is a struct, not a class` |
| Method in the body | `struct A { x: int; fn f() {} }` |
| Duplicate field | `struct A { x: int; x: int; }` |
| Field named like a type | `struct A { u8: int; }` or `struct A { Str: int; }`, with or without an `__init__`: `'u8' is a type name and cannot be used as a field name` |
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
| `__init__` reads a field, or uses `self`, before every field is assigned | `self.x = self.y;` before `self.y = ...`: `field 'y' is read before it is assigned` |
| `__super__` | in a struct's `__init__` |
| Assigning `self` | `self = A(1)` in a method |
| Duplicate method | two `fn m()` across `impl` blocks; `fn toString()` |
| `__deinit__` declared otherwise | One with a parameter, a result type or no body, or declared `static`, `view`, `shared` or `async`: `'__deinit__' takes no parameters ('fn __deinit__() { ... }' runs when the value is destroyed)`; two of them; one in a conditional `impl` block of a generic struct (see [`__deinit__`](#running-code-when-a-value-is-destroyed-__deinit__)) |
| `__deinit__` called or waiting | `g.__deinit__()`; an `await` in its body |
| Copying a struct with a `__deinit__` | `q = p` or `f(p)`: `cannot copy 'p' (a 'Lock'); write 'move p' to transfer it`; a capture of `p`: `cannot capture 'p' (a unique 'Lock'): a function captures by copy`; `p.clone()` without an `impl Cloneable for Lock` block: `a struct with a '__deinit__' is Cloneable only with an 'impl Cloneable for Lock' block` |
| Moving a field out of a struct with a `__deinit__` | `n = move g.name`: write `mem::replace(g.name, v)` |
| `self` lent `inout` whole in a struct with a `__deinit__` | `mem::replace(self, v)`, `x: inout = self`, `fn me() -> inout Lock { return self; }` in one of its methods: `'self' cannot be passed to 'inout' parameter 'slot': a 'Lock' has a '__deinit__', and what a method is called on stays the value it is (lend its fields)` |
| Replacing what a `match` binding reads | `match o { l: Lock { o = None; } None { } }`: `'o' cannot be assigned here: 'match' binding 'l' reads a value of it that runs a '__deinit__' where it is replaced, until its arm ends` |
| A look into a temporary that ends first | `v: view = make().o;` for a `Lock?` part; a branch of a conditional `match` subject reading a part of a shared temporary object: `a branch of a 'match' subject cannot read 'make(...).o': it is in a temporary, which is destroyed before the arms run` |
| A part leaving a temporary object that runs a `__deinit__` | `let g = makeObject().g;` where the object's class has one: `cannot move 'makeObject(...).g' out of 'makeObject(...)': a 'Loud' has a '__deinit__', which runs on the whole object` |
| A buffer of structs with a `__deinit__` | `mem::Buffer<Lock>`, `cl::List<Lock>`: `a buffer cannot hold a 'Lock' yet` |
| A method named `destroy` | `fn destroy()` or `static fn destroy()` in an `impl` block: `'destroy' is reserved for the compiler-generated destructor and cannot be declared in struct 'P' (a struct has none)` |
| `let` field assignment | `let a = A(1); a.x = 2;` |
| Field of a struct in an object that is not `mut` | `let s = Shape(Point(0, 0)); s.at.x = 7;`: `'s' is immutable (a 'Shape'; write 'mut Shape' to change it); cannot assign to its field 'x'` |
| `view` field assignment | `fn f(a: view A) { a.x = 1; }`; in a `view fn` |
| Field of a struct in a tuple | `t.1.x = 3` |
| Field of a temporary as `inout` | `bump(mk().x)` |
| Built-in trait | `impl Showable for A {}`; `impl ValueType for A {}` |
| Another module's struct | `impl other::A { ... }` |
| Crossing into C | `native fn f(a: view A)` |

---

## Not Yet Supported

`match` on a struct (other than unwrapping a `Point?`), a fixed-size array field, an `impl`
block for a struct of another module, a
field of an array element as an `inout` place, and a struct crossing into C other than as
a type argument of a generic native function ([Generic native
functions](03-functions.md#generic-native-functions)).  Like a tuple, a struct has no `equals` method and cannot be ordered
with `<`.

---

## Notes

- At run time a struct is laid out in place like a C struct of its fields, exactly as the
  tuple of its field types is ([Tuples](10-tuples.md#notes)): a struct of `int`s allocates
  nothing, a reference field is its box, retained once per copy and released when the
  variable goes out of scope, the holding object is destroyed or the array goes.  A
  `move`, a `return` of the variable, and an assignment or a store at its last use move its
  references instead, and the variable releases none
  ([Memory](16-memory.md#ownership-transfers)).  A
  method takes the receiver's address as its first argument, and a `view fn` the same;
  a constructor writes its result into the caller's storage.
- A struct with a `__deinit__` has one byte more than its fields, after the last of them
  (it counts toward the 64 bytes an optional's value may have): whether the storage holds a
  value still to destroy. The constructor sets it, a move
  clears it where the value was, and each place that lets the storage go runs the
  `__deinit__` only where it is set. `==` and `toString` do not see it.
- `toString` and `==` go through the struct's layout, constant data the compiler emits
  for each struct type that is printed or compared ([PIR](../pir.md)): the field count,
  the struct's name, then each field's name and the words a tuple element has.

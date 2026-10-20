# Classes

Classes group data with the functions that work on it. A class has fields, methods and a
constructor; it can extend one other class, and its methods are virtual. This chapter
first teaches classes, with a task in `tasks` becoming an object, and then gives the precise
rules: declarations, fields, methods, construction, inheritance and the `Obj` root.

## Declaring a class

A class lists its **fields** (each with a type) and then its **methods**. Its constructor,
the method called `__init__`, goes in an `impl` block for the class; it runs when an object
is created and must give every field a value. Inside methods, `self` is the object the
method was called on. You never write `self` in a parameter list:

```pkn
import ::io;

class Counter {
  name: Str;
  count: int;

  fn increment() {
    self.count = self.count + 1;
  }

  view fn report() -> Str {
    return self.name + ": " + Str(self.count);
  }
}

impl Counter {
  fn __init__(name: Str) {
    self.name = name;
    self.count = 0;
  }
}

fn main() -> int {
  c = Counter("visitors");
  c.increment();
  c.increment();
  io::println(c.report());
  c.count = 10;            // fields are accessible from outside
  io::println(c.report());
  return 0;
}
```

Output:

```
visitors: 2
visitors: 10
```

Create an object by calling the class name with the constructor's arguments, as in
`Counter("visitors")`. There is no `new`. Some rules:

- Fields have no default values: the compiler checks that `__init__` assigns every field on
  every path (`field 'count' of class 'Counter' is not assigned on every path through
  '__init__'`). The one exception, optional fields, comes in
  [Optionals](12-optionals.md#optional-fields).
- A class has exactly one `__init__`, and method names are unique within a class: there is
  no overloading. A class that leaves out `__init__` gets one that takes a value for each
  field, in the order they are declared: without its `impl` block, `Counter` would be
  built as `Counter("visitors", 0)` (see [The default `__init__`](#the-default-__init__)).
- A field or method is public unless its name says otherwise. A name starting with two
  underscores (`__count`) is private to its class: only the class's own methods reach it. A
  name starting with one underscore (`_cache`) is private to its module. See
  [Private fields and methods](#private-fields-and-methods).
- By convention class names are `UpperCamelCase`.

## Methods that only read: `view fn`

A method may change its object. A method that only reads it is written `view fn`, and the
compiler holds it to that: inside it, `self` is read-only. A `view` parameter (see
[Parameter modes](03-functions.md#parameter-modes-view-and-inout)) promises the caller that
the function will not change what it was given, so only `view fn` methods can be called on
one:

```pkn
import ::io;

class Account {
  balance: int;
  fn deposit(amount: int) { self.balance = self.balance + amount; }
  view fn report() -> Str { return "balance " + Str(self.balance); }
}

impl Account {
  fn __init__(start: int) { self.balance = start; }
}

fn show(a: view Account) {
  io::println(a.report());
}

fn main() -> int {
  a = Account(10);
  a.deposit(5);
  show(a);
  return 0;
}
```

Output:

```
balance 15
```

Calling `deposit` in `show`, or changing `self` inside `report`, is an error that names
what to fix:

```pkn
class Account {
  balance: int;
  fn deposit(amount: int) { self.balance = self.balance + amount; }
  view fn report() -> Str { self.balance = 0; return "empty"; }
}

impl Account {
  fn __init__(start: int) { self.balance = start; }
}

fn show(a: view Account) {
  a.deposit(5);
}

fn main() -> int {
  show(Account(10));
  return 0;
}
```

Error:

```
error: 'self' is read-only in 'view fn report'; cannot assign to its field 'balance'
error: 'a' is a 'view' parameter; 'deposit' is not a 'view fn'
```

The compiler warns about a method that never changes its object but is not a `view fn`,
so mark such methods as you write them. A `view fn` that returns part of its object, such
as a `Str` field, says so with `-> view Str`: the caller gets a `view` of it, which it can
read but not change or keep. The exact rules are in
[Methods that don't change self](#methods-that-dont-change-self).

## Objects are references

Objects live on the heap, and a variable holds a **reference** to one. Assigning an object
to another variable, or passing it to a function, shares the same object; it does not copy
it:

```pkn
import ::io;

class Box {
  value: int;
}

impl Box {
  fn __init__(v: int) { self.value = v; }
}

fn bump(b: Box) {
  b.value = b.value + 1;
}

fn main() -> int {
  a = Box(1);
  b = a;           // b and a are the same object
  b.value = 5;
  bump(a);
  io::println(Str(a.value) + " " + Str(b.value));
  return 0;
}
```

Output:

```
6 6
```

PaykanLang frees an object automatically as soon as nothing refers to it any more;
[Memory](16-memory.md) explains how. A value with named fields that is copied instead of
shared is a [struct](11-structs.md).

## Inheritance and virtual methods

A class can extend one other class with `class Derived : Base`. The derived class inherits
the base's fields and methods, may add its own, and may **override** a method by declaring
one with the same name and the same signature. Every method declared in a class is
virtual: the call runs the version of the object's actual class, even through a variable of
the base type.

The derived constructor must call the base constructor with `__super__(...)` as its first
statement:

```pkn
import ::io;

class Animal {
  name: Str;
  view fn sound() -> Str { return "..."; }
  view fn speak() -> Str { return self.name + " says " + self.sound(); }
}

impl Animal {
  fn __init__(name: Str) { self.name = name; }
}

class Dog : Animal {
  view fn sound() -> Str { return "woof"; }
}

impl Dog {
  fn __init__(name: Str) { __super__(name); }
}

class Cat : Animal {
  lives: int;
  view fn sound() -> Str { return "meow"; }
}

impl Cat {
  fn __init__(name: Str) {
    __super__(name);
    self.lives = 9;
  }
}

fn main() -> int {
  pets: Animal[3] = [Dog("Rex"), Cat("Tom"), Animal("Generic")];
  i = 0;
  while i < pets.len() {
    io::println(pets[i].speak());
    i = i + 1;
  }
  return 0;
}
```

Output:

```
Rex says woof
Tom says meow
Generic says ...
```

`speak` is declared once, in `Animal`, but its call to `self.sound()` runs `Dog`'s or `Cat`'s
version. A `Dog` can be stored wherever an `Animal` is expected; the reverse needs a
runtime check with `match`, shown below.

Things to know about inheritance:

- A class has at most one base class, and there are no abstract classes. A base class whose
  methods are meant to be overridden simply provides a default body, as `Animal.sound` does.
  A [trait](06-traits.md) (`trait Named { view fn name() -> Str; }`) lists methods that a
  class can declare it has with `impl Named for Animal {}`, but it is not a type: generic
  code requires it of a type parameter.
- An overriding method cannot call the base class's version of itself (`__super__(...)` can
  only be called in `__init__`), so put shared logic in a separate method (like `speak`
  above) or a free function.
- Arrays are not covariant: a `Dog[2]` is not an `Animal[2]`. Declare the array with the
  type you need, as `pets: Animal[3]` above.

## `Obj`, `toString` and `equals`

Every class extends `Obj`, the root of the class hierarchy, directly or indirectly. `Obj`
provides two methods that your classes can override:

- `view fn toString() -> Str`, used by `io::print` and `io::println` to print an object. The
  default prints `Object@` and an address.
- `view fn equals(other: Obj) -> bool`, used by `==` and `!=`. The default compares
  identity: two separately created objects are unequal even if their fields are the same.

Overriding `toString` makes objects printable, and overriding `equals` gives them value
equality. `equals` receives an `Obj`, so it uses `match` to check that the other object is
of the right class:

```pkn
import ::io;

class Point {
  x: int;
  y: int;

  view fn toString() -> Str {
    return "(" + Str(self.x) + ", " + Str(self.y) + ")";
  }

  view fn equals(other: Obj) -> bool {
    match other {
      p: Point { return self.x == p.x && self.y == p.y; }
      _        { return False; }
    }
  }
}

impl Point {
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }
}

fn main() -> int {
  a = Point(1, 2);
  b = Point(1, 2);
  io::println(a);
  io::println("a == b: " + Str(a == b));
  io::println("a != Point(2, 1): " + Str(a != Point(2, 1)));
  return 0;
}
```

Output:

```
(1, 2)
a == b: True
a != Point(2, 1): True
```

`io::println(a)` works because `io::println` accepts any object (any `Show` value) and
prints it with `toString()`.
`Str(a)` does not: the `Str` conversions are only for the built-in types. Call
`a.toString()` when you need the string.

## Checking the class at run time

A `match` on an object compares its **exact** runtime class against each arm, and an arm of
the form `name: Class` binds the object with that type:

```pkn
import ::io;

class Shape {}
impl Shape { fn __init__() {} }
class Circle : Shape {
  r: float;
}

impl Circle {
  fn __init__(r: float) { __super__(); self.r = r; }
}
class Rect : Shape {
  w: float;
  h: float;
}

impl Rect {
  fn __init__(w: float, h: float) { __super__(); self.w = w; self.h = h; }
}

fn describe(s: Shape) -> Str {
  match s {
    c: Circle { return "a circle of radius " + Str(c.r); }
    r: Rect   { return "a " + Str(r.w) + " by " + Str(r.h) + " rectangle"; }
    _         { return "some shape"; }
  }
}

fn main() -> int {
  io::println(describe(Circle(1.5)));
  io::println(describe(Rect(2.0, 3.0)));
  io::println(describe(Shape()));
  return 0;
}
```

Output:

```
a circle of radius 1.5
a 2 by 3 rectangle
some shape
```

The arm matches only the exact class, never a subclass of it. The `_` arm catches
everything else. When you can, prefer a virtual method over a `match` on classes: adding a
new subclass then does not require updating every `match`. [Match](08-match.md#type-mode--matching-on-the-runtime-class)
has the full rules.

## Project: the `Task` class

A task has a title and a done flag. A task with a deadline is a task too, so it is a
subclass that adds a `due` field and overrides `toString` to show it:

```pkn
import ::io;

class Task {
  title: Str;
  done: bool;

  fn complete() { self.done = True; }

  view fn checkbox() -> Str {
    return if self.done then "[x]" else "[ ]";
  }

  view fn toString() -> Str {
    return self.checkbox() + " " + self.title;
  }
}

impl Task {
  fn __init__(title: Str) {
    self.title = title;
    self.done = False;
  }
}

class DeadlineTask : Task {
  due: Str;

  view fn toString() -> Str {
    return self.checkbox() + " " + self.title + " (due " + self.due + ")";
  }
}

impl DeadlineTask {
  fn __init__(title: Str, due: Str) {
    __super__(title);
    self.due = due;
  }
}

fn main() -> int {
  milk = Task("buy milk");
  taxes: Task = DeadlineTask("file taxes", "April 15");
  milk.complete();
  io::println(milk);
  io::println(taxes);
  return 0;
}
```

Output:

```
[x] buy milk
[ ] file taxes (due April 15)
```

The rest of this chapter gives the precise rules.

---

## Quick Summary

- Class instances are **heap-allocated** with a leading vtable pointer; layout is
  `{ vptr, strong count, weak count, fields… }` (see [Memory](16-memory.md) for the counts).
- Every class transitively extends `Obj`. There is no multiple inheritance.
- **All methods declared inside a class are virtual**: dispatched through the vtable.
  Methods of an `impl` block are called directly (see [`impl` Blocks](#impl-blocks)).
- **`__init__` is the constructor.** It goes in the class's `impl` block, is called
  automatically when the object is created, and must assign every field. A class without
  one gets the default `__init__`, which takes the fields as arguments.
- A value with named fields but no vtable, inheritance or identity is a **struct**
  ([Structs](11-structs.md)): held in place like a tuple, with `impl` blocks for its methods.
- **`destroy` is the destructor.** Generated by the compiler and run automatically when the
  reference count drops to zero, it releases the object's fields and frees it. `destroy` is
  **final**: a class cannot define, override, or call it.

---

## Declaration

- The keyword is `class`, followed by an `UpperCamelCase` name. The body contains **field
  declarations** then **method declarations**.
- `self` is in scope inside every method. It is **never written in the parameter list**, and
  it cannot be assigned (`cannot assign to 'self'`).
- Construct with `ClassName(args)`, the arguments of its `__init__`.

---

## Fields

- Each field has a type annotation. Fields **do not have default values**: every field must be
  assigned inside `__init__`, or passed to the default one. An assignment in every arm of an
  exhaustive `match` counts ([Exhaustiveness and Control
  Flow](08-match.md#exhaustiveness-and-control-flow)).
- Field types may be builtins, `Str`, fixed-size arrays (`T[n]`), other class types, enums,
  tuples, structs, optionals and function types; not a slice (`T[]`).
- A field declared `weak` (`weak parent: Node?;`) refers to its object without keeping it
  alive and reads as `None` once the object is gone; its type is `T?` for a class `T`. It is
  the back link that breaks a reference cycle (see [Weak references](16-memory.md#weak-references)).

```pkn
class User {
  id: int;
  name: Str;
  scores: int[3];
}

impl User {
  fn __init__(id: int, name: Str) {
    self.id = id;
    self.name = name;
    self.scores = [0; 3];
  }
}
```

### Private fields and methods

A field or method whose name starts with two underscores is **private to its class**: it can
be read, assigned or called only from the class's own methods (the ones in its body, in its
`impl` blocks and in its trait impls), and nowhere else: not from a subclass, not from a free
function or another class in the same module, not from an importer. `__init__` and `__super__`
keep their special meaning. The default `__init__` still takes every field in order, private
ones included, since a constructor is the class's own code:

```pkn
import ::io;

class Counter {
  __count: int;
  step: int;

  fn tick() { self.__bump(self.step); }          // its own private method
  view fn value() -> int { return self.__count; }
  fn __bump(by: int) { self.__count = self.__count + by; }
}

fn main() -> int {
  c = Counter(0, 2);                               // the private field is a constructor argument
  c.tick();
  c.tick();
  io::println(Str(c.value()) + " " + Str(c.step));
  return 0;
}
```

Output:

```
4 2
```

`c.__count` or `c.__bump(1)` in `main`, or in a method of another class, is an error:
`'__count' is private to 'Counter': it is reachable only from its own methods`. The same holds
in a subclass: `self.__count` inside `class Sub : Counter` is rejected. A subclass cannot
declare a field of the same name either, private or not: `field '__count' in class 'Sub'
shadows a field in superclass 'Counter'`.

A field or method whose name starts with **one** underscore (`_cache`, `_rebuild()`, `static fn
_make()`) is **private to its module** instead: every function and class of the module that
declares the class reaches it, a subclass and a free function included, and an importer does
not (`'_cache' of 'Counter' is private to module 'm'`). A public field or a public method whose
type or signature names a module-private class (`fn make() -> _Node`) is rejected in the
declaring module, because an importer would receive a value it cannot name; see
[Modules](15-modules.md#private-to-the-module).

An initializer written `___init__` (three underscores) is **private to the module**. It is
the class's `__init__` in every other way (the one initializer, in an `impl` block, what a
subclass's `__super__` calls), but `Counter(...)`, and `__super__(...)` into it from a
subclass, are allowed only in the module that declares the class: its functions, its other
types' methods, and its templates wherever they are instantiated. An importer gets
`'m::Counter' cannot be constructed here: its initializer '___init__' is private to module
'm'` (`cannot be extended here` from a subclass), and makes one through a function the module
provides, as `io::open` and `io::stdin` make an `io::File`.
Privacy does not hide a field from what the compiler generates: a struct's `toString` and
`==` include its private fields ([Structs](11-structs.md)). The rules are in
[the visibility design](../design/visibility.md).

### The default `__init__`

A class that declares no `__init__` gets one. It takes one argument per field, in the order
the fields are declared, and stores each. A subclass's takes the superclass constructor's
arguments first, those of its `__init__` (written or default), and passes them to
`__super__`:

```pkn
import ::io;

class Point {
  x: int;
  y: int;
}

class Named : Point {
  name: Str;
}

fn main() -> int {
  p = Point(1, 2);        // x = 1, y = 2
  n = Named(3, 4, "n");   // Point's x and y, then name
  io::println(Str(p.x + p.y) + " " + n.name + Str(n.y));
  return 0;
}
```

Output:

```
3 n4
```

Every field is an argument; `Point()` does not leave them unset:

```pkn
class Point {
  x: int;
  y: int;
}

fn main() -> int {
  p = Point(1);
  return p.x;
}
```

Error:

```
function 'Point' expects 2 argument(s), got 1 (it has no '__init__', so it takes x: int, y: int)
```

- A class with no fields, and no superclass with any, is built with no arguments: `Empty()`.
- A generic class's infers its type arguments from the fields: with `class Box<T> { v: T; }`,
  `Box(3)` is a `Box<int>` (see [Generics](13-generics.md)).
- Writing an `__init__` replaces the default one.

---

## Methods

Every method declared inside a class is **virtual**: dispatched through the vtable.

There is **no method overloading**: each method name must be unique within a class.
This also means a class has **exactly one `__init__`**; multiple constructors with
different parameter lists are not supported.

### Methods that don't change self

A method may change `self`. One that does not is written `view fn`, like `report` in
`Account` above, and `self` is read-only in its body: through `self` it cannot

- assign a field or an element (`self.n = 1`, `self.xs[0] = 1`);
- call a method that is not a `view fn` (`self.tick()`, `self.child.tick()`);
- pass `self`'s fields to an `inout` parameter (`bump(self.n)`) or name one with an
  `inout` local;
- pass `self` or its fields to a parameter that is not `view`, or store `self` or a
  reference reached through it ([A `view` stays a
  `view`](03-functions.md#a-view-of-a-reference-stays-a-view)); a `weak` local
  may name a reference reached through `self`, since it takes none
  ([Weak references](16-memory.md#weak-references)).

Each is an error that names the method: `'self' is read-only in 'view fn value'; 'tick' is
not a 'view fn'`. Other objects can change as usual.

Part of `self` that is an object, string or optional, or an array or tuple holding one,
is a `view` in a `view fn`, so the method returns it as one, `-> view T`
([Borrowed results](03-functions.md#borrowed-results)), and its caller cannot
change it. `view fn name() -> Str { return self.name; }` is an error: `'self' is read-only
in 'view fn name', so part of it can only be returned as a 'view': write '-> view Str'`. A
field of a value type, or an array or tuple of them, is returned as a copy (`view fn
count() -> int`, `view fn bounds() -> (int, int)`). An override of a
method that returns a copy, such as `toString`, returns a new value instead. A method that
is not a `view fn` may return part of `self` for its caller to change.

Only a `view fn` can be called on a `view`, a parameter or a local
([`view`: read-only](03-functions.md#view-read-only)): `'c' is a 'view' parameter;
'increment' is not a 'view fn'`. The builtin methods that only read are `view fn`s:
`toString`, `equals`, `len`; `concat` is not. (The standard library's
`io::File` methods are `view fn`s.) A `let`
local only cannot be reassigned: any method can be called on what it holds.

- `__init__` cannot be a `view fn`: it sets up `self`.
- An override keeps the marker of the method it overrides, both ways:
  `override of 'toString' must be a 'view fn', like the method it overrides`, and
  `override of 'tick' cannot be a 'view fn': the method it overrides may change 'self'`.
  So an override of `toString` or `equals` is a `view fn`.
- Only a method can be one: `only a method can be a 'view fn': 'f' is a free function,
  with no 'self'`.
- A module's `view fn` markers are part of its interface, whether it is imported from
  source or from a `.pkm` file.

A method that never changes `self` but is not a `view fn` gets a warning, so that `view`
parameters can call it: `'value' never changes 'self': make it a 'view fn' so 'view'
parameters can call it`. A method counts as changing `self` when it does one of the
things a `view fn` cannot (returning part of `self` for its caller to change is one), or
calls on `self` a method that does; a method with an
override that changes `self` gets no warning, and neither does its override. The warning
is not given while the program has errors.

### `impl` Blocks

An `impl` block adds methods to a class of the same module, outside its declaration. Its
methods are **statically dispatched**: called directly, not through the vtable. A class
may have several `impl` blocks, and its subclasses inherit their methods.

```pkn
class Point {
  x: int;
  y: int;
}

impl Point {
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }
  static fn origin() -> Point { return Point(0, 0); }      // Point::origin()
  view fn lengthSquared() -> int { return self.x * self.x + self.y * self.y; }
  fn shift(d: int) { self.x = self.x + d; self.y = self.y + d; }
}

fn main() -> int {
  p = Point::origin();
  p.shift(3);
  return p.lengthSquared();   // 18
}
```

- `fn` and `view fn` methods follow the same rules as those of the class body, and the
  two can call each other.
- A `static fn` has no `self`. It is called as `Type::name(...)`, or `mod::Type::name(...)`
  from a module that imports it; one of a generic class is called on an instantiation,
  `Box<int>::of(1)` (see [Generics](13-generics.md)).
- No method may reuse a name that the class or one of its superclasses already has, and
  a subclass cannot override an `impl` method (`method 'g' of 'A' is declared in an 'impl'
  block and cannot be overridden`).
- `__init__` goes in an `impl` block, not the class body: it is the class's constructor,
  not a virtual method, and a class has one.
- An `impl` block can only add to a class of its own module; built-in types and enums
  cannot have one. A generic class's names its type parameters: `impl Box<T> { … }`
  (see [Generics](13-generics.md)).

---

## Inheritance

- Inheritance is declared with `:`: `class Derived : Base { … }`.
- A class may extend at most one base (no multiple inheritance).
- An `__init__` that a derived class writes **must call `__super__(args…)` as its first
  statement**, also when the base's takes no arguments. Only when the base's constructor is the
  default one of a class with no fields, and so are its superclasses', may `__super__()` be left
  out: it does nothing. A derived class without an `__init__` gets the default one, which
  calls `__super__` for it.
- `__super__(…)` may only be called inside `__init__`.
- A method with the same name as a base method is an **override**. The override must have the
  same parameter and return types, and keep each parameter's mode (`view`, `inout`; see
  [Parameter modes](03-functions.md#parameter-modes-view-and-inout)) and the `view fn`
  marker.

### Three-Level Inheritance

```pkn
class Vehicle {
  speed: int;
  fn kind() -> Str    { return "vehicle"; }
}

impl Vehicle {
  fn __init__(s: int) { self.speed = s; }
}

class Car : Vehicle {                  // the default __init__(s: int) calls __super__(s)
  fn kind() -> Str    { return "car"; }
}

class SportsCar : Car {
  boost: int;
  fn kind() -> Str    { return "sports car"; }
  fn topSpeed() -> int { return self.speed + self.boost; }
}

impl SportsCar {
  fn __init__(s: int, b: int) {
    __super__(s);
    self.boost = b;
  }
}
```

---

## The `Obj` Root

Every class transitively extends `Obj`. `Obj` defines:

| Slot       | Signature                            | Default                      |
|------------|--------------------------------------|------------------------------|
| `toString` | `view fn toString() -> Str`          | Returns `Object@<address>` (a per-class name is planned) |
| `equals`   | `view fn equals(other: Obj) -> bool` | Identity (pointer) comparison |
| `destroy`  | (compiler-generated)                 | Frees the object             |

Override `toString` and `equals` in your class to customise behaviour. `destroy` is generated
by the compiler and is final: it cannot be declared, overridden or called.

The `==` and `!=` operators are defined in terms of `equals`: `a == b` evaluates `a.equals(b)`
and `a != b` its negation, dispatched through the vtable. So overriding `equals` also changes how
`==`/`!=` behave for your class. Through `Obj`, every class implements the built-in trait
`Show` ([Traits](06-traits.md)).

---

## Subtyping

- `Derived <: Base` whenever `class Derived : Base`.
- A `Derived` instance may be stored in a variable of type `Base` or `Obj`.
- `Derived[n]` is **not** a `Base[n]`, and a `view Base[]` parameter does not take one
  (an array's element type is exact).

```pkn
a: Animal = Dog();   // Dog stored as Animal — valid
```

---

## Generics

A class may take type parameters (`class Box<T> { v: T; … }`) and is then used
as `Box<int>`, `Box<Str>`, `Box<Box<int>>`, each of which is an ordinary class
created for that argument list. See [Generics](13-generics.md).

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Class name taken | The class is named like a builtin class (`class Str`) or type (`class int`), an enum, or another class — a class name is also its constructor, so top-level names are shared (see [Builtin Names Are Reserved](02-basics.md#builtin-names-are-reserved)) |
| Missing field init in `__init__` | `__init__` exits with an uninitialised field on some path |
| Default `__init__` arguments | A class without an `__init__` is constructed with other than its superclass constructor's arguments followed by its fields (`Point()` for `class Point { x: int; }`) |
| `__super__(…)` not first | Derived `__init__` body does not begin with `__super__(…)` |
| `__super__(…)` outside `__init__` | `__super__(…)` called in any method other than `__init__` |
| Direct call to `destroy` | User code calls `obj.destroy()` explicitly |
| `weak` field of the wrong type | `weak f: Node;` (not optional) or `weak f: int?;` (not a class): `'weak' field 'f' of class 'C' must be optional ...` / `... has type 'int?'; a weak reference is an optional class type` |
| `weak` field passed `inout` | `f(obj.parent)` where `parent` is `weak` and the parameter is `inout`: a weak reference is not a place a strong reference could be written to |
| Override of `destroy` | A class declares its own `fn destroy()` |
| Override signature mismatch | Override has a different parameter or return type than the base method |
| Override changes a parameter mode | Override drops, adds or changes a parameter's `view` / `inout` |
| Override changes `view fn` | Override of a `view fn` is not one, or an override of another method is one |
| `view fn __init__` or a free `view fn` | `__init__` sets up `self`; a free function has no `self` |
| `self` changed in a `view fn` | A `view fn` assigns `self`'s fields or elements, calls a method on `self` that is not a `view fn`, passes them to `inout`, or passes on or stores a reference reached through `self` |
| Multiple bases | More than one `:` clause |
| `self` in parameter list | Writing `self` as an explicit method parameter |
| Private member used outside its class | `c.__count`, `c.__bump()`, `self.__count` in a subclass, or in a free function: `'__count' is private to 'Counter': it is reachable only from its own methods` |
| Module-private member used by an importer | `c._cache` or `m::Counter::_make()` outside the module that declares `Counter`: `'_cache' of 'Counter' is private to module 'm'` |
| Private type in a public signature | A public field, a public method or a free function naming a `_`-prefixed class, struct, enum or trait: `'make' is public but its result names the private type '_Node'; make the function private or the type public` |
| `__` on a top-level declaration | `fn __f()`, `class __C`: `function '__f': the '__' prefix makes a member private to its class or struct; a module's own function is spelled '_f'` |
| Assigning `self` | `self = C(1)` in a method: `self` names the object the method was called on (`cannot assign to 'self'`) |
| `__init__` in the class body | `class A { fn __init__() { … } }`; it goes in `impl A { … }` |
| `impl` method name taken | An `impl` method or `static fn` has the name of a method of the class or a superclass, or a subclass declares one with an `impl` method's name |
| `impl` for another type | The block names a built-in type, an enum, or a class of another module |

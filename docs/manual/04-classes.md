# Classes and inheritance

Classes group data with the functions that work on it. PaykanLang has classes with fields,
methods and a constructor, single inheritance, and virtual methods. In this chapter a task
in `tasks` becomes an object.

## Declaring a class

A class lists its **fields** (each with a type) and then its **methods**. The method called
`__init__` is the constructor; it runs when an object is created and must give every field a
value. Inside methods, `self` is the object the method was called on. You never write
`self` in a parameter list:

```pkn
class Counter {
  name: Str;
  count: int;

  fn __init__(name: Str) {
    self.name = name;
    self.count = 0;
  }

  fn increment() {
    self.count = self.count + 1;
  }

  view fn report() -> Str {
    return self.name + ": " + Str(self.count);
  }
}

fn main() -> int {
  c = Counter("visitors");
  c.increment();
  c.increment();
  println(c.report());
  c.count = 10;            // fields are accessible from outside
  println(c.report());
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
  [Optionals](08-optionals.md).
- A class has exactly one `__init__`, and method names are unique within a class: there is
  no overloading. A class without fields may leave out `__init__`.
- There are no access modifiers: every field and method is public.
- By convention class names are `UpperCamelCase`.

## Methods that only read: `view fn`

A method may change its object. A method that only reads it is written `view fn`, and the
compiler holds it to that: inside it, `self` is read-only. A `view` parameter (see
[Basics](02-basics.md#functions)) promises the caller that the function will not change what it
was given, so only `view fn` methods can be called on one:

```pkn
class Account {
  balance: int;
  fn __init__(start: int) { self.balance = start; }
  fn deposit(amount: int) { self.balance = self.balance + amount; }
  view fn report() -> Str { return "balance " + Str(self.balance); }
}

fn show(a: view Account) {
  println(a.report());
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
  fn __init__(start: int) { self.balance = start; }
  fn deposit(amount: int) { self.balance = self.balance + amount; }
  view fn report() -> Str { self.balance = 0; return "empty"; }
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
so mark such methods as you write them. An override keeps the marker of the method it
overrides, so `toString` and `equals` are always `view fn`s. The details are in
[the language reference](../language/04-classes.md#methods-that-dont-change-self).

## Objects are references

Objects live on the heap, and a variable holds a **reference** to one. Assigning an object
to another variable, or passing it to a function, shares the same object; it does not copy
it:

```pkn
class Box {
  value: int;
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
  println(Str(a.value) + " " + Str(b.value));
  return 0;
}
```

Output:

```
6 6
```

PaykanLang frees an object automatically as soon as nothing refers to it any more;
[Memory](12-memory.md) explains how.

## Inheritance and virtual methods

A class can extend one other class with `class Derived : Base`. The derived class inherits
the base's fields and methods, may add its own, and may **override** a method by declaring
one with the same name and the same signature. Every method is virtual: the call runs the
version of the object's actual class, even through a variable of the base type.

The derived constructor must call the base constructor with `__super__(...)` as its first
statement:

```pkn
class Animal {
  name: Str;
  fn __init__(name: Str) { self.name = name; }
  view fn sound() -> Str { return "..."; }
  view fn speak() -> Str { return self.name + " says " + self.sound(); }
}

class Dog : Animal {
  fn __init__(name: Str) { __super__(name); }
  view fn sound() -> Str { return "woof"; }
}

class Cat : Animal {
  lives: int;
  fn __init__(name: Str) {
    __super__(name);
    self.lives = 9;
  }
  view fn sound() -> Str { return "meow"; }
}

fn main() -> int {
  pets: Animal[] = [Dog("Rex"), Cat("Tom"), Animal("Generic")];
  i = 0;
  while (i < pets.len()) {
    println(pets[i].speak());
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

- A class has at most one base class, and there are no interfaces or abstract classes. A
  base class whose methods are meant to be overridden simply provides a default body, as
  `Animal.sound` does.
- An override must have exactly the same parameter and return types as the method it
  overrides, and the same parameter modes (`view`, `inout`, [Basics](02-basics.md#functions)).
- `__super__(...)` can only be called in `__init__`. An overriding method cannot call the
  base class's version of itself, so put shared logic in a separate method (like `speak`
  above) or a free function.
- Arrays are not covariant: a `Dog[]` is not an `Animal[]`. Declare the array with the type
  you need, as `pets: Animal[]` above.

## `Obj`, `toString` and `equals`

Every class extends `Obj`, the root of the class hierarchy, directly or indirectly. `Obj`
provides two methods that your classes can override:

- `view fn toString() -> Str`, used by `print` and `println` to print an object. The default
  prints `Object@` and an address.
- `view fn equals(other: Obj) -> bool`, used by `==` and `!=`. The default compares identity: two
  separately created objects are unequal even if their fields are the same.

Overriding `toString` makes objects printable, and overriding `equals` gives them value
equality. `equals` receives an `Obj`, so it uses `match` to check that the other object is
of the right class:

```pkn
class Point {
  x: int;
  y: int;
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }

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

fn main() -> int {
  a = Point(1, 2);
  b = Point(1, 2);
  println(a);
  println("a == b: " + Str(a == b));
  println("a != Point(2, 1): " + Str(a != Point(2, 1)));
  return 0;
}
```

Output:

```
(1, 2)
a == b: True
a != Point(2, 1): True
```

`println(a)` works because `println` accepts any object and prints it with `toString()`.
`Str(a)` does not: the `Str` conversions are only for the built-in types. Call
`a.toString()` when you need the string.

`destroy`, the method that frees an object, is generated by the compiler; a class cannot
declare, override or call it.

## Checking the class at run time

A `match` on an object compares its **exact** runtime class against each arm, and an arm of
the form `name: Class` binds the object with that type:

```pkn
class Shape { fn __init__() {} }
class Circle : Shape {
  r: float;
  fn __init__(r: float) { __super__(); self.r = r; }
}
class Rect : Shape {
  w: float;
  h: float;
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
  println(describe(Circle(1.5)));
  println(describe(Rect(2.0, 3.0)));
  println(describe(Shape()));
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
new subclass then does not require updating every `match`.

## Project: the `Task` class

A task has a title and a done flag. A task with a deadline is a task too, so it is a
subclass that adds a `due` field and overrides `toString` to show it:

```pkn
class Task {
  title: Str;
  done: bool;

  fn __init__(title: Str) {
    self.title = title;
    self.done = False;
  }

  fn complete() { self.done = True; }

  view fn checkbox() -> Str {
    return if self.done then "[x]" else "[ ]";
  }

  view fn toString() -> Str {
    return self.checkbox() + " " + self.title;
  }
}

class DeadlineTask : Task {
  due: Str;

  fn __init__(title: Str, due: Str) {
    __super__(title);
    self.due = due;
  }

  view fn toString() -> Str {
    return self.checkbox() + " " + self.title + " (due " + self.due + ")";
  }
}

fn main() -> int {
  milk = Task("buy milk");
  taxes: Task = DeadlineTask("file taxes", "April 15");
  milk.complete();
  println(milk);
  println(taxes);
  return 0;
}
```

Output:

```
[x] buy milk
[ ] file taxes (due April 15)
```

The full rules are in [Classes](../language/04-classes.md) in the reference. Next:
[Enums and match](05-enums-and-match.md).

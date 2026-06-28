# PaykanLang — Classes

This document covers user-defined classes: declarations, fields, methods, construction,
inheritance, and the `Obj` root.

---

## Quick Summary

- Class instances are **heap-allocated** with a leading vtable pointer; layout is `{ vptr, fields… }`.
- Class names use `UpperCamelCase`.
- Every class transitively extends `Obj`. There is no multiple inheritance.
- **All methods declared inside a class are virtual** — dispatched through the vtable.
- The receiver `self` is **implicit** for methods (never written in the parameter list).
- **`__init__` is the constructor.** Called automatically when the object is created. Must assign every field.
- **`destroy` is the destructor.** Called automatically when the reference count drops to zero.
  Users cannot call `destroy` directly. Override it to run custom cleanup.

---

## Declaration

```pkn
class Point {
  x: int;
  y: int;

  fn __init__(px: int, py: int) {
    self.x = px;
    self.y = py;
  }

  fn toString() -> Str {
    return "(" + StrInt(self.x) + "," + StrInt(self.y) + ")";
  }

  fn sum() -> int {
    return self.x + self.y;
  }
}

p: Point = Point(1, 2);
println(p.toString());     // (1,2)
```

- The keyword is `class`. The body contains **field declarations** then **method declarations**.
- `self` is in scope inside every method. It is **never written in the parameter list**.
- Construct with `ClassName(args)`.

---

## Fields

- Each field has a type annotation. Fields **do not have default values** — every field must be
  assigned inside `__init__`.
- Field types may be builtins, `Str`, arrays, or other class types.

```pkn
class User {
  id: int;
  name: Str;
  friends: User[];

  fn __init__(id: int, name: Str) {
    self.id = id;
    self.name = name;
    self.friends = [];
  }
}
```

---

## Methods

Every method declared inside a class is **virtual** — dispatched through the vtable.

```pkn
class Counter {
  count: int;

  fn __init__(start: int) { self.count = start; }
  fn increment()          { self.count = self.count + 1; }
  fn value() -> int       { return self.count; }
}

c: Counter = Counter(0);
c.increment();
println(StrInt(c.value()));   // 1
```

There is **no method overloading** — each method name must be unique within a class.
This also means a class has **exactly one `__init__`**; multiple constructors with
different parameter lists are not supported.

### Static Dispatch via Free Functions

There is no per-method opt-out for virtual dispatch. For static dispatch (e.g. hot
numerical code), define a free function in the same module that takes the object as its
first parameter:

```pkn
class Point {
  x: int;
  y: int;
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }
}

fn distance(a: Point, b: Point) -> float {
  dx: int = a.x - b.x;
  dy: int = a.y - b.y;
  return sqrt((dx * dx + dy * dy) as float);   // statically dispatched
}
```

---

## Inheritance

```pkn
class Animal {
  fn __init__() {}
  fn sound() -> Str { return "..."; }
}

class Dog : Animal {
  fn __init__() { __super__(); }
  fn sound() -> Str { return "woof"; }   // override
}

class Cat : Animal {
  fn __init__() { __super__(); }
  fn sound() -> Str { return "meow"; }   // override
}

d: Animal = Dog();
println(d.sound());   // woof  (virtual dispatch)
```

- Inheritance is declared with `:` — `class Derived : Base { … }`.
- A class may extend at most one base (no multiple inheritance).
- The derived `__init__` **must call `__super__(args…)` as its first statement** when the base
  has an `__init__` with parameters. For a zero-parameter base `__init__`, `__super__()` is still
  required in the body.
- `__super__(…)` may only be called inside `__init__`.
- `destroy` chaining is automatic: after the derived `destroy` body runs, the base `destroy` is
  called automatically up to `Obj`. No explicit `__super__` call needed in `destroy`.
- A method with the same name as a base method is an **override**. The override must have the
  same signature.

### Three-Level Inheritance

```pkn
class Vehicle {
  speed: int;
  fn __init__(s: int) { self.speed = s; }
  fn kind() -> Str    { return "vehicle"; }
}

class Car : Vehicle {
  fn __init__(s: int) { __super__(s); }
  fn kind() -> Str    { return "car"; }
}

class SportsCar : Car {
  boost: int;
  fn __init__(s: int, b: int) {
    __super__(s);
    self.boost = b;
  }
  fn kind() -> Str    { return "sports car"; }
  fn topSpeed() -> int { return self.speed + self.boost; }
}
```

---

## The `Obj` Root

Every class transitively extends `Obj`. `Obj` defines:

| Slot       | Signature                       | Default                      |
|------------|---------------------------------|------------------------------|
| `toString` | `fn toString() -> Str`          | Returns the class name       |
| `equals`   | `fn equals(other: Obj) -> bool` | Identity (pointer) comparison |
| `destroy`  | `fn destroy()`                  | Frees the object             |

Override `toString` and `equals` in your class to customise behaviour.

```pkn
class Person {
  name: Str;
  age: int;
  fn __init__(n: Str, a: int) { self.name = n; self.age = a; }
  fn toString() -> Str { return self.name + " (age " + StrInt(self.age) + ")"; }
}

p: Person = Person("Alice", 30);
println(p.toString());   // Alice (age 30)
```

---

## Subtyping

- `Derived <: Base` whenever `class Derived : Base`.
- A `Derived` instance may be stored in a variable of type `Base` or `Obj`.
- `Derived[]` is **not** a subtype of `Base[]` (arrays are invariant).

```pkn
a: Animal = Dog();   // Dog stored as Animal — valid
```

---

## Runtime Type Dispatch (`match`)

Use `match` to test and downcast at runtime. See `03-enums.md` for full `match` documentation.

```pkn
a: Animal = Labrador();
match a {
  lab: Labrador { println("Labrador: " + lab.name); }
  Dog           { println("plain Dog"); }
  _             { println("other"); }
}
```

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Missing field init in `__init__` | `__init__` exits with an uninitialised field on some path |
| `__super__(…)` not first | Derived `__init__` body does not begin with `__super__(…)` |
| `__super__(…)` outside `__init__` | `__super__(…)` called in any method other than `__init__` |
| Direct call to `destroy` | User code calls `obj.destroy()` explicitly |
| Override signature mismatch | Override has a different parameter or return type than the base method |
| Multiple bases | More than one `:` clause |
| `self` in parameter list | Writing `self` as an explicit method parameter |

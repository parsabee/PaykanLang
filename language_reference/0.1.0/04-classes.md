# PaykanLang — Classes

This document covers user-defined classes: declarations, fields, methods, construction,
inheritance, and the `Obj` root.

---

## Quick Summary

- Class instances are **heap-allocated** with a leading vtable
  pointer; layout is `{ vptr, fields… }`.
- Class names use `UpperCamelCase`.
- Every class transitively extends `Obj`. There is no multiple inheritance.
- **All methods declared inside a class are virtual. Except class private methods** For static dispatch, write a free function
  in the same module, that takes the object as the first argument called self.
- The receiver `self` is **implicit** for methods.
- **Visibility** by member name:
  - Plain name (`x`, `speak`) — public.
  - Leading single underscore (`_x`, `_helper`) — private to module (this class, methods, subclasses and functions in the same module only).
  **Free functions in the same module file** that take the class as
    their first parameter may access `_private` members — PaykanLang's only "friend" mechanism, keyed on the module boundary. See `06-modules.md` ("Same-Module Privacy").
- **`__init__` is the constructor.** It is called automatically when the object is created.
  `__init__` takes explicit parameters and must assign every field before returning. The field
  values are **not** passed implicitly by position; you write `self.x = ...` yourself.
- **`__del__` is the destructor.** It is `virtual`, called automatically when the object is
  destroyed (scope exit for refcount -> 0). Users **cannot call `__del__` directly.** Destruction of member fields
  (dropping unique, releasing shared) is automatic after `__del__` returns; you only need
  to write custom cleanup (e.g. closing a handle).

---

## Declaration

```
class Point {
  x: float;
  y: float;

  fn __init__(x: float, y: float) {
    self.x = x;
    self.y = y;
  }

  fn distance(other: Point) -> float {
    dx: float = self.x - other.x;
    dy: float = self.y - other.y;
    return sqrt(dx * dx + dy * dy);
  }
}


p: Point = Point(1.0, 2.0); 
q = Point(4.0, 6.0);
d: float = p.distance(q);
```

- The keyword is `class`. The body contains **field declarations** and **method declarations**
  in that order.
- `self` is in scope inside every method. It is **never written in the parameter list.**
- The canonical form for **declaring a class-typed variable** is
  `var: ClassType = ClassType(args);` or `var = ClassType(args);`.

---

## Methods

A method declared inside a class is implicitly:

- **Virtual** — dispatched through the vtable.

```
class Counter {
  n: int;

  fn __init__(n: int)   { self.n = n; }
  fn get() -> int       { return self.n; }  
  fn inc()              { self.n = self.n + 1; }
}

c = Counter(0);
c.inc();
v = c.get();
```

### Static Dispatch

There is no per-method opt-out for virtual dispatch. To get static dispatch (e.g. for hot paths, or methods that genuinely have no overridable behaviour), define a **free function** in the same module as the class definition
that takes the object as self:

```
class Point {...}

fn distance(self: Point, other: Point) -> float {
  dx: float = self.x - other.x;
  dy: float = self.y - other.y;
  return sqrt(dx * dx + dy * dy);
}

p = Point(1, 2);
q = Point(10, 20);
d = distance(p, q);
```

### Performance: prefer free functions in hot code

**Every method declared inside a `class` is virtual** — it goes through the
vtable on every call. That is the right default for polymorphic hierarchies
(it is what makes overrides work), but it is the **wrong** default for hot
numerical / data-processing blocks.

The recommended idiom for any class that exists primarily to *own data*
(rather than to *be subclassed*) is therefore:

1. Declare the class with **only `__init__`** (and `__del__` if it owns a
   non-trivial resource).
2. Mark the data fields `_private` (single leading underscore) so they are not part of the
   public surface.
3. Write **all** operations as free functions in the same module file,
   with the receiver as the **first parameter literally named `self`**
   (`self: ClassName`). Per "Same-Module Privacy" (`06-modules.md`),
   those functions can read and write the class's `_private` fields
   directly — no accessor methods needed.

```
// counter.pkn
class Counter {
  _n: int;
  fn __init__()                  { self._n = 0; }
}

// Free functions in the same module — statically dispatched, inlinable,
// and able to touch _n directly.
fn get(self: Counter) -> int       { return self._n; }
fn inc(self: Counter)              { self._n = self._n + 1; }
fn reset(self: Counter)            { self._n = 0; }
```

Reach for an in-class method only when you genuinely want one of:

- **Virtual dispatch** through `Obj` or a user hierarchy (the whole
  point of methods).
- **Override** of an `Obj` slot like `toString` / `equals`.

Anything else — accessors, builders, operations on the data — should be
a free function.

---

## Visibility

Visibility is encoded in the member name (no keywords):

| Prefix | Visibility | Accessible from |
|---|---|---|
| (none)        | public    | anywhere |
| `_name`       | private   | the declaring class, its subclasses, and free functions in the same module whose **first parameter is literally named `self`** and typed as the class (`self: ClassName`) |

This applies uniformly to fields and methods. Examples:

```
class User {
  id: int;            // public
  _cache: Str;        // private

  fn touch() { self._rebuild(); }      // OK
  fn _rebuild() { self._cache = ""; }
}
```

A subclass may read/write `_cache` and call `_rebuild`. Outside the hierarchy, only `id` and `touch` are accessible.

---

## Fields

- Each field has a type. Fields **do not have default values** — every field must be assigned
  inside `__init__`.
- Field types may be class types, builtins, enums or arrays.

```
class User {
  id: int;
  name: Str;
  friends: User[];         // array of users

  fn __init__(id: int, name: Str, friends: User[]) {
    self.id = id;
    self.name = name;
    self.friends = friends;
  }
}
```

---

### Inherited Fields

For a class that extends a base, the constructor takes the **base fields first, then the
derived fields**, all in declaration order:

```
class Animal {
  name: Str;

  fn __init__(name: Str) {
    self.name = name;
  }
}

class Dog extends Animal {
  breed: Str;

  fn __init__(name: Str, breed: Str) {
    __super__(name);
    self.breed = breed;
  }
}
```

---

## Inheritance

```
class Animal {
  name: Str;

  fn __init__(name: Str) {
    self.name = name;
  }

  fn speak() { std::write(std::out(), "..."); }        // virtual (default)
  fn id() -> Str { return self.name; }
}

class Dog extends Animal {
  breed: Str;

  fn __init__(name: Str, breed: Str) {
    __super__(name);                                  // call Animal.__init__ first
    self.breed = breed;
  }

  fn speak() { std::write(std::out(), "woof"); }                  // override
}

d: Dog = Dog("rex", "labrador");
d.speak();                  // prints: woof
std::write(std::out(), d.name);
```

- A class declares at most one base class with `extends`. There is no multiple inheritance.
- The derived `__init__` **must call `__super__(args...)` as its first statement.** The compiler
  enforces this. If the base has a zero-parameter `__init__`, it is called implicitly and
  `__super__()` may be omitted.
- `__super__(...)` may **only** be called inside `__init__`. Calling it anywhere else is a
  compile error.
- `__del__` chaining is **automatic**: after the derived `__del__` body finishes, the runtime
  calls the base `__del__`, continuing up to `Obj.__del__`. No `__super__` call is needed.
- Methods with the same name and signature as a base method are **overrides**. The override
  must be type-compatible (covariant return on class types is allowed; parameters invariant).
- `Obj` is the implicit root.

### `Obj` Vtable

Every class transitively extends `Obj`, which defines:

| Slot | Signature | Default |
|---|---|---|
| `toString` | `fn toString() -> Str` | Returns the class name |
| `equals`   | `fn equals(other: Obj) -> bool` | Identity comparison |
| `__del__`  | `fn __del__()` | Destructor |

User classes may override either.

---

## Method Resolution

- All methods declared inside a class are dispatched through the **vtable**.
- The implementation chosen depends on the runtime class of the receiver, not the static type.
- Free functions are always statically dispatched and never participate in override resolution.

To call a base method implementation explicitly from inside an overriding method, use
`__super__.method(args)`. This is non-virtual and resolves to the immediate base's vtable slot.

---

## Subtyping Rules

- `Derived <: Base` whenever `class Derived extends Base`.
- `Derived[]` is **not** a subtype of `Base[]` (arrays are invariant) — use `Base[]` from the
  start, or copy/convert.

---

## Casting

Cast expressions use `(T)expr` for upcast and `match expr { T{} _ {}}` for downcast. For class types, the rules are:

- **Upcast** (toward base): always succeeds; result type is `T` (non-optional).
- **Downcast** (toward derived): checked at runtime against the vtable; must be used in a match expression.

```
import std;

class Animal {
    name: Str;
    fn __init__(name: Str) { self.name = name; }
    fn speak() { std::write(std::out(), "..."); }
}

class Dog extends Animal {
    breed: Str;
    fn __init__(name: Str, breed: Str) {
        __super__(name);
        self.breed = breed;
    }
    fn speak() { std::write(std::out(), "woof"); }
}

a = (Animal)Dog("rex", "lab");   // upcast: Dog -> Animal, always succeeds, type is Animal
match a {                        // downcast
    Dog  { std::write(std::out(), a.breed); }    // safe — a is Dog inside this arm
    _    {}
}
```

---

## Semantic Checks (Classes)

| Error | Trigger |
|---|---|
| Missing field init in `__init__` | `__init__` returns with an uninitialised field on some path |
| Virtual call before fields initialised | Calling a virtual method on `self` inside `__init__` before all fields are set |
| `__super__(...)` not first | Derived `__init__` body does not begin with `__super__(...)` |
| `__super__(...)` outside `__init__` | `__super__(...)` called in any method other than `__init__` |
| Direct call to `__del__` | User code calls `obj.__del__()` explicitly |
| Override mismatch | Override signature incompatible with base |
| Multiple bases | More than one `extends` clause |
| Visibility violation | Access to `_name` from outside the hierarchy or same module free functions |
| `self` in parameter list | Writing `self` as an explicit method parameter (it is implicit) |

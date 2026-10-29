# Match

`match` takes a value and runs the first **arm** whose pattern fits it. It is PaykanLang's
structured branching construct: it tells enum variants apart, compares a number or a string
against literals, checks the runtime class of an object, and unwraps an optional. This
chapter introduces each of these modes, gives tasks in `tasks` a priority, and then gives the
precise rules.

What a pattern can be depends on the type of the value, the **subject**. The mode is chosen
by the static type of the subject; you do not select it explicitly:

| Subject type | Mode | Arms compare against |
|--------------|------|----------------------|
| A class type (`Obj`, a user class, `io::File`, …) | **type mode** | the **runtime class** of the object |
| An `enum` | **variant mode** | the enum's **variants** (and binds a variant's fields) |
| A builtin (`int`, `float`, `bool`, `char`, `Str`, a sized numeric type) | **value mode** | **literal** values |
| An optional `T?` | **optional mode** | **present** (`T` arm) vs **absent** (`None` arm) |

## Matching enum variants

On an enum, each arm names a variant, **without** the `Enum::` prefix. A `match` that names
every variant is **exhaustive**, which the compiler knows, so a function that returns from
each arm needs no `return` after the `match` (see [Enums](07-enums.md#exhaustiveness)). To
handle only some variants, end with the wildcard arm `_`, which matches anything:

```pkn
import ::io;

enum Day { Mon, Tue, Wed, Thu, Fri, Sat, Sun }

fn isWeekend(d: Day) -> bool {
  match d {
    Sat { return True; }
    Sun { return True; }
    _   { return False; }
  }
}

fn main() -> int {
  io::println(Str(isWeekend(Day::Sun)) + " " + Str(isWeekend(Day::Wed)));
  return 0;
}
```

Output:

```
True False
```

## Matching values

On an `int`, `float`, `char`, `bool`, `Str` or a sized numeric type (`u8`, …), the arms are
literals, compared by value (strings by content):

```pkn
import ::io;

fn command(word: Str) -> Str {
  match word {
    "add"  { return "adding"; }
    "done" { return "completing"; }
    "list" { return "listing"; }
    _      { return "unknown command '" + word + "'"; }
  }
}

fn main() -> int {
  io::println(command("add"));
  io::println(command("list"));
  io::println(command("frobnicate"));
  n = 2;
  match n * 3 {
    0 { io::println("zero"); }
    6 { io::println("six"); }
    _ { io::println("something else"); }
  }
  return 0;
}
```

Output:

```
adding
listing
unknown command 'frobnicate'
six
```

A negative number is a pattern too, `-` and the number written together or apart: `-1`,
`- 2.5`, and the smallest `int`, `-9223372036854775808` (which an expression writes
`-9223372036854775807 - 1`, [Basics](02-basics.md#literals)):

```pkn
import ::io;

fn sign(n: int) -> Str {
  match n {
    -1 { return "minus one"; }
    0 { return "zero"; }
    -9223372036854775808 { return "the smallest int"; }
    _ { return "something else"; }
  }
}

fn main() -> int {
  io::println(sign(-1) + ", " + sign(-9223372036854775807 - 1) + ", " + sign(5));
  return 0;
}
```

Output:

```
minus one, the smallest int, something else
```

An arm is any literal of the subject's type, a hexadecimal or binary one too, and on an
`unsigned` the arms reach its whole range, up to `0xFFFF_FFFF_FFFF_FFFF`
([Basics](02-basics.md#literals)). An arm whose literal does not fit the subject's type is
the error an assignment of it is: `0xFF { ... }` on an `i8` is `the literal 255 does not fit
in 'i8' (-128 to 127)`.

```pkn
import ::io;

fn bits(u: unsigned) -> Str {
  match u {
    0 { return "none"; }
    0b1 { return "the lowest"; }
    0x8000_0000_0000_0000 { return "the highest"; }
    0xFFFF_FFFF_FFFF_FFFF { return "all of them"; }
    _ { return "some"; }
  }
}

fn main() -> int {
  io::println(bits(1) + ", " + bits(0x8000_0000_0000_0000) + ", " + bits(unsigned(-1)) + ", " + bits(6));
  return 0;
}
```

Output:

```
the lowest, the highest, all of them, some
```

A `bool` match with both `True` and `False` arms is exhaustive. On numbers and strings
there are always values you did not list, so a `match` that must produce a result needs a
`_` arm.

## Matching classes

On an object, the arms name classes and match the object's exact runtime class, as shown
in [Classes](05-classes.md#checking-the-class-at-run-time). An arm can bind the object to a
name with the narrower type: `c: Circle { ... }`.

## Matching optionals

On an optional value (`int?`, `Str?`, `Task?`) the arms tell a present value from `None`.
This is how a parsed number is unwrapped
([Strings and conversions](04-strings-and-conversions.md)); [Optionals](12-optionals.md)
covers it, and [Optional Mode](#optional-mode--unwrapping-a-t) below gives the rules.

## Project: task priorities

Each task gets a priority. The enum has three variants; `priorityName` turns one into text,
and `parsePriority` reads the words a user would type. Since an unknown word must still
give a priority, it falls back to `Normal`:

```pkn
import ::io;

enum Priority { Low, Normal, High }

fn priorityName(p: Priority) -> Str {
  match p {
    Low    { return "low"; }
    Normal { return "normal"; }
    High   { return "high"; }
  }
}

fn parsePriority(word: Str) -> Priority {
  match word {
    "low"  { return Priority::Low; }
    "high" { return Priority::High; }
    "!"    { return Priority::High; }
    _      { return Priority::Normal; }
  }
}

class Task {
  title: Str;
  done: bool;
  priority: Priority;

  fn complete() { self.done = True; }

  view fn toString() -> Str {
    box = if self.done then "[x]" else "[ ]";
    marker = if self.priority == Priority::High then " !" else "";
    return box + " " + self.title + marker;
  }
}

impl Task {
  fn __init__(title: Str, priority: Priority) {
    self.title = title;
    self.done = False;
    self.priority = priority;
  }
}

fn main() -> int {
  a = Task("water the plants", parsePriority("low"));
  b = Task("fix the leak", parsePriority("!"));
  c = Task("read a book", parsePriority("whenever"));
  b.complete();
  io::println(a);
  io::println(b);
  io::println(c.title + " has priority " + priorityName(c.priority));
  return 0;
}
```

Output:

```
[ ] water the plants
[x] fix the leak !
read a book has priority normal
```

The rest of this chapter gives the precise rules.

---

## Quick Summary

- Syntax: `match <expr> { arm … }`. Each arm is `pattern { statements }`.
- **`match` is a statement, not an expression**: it does not produce a value. Arms act by
  running statements (assigning, calling, `return`ing).
- Arm patterns:
  - `TypeName { … }`: type/variant pattern, no binding.
  - `name: TypeName { … }`: type pattern that **binds** the narrowed object to `name`; on
    an enum, `name: Variant { … }` binds the variant's fields.
  - `name: view TypeName { … }` / `name: inout TypeName { … }`: the binding borrows the
    subject, read-only or as its storage ([Borrowing the subject](#borrowing-the-subject)).
  - `literal { … }`: value pattern (`int`, `float`, `bool`, `char`, or `Str` literal, or
    a negative number, `-1 { … }`; a numeric literal takes a sized subject's type, `match
    b { 255 { … } }` with `b: u8`, and one that does not fit it is an error, `-1` on an
    `unsigned` as when assigned).
  - `_ { … }`: wildcard catch-all.
- The first matching arm wins, and only that one runs; there is no fallthrough.
- `_` must be the **last** arm. Any arm after it is unreachable and rejected.
- Each arm body is its own scope and is fully type-checked.
- If no arm matches, nothing happens and execution continues after the `match`. A `match`
  may be **exhaustive** (covers every case) and then needs no `_`; otherwise a `_` arm is
  required wherever every path must produce a result.
- A tuple, an array, a struct or a function value cannot be a subject (`match subject must
  be a class type, got '(int, int)'`; `match on a struct ('S') is not supported yet: a
  struct is a value, not an object`).

---

## Type Mode — matching on the runtime class

When the subject is a class-typed value, `match` dispatches on the object's **runtime class**
via its vtable identity. This is how you test and downcast a value held under a more general
static type such as `Obj` or a base class.

```pkn
a: Animal = Dog();
match a {
  Dog  { io::println("a dog"); }
  Cat  { io::println("a cat"); }
  Bird { io::println("a bird"); }
  _    { io::println("some other animal"); }
}
```

### Exact-type comparison

A type arm matches **only** when the object's runtime class is *exactly* that class; a
subclass does **not** match a base-class arm:

```pkn
class Animal {}
impl Animal { fn __init__() {} }
class Dog : Animal {}
impl Dog { fn __init__() { __super__(); } }
class Labrador : Dog {}
impl Labrador { fn __init__() { __super__(); } }

e: Animal = Labrador();
match e {
  Dog      { io::println("Dog"); }       // NOT taken — Labrador is not exactly Dog
  Labrador { io::println("Labrador"); }  // taken
  _        { io::println("other"); }
}
```

Because matching is exact, ordering type arms from most- to least-derived is not required for
correctness; list them in whatever order reads best, and use `_` for everything you do not name.

### Valid arm types

In type mode, every arm type must be the subject's type or a subtype of it. Naming an unrelated
class is an error, as is naming a type that does not exist:

```pkn
match a {          // a: Animal
  Car { }          // error: type 'Car' is not a subclass of 'Animal'
}

match x {          // x: Obj
  Dragon { }       // error: match arm has unknown class type 'Dragon'
}
```

A subject typed as `Obj` accepts any class as an arm, since every class is a subtype of `Obj`.

An arm naming an **abstract class** (one with a pure virtual method it does not implement,
[Classes](05-classes.md#pure-virtual-methods)) is an error too: the arm tests for exactly
that class, and an object is of an abstract class only while it is being constructed
([Every field before `self`](05-classes.md#every-field-before-self)), so the arm could
only run in that moment. Name the concrete subclasses instead, or call a virtual method:

```pkn
class Shape { view fn area() -> float; }
class Square : Shape {
  side: float;
  view fn area() -> float { return self.side * self.side; }
}

fn describe(o: Obj) -> Str {
  match o {
    Shape  { return "a shape"; }    // error: names an abstract class
    Square { return "a square"; }
    _      { return "something else"; }
  }
}

fn main() -> int {
  return 0;
}
```

Error:

```
error: a 'match' arm cannot name abstract class 'Shape': an object is of it only while it is being constructed
```

---

## Bindings

A type arm may bind the narrowed object to a name with `name: TypeName`. Inside that arm body the
name has the arm's type, so you can access fields and call methods directly:

```pkn
x: Obj = Dog("Rex");
match x {
  d: Dog {
    io::println(d.name);          // d has type Dog here
    io::println(d.bark());
  }
  _ { }
}
```

A binding is an ordinary reference to the matched object: it can be passed to functions,
stored into fields or array slots, and assigned to variables that outlive the `match`; the
object's lifetime is managed by ARC like any other reference (see [Memory](16-memory.md)).

The binding is scoped to its arm body. Redeclaring the binding name inside the same body is an
error:

```pkn
match x {
  d: Dog {
    d: int = 1;   // error: redeclaration of variable 'd'
  }
}
```

Bindings are available on type arms (including the `T` arm of an optional) and on the arms
of an enum's variants with fields, which bind the fields
([Variant Mode](#variant-mode--matching-on-an-enum)): value-pattern arms, the arms of a
variant without fields and `_` do not bind.

### Borrowing the subject

A binding may borrow the subject instead, written with a mode where the type goes, as for a
[local borrow](02-basics.md#local-borrows): `name: view TypeName { … }` or
`name: inout TypeName { … }`.

With `view`, the binding is a `view` local of the subject, whatever the subject is:

- nothing changes through it: no field or element write, only `view fn` calls, no `inout`
  argument (`'d' is a 'view' local; 'tick' is not a 'view fn'`);
- it can only be passed on to a `view` parameter, and if it shares what it holds (an
  object or a string) it is never stored or returned;
- it cannot be assigned (`cannot assign to 'view' local 'd'`);
- while it is live, from the start of the arm to its last use there, the variable the
  subject starts from cannot change (`'a' is viewed by 'view' local 'd' until 'd' is last
  used; 'tick' is not a 'view fn'`).

```pkn
fn describe(a: Animal) -> Str {
  match a {
    d: view Dog { return "a dog, " + d.name; }
    _ { return "an animal"; }
  }
}
```

With `inout`, the binding stands for the subject, narrowed to the arm's type, like an
`inout` local of it: assigning to it, or to its fields, changes the matched variable or
field. The binding holds the matched value (a copy, as a plain binding does), which goes
back into the subject after the binding's last use in the arm, or when the arm ends before
then, whichever way it ends: `return`, `break` or `continue`.

```pkn
import ::io;

class Animal { n: int; }
impl Animal { fn __init__(n: int) { self.n = n; } }
class Dog : Animal {}
impl Dog { fn __init__(n: int) { __super__(n); } }

fn main() -> int {
  a: Animal = Dog(1);
  match a {
    d: inout Dog { d = Dog(d.n + 1); }   // a now holds the new Dog
    _ { }
  }
  io::println(Str(a.n));
  return 0;
}
```

Output:

```
2
```

- The subject must be a place an `inout` argument could be: a variable or a field, not an
  expression (`'inout' arm 'd' needs a variable or a field to match on`), `self`, a `let`
  local or a `view` (`'v' is a 'view' parameter; it cannot be bound by 'inout' arm 'd'`).
- The subject's storage is found once, when the `match` starts, and the binding's value is
  read there: a call on the way to it (`b.slot().a`, with `slot` returning `inout`, or
  `mk().a`) runs once, and the binding goes back to that storage, wherever another call
  would lead.
- What is assigned to the binding has its type, `Dog`, which the subject's storage can
  hold: `d = Animal(1)` is an error.
- While the binding is live, from the start of the arm to its last use there, the variable
  the subject starts from cannot be used except through it (`'a' is borrowed by 'inout'
  local 'd' until 'd' is last used`).
- Another reference to the subject's object can still reach it: `g` after `g = h` in an arm
  of `match h.s`, a parameter that is the same object as `self`, or a variable `h` passed
  next to `h.s` to a function whose `inout` parameter is matched. What it writes there
  before the binding's last use does not change the binding, which keeps the arm's type
  (the variant, the present value, the subclass), and the binding's value replaces it at
  that last use; what it writes after stays, as does an assignment of the subject's
  variable then. An object the subject of the `match` is reached through (`o.h` in `match
  o.h.p.s`, with `p` a struct) is kept until then: if another reference replaces it
  (`g.h = H(...)` with `g = o`), the binding's value goes back to that object, which
  nothing else reaches any more.
- The `match` keeps the object it matched until it ends, so the arm can replace it.
- A tuple or a struct inside an optional (`p: inout Point` on a `Point?`) is the present
  value, put back as one, so a write through the binding changes the subject. A
  primitive or an enum inside one (`n: inout int` on an `int?`, `k: inout Suit` on a
  `Suit?`) cannot be changed this way yet (`'inout' arm 'n' cannot change the 'int' inside
  an optional yet`).

The mode goes after the colon: `view d: Dog { }` is a syntax error that shows the fix,
`d: view Dog`. A binding of a `view` subject is a `view` even without the mode
([A `view` stays a `view`](03-functions.md#a-view-of-a-reference-stays-a-view)).

---

## Variant Mode — matching on an enum

When the subject is an `enum`, each arm names a **bare variant** (not `Enum::Variant`):

```pkn
fn dirName(d: Direction) -> Str {
  match d {
    North { return "North"; }
    East  { return "East"; }
    South { return "South"; }
    West  { return "West"; }
  }
}
```

An enum `match` that names every variant is **exhaustive** and needs no `_`. Naming an identifier
that is not a variant of the subject enum is an error, and so is a qualified arm
(`base::Color::Green`). See [Using `enum` with `match`](07-enums.md#using-enum-with-match)
for the full enum treatment.

On a tagged union, an arm `name: Variant { … }` binds the fields of a variant that has them
([Variants With Fields](07-enums.md#variants-with-fields)): a copy, or with `view` or `inout`
a borrow of the subject's own ([Borrowing the subject](#borrowing-the-subject)):

```pkn
fn area(s: Shape) -> float {
  match s {
    Empty { return 0.0; }
    c: Circle { return 3.0 * c.r * c.r; }
    r: Rect { return float(r.w * r.h); }
  }
}
```

---

## Value Mode — matching on a builtin value

When the subject is a builtin (`int`, `float`, `bool`, `char`, `Str`, or a sized numeric
type), arms are **literal patterns** and the subject is compared by value. The first equal
literal wins. The subject can be any expression of a builtin type: a variable, a call, a
temporary such as `a + b`, or even a literal (`match "s" { … }`).

```pkn
n: int = 2;
match n {
  1 { io::println("one"); }
  2 { io::println("two"); }     // taken
  3 { io::println("three"); }
  _ { io::println("many"); }
}
```

- **`int` / `float` / `char`** arms compare by value.
- **`Str`** arms compare by **content**, not reference identity:

  ```pkn
  greeting: Str = "World";
  match greeting {
    "Hello" { io::println("hi"); }
    "World" { io::println("hello world"); }   // taken — content match
    _       { io::println("other"); }
  }
  ```

- **`bool`** has only two values, so `True` / `False` arms are exhaustive without a `_`:

  ```pkn
  match flag {
    True  { io::println("yes"); }
    False { io::println("no"); }
  }
  ```

Each literal must have the same type as the subject, and a number must fit a sized one as
it must when assigned to it (`-1` on an `unsigned` is `the literal -1 does not fit in
'unsigned' (0 to 18446744073709551615)`). A literal of the wrong type, or a type-name arm in
a value-mode `match`, is an error:

```pkn
match n {            // n: int
  1     { }
  "two" { }          // error: match arm literal of type 'Str' does not match subject type 'int'
  _     { }
}

match x {            // x: int
  Animal { }         // error: match on a value of type 'int' requires literal patterns, not type names
  _ { }
}
```

---

## Optional Mode — unwrapping a `T?`

When the subject is an optional type `T?` (see [Optionals](12-optionals.md)), `match` is the
way to get at the wrapped value. The arm naming `T` itself matches **every present value**,
whatever its runtime subclass, and binds it as `T`; a `None` arm (or `_`) covers the absent
case:

```pkn
match find(head, key) {          // find returns Node?
  n: Node { io::println(Str(n.v)); }
  None    { io::println("not found"); }
}
```

Two differences from type mode:

- The `T` arm is **not** an exact-type test: a `Leaf` stored in a `Node?` is matched by
  `n: Node`. An arm naming a strict subclass keeps exact-type semantics and must come before
  the `T` arm (anything after the `T` arm is unreachable and rejected: `unreachable arm: the
  'Node' arm above already matches every non-None value`). So `T` may be an abstract class,
  `s: Shape` unwrapping a `Shape?`, while a strict subclass's arm may not (it is an exact
  test, as in type mode).
- A `T` arm alone is **not exhaustive**; `T` together with `None` (or `_`) is, and then the
  `match` satisfies the "always returns" and field-initialisation analyses.

A `None` arm is only valid on an optional subject; matching on an `Obj` is unchanged.

---

## The Wildcard `_`

`_` matches anything and is the catch-all. It must appear as the **last** arm; anything after
it can never run:

```pkn
match x {
  _   { }
  Dog { }   // error: unreachable arm: wildcard '_' must be the last arm
}
```

`_` is optional. When no arm matches the subject's value, the `match` simply runs no arm and
execution continues after it, so `match 3 { 1 { … } }`, a type match that leaves some classes
uncovered, or an enum match naming one variant all compile. A `_` (or full coverage: every enum
variant, or both `bool` values) is needed only where every path must produce a result, such as
a `match` that ends a non-`void` function by returning from each arm; without it the function
is rejected with "does not always return a value".

---

## Arm Bodies

An arm body is a normal block: it has its own scope, may declare locals, and is fully
type-checked. Errors inside an arm are reported like any other code:

```pkn
match x {
  d: Dog {
    sound: Str = d.bark();   // local to this arm
    io::println(sound);
  }
  Dog {
    y: int = 3.14;           // error: initializer of type 'float' does not match declared type 'int' for variable 'y'
  }
  _ { }
}
```

---

## Exhaustiveness and Control Flow

`match` participates in the same flow analysis as the rest of the language. When a `match` is the
last thing in a function whose every path must `return` a value, the analysis treats the `match`
as guaranteeing a result only if it is exhaustive: every arm returns *and* the arms cover every
case (via `_`, all enum variants, both `bool` values, or `T` and `None` on an optional). A
`match` with uncovered cases and no fallthrough is rejected:

```pkn
fn main() -> int {
  x: Obj = Dog();
  match x {
    Dog { return 0; }
    Cat { return 1; }
    // no '_' arm and no return after the match —
    // error: non-void function 'main' does not always return a value
  }
}
```

The same exhaustiveness rule lets `match` satisfy field-initialisation analysis inside `__init__`
(see [Fields](05-classes.md#fields)): a field assigned in every arm of an exhaustive `match`
counts as definitely assigned.

---

## Common Patterns

### Matching a `Result`

`io::open()` returns a `res::Result<io::File, err::Error>` (module `::result`, see
[Result](../stdlib/result.md)): a [tagged union](07-enums.md#generic-enums) that is `Ok`,
holding the file as its `value`, or `Err`, holding the `error` that says why the open
failed. Match it in variant mode, binding each variant's fields:

```pkn
match io::open("/tmp/data.txt", "r") {
  r: Ok {
    while True {
      match r.value.readln() {
        line: Ok { io::print(line.value); }
        _        { break; }      // Err — the end of the file
      }
    }
  }
  e: Err { io::printerrln("open failed: " + e.error.message()); }
}
```

### Unwrapping parsed primitives

`s.toInt()` / `s.toFloat()` return an `int?` / `float?`: `None` when `s` is not a valid
number. They are unwrapped in optional mode, and the arm binds a plain `int`:

```pkn
match s.toInt() {
  n: int { return "ok: " + Str<int>(n); }
  None   { return "not a number: " + s; }
}
```

### Detecting end of input

`readln()` returns a `res::Result<Str, err::Error>` that is `Err` at EOF (its error's code
is then `EndOfFile`); the `Err` is matched by falling through to `_` (or an `Err` arm):

```pkn
match f.readln() {
  line: Ok { process(line.value); }
  _        { break; }   // Err
}
```

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Subject is not matchable | The subject type supports neither type-, variant-, value- nor optional-mode matching: a tuple, an array, a struct or a function type |
| Arm type not a subtype | A type arm names a class that is not the subject type or a subtype of it |
| Arm naming an abstract class | A type arm names an abstract class (or, on a `T?`, a strict subclass of `T` that is abstract): `a 'match' arm cannot name abstract class 'Shape': an object is of it only while it is being constructed` |
| Unknown arm type | A type arm names a class that is not declared anywhere |
| Unknown enum variant | A variant arm names an identifier that is not a variant of the subject enum, or a type that is no name (`a: int { }`: `'int' is not a variant of enum 'E': an arm names one of its variants, such as 'A'`) |
| Binding a variant without fields | `x: Variant { … }` where the variant has no fields to bind |
| Qualified variant arm | A variant arm is written qualified (`base::Color::Green`); arms use the bare variant name (`Green`) |
| Literal type mismatch | A value arm's literal has a different type than the subject |
| Literal out of range | A value arm's number does not fit the subject's sized type (`-1` on an `unsigned`, `256` on a `u8`), as when assigned to it |
| Type name in value mode | A value-mode `match` has a type-name arm instead of a literal |
| Binding redeclaration | An arm body redeclares the arm's binding name |
| Binding named `self` | In a method's body, an arm's binding is named `self`, which names the method's object or receiver (`'self' cannot be used as a variable name in method 'm' of class 'C'; it is the implicit receiver`) |
| `view` binding changed or passed on | A `name: view T` binding is assigned, written through, passed to a parameter that is not `view`, or stored; or the subject's variable changes while it is live |
| `inout` binding of something else | A `name: inout T` arm's subject is not a variable or a field, is `self`, a `let` local, a `view` or a `weak` reference, or is an optional primitive or enum; or the subject's variable is used while the binding is live |
| Arm body type error | Any type error inside an arm body |
| Wildcard not last | A `_` arm is followed by another arm |
| Non-exhaustive match | A `match` leaves cases uncovered where every path must produce a value |
| `None` arm outside optional mode | A `None` literal arm on a non-optional subject |
| Unreachable arm after the `T` arm | A type arm follows the `T` arm of an optional-mode `match` |

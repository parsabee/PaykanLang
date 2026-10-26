# Enums

An **enum** is a type with a fixed set of named values, called **variants**: the days of the
week, a card suit, a traffic light, a parser state, the priority of a task. Enums model
closed choices, where every possibility is known at compile time. A variant can also carry
values of its own, its **fields** (`Rect(w: int, h: int)`), which makes the enum a **tagged
union**: a value is one of the variants and holds that variant's fields
([Variants With Fields](#variants-with-fields)), and an enum can take type parameters, so one
declaration serves every type of field ([Generic Enums](#generic-enums)). This chapter shows
how to declare and use an enum, then gives the rules; [Match](08-match.md) covers the statement that branches on enums,
and on much more.

## Declaring an enum

```pkn
import ::io;

enum Direction { North, East, South, West }

fn turnRight(d: Direction) -> Direction {
  match d {
    North { return Direction::East; }
    East  { return Direction::South; }
    South { return Direction::West; }
    West  { return Direction::North; }
  }
}

fn name(d: Direction) -> Str {
  match d {
    North { return "north"; }
    East  { return "east"; }
    South { return "south"; }
    West  { return "west"; }
  }
}

fn main() -> int {
  d = Direction::North;
  i = 0;
  route = name(d);
  while i < 4 {
    d = turnRight(d);
    route = route + " -> " + name(d);
    i = i + 1;
  }
  io::println(route);
  io::println(Str(d == Direction::North));
  return 0;
}
```

Output:

```
north -> east -> south -> west -> north
True
```

The variants are written `Enum::Variant` in expressions, and bare (`North`) in the arms of a
`match` on the enum. An enum is a type of its own: it is not a number and it has no
ordering. It prints as its variant's name (`io::println(d)` prints `North`, and
`d.toString()` is `"North"`); a function such as `name` above gives it other text. It is a
plain value like an `int`: copied on assignment, never allocated.

---

## Quick Summary

- Declared with `enum Name { Variant1, Variant2, … }`. A trailing comma after the last variant
  is allowed, which suits enums written one variant per line.
- Variants are referenced as `Name::Variant`.
- Each variant has an implicit ordinal value `0, 1, 2, …` assigned in declaration order.
- An enum is a **distinct nominal type**, represented as a 64-bit integer at runtime. It is
  **not** an `int` and does **not** convert to or from one, in either direction; there is no
  arithmetic on enums.
- The only operators are `==` and `!=`, and both sides must be the **same** enum type.
- An enum's one method is the builtin `toString()`: its variant's name, `"North"`, so it
  implements `Show` and prints with `io::println`; it implements `ValueType` too, but not
  `Hashable` ([Traits](06-traits.md)).
- Enums are **value types**: stack-allocated, copied on assignment, no reference counting.
- A variant may have fields, `Rect(w: int, h: int)`, made with `Shape::Rect(2, 3)` and bound
  by a `match` arm, `r: Rect { … r.w … }`: the enum is then a **tagged union**, a value held
  in place like a struct ([Variants With Fields](#variants-with-fields)).
- An enum can be generic, `enum Maybe<T> { Just(value: T), Nothing }`: each instantiation,
  `Maybe<int>`, is an enum of its own, and a variant's type arguments are written, expected
  or inferred, `Maybe::Just(3)` ([Generic Enums](#generic-enums)).
- An enum can be a variable, a function parameter or return type, a class or struct field, a
  tuple element or an array element type, and has an optional (`Direction?`, see
  [Optionals](12-optionals.md)).
- `match` is the idiomatic way to branch on an enum and can be **exhaustive** over the variants
  without a wildcard.

---

## Declaration

```pkn
enum Direction {
  North,
  East,
  South,
  West,        // trailing comma allowed
}

enum Suit { Hearts, Diamonds, Clubs, Spades }

enum Signal { Only }   // a single-variant enum is valid
```

- The keyword is `enum`, followed by an `UpperCamelCase` type name and a brace-delimited list of
  variant names.
- Multiple enums may be declared in the same file.
- Enums are top-level declarations, like functions and classes, and may be imported from other
  modules (see [Modules](15-modules.md)).
- An enum cannot have an `impl` block (`an 'impl' block for an enum is not supported yet`).
- A variant may have fields: `Circle(r: float)` ([Variants With Fields](#variants-with-fields)).

---

## Variants and Ordinal Values

A variant is named with the `::` qualifier: `Direction::North`. Variants are numbered from `0`
in declaration order:

```pkn
enum Direction { North, East, South, West }
// North = 0, East = 1, South = 2, West = 3
```

These ordinals are an implementation detail of the runtime representation: they define the
storage but are **not** exposed as integers. You cannot read a variant as an `int`, do
arithmetic on it, or build one from a number. The ordinal only matters in that it is fixed and
stable for a given declaration order.

```pkn
d: Direction = Direction::East;
```

---

## A Distinct Nominal Type

An enum value is never interchangeable with an `int`, even though both are integers underneath:

```pkn
enum Color { Red, Green }

x: int = Color::Red;     // error: initializer of type 'Color' does not match declared type 'int' for variable 'x'
```

Two different enums are also incompatible with each other, so a comparison across enum types is
rejected before it can produce a meaningless answer:

```pkn
enum A { X, Y }
enum B { P, Q }

if A::X == B::P { }    // error: operands of '==' have mismatched types 'A' and 'B'
```

Referencing a variant the enum does not declare is an error:

```pkn
c: Color = Color::Nope;  // error: enum 'Color' has no variant 'Nope'
```

---

## Equality

`==` and `!=` are the only operators defined on enums. Both operands must be values of the same
enum:

```pkn
d: Direction = Direction::East;

if d == Direction::East  { io::println("facing east"); }
if d != Direction::North { io::println("not north"); }
```

There is no ordering (`<`, `>`) on enums: even though variants have ordinals, the language does
not expose them for relational comparison.

---

## Enums Across Functions, Fields, and Arrays

An enum is an ordinary value type and flows anywhere a value can.

### As a parameter and return type

```pkn
fn opposite(d: Direction) -> Direction {
  match d {
    North { return Direction::South; }
    South { return Direction::North; }
    East  { return Direction::West; }
    West  { return Direction::East; }
  }
}
```

### As a class field

```pkn
class Player {
  facing: Direction;
  fn turnAround()  { self.facing = opposite(self.facing); }
  fn report() -> Str { return dirName(self.facing); }
}

impl Player {
  fn __init__(d: Direction) { self.facing = d; }
}
```

### In an array

```pkn
route: Direction[4] = [Direction::North, Direction::East, Direction::South, Direction::West];
route[0] = opposite(route[0]);        // North -> South

j: int = 0;
while j < route.len() {
  io::println(dirName(route[j]));
  j = j + 1;
}
```

---

## Using `enum` with `match`

`match` is the primary way to act on an enum. When the subject of a `match` is an enum, each arm
names a **bare variant**: not the fully-qualified `Enum::Variant`, just the variant name:

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

### Exhaustiveness

When every variant of the enum is covered, the `match` is **exhaustive** and no wildcard `_` arm
is needed, as in `dirName` above; that is also why `dirName` needs no `return` after its
`match`. This is the recommended style: if you later add a variant to the enum, the compiler
reports that each such function no longer always returns a value, pointing you at every place
that needs updating.

When you only care about some variants, add a wildcard `_` as the final arm to catch the rest:

```pkn
fn isRed(s: Suit) -> bool {
  match s {
    Hearts   { return True; }
    Diamonds { return True; }
    _        { return False; }
  }
}
```

Naming an identifier that is not a variant of the subject enum is an error:

```pkn
match c {            // c: Color, where enum Color { Red, Green }
  Red   { }
  Green { }
  Blue  { }          // error: 'Blue' is not a variant of enum 'Color'
  _     { }
}
```

Arms always use the bare variant name, also for an imported enum. A qualified arm such as
`base::Color::Green { }` is an error: `'base::Color::Green' is not a valid match arm; use the
bare variant name 'Green'`.

### A state machine

Because an enum can be returned from a function and reassigned, a transition function plus a loop
gives a compact state machine:

```pkn
enum Light { Red, Yellow, Green }

fn nextLight(l: Light) -> Light {
  match l {
    Red    { return Light::Green; }
    Green  { return Light::Yellow; }
    Yellow { return Light::Red; }
  }
}

l: Light = Light::Red;
i: int = 0;
while i < 4 {
  io::println(lightName(l));   // Red, Green, Yellow, Red
  l = nextLight(l);
  i = i + 1;
}
```

For the full mechanics of `match`, including value-mode matching on builtin types, bindings,
and the complete set of rules, see [Match](08-match.md).

---

## Variants With Fields

A variant can carry values, its **fields**, written after its name as a function's parameters
are. An enum with such variants is a **tagged union**: each value is one of the variants and
holds that variant's fields, and a `match` tells the variants apart and reads the fields:

```pkn
import ::io;

enum Shape {
  Empty,
  Circle(r: float),
  Rect(w: int, h: int),
  Label(text: Str)
}

fn area(s: Shape) -> float {
  match s {
    Empty { return 0.0; }
    c: Circle { return 3.0 * c.r * c.r; }
    r: Rect { return float(r.w * r.h); }
    _ { return -1.0; }
  }
}

fn main() -> int {
  shapes = [Shape::Circle(1.5), Shape::Rect(2, 3), Shape::Label("hi"), Shape::Empty];
  for i in 0..4 {
    io::println(shapes[i].toString() + " has area " + Str(area(shapes[i])));
  }
  io::println(Str(shapes[1] == Shape::Rect(2, 3)) + " " + Str(shapes[1] == shapes[3]));
  return 0;
}
```

Output:

```
Circle(r: 1.5) has area 6.75
Rect(w: 2, h: 3) has area 6
Label(text: hi) has area -1
Empty has area 0
True False
```

### Making a value

`Enum::Variant(a, b)` makes a variant with fields: one argument per field, in order, each
assignable to its field's type as a constructor's arguments are (`Shape::Circle(2)` promotes
the `int`, a `T` becomes a `T?` field's present value). A variant without fields is still
`Enum::Variant`, without parentheses, also in a tagged union (`Shape::Empty`). The compiler
says which form a variant needs:

```pkn
s = Shape::Rect;          // error: variant 'Rect' of enum 'Shape' has fields: write 'Shape::Rect(w, h)'
t = Shape::Empty(1);      // error: variant 'Empty' of enum 'Shape' has no fields: write 'Shape::Empty' without arguments
u = Shape::Rect(1);       // error: variant 'Rect' of enum 'Shape' has 2 field(s) (w: int, h: int), got 1 argument(s)
```

### Binding the fields

An arm `name: Variant { … }` runs when the value is that variant and binds `name` to its
fields, read as a struct's are (`r.w`). The binding is a copy, as a by-value parameter is;
with `view` (`r: view Rect`) it is read-only, and with `inout` (`r: inout Rect`) it stands
for the subject, which the arm sets to the variant it holds after its last use, so writing
through it changes the subject:

```pkn
import ::io;

enum Account {
  Closed,
  Open(owner: Str, balance: int)
}

fn deposit(a: inout Account, amount: int) {
  match a {
    o: inout Open { o.balance += amount; }
    Closed { io::println("closed: nothing deposited"); }
  }
}

fn describe(o: Account::Open) -> Str {
  return o.owner + " has " + Str(o.balance);
}

fn main() -> int {
  a = Account::Open("ada", 10);
  deposit(a, 5);
  match a {
    o: Open { io::println(describe(o)); }
    _ { }
  }
  c = Account::Closed;
  deposit(c, 5);
  io::println(a);
  return 0;
}
```

Output:

```
ada has 15
closed: nothing deposited
Open(owner: ada, balance: 15)
```

The fields of a variant are a struct of their own named `Enum::Variant`, so a function can take
them (`describe(o: Account::Open)` above) and a binding can be passed to it. Only a `match`
makes one: `Account::Open("ada", 10)` is an `Account`, not an `Account::Open`. A variant
without fields has nothing to bind (`variant 'Closed' of enum 'Account' has no fields to
bind: write the arm 'Closed { ... }'`), and the rules of a `view` or an `inout` binding are
the ones every `match` arm has ([Borrowing the subject](08-match.md#borrowing-the-subject)).

### Text and equality

`toString()`, and so `io::println`, gives the variant's name followed by its fields as a struct
shows them, `Rect(w: 2, h: 3)`, or the bare name for a variant without fields, `Empty`. An
enum with a variant field that does not implement `Show`, a function value, does not either,
whatever its value's variant: `io::println(e)` and `e.toString()` are errors naming the field
(`its field 'h' of variant 'F' has type '(int) -> int', which does not implement it`). `==`
and `!=` compare two values of the same tagged union: equal when they are the same variant
with equal fields (compared as a struct's are: a `Str` by its text, an object by its
`equals`). As for any enum, there is no ordering.

### Held in place

A tagged union is a value held in place, like a struct ([Structs](11-structs.md)): its
**tag**, the variant's index, followed by each variant's fields one after another, only the
current variant's set. So a value is as large as the tag and all its variants' fields
together, assigning one copies those bytes, and the references among the fields (a `Str`,
an object) are shared, as a struct's are. A tagged union can be a variable, a parameter or a
result, a field of a class or a struct, a tuple or array element, the value of an optional
(`Shape?`) and the element of a container (`List<Shape>`), and a lambda can capture one.

Held in place, a variant cannot hold its own enum, directly or through a struct or a tuple
(`enum 'Expr' contains itself: variant 'Add' holds an 'Expr'; an enum is held in place, so
its variants cannot hold it (hold it in a class instead)`). A class is a reference, so
recursive data goes through one:

```pkn
import ::io;

enum Expr {
  Num(v: int),
  Add(args: Pair),
  Mul(args: Pair)
}

class Pair {
  l: Expr;
  r: Expr;
}

fn eval(e: Expr) -> int {
  match e {
    n: Num { return n.v; }
    a: Add { return eval(a.args.l) + eval(a.args.r); }
    m: Mul { return eval(m.args.l) * eval(m.args.r); }
  }
}

fn main() -> int {
  // (2 + 3) * 4
  sum = Expr::Add(Pair(Expr::Num(2), Expr::Num(3)));
  e = Expr::Mul(Pair(sum, Expr::Num(4)));
  io::println(eval(e));
  return 0;
}
```

Output:

```
20
```

### Fields and their limits

- A field is a name and a type, `x: int`: no mode, no default value, and the names of one
  variant's fields are distinct (`duplicate field 'x' in variant 'B' of enum 'E'`). A variant
  with fields has at least one: `B()` is written `B`.
- A field's type is any type a struct field can have but a fixed-size array (`a variant's
  field cannot be a fixed-size array yet`); a generic struct is fine
  (`Buf(buffer: mem::Buffer<int>)`).
- A `view` is never stored in a field: `E::B(v)` with a `view` parameter `v` is an error, as
  storing it in a struct is.
- A tagged union does not cross into C ([Native functions](03-functions.md#native-functions)),
  and is not `Hashable` (so it is not a `HashMap` key).
- An imported tagged union is used the same way: `geo::Shape::Rect(1, 2)` makes one and the
  arms are bare (`r: Rect`); its fields' struct is `geo::Shape::Rect`.

## Generic Enums

An enum can take **type parameters**, written after its name as a generic class's are
([Generics](13-generics.md)), and its variants' fields name them. One declaration then serves
every type of value: an optional result, the outcome of something that can fail.

```pkn
import ::io;

enum Maybe<T> {
  Just(value: T),
  Nothing
}

enum Result<T, E> {
  Ok(value: T),
  Err(error: E)
}

fn divide(a: int, b: int) -> Result<int, Str> {
  if (b == 0) {
    return Result::Err("divide by zero");
  }
  return Result::Ok(a / b);
}

fn orElse<T>(m: Maybe<T>, d: T) -> T {
  match m {
    j: Just { return j.value; }
    Nothing { return d; }
  }
}

fn main() -> int {
  a = Maybe::Just(3);             // a Maybe<int>, from the field
  b: Maybe<int> = Maybe::Nothing; // from the variable's type
  c = Maybe<Str>::Nothing;        // written
  io::println(a.toString() + " " + b.toString() + " " + c.toString());
  io::println(orElse(a, 0) + orElse(b, 10));
  match divide(7, 0) {
    q: Ok { io::println(q.value); }
    e: Err { io::println(e.error); }
  }
  return 0;
}
```

Output:

```
Just(value: 3) Nothing Nothing
13
divide by zero
```

Each use with type arguments, `Maybe<int>`, is an **instantiation**: an enum of its own, with
its own variants' fields (the struct `Maybe<int>::Just`), made where the program first names
it, as a generic class's is. `Maybe<int>` and `Maybe<Str>` are different types.

A variant written without its type arguments gets them, in this order:

1. **Written**: `Maybe<int>::Nothing`, `Result<int, Str>::Ok(1)`.
2. **From the type the value flows into**, when that is an instantiation of the same enum: a
   variable's declared type, a parameter's, a result's, a field's, an array element's, the
   other side of `==` or `!=`, or the other arm of a conditional expression (`if n > 0 then
   Maybe::Just(n) else Maybe::Nothing`). The other side or arm may be on either side
   (`Maybe::Nothing == m`, `if n > 0 then Maybe::Nothing else Maybe::Just(n)`): the one that
   needs the other's type less is typed first. It comes before inference, so `m: Maybe<u8> =
   Maybe::Just(200)` holds a `u8`.
3. **Inferred from the variant's fields**, as a generic class's constructor infers them:
   `Maybe::Just(3)` is a `Maybe<int>`, and `Maybe::Just(x)` in a generic function a `Maybe<T>`.

A variant without fields has nothing to infer from, so it needs one of the first two: `x =
Maybe::Nothing;` alone is an error (`cannot infer the type arguments of generic enum 'Maybe'
for 'Maybe::Nothing': write them ('Maybe<...>::Nothing'), or give the value a type ('x:
Maybe<...> = Maybe::Nothing')`). So is a variant whose fields leave a type parameter open:
`Result::Ok(1)` says nothing about `E`.

Everything else is as for any enum. The arms of a `match` are bare variant names and bind an
instantiation's fields with their types (`j.value` is an `int` in a `Maybe<int>`); a generic
function's parameter `Maybe<T>` infers `T` from the instantiation passed to it. Each
instantiation is shown, compared and held in place as [Variants With
Fields](#variants-with-fields) describes. So `enum List<T> { Nil, Cons(head: T, tail: List<T>)
}` is an error at its first instantiation (`enum 'List<int>' contains itself: variant 'Cons'
holds it in field 'tail'`), and a recursive one goes through a class, as `Expr` does above.

- A type parameter may have bounds (`enum Sorted<T: Ord>`), checked at each instantiation
  ([Traits](06-traits.md)).
- An imported generic enum is spelled with its module's qualifier: `opt::Maybe<int>`,
  `opt::Maybe::Just(1)`. Type names are global across imports, so `Maybe<int>` is one enum
  whether the module, its importer or another module made it
  ([Generics: Modules](13-generics.md#modules)).
- The standard library's `Result<T, E>` is one: the value an operation made or the error
  saying why it could not ([`::result`](../stdlib/result.md)).
- A generic enum has no `impl` block yet.

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Enum name taken | The enum is named like a builtin (`enum Str {…}`, `enum int {…}`), a class, a function, or another enum — top-level names are shared (see [Builtin Names Are Reserved](02-basics.md#builtin-names-are-reserved)) |
| Unknown variant | `Enum::Name` where `Name` is not a declared variant |
| Enum / `int` mismatch | Using an enum value where an `int` is required (or vice versa) |
| Cross-enum comparison | `==` / `!=` between two different enum types |
| Unknown variant in `match` arm | A bare-identifier arm that is not a variant of the subject enum |
| Non-exhaustive `match` | Enum `match` that covers neither all variants nor a wildcard, on a path that must yield a value |
| Method on an enum | A method call on an enum value other than `toString()` (`method call on non-class type 'Direction'`); `impl` blocks for enums are not supported yet |
| Variant made the wrong way | A variant with fields written without arguments, a variant without fields called, or the wrong number or types of arguments (`field 'w' of 'Shape::Rect' has type 'int', got 'Str'`) |
| Binding a variant without fields | `x: Empty { … }` where the variant `Empty` has no fields |
| Unknown field | `r.z` where the variant's fields have no `z` (`no field 'z' in variant 'Rect' of enum 'Shape'`) |
| Enum holds itself | A variant's fields hold the enum, directly or through a struct or a tuple |
| Bad field | Two fields of one variant share a name, a field is a fixed-size array, or a `view` is stored in one |
| Generic enum without type arguments | `m: Maybe = …` for a generic `Maybe` (`write 'Maybe<...>'`) |
| Type arguments cannot be inferred | A variant of a generic enum without written type arguments, an expected type of the same enum, or fields that determine them (`x = Maybe::Nothing;`, `Result::Ok(1)`) |
| Wrong type arguments | `Maybe<int, int>` for a one-parameter enum, an argument that does not satisfy a bound, or `Color<int>` for an enum that is not generic |

# Enums

An **enum** is a type with a fixed set of named values, called **variants**: the days of the
week, a card suit, a traffic light, a parser state, the priority of a task. Enums model
closed choices, where every possibility is known at compile time. This chapter shows how to
declare and use one, then gives the rules; [Match](08-match.md) covers the statement that
branches on enums, and on much more.

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
`match` on the enum. An enum is a type of its own: it is not a number, it has no ordering,
and it cannot be printed directly (`io::println(d)` is an error: an enum is not `Show`), so
a function such as `name` above turns it into a string. It is a plain value like an `int`:
copied on assignment, never allocated.

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
- An enum has no methods (`d.toString()` is an error) and implements neither `Show` nor
  `Hashable`; it does implement `ValueType` ([Traits](06-traits.md)).
- Enums are **value types**: stack-allocated, copied on assignment, no reference counting.
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

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Enum name taken | The enum is named like a builtin (`enum Str {…}`, `enum int {…}`), a class, a function, or another enum — top-level names are shared (see [Builtin Names Are Reserved](02-basics.md#builtin-names-are-reserved)) |
| Unknown variant | `Enum::Name` where `Name` is not a declared variant |
| Enum / `int` mismatch | Using an enum value where an `int` is required (or vice versa) |
| Cross-enum comparison | `==` / `!=` between two different enum types |
| Unknown variant in `match` arm | A bare-identifier arm that is not a variant of the subject enum |
| Non-exhaustive `match` | Enum `match` that covers neither all variants nor a wildcard, on a path that must yield a value |
| Method on an enum | `d.toString()` or any method call on an enum value (`method call on non-class type 'Direction'`); `impl` blocks for enums are not supported yet |

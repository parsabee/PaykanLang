# PaykanLang — Match Statements

`match` is PaykanLang's structured branching construct. It inspects a single subject and runs the
first arm that applies. Depending on the subject's type, `match` operates in one of four modes:

| Subject type | Mode | Arms compare against |
|--------------|------|----------------------|
| A class type (`Obj`, `File`, a user class, …) | **type mode** | the **runtime class** of the object |
| An `enum` | **variant mode** | the enum's **variants** |
| A builtin (`int`, `float`, `bool`, `char`, `Str`) | **value mode** | **literal** values |
| An optional `T?` | **optional mode** | **present** (`T` arm) vs **absent** (`None` arm) |

The mode is chosen by the static type of the subject — you do not select it explicitly.

---

## Quick Summary

- Syntax: `match <expr> { arm … }`. Each arm is `pattern { statements }`.
- **`match` is a statement, not an expression** — it does not produce a value. Arms act by
  running statements (assigning, calling, `return`ing).
- Arm patterns:
  - `TypeName { … }` — type/variant pattern, no binding.
  - `name: TypeName { … }` — type pattern that **binds** the narrowed object to `name`.
  - `name: view TypeName { … }` / `name: inout TypeName { … }` — the binding borrows the
    subject, read-only or as its storage ([Borrowing the subject](#borrowing-the-subject)).
  - `literal { … }` — value pattern (`int`, `float`, `bool`, `char`, or `Str` literal).
  - `_ { … }` — wildcard catch-all.
- The first matching arm wins; remaining arms are not considered.
- `_` must be the **last** arm. Any arm after it is unreachable and rejected.
- Each arm body is its own scope and is fully type-checked.
- A `match` may be **exhaustive** (covers every case) and then needs no `_`; otherwise a `_` arm
  is required wherever every path must produce a result.

---

## Type Mode — matching on the runtime class

When the subject is a class-typed value, `match` dispatches on the object's **runtime class**
via its vtable identity. This is how you test and downcast a value held under a more general
static type such as `Obj` or a base class.

```pkn
a: Animal = Dog();
match a {
  Dog  { println("a dog"); }
  Cat  { println("a cat"); }
  Bird { println("a bird"); }
  _    { println("some other animal"); }
}
```

### Exact-type comparison

A type arm matches **only** when the object's runtime class is *exactly* that class — a subclass
does **not** match a base-class arm:

```pkn
class Animal { fn __init__() {} }
class Dog : Animal { fn __init__() { __super__(); } }
class Labrador : Dog { fn __init__() { __super__(); } }

e: Animal = Labrador();
match e {
  Dog      { println("Dog"); }       // NOT taken — Labrador is not exactly Dog
  Labrador { println("Labrador"); }  // taken
  _        { println("other"); }
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

---

## Bindings

A type arm may bind the narrowed object to a name with `name: TypeName`. Inside that arm body the
name has the arm's type, so you can access fields and call methods directly:

```pkn
x: Obj = Dog("Rex");
match x {
  d: Dog {
    println(d.name);          // d has type Dog here
    println(d.bark());
  }
  _ { }
}
```

A binding is an ordinary reference to the matched object: it can be passed to functions,
stored into fields or array slots, and assigned to variables that outlive the `match` — the
object's lifetime is managed by ARC like any other reference (see `08-memory-model.md`).

The binding is scoped to its arm body. Redeclaring the binding name inside the same body is an
error:

```pkn
match x {
  d: Dog {
    d: int = 1;   // error: redeclaration of 'd'
  }
}
```

Bindings are available only on type arms — value-pattern arms, enum-variant arms and `_` do
not bind.

### Borrowing the subject

A binding may borrow the subject instead, written with a mode where the type goes, as for a
[local borrow](01-language-basics.md#local-borrows): `name: view TypeName { … }` or
`name: inout TypeName { … }`.

With `view`, the binding is a `view` local of the subject, whatever the subject is:

- nothing changes through it: no field or element write, only `view fn` calls, no `inout`
  argument (`'d' is a 'view' local; 'tick' is not a 'view fn'`);
- it can only be passed on to a `view` parameter, and if it shares what it holds (an
  object, string or array) it is never stored or returned;
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

With `inout`, the binding is the subject's storage, like an `inout` local of it: assigning
to it writes the matched variable or field.

```pkn
class Animal { n: int; fn __init__(n: int) { self.n = n; } }
class Dog : Animal { fn __init__(n: int) { __super__(n); } }

fn main() -> int {
  a: Animal = Dog(1);
  match a {
    d: inout Dog { d = Dog(d.n + 1); }   // a now holds the new Dog
    _ { }
  }
  println(Str(a.n));
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
- What is assigned to the binding has its type, `Dog`, which the subject's storage can
  hold: `d = Animal(1)` is an error.
- While the binding is live, from the start of the arm to its last use there, the variable
  the subject starts from cannot be used except through it (`'a' is borrowed by 'inout'
  local 'd' until 'd' is last used`).
- The `match` keeps the object it matched until it ends, so the arm can replace it.
- A primitive inside an optional (`n: inout int` on an `int?`) cannot be changed this way
  yet.

The mode goes after the colon: `view d: Dog { }` is a syntax error that shows the fix,
`d: view Dog`. A binding of a `view` subject is a `view` even without the mode
([A `view` stays a `view`](02-functions-and-calling.md#a-view-stays-a-view)).

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
that is not a variant of the subject enum is an error. See `03-enums.md` for the full enum
treatment.

---

## Value Mode — matching on a builtin value

When the subject is a builtin (`int`, `float`, `bool`, `char`, or `Str`), arms are **literal
patterns** and the subject is compared by value. The first equal literal wins. The subject
can be any expression of a builtin type: a variable, a call, a temporary such as `a + b`, or even
a literal (`match "s" { … }`).

```pkn
n: int = 2;
match n {
  1 { println("one"); }
  2 { println("two"); }     // taken
  3 { println("three"); }
  _ { println("many"); }
}
```

- **`int` / `float` / `char`** arms compare by value.
- **`Str`** arms compare by **content**, not reference identity:

  ```pkn
  greeting: Str = "World";
  match greeting {
    "Hello" { println("hi"); }
    "World" { println("hello world"); }   // taken — content match
    _       { println("other"); }
  }
  ```

- **`bool`** has only two values, so `True` / `False` arms are exhaustive without a `_`:

  ```pkn
  match flag {
    True  { println("yes"); }
    False { println("no"); }
  }
  ```

Each literal must have the same type as the subject. A literal of the wrong type, or a type-name
arm in a value-mode `match`, is an error:

```pkn
match n {            // n: int
  1     { }
  "two" { }          // error: Str literal does not match int subject
  _     { }
}

match x {            // x: int
  Animal { }         // error: value-match requires literal patterns, not type names
  _ { }
}
```

---

## The Wildcard `_`

`_` matches anything and is the catch-all. It must appear as the **last** arm — anything after it
can never run:

```pkn
match x {
  _   { }
  Dog { }   // error: unreachable arm after wildcard
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
    println(sound);
  }
  Dog {
    y: int = 3.14;           // error: cannot assign float to int
  }
  _ { }
}
```

---

## Exhaustiveness and Control Flow

`match` participates in the same flow analysis as the rest of the language. When a `match` is the
last thing in a function whose every path must `return` a value, the analysis treats the `match`
as guaranteeing a result only if it is exhaustive — every arm returns *and* the arms cover every
case (via `_`, all enum variants, or both `bool` values). A `match` with uncovered cases and no
fallthrough is rejected:

```pkn
fn main() -> int {
  x: Obj = Dog();
  match x {
    Dog { return 0; }
    Cat { return 1; }
    // error: no '_' arm and no return after the match —
    //        the function does not always return a value
  }
}
```

The same exhaustiveness rule lets `match` satisfy field-initialisation analysis inside `__init__`
(see `04-classes.md`): a field assigned in every arm of an exhaustive `match` counts as
definitely assigned.

---

## Common Patterns

### Distinguishing `File` from `Error`

`open()` returns `Obj` — either a `File` or an `Error`. `match` separates the two:

```pkn
match open("/tmp/data.txt", "r") {
  err: Error { printerrln("open failed: " + err.toString()); }
  f: File {
    while (True) {
      match f.readln() {
        line: Str { print(line); }
        _         { break; }      // None — end of file
      }
    }
  }
}
```

### Unwrapping parsed primitives

`int<Str>(s)` / `float<Str>(s)` return an `int?` / `float?`: `None` when `s` is not a valid
number. They are unwrapped in optional mode (below), and the arm binds a plain `int`:

```pkn
match int<Str>(s) {
  n: int { return "ok: " + Str<int>(n); }
  None   { return "not a number: " + s; }
}
```

### Detecting end of input

A `readln` that yields `None` at EOF is matched by falling through to `_`:

```pkn
match f.readln() {
  line: Str { process(line); }
  _         { break; }   // None
}
```

---

## Optional Mode — unwrapping a `T?`

When the subject is an optional type `T?` (see `10-optionals.md`), `match` is the way to get at
the wrapped value. The arm naming `T` itself matches **every present value** — whatever its
runtime subclass — and binds it as `T`; a `None` arm (or `_`) covers the absent case:

```pkn
match find(head, key) {          // find returns Node?
  n: Node { println(Str(n.v)); }
  None    { println("not found"); }
}
```

Two differences from type mode:

- The `T` arm is **not** an exact-type test: a `Leaf` stored in a `Node?` is matched by
  `n: Node`. An arm naming a strict subclass keeps exact-type semantics and must come before
  the `T` arm (anything after the `T` arm is unreachable and rejected).
- A `T` arm alone is **not exhaustive**; `T` together with `None` (or `_`) is, and then the
  `match` satisfies the "always returns" and field-initialisation analyses.

A `None` arm is only valid on an optional subject — matching on an `Obj` (the `readln` idiom
above) is unchanged.

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Subject is not matchable | The subject type supports neither type-, variant-, nor value-mode matching |
| Arm type not a subtype | A type arm names a class that is not the subject type or a subtype of it |
| Unknown arm type | A type arm names a class that is not declared anywhere |
| Unknown enum variant | A variant arm names an identifier that is not a variant of the subject enum |
| Qualified variant arm | A variant arm is written qualified (`base::Color::Green`); arms use the bare variant name (`Green`) |
| Literal type mismatch | A value arm's literal has a different type than the subject |
| Type name in value mode | A value-mode `match` has a type-name arm instead of a literal |
| Binding redeclaration | An arm body redeclares the arm's binding name |
| `view` binding changed or passed on | A `name: view T` binding is assigned, written through, passed to a parameter that is not `view`, or stored; or the subject's variable changes while it is live |
| `inout` binding of something else | A `name: inout T` arm's subject is not a variable or a field, is `self`, a `let` local or a `view`, or is an optional primitive; or the subject's variable is used while the binding is live |
| Arm body type error | Any type error inside an arm body |
| Wildcard not last | A `_` arm is followed by another arm |
| Non-exhaustive match | A `match` leaves cases uncovered where every path must produce a value |
| `None` arm outside optional mode | A `None` literal arm on a non-optional subject |
| Unreachable arm after the `T` arm | A type arm follows the `T` arm of an optional-mode `match` |

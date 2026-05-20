# PaykanLang — Enums & Pattern Matching

Enums are **zero-cost tagged sums**. They cover C-style "named constant set"
case (`enum Op { Add, Sub, Mul }`). They compile to an integer tag.

---

## Quick Summary

- Declared with `enum`.
- Each variant converts to an integer.
- No inheritance, no methods on the enum itself.

---

## Declaration

```
enum Op {
  Add,
  Sub,
  Mul,
  Div,
  Mod,
}
```

- Each variant is a distinct value of type `Op`.
- Layout: a single `i32` tag.
- Equality is defined: `==` and `!=` compare tags.
- Use `match` to enumerate on the variants.

---

## Construction

```
o: Op = Op::Add;                    
e: Token = Token::Eof;
```

The `enum_name::` prefix is **required**. There are no implicit unqualified variants
(this prevents collisions when two enums share a variant name).

---

## Pattern Matching — `match`

`match` is the **standard way** to read a variant; variants can
also be compared with `==` but `match` is the idiomatic way.

```
fn describe(t: Token, v: Value) -> Str {
  match t {
    Num    { return v.toString(); }
    Op     { return opName(t); }
    LParen { return "("; }
    RParen { return ")"; }
    Eof    { return "<eof>"; }
  }
}
```

### Syntax

```
match <scrutinee> {
  <pattern> { <statements> }
  <pattern> { <statements> }
  ...
}
```

- The scrutinee is any expression of an enum type, or class type.
- Each arm has the form `Variant { body }`.
- Arms are separated by whitespace; ordering does not matter except that `_` (when present)
  must be the last arm.
- The `_` pattern matches any remaining variant. Exactly one `_` arm is allowed per `match`.
- A literal pattern is **not** supported in `match` *for enum scrutinees*. Literal patterns **are**
  supported when the scrutinee is a builtin type — see *Matching on Builtin Types* below.

### Exhaustiveness

The compiler rejects a `match` that omits at least one variant and has no `_` arm:

```
match t {
  Num { ... }
  Op  { ... }
  // ERROR: missing variants: LParen, RParen, Eof
}
```

A `_` arm satisfies exhaustiveness:

```
match t {
  Num { ... }
  _   { /* ignore */ }
}
```

### `match` is a statement, not an expression

`match` desugars to a chained `if`/`else` for class downcasts, and a `switch` for enums.

```
result: int = 0;
match tok {
  Num { result = (int)tok; }
  _   { result = -1; }
}
```

---

## Matching on Builtin Types

`match` is **not** restricted to enums and classes — it also accepts a scrutinee of any
builtin scalar type (`char`, `int`, `float`, `bool`) plus `Str`. This is the canonical way
to write a multi-way dispatch that would otherwise be a chain of `if`/`else if` over
the same value: cleaner at the source level and easier for the compiler to lower to a
jump table when the arms are dense integers.

### Syntax

For builtin scrutinees, each arm is a **literal pattern** of the scrutinee's type, or
the wildcard `_`:

```
match ch {
  '=' { handleEq(); }                  // '='
  '+' { handlePlus(); }                // '+'
  '-' { handleMinus(); }               // '-'
  _   { handleOther(ch); }             // catch-all
}
```

```
match flag {
  True  { std::print("on"); }
  False { std::print("off"); }
}
```

```
match greeting {
  "hi"    { wave(); }
  "hello" { wave(); }
  _       { ignore(); }
}
```

### Rules

- **Patterns must be compile-time literals** of the scrutinee's exact type:
  - `int` arms — integer literals (`0`, `42`, `0 - 1`).
  - `float` arms — float literals (`0.0`, `3.14`). Float arms compare by bitwise
    equality, not `==`; `NaN` therefore matches no arm. Prefer `int`/`bool` scrutinees
    when you can.
  - `bool` arms — `True` and `False`.
  - `Str` arms — string literals; comparison is byte-wise (`Str.equals`).
- **A `_` arm is required for `int`, `float`, and `Str`** scrutinees (their value
  spaces are unbounded). For `bool`, listing both `True` and `False` is exhaustive
  and a `_` arm is then forbidden.
- **Duplicate literals across arms are rejected.**
- **Range patterns are not supported** (no `0..10 { ... }` arm) — chain literal arms
  or fall through to `_` and use an `if` inside.

### When to prefer `match` over `if`

Reach for `match` whenever you'd write three or more `if`/`else if` arms over the same
value. The compiler can lower a dense `int` match to a jump table; chained `if`s
generally cannot be folded that way.

---

## Comparison and Equality

For enums, `==` and `!=` compare the tag:

```
if (o == Op::Add) { ... }
```

---

## Casting

| Direction | Allowed | Notes |
|---|---|---|
| enum → `int` | Yes | Yields the ordinal |
| `int` → enum | No | Would allow invalid tags; use a function with a `match` if you really need this |

---

## Semantic Checks (Enums)

| Error | Trigger |
|---|---|
| Naming convention | Enum name, or variant not `UpperCamelCase` |
| Unqualified variant | `Add` instead of `Op::Add` |
| Non-exhaustive match | `match` over an enum that omits a variant and has no `_` arm |
| Duplicate match arm | Same variant matched twice, or the same literal matched twice in a builtin `match` |
| `_` not last | A `_` arm followed by another arm |
| Missing `_` on builtin match | `char`/ `int` / `float` / `Str` scrutinee with no `_` arm |
| `_` on exhaustive `bool` match | `bool` scrutinee with both `True` and `False` arms plus a `_` arm |
| Non-literal arm on builtin match | An expression or binding used as a pattern for an `int` / `float` / `bool` / `Str` scrutinee |
| Type mismatch in builtin arm | Arm literal's type differs from the scrutinee's type (e.g. `0.0` arm on an `int` match) |
| Cast `int → enum` | Not allowed |

---

## Convention Reminders

- `Enum` — `UpperCamelCase`, zero-cost.
- `Variant` — `UpperCamelCase`, no prefix on the variant itself in declarations.
- Use `EnumName::Variant` everywhere you construct or compare a variant.
- Enums appear in argument lists by **value**

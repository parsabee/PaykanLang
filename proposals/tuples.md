# Proposal: Tuples (prototype)

Status: **prototype** — implemented end-to-end on the `proto/tuples` branch for
issue #4, behind no flag, but deliberately minimal.  This document records the
design as built, the decisions taken, the alternatives considered, and what is
left open.  See `language_reference/09-tuples.md` for the user-facing rules.

---

## Motivation

Paykan has no way to return more than one value from a function without
declaring a class for the purpose, and no lightweight way to group a few
values of different types.  Tuples give both:

```pkn
fn divmod(a: int, b: int) -> (int, int) { return (a / b, a % b); }

q, r = divmod(7, 2);
```

---

## Syntax

| Form | Example | Notes |
|------|---------|-------|
| Type | `(int, Str)`, `(int, (Str, bool))`, `(int, Str)[]`, `(int[], Str)` | arity >= 2; nesting, arrays of tuples and tuples of arrays allowed |
| Literal | `(1, "a")`, `(x, y + 1)` | arity >= 2; `(x)` is a parenthesised expression, `()` is a syntax error |
| Element access | `t.0`, `t.1`, `t.0.1` | compile-time constant index; out of range is a Sema error |
| Multiple return | `fn f() -> (int, int) { return (1, 2); }` | a tuple is an ordinary value |
| Destructuring | `q, r = f();`, `a: int, _ = f();`, `_, b = f();` | statement form only; `_` skips an element |

Element access is spelled with a single **`TUPLE_INDEX` token** (`\.[0-9]+`,
no leading zeros).  Flex picks the longest match at the dot, so `t.0` lexes
as `IDENT TUPLE_INDEX`, and because a float literal must start with a digit,
`1.5` still lexes as `FLOAT` (at the `1`, the three-character float match
beats the one-character int match before the dot is ever considered alone).
The only sequence whose meaning changed is the chain `t.0.1`, which used to
lex as `IDENT DOT FLOAT(0.1)` and is now two indices — exactly the nested
access we want.  `.5` on its own was already a syntax error and still is.

Destructuring is parsed from a dedicated target list (`IDENT`,
`IDENT ":" type`, or `_`) rather than from an expression, so the LALR(1)
lookahead after the first `IDENT` — `,` versus `=`/`:`/operators — decides
between a destructuring statement, a plain assignment and a `x: T = e`
declaration with **zero grammar conflicts**.  Nested patterns
`(a, (b, c)) = e` are a syntax error (out of scope).

`t.0 = v` is syntactically accepted (it becomes a `MemberAssignStmt` whose
field name is the index) so that Sema can reject it with a typed
"tuples are immutable" diagnostic instead of a parse error.

---

## Semantics

* **Type identity.** Each `(T1, T2, …)` is one canonical `TupleType` node
  interned per element-type list by `ASTContext::getTupleType`, exactly like
  `ArrayType` per element type, so resolved tuple types compare by pointer and
  nested tuples share their inner nodes.  `ast::typeName` spells it
  `(int, Str)`, which is also the module-export serialisation;
  `resolveExportedType` parses it back (nesting and `[]` suffixes included), so
  tuple-typed signatures round-trip across imports.
* **Literal typing.** `(1, "a")` is `(int, Str)`; there is no contextual
  typing from the declared type.  A `void` element or an empty array literal
  `[]` inside a tuple literal is an error (nothing could supply the array's
  element type).
* **Assignability.** Every tuple is an instance of the builtin `Tuple` class
  (an `Obj` subtype), so a tuple is assignable to `Obj` (and can be printed).
  Between tuple types assignability is element-wise and
  *representation-preserving*: primitive elements must match exactly — no
  `int -> float` promotion inside tuples, because the runtime slot holds raw
  bits and no conversion is emitted — while reference-typed elements are
  covariant (`(int, Str)` to `(int, Obj)`), which is sound because tuples are
  immutable and every reference slot holds a box regardless of its static
  type.
* **Immutability.** `t.0 = v` and `mov t.0` are Sema errors; build a new tuple
  instead.  Objects *inside* a tuple keep their own mutability
  (`arrs.0.push(4)` works).
* **Equality.** `==` / `!=` require the same tuple type and lower, like every
  reference type, to the virtual `equals`: element-wise, primitives by value,
  references through their own `equals` (so `Str` by content, class instances
  by identity unless they override `equals`).  Ordering operators are not
  defined.
* **Methods.** Tuples expose only `Obj`'s `toString` and `equals` (through a
  per-element-list `Tuple<int, Str>` specialisation of the `Tuple` class that
  mirrors `Array<T>`); `toString` renders `(1, a)` using each element's
  `toString`.
* **Destructuring.** The value must be a tuple whose arity equals the number
  of targets.  An annotated target `a: T` is a fresh declaration (VarDecl
  rules: no redeclaration in the current scope, element assignable to `T`,
  `int -> float` allowed there); a bare name follows the implicit-declaration
  rule of assignment (declared on first use, re-assigned — and revived after a
  `mov` — if already visible); `_` discards the element.  Duplicate targets
  are an error.
* **Ownership.** A tuple is a reference type under ARC with no new rules: it
  is declared as an owned variable, retained when copied, passed to callees as
  a +1 box, returned as a box, released at scope exit and transferred by
  `mov`.  Reading a reference element (`t.1`) yields the slot's box
  *borrowed*; a new owner retains it.  When the tuple itself is a temporary
  (`mk().1`), the element is retained before the temporary is torn down — the
  same discipline as call-rooted field reads (`makeH().a`).

---

## Representation

**Decision: a heap object, not a flat aggregate.**  A tuple value is one
generic runtime object, `PaykanTuple` (`src/Runtime/Tuple.c`):

```
{ vtable, shared-backpointer, count, slots*, kinds* } + count x 8-byte slots + count kind bytes
```

allocated as a **single block** and boxed in a `PaykanShared` like every other
object.  Primitive elements are stored raw (int/enum, double bits, bool, char);
reference elements (classes, `Str`, arrays, nested tuples) store a retained
`PaykanShared*`.  The per-slot *kind* byte lets one vtable
(`PaykanTuple_vtable`: destroy / toString / equals) serve every tuple type:
`destroy` releases exactly the reference slots, `equals` compares element-wise
(dispatching `equals` on references with the consumed-box ABI), and `toString`
knows how to print a raw slot.  CodeGen emits the kinds as an interned
`[N x i8]` constant per distinct kind sequence and still uses the static
element types to reinterpret slots on read.

Why not a flat LLVM aggregate (`{i64, ptr}` by value)?

* Every existing ownership path — parameters, returns, fields, array
  elements, `mov`, scope cleanup, `Obj` storage, `match` subjects, printing —
  is written for "a pointer to a boxed heap object".  A by-value aggregate
  would need a second, parallel discipline in Sema and CodeGen (element-wise
  retain/release on copy, a different calling convention, no `Obj`
  compatibility), which is far more than a prototype should carry.
* `Obj`-compatibility (printing, `Obj[]`, future `match`) falls out for free.
* The cost — one allocation plus a box per tuple value — is the same as an
  array literal and acceptable for a prototype; a flat representation with
  escape analysis is a possible optimisation later, not a semantic change,
  because the language surface (immutability, value-like equality) was chosen
  so both representations are observably equivalent.

Why one generic object with kinds instead of one struct per tuple type?

* No per-type runtime code and no per-type vtable globals to synthesise or
  link; the JIT symbol table gains one small fixed set of symbols.
* `equals` / `toString` / `destroy` are written once.
* The `Tuple<T1, T2>` specialisations therefore share the single runtime
  vtable, which is why `match` on a tuple *type* is out of scope (there is no
  per-type identity at runtime yet).

---

## What works / what is rejected

Works: everything in the *Syntax* and *Semantics* sections above, including
tuples as class fields, parameters and return types, arrays of tuples, tuples
of arrays and nested tuples, `println(t)`, `t.toString()`, `t == u`, `mov t`,
destructuring from calls / variables / literals, and imported tuple-typed
signatures.  Leak-checked in `tests/CodeGen/TupleTests.cpp` and
`samples/leak-check/17_tuple.pkn`.

Rejected (diagnostic):

| Program | Diagnostic |
|---------|-----------|
| `t: (int) = 1;` / `()` | syntax error (no 1-tuples / empty tuples) |
| `t.2` on `(int, Str)` | `tuple index '.2' is out of range for type '(int, Str)' (valid indices are .0 to .1)` |
| `x.0` on `int` | `tuple index '.0' applied to non-tuple type 'int'` |
| `t.0 = v` | `cannot assign to element '.0' of type '(int, int)': tuples are immutable; build a new tuple instead` |
| `mov t.0` | `cannot 'mov' a tuple element; …` |
| `a, b, c = (1, 2)` | `cannot destructure a value of type '(int, int)' into 3 targets (it has 2 elements)` |
| `a, b = 42` | `cannot destructure a value of type 'int'; only tuples can be destructured` |
| `a: Str, b: int = (1, 2)` | `element 0 of type 'int' does not match declared type 'Str' for target 'a'` |
| `a, a = (1, 2)` | `duplicate target 'a' in destructuring` |
| `t: (float, int) = (1, 2)` | `initializer of type '(int, int)' does not match declared type '(float, int)' …` |
| `(1, "a") == (1, 2)` | `operands of '==' have mismatched types '(int, Str)' and '(int, int)'` |
| `(nothing(), 1)` | `tuple element 0 has type 'void' …` |
| `([], 1)` | `cannot infer element type of empty array literal '[]' inside a tuple literal; …` |
| `match o { (int, int) { } }` | `match arm type must be a class or array type` |
| `class Tuple {}` | `'Tuple' is a builtin class and cannot be redeclared` |

---

## Alternatives considered

* **Contextual typing of literals** (`t: (float, int) = (1, 2)` promoting the
  `1`).  Rejected for the prototype: it would need bidirectional inference in
  `visitTupleLiteralExpr`; the explicit rule is simple and the error is clear.
* **Full covariance including primitives.** Rejected: primitive slots are raw
  bits, so `(int, int)` read as `(float, int)` would reinterpret the bits.
* **Mutable tuples (`t.0 = v`).** Rejected: immutability is what makes
  covariance and value-like equality sound and keeps a by-value representation
  possible later.
* **Destructuring as an expression / in `match` patterns.** Deferred (see
  below); the statement form covers multiple return.
* **A separate `TUPLE_INDEX`-free grammar (`t[0]` with constant index).**
  Rejected: overloading `[]` would blur the array/tuple distinction and make
  the index a runtime value syntactically.

---

## Out of scope (not implemented)

* Tuples in `match` patterns (`match t { (0, _) { … } }`) and `match` arms on
  tuple types.
* Nested destructuring `(a, (b, c)) = e`.
* Mutation of elements; named elements (`(x: int, y: int)`); 1-tuples and the
  empty tuple.
* Spreading a tuple into call arguments (`f(...t)`).
* Contextual typing / promotion of literal elements.

---

## Open questions

1. **Representation of small all-primitive tuples.** `(int, int)` costs two
   allocations today.  A flat by-value lowering guarded by escape analysis
   would be a pure optimisation given immutability — is that the intended
   direction before tuples leave prototype status?
2. **Per-type runtime identity.** `match` on tuple types needs a per-type
   vtable (or a type tag in the object).  The `Tuple<T1, T2>` specialisations
   already exist on the Sema side; is `match` on tuples wanted at all, or
   should destructuring in patterns be the only pattern form?
3. **Equality of floats** uses IEEE `==` (so `(nan, 1) != (nan, 1)`), matching
   `float == float` elsewhere.  Fine?
4. **`Tuple` as a spellable type name.** `x: Tuple = (1, 2)` type-checks (it is
   the opaque base class, like `Obj`) but nothing can be done with `x` except
   `toString`/`equals`.  Should `Tuple` be reserved-but-unspellable instead?
5. **Contextual typing** (question 1 of *Alternatives*): worth adding once
   literals get a general bidirectional pass?

---

## Known limitations of the prototype

* No `int -> float` promotion inside tuple literals against a declared type.
* `toString` prints elements without quotes (`(1, a)`), consistent with
  `println("a")`, so `("a, b", 1)` renders ambiguously.
* Arity is limited only by the runtime's `int64_t` count; there is no
  diagnostic for absurdly large tuples.
* A tuple variable declared without annotation from another *variable*
  (`u = t`) keeps its precise type in Sema but the codegen scope records the
  canonical tuple type only when Sema attached it; method dispatch still works
  because every tuple shares one vtable layout.

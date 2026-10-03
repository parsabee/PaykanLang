# Proposal: Optional (nullable) types — `T?`

Status: **prototype** (issue #5, branch `proto/optionals`). The implementation described here
is a coherent, tested end-to-end slice, not the finished feature; the open questions and the
out-of-scope list at the end are the parts still to be decided.

---

## Motivation

PaykanLang has exactly one way to say "there may be no value here": type the slot as `Obj`,
store `None` (a singleton `Obj`), and `match` on it later. This is what `open()` and
`readln()` do today (and what `IntStr()` did, before #64 replaced it by `int<Str>()`
returning `int?`). It works, but it throws the static type away — a `next: Obj` field of
a linked list accepts a `Str` as happily as a `Node`, every read needs a `match` that must
also handle "some other object", and the compiler cannot tell the reader (or itself) that a
value is *either a `Node` or nothing*.

`T?` keeps the type: `next: Node?` is a `Node` or `None`, nothing else. The compiler can then
refuse to use it as a `Node` until it has been unwrapped, and can treat `None` as a first-class
case in `match` exhaustiveness.

---

## Design

### Syntax

`?` is a type suffix, allowed in every type position (variables, fields, parameters, return
types, match arms):

```pkn
n: Node? = None;
fn find(head: Node?, key: int) -> Node? { … }
class Node { next: Node?; … }
xs: Node?[] = [];        // array of optionals
ys: int[]? = None;       // optional array
```

Reference types (classes, `Str`, arrays) may be optional, and so may the primitives `int`,
`float`, `bool` and `char` (#66: a present value is boxed, see open question 5). `void?` is
rejected at parse time, `Enum?` in Sema (*"optional enum types are not supported yet"*), and
`T??` as unsupported.

`None` is also accepted as a **match-arm pattern** so the absent case can be named.

### Typing rules

| Rule | Example | Result |
|------|---------|--------|
| `None` is assignable to any `T?` | `n: Node? = None;` | ok |
| `T` widens to `T?` | `n: Node? = Node(1);` | ok |
| `S?` converts to `T?` when `S` converts to `T` | `n: Node? = leaf;` (`leaf: Leaf?`) | ok |
| `T?` converts to `Obj` | `o: Obj = n; println(n);` | ok (see *Representation*) |
| `T?` never converts to `T` | `m: Node = n;` | error: cannot use optional 'Node?' as 'Node' without unwrapping (use match) |
| `Obj` does not convert to `T?` | `n: Node? = someObj;` | error (ordinary mismatch) |

On an optional value the only permitted operations are: assignment, passing and returning,
storing into fields and arrays, `mov`, `==`/`!=`, and `match`. Member access, method calls,
subscripts, `+`, unary operators and `toString()` are errors pointing at `match`.

**Decision: `None` keeps its static type `Obj`.** Every existing program types `None` as
`Obj` (`x: Obj = None`, `return None` from `-> Obj`, `readln()` returning `Obj`), and the
prototype must not change any of that. `None` is therefore *additionally* accepted wherever a
`T?` is expected — an expression-level rule (`Sema::checkAssignable`) rather than a type-level
one, because `Obj` in general must not flow into `Node?`. Sema records the contextual `T?` on
the literal for CodeGen (see below). The alternative — a dedicated bottom-ish `None` type
assignable to `Obj` and to every `T?` — is cleaner and is the natural end state; it was not
done here because it changes the type of every existing `None` expression and every
diagnostic that mentions it, which is out of proportion for a prototype.

**Decision: `T?` → `Obj` is allowed.** An `Obj` slot may hold `None` today, so widening an
optional into it loses no safety. The subtlety is representational (an `Obj` never holds a
NULL box; see *Representation*), and it is handled by CodeGen at the conversion site, which
Sema marks on the expression (`Expr::CoercedType`).

### Equality

- `x == None` / `x != None` (either order) is a **null check on the box**. It never dispatches
  `equals`, so it is safe for a `None` value and cheap.
- `a == b` with both operands optional: both `None` → equal; exactly one `None` → not equal;
  otherwise the usual virtual `equals` dispatch on the present values (user overrides are
  honoured, `Str` compares by content). The wrapped types must be equal or class-related.
- `opt == present` (one side optional, the other a non-optional non-`None` value) is
  **rejected**: "compare against None or unwrap it with match". Allowing it would mean picking
  a widening for the present side; rather than guess, the prototype asks for an explicit form.

### `match` is the unwrap

```pkn
match maybe {                // maybe: Node?
  n: Node { … n.v … }        // present: n is a Node
  None    { … }              // absent
}
```

- A type arm naming the wrapped type `T` **matches every non-`None` value**, whatever its
  runtime subclass, and binds it as `T`. This is a deliberate departure from class-mode
  `match`, where a type arm is an exact-runtime-type test: for an optional the `T` arm is the
  "present" pattern, and an unwrap that silently fell through to `_` for a `Leaf` stored in a
  `Node?` would be a trap. A type arm naming a **strict subclass** keeps exact-type semantics
  (`l: Leaf { … }` before `n: Node { … }`), and an arm after the `T` arm is diagnosed as
  unreachable.
- `None` or `_` covers the absent case. A `T` arm alone is **not** exhaustive; `T` + `None`
  (or `_`) is. Exhaustiveness feeds the existing "does not always return a value" and
  `__init__` definite-assignment analyses through one shared helper
  (`detail::matchIsExhaustive`).
- The subject may be any optional expression: a variable, a field (`match self.next`), a call
  result, an array element. The existing `Obj`-subject match (`match Stdin.readln() { line: Str
  { } _ { } }`) is untouched — a `None` arm on an `Obj` subject is still an error.

### Optional fields default to `None`

`__init__` need not assign a field of type `T?`: the constructor zero-initialises every slot
and a NULL box *is* `None`, so the definite-assignment analysis treats optional fields as
assigned at entry. Non-optional fields keep the existing rule. This is what makes
`class Node { next: Node?; fn __init__(v: int) { self.v = v; } }` write the way a linked list
wants to be written.

### Arrays

- `T?[]` (array of optionals) is allowed. It is an object array whose slots may be NULL; every
  runtime array operation the compiler emits (`get`, `set_obj`, `push_obj`, `pop_obj`,
  `toString`, `equals`, `destroy_obj`) was checked to tolerate NULL slots and is covered by
  `tests/Runtime/OptionalNullTests.cpp`. `pop()` on a `T?[]` yields a `T?`.
- `T[]?` (optional array) is allowed; the empty literal `[]` takes the wrapped array type
  (`xs: Str[]? = []`).

### Ternary

`if c then x else None` has type `T?` when `x` is a reference type `T` (or `T?`); two optional
branches unify to the optional of their common ancestor.

---

## Representation and code generation

**A `T?` is exactly the `PaykanShared*` box of a `T`, with `NULL` meaning `None`.** No runtime
layout change, no new runtime symbols, no wrapper object. This rests on properties the runtime
already had and that are now pinned down by tests:

- `Paykan_retain(NULL)`, `Paykan_release(NULL)`, `PaykanShared_get(NULL)` are no-ops / return
  NULL; `Paykan_print(NULL)` prints nothing.
- Object arrays zero-initialise their slots and skip NULL on set/push/pop/destroy.
- Every *present* box holds a non-NULL object (unique-box invariant), so unwrapping a box with
  `PaykanShared_get` yields NULL exactly when the optional is `None` — this is what makes
  `x == None` a single `icmp eq ptr … null` for every expression form.

Consequences in CodeGen:

- Optional locals, parameters and fields are ordinary *owned* reference slots: scope cleanup,
  destructors, `mov` (which leaves a NULL slot behind) and the null-checked field-store
  release all work unchanged.
- `None` written into a `T?` slot is the **null box**, never the boxed `None` singleton. Sema
  marks the literal (its resolved type becomes the target `T?`), and `emitAsShared`, variable
  rebinding and field stores honour it. An optional variable initialised with `None` is
  therefore an owned null box, unlike the legacy unowned `o: Obj = None`.
- A `T?` flowing into an `Obj` slot (`Expr::CoercedType`) has a null box replaced by a fresh
  box of the immortal `None` singleton, so an `Obj` never holds a NULL box and every existing
  `Obj` consumer (method dispatch, `match`, `println`, `equals`) keeps working; on the raw
  pointer path of the print builtins the singleton's address is substituted, so
  `println(maybe)` prints `None`.
- Optional-subject `match` lowers to a leading null test that routes `None` to the `None` arm
  (else the wildcard / `match.end`), followed by the ordinary vtable chain on the non-`None`
  path — no vtable load ever dereferences NULL — with the `T` arm branching unconditionally.
- Two-optional equality retains both operands, tests for NULLs, and only then performs the
  vtable `equals` call (which consumes the right operand as usual), releasing on every path.

Module exports serialise `T?` by name (`ast::typeName` → `"Node?"`, `"Str?[]"`, `"int[]?"`)
and the import side parses the `?` suffix back, so optional signatures and fields round-trip
across modules with type identity preserved.

---

## Alternatives considered

- **A distinct `None` type** (bottom of the reference lattice, assignable to `Obj` and to every
  `T?`). Cleaner than the contextual rule and the recommended end state; deferred because it
  retypes every existing `None` expression (see *Decision* above).
- **A tagged wrapper / a second word in the box** for `None`. Unnecessary: a NULL box already
  distinguishes absence, and every runtime entry point tolerates it. A wrapper would cost an
  allocation per optional and a runtime ABI bump.
- **Exact-type semantics for the `T` arm** (consistent with class mode). Rejected for optionals
  because the point of `match` here is unwrapping; a subclass instance must not fall through.
  The inconsistency is documented and confined to optional subjects.
- **Rejecting `T?` → `Obj`.** Simpler, but it would make `println(maybe)` and every existing
  `Obj`-taking API unusable with optionals without a `match` first.
- **Representing `None` for `Obj` as NULL too** (unifying both spellings). Would have changed
  the observable behaviour of `x: Obj = None` and the `Obj`-subject `match` lowering; out of
  scope for a prototype.

---

## Open questions

1. Should `None` get its own static type (making `checkAssignable`'s special case a plain
   subtyping rule)? Recommended for the real feature.
2. Should `opt == present` be allowed by widening the present side (`a == Node(1)` ⇒
   `a == (Node(1) as Node?)`)? Currently rejected.
3. Array-literal typing: `[None]` and `[node, None]` type as `Obj[]` and do not unify to
   `Node?[]`; a bidirectional (expected-type) pass on literals would fix this and also allow
   `xs: Node?[] = [None, n]`.
4. Should a `None` arm be allowed on an `Obj` subject (matching the singleton)? It would let
   the `readln()` idiom name its absent case; today it is an error to keep `Obj` matching
   unchanged.
5. ~~Optional primitives need a real representation (a tag word or boxing).~~ **Resolved
   (#66): boxing.** A present `int?` / `float?` / `bool?` / `char?` is the runtime's boxed
   `Int` / `Float` / `Bool` / `Char` object, in the same nullable `PaykanShared*` box as every
   other optional. Every existing optional mechanism (ARC, `mov`, `None` fields, `T?[]`,
   `T?` -> `Obj`) therefore applies unchanged, and `None` allocates nothing. A tag word would
   avoid the allocation for a present value, but it would need a second representation
   throughout the lowering, both backends and the tuple/array slot ABI. Sema marks each
   primitive that flows into an optional primitive slot (`Expr::CoercedType`), and the
   lowering boxes it. A `match` arm `n: int` reads the value out of the box into a plain local.
   `int?` -> `float?` is rejected because it would re-box. `Enum?` stays rejected until there
   is an enum box that can print its variant name.

---

## Known limitations / out of scope (with sketches)

- **Flow typing** — `if (x != None) { x.foo(); }` does not narrow `x`. Sketch: in
  `visitIfStmt`, when the condition is `id != None` (or `id == None` with an `else`), shadow
  `id` in the branch scope with the wrapped type; CodeGen would unwrap through the same
  null-safe `PaykanShared_get` it uses for `match` bindings. Invalidation on re-assignment of
  `id` inside the branch is the hard part (treat like the `mov` flow analysis).
- **`if let`** — `if let n = maybe { … } else { … }` is syntactic sugar for the two-arm
  `match`; it can be desugared in the parser to a `MatchStmt` with a `T` arm and a `_` arm.
- **`??` (default)** — `maybe ?? fallback` lowers to the phi already used by
  `emitOptionalToObj`, with the fallback expression in the null block; typing: `T? ?? T → T`.
- **`?.` (optional chaining)** — `maybe?.field` needs the result to be `U?` and a null-guarded
  load; composes naturally with the null test above.
- **Optional enums** — `Enum?` could box through `Int` like `int?`. But in an `Obj` slot it
  would print its ordinal and match as an `Int`, so it waits for an enum box that knows its
  variant names.
- **`[None]` / mixed array literals** — see open question 3.
- **`opt == present`** — see open question 2.
- The `T` arm's "any subtype" rule means an arm order like `n: Node { } l: Leaf { }` is
  diagnosed as unreachable; the reverse order is required.

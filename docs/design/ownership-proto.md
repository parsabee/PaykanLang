# Ownership qualifiers: `own`, `mut` and views (prototype)

Status: **prototype** on branch `proto/ownership`, behind `--ownership`.
Nothing here is a commitment; it exists to try the model end to end.

## The model

Every declared thing (local, parameter, `self`, field, function result)
has exactly one *kind*. The default is the **view**, which has no keyword.

| | **view** (default) | **`mut`** | **`own`** |
|---|---|---|---|
| change the object / value through it | no | yes (its contents; never which object the caller holds) | yes |
| class type (classes, `Str`, arrays, tuples, `Obj`, their optionals) | shared ARC reference; may escape (be returned, kept in a local, stored in a view field) | shared ARC reference; may escape and be stored in a `mut` field | the owner |
| value type (`int`, `float`, `bool`, `char`, enums) | read-only value; may not escape (return it or store it only as `own`) | **parameter only:** the caller's storage, changed in place (inout) | its own copy |
| passing / assigning into it | implicit | implicit | **`cp e`** (deep clone), **`mv x`** (hand over), or a fresh value |

- **Fresh values** need neither keyword: constructor calls, array / string /
  numeric literals, results of functions declared `-> own T`, and `cp e`.
- **`cp e`** deep-clones: a class object's `own` fields are cloned
  recursively, its view and `mut` fields are shared (copied as links). A `Str`
  is copied, an array is copied and its elements cloned when they are owned.
  A value is copied.
- **`mv x`** hands over what the local or parameter `x` holds, with no retain
  or copy; `x` cannot be used again until it is assigned. Only locals and
  parameters can be moved: never a field, an element or `self`.
- **Permission only narrows:** own -> mut -> view. Giving `mut` access to
  something reached through a view is an error. Through a view nothing inside
  the object can change (fields, elements); through `own`/`mut` access, an
  `own` field is changeable, a `mut` field is changeable, a view field is not.
- **Fields:** `own` any type; view or `mut` of class types. A value-type field
  without `own` is an error (`add 'own'`).
- **Results:** `-> own T` (any type), `-> T` (view, class types), `-> mut T`
  (class types). Returning a value-type view is an error.
- **`self`:** written as an optional first parameter of a method,
  `fn m(self)` (view), `fn m(self: mut)` or `fn m(self: mut Self)`. Without
  it the method's `self` is a view. `__init__`'s `self` is always `mut`.
  `self: own` is rejected.
- **`let`** only on a local declaration with an initializer: the variable
  cannot be reassigned (or moved from). It says nothing about changing.
- **Changing operations** need `own` or `mut` access to the object: assigning
  a field, element assignment, `push`, `pop`, calling a `self: mut` method,
  passing to a `mut` parameter.
- **Calls are never marked** for view and `mut` parameters; `own` parameters
  need `cp`, `mv` or a fresh value.
- **No qualifier with the type left off** (`x = e;`) declares a view; write
  `x: own = e;` or `x: mut = e;` for the others. A view bound to a fresh value
  keeps it alive until the end of the scope.

### Example

```pkn
class Person {
  name: own Str;
  fn __init__(self: mut, n: own Str) { self.name = mv n; }
  fn rename(self: mut, n: own Str) { self.name = mv n; }
}

class Team {
  members: own Person[];   // owned: cloned by cp
  lead: mut Person;        // a changeable link: shared by cp
  fn __init__(self: mut, l: mut Person) { self.members = []; self.lead = l; }
  fn add(self: mut, p: own Person) { self.members.push(mv p); }
  fn leader(self) -> Person { return self.lead; }   // a view escapes
}

fn bump(n: mut int) { n = n + 1; }

fn main() -> int {
  ana: own = Person("Ana");
  t: own = Team(ana);          // ana -> mut parameter: implicit, shared
  t.add(Person("Bo"));         // fresh -> own: implicit
  t.add(cp ana);               // explicit deep clone
  u: own = cp t;               // new members, same lead
  u.lead.rename("Ann");        // t's lead is Ann too (shared link)
  w: own = mv t;               // t unusable until reassigned
  k: own int = 1;
  bump(k);                     // k == 2
  return 0;
}
```

## Prototype scope

- Opt-in: `paykan --ownership`. Without it nothing changes: the new keywords
  are not reserved and the checks do not run. The frontend option
  (`frontend::Options`) enables the keywords `own`, `mut`, `let`, `cp`, `mv`;
  Sema runs the checks; the lowering implements `cp`, `mv` and `mut` value
  parameters. Every existing test and sample is unchanged.
- Recursive-descent frontend only; the AST interchange carries the new nodes.
- Not in the prototype: closures, generics with qualifiers, `match`-binding
  qualifiers beyond the default view, cross-module checks of qualifiers in
  `.pkm` interfaces (modules are checked from source).

## Plan (stacked PRs against `proto/ownership`, about 500 changed lines each)

1. Syntax: keywords under the option, qualifiers in every declared position,
   `let`, `cp` / `mv` expressions, explicit `self`, AST, printer,
   interchange, `--ownership`. Parser tests.
2. Sema: access (view / `mut` / `own`), changing operations, deep rules,
   `self` qualifiers, `let`. Sema tests.
3. Sema: ownership transfer (`own` needs `cp` / `mv` / fresh), `mv` rules and
   use after move, field and result kind rules. Sema tests.
4. Lowering: `cp` (deep clone, per-class clone functions, `Str` and array
   clones) and `mv` (transfer, slot cleared). CodeGen tests on both backends.
5. Lowering: `mut` value parameters (address of the caller's storage).
6. Samples (`samples/ownership/`), a walkthrough, and the demo script.

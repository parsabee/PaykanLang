# PaykanLang — Optional Types (prototype)

> **Prototype.** Optional types are experimental (issue #5): implemented and tested, but the syntax,
> the rules and the diagnostics below may change before they are declared stable.

An **optional type** `T?` holds either a value of type `T` or `None`. It is how a program says
"this may be absent" without giving up the static type: a `next: Node?` field is a `Node` or
nothing — never a `Str` — and a `count: int?` is an `int` or nothing.

```pkn
class Node {
  v: int;
  next: Node?;                       // implicitly None
  fn __init__(x: int) { self.v = x; }
}

fn find(head: Node?, key: int) -> Node? {
  cur: Node? = head;
  while (cur != None) {
    match cur {
      n: Node {                      // n is the unwrapped Node
        if (n.v == key) { return n; }
        cur = n.next;
      }
      None { }
    }
  }
  return None;
}
```

---

## Quick Summary

- `T?` is written as a suffix and is allowed in every type position: variables, fields,
  parameters, return types, match arms, array element types (`Node?[]`) and arrays
  themselves (`int[]?`).
- Reference types (classes, `Str`, arrays) and the primitives `int`, `float`, `bool` and
  `char` can be optional (see [Optional Primitives](#optional-primitives)). `Enum?`, `void?`,
  `T??` and optional tuples are rejected.
- `None` is the absent value. A `T` converts to `T?` implicitly; a `T?` **never** converts back
  to `T` — unwrap it with `match`.
- On a `T?` you may: assign it, pass and return it, store it in fields and arrays, `mov` it,
  compare it with `==`/`!=` against `None` or another optional, and `match` on it. Everything
  else (fields, methods, subscripts, `+`, `toString()`) needs a `match` first.
- A field of type `T?` is `None` unless `__init__` assigns it.
- A `T?` may be used where an `Obj` is expected (so `println(maybe)` prints `None` or the
  value).

---

## Declaring and Assigning

```pkn
a: Node? = None;          // absent
b: Node? = Node(1);       // Node widens to Node?
n: Node = Node(2);
c: Node? = n;             // a plain Node is fine
a = b;                    // shares the same object
a = None;                 // back to absent
r = find(b, 1);           // inferred Node?
```

A `Leaf?` converts to `Node?` when `Leaf` is a subclass of `Node`. An `Obj` (other than the
literal `None`) does **not** convert to `Node?`: it might be anything.

The narrowing direction is always an error:

```pkn
m: Node = a;   // error: cannot use optional 'Node?' as 'Node' without unwrapping (use match)
```

The same diagnostic is reported for assignments, `return`, call arguments, and field or array
stores that would need the unwrapped value.

---

## Unwrapping with `match`

`match` on a `T?` subject has its own mode:

```pkn
match maybe {
  n: Node { println(Str<int>(n.v)); }   // present — any Node, whatever its runtime subclass
  None    { println("nothing"); }     // absent
}
```

- The arm naming the wrapped type `T` matches **every** present value and binds it as `T`. This
  differs from a class-typed subject, where a type arm is an exact-runtime-type test: for an
  optional, the `T` arm is the "present" pattern.
- An arm naming a **strict subclass** of `T` matches on exact runtime type, as in type mode,
  and must come before the `T` arm (an arm after the `T` arm is unreachable and rejected):

  ```pkn
  match maybe {
    l: Leaf { println("a leaf"); }
    n: Node { println("some other node"); }
    None    { }
  }
  ```

- `None` or `_` covers the absent case. A `T` arm **alone is not exhaustive**; `T` plus `None`
  (or `_`) is, and then the `match` counts as always returning / always assigning for the usual
  flow analyses:

  ```pkn
  fn value(n: Node?) -> int {
    match n {
      x: Node { return x.v; }
      None    { return -1; }        // without this arm: "does not always return a value"
    }
  }
  ```

- The subject can be any optional expression: a variable, a field (`match self.next { … }`), a
  call result, an array element. An optional array subject binds the array:

  ```pkn
  match nums {                      // nums: int[]?
    a: int[] { a.push(4); }
    None     { }
  }
  ```

Matching on an `Obj` subject is unchanged; in particular the `match Stdin.readln() { line: Str
{ … } _ { … } }` idiom keeps working exactly as before, and a `None` arm is only valid on an
optional subject.

---

## Comparing with `None`

`x == None` and `x != None` (in either order) test for absence directly and never call
`equals`:

```pkn
while (cur != None) { … }
if (find(head, 9) == None) { println("not found"); }
```

Two optionals can be compared with each other when their wrapped types are the same or
class-related: both `None` → equal, exactly one `None` → not equal, otherwise the ordinary
`equals` dispatch on the two present values (so `Str?` compares by content and a class's
`equals` override is honoured).

```pkn
a: Str? = "same";  b: Str? = "same";  c: Str? = None;
a == b   // True
a == c   // False
c == None  // True
```

Comparing an optional with a present, non-optional value (`a == Node(1)`) is an error: compare
against `None` or unwrap first.

---

## Optional Primitives

`int?`, `float?`, `bool?` and `char?` follow the same rules as a reference optional:

```pkn
fn half(n: int) -> int? {
  if (n % 2 != 0) { return None; }
  return n / 2;                      // an int widens to int?
}

fn show(o: int?) -> Str {
  match o {
    n: int { return Str(n); }     // n is a plain int
    None   { return "none"; }
  }
}
```

- The parsing conversions return optional primitives: `int<Str>(s)` is an `int?`,
  `float<Str>(s)` a `float?` and `bool<Str>(s)` a `bool?`, `None` for an invalid string;
  their boxed forms `Int<Str>(s)` & co. return the optional box `Int?` & co. (see
  `01-language-basics.md`).
- A primitive widens to its optional implicitly (`x: int? = 5`). An `int` also widens to
  `float?`, after the usual `int` -> `float` promotion (`f: float? = 3`).
- An `int?` does **not** convert to a `float?` (or any other optional). `int[]` is not an
  `int?[]` either. An array or tuple literal still works where its elements need boxing
  (`xs: int?[] = [1, 2]`, `p: (int?, Str) = (1, "a")`), but destructuring does not box:
  `a: int?, s: Str = (1, "a")` is an error. See [Literals take their destination's
  types](#literals-take-their-destinations-types) for `None` and mixed elements.
- In a `match` on an `int?` subject, the only type arm is `n: int`. It matches every present
  value and binds `n` to a plain `int`: an ordinary value, owning nothing, which the arm may
  reassign. Any other arm type is an error, and as for a reference optional, `n: int` plus
  `None` (or `_`) is exhaustive.
- `x == None` and `x != None` work. Two optionals of the same primitive compare equal when both
  are `None`, or when both are present and their values are `==`. For `float?` that means `NaN`
  is never equal to `NaN`, and `-0.0` equals `0.0`. Comparing an `int?` with a plain `int`
  (`x == 5`) is an error: unwrap it first.
- Optional primitives work everywhere a type is written. That includes fields (implicitly
  `None`), array elements, tuple elements, parameters, returns, ternaries
  (`if c then 3 else None` is an `int?`) and generic arguments (`Box<int?>`). A `T?` inside a
  generic class or function instantiated with `T = int` is an `int?`.
- In an `Obj` slot, a present value is its boxed class: `Int`, `Float`, `Bool` or `Char`. So
  `println(x)` prints `5` or `None`, and an `Obj` match can name `i: Int { … }`.
- `Enum?` is not supported yet ("optional enum types are not supported yet"). An enum would box
  as a bare `Int`, which in an `Obj` slot would print the variant's ordinal rather than its name.

At runtime, a present `int?` is an `Int` box: one heap object, released like any other
reference. `None` is the null box, as for every optional.

---

## Optional Fields

A field of type `T?` need not be assigned in `__init__`; it starts as `None`:

```pkn
class Node {
  v: int;
  next: Node?;                       // None until assigned
  fn __init__(x: int) { self.v = x; }
}
```

Non-optional fields keep the usual rule (assigned on every path through `__init__`).
Assigning `None` to an optional field releases whatever it held.

---

## Arrays

- **Array of optionals** — `Node?[]`: slots may hold `None`; `push(None)`, `xs[i] = None` and
  `pop()` (which yields a `Node?`) all work, and destroying the array releases only the present
  elements.
- **Optional array** — `int[]?`: the whole array may be absent; the empty literal `[]` takes
  the wrapped array type (`xs: Str[]? = []`).

```pkn
xs: Node?[] = [];
xs.push(None);
xs.push(Node(1));
xs[0] = Node(2);
match xs[1] { n: Node { … } None { … } }
```

An array literal containing `None` takes its destination's element type
(`xs: Node?[] = [None, n]`). Without a declared destination a bare `None` is `Obj`:
`xs = [None, Node(1)]` is an `Obj[]`, and `xs = [None, n]` (with `n: Node?`) is an error.
Annotate the variable, or build the array with `push`.

---

## Literals take their destination's types

An array or tuple literal flowing into a typed slot (a declaration, an assignment to an
existing variable, an argument, a return, a field or element store, `push`) takes the slot's
element types where its own elements need them, including in nested literals:

```pkn
a: (int?, Str) = (None, "a");             // None takes the slot's int?
b: (Node?, int) = (None, 1);              // ... or Node?
nb: int? = None;
tbl: (Str, int?)[] = [("a", 1), ("b", nb), ("c", None)];   // int and int? elements
nodes: Node?[] = [Node(1), None];
nested: (Str, (int?, Node?))[] = [("a", (1, None))];
```

A literal still has to fit: `(None, "a")` is not an `(int, Str)`, and
`[("a", 1), ("b", nb)]` is not a `(Str, int)[]` ("array literal element of type '(Str, int?)'
does not match the expected element type '(Str, int)'"). There is no promotion inside a tuple
literal either (`(1, 2)` is not a `(float, int)`). Where there is no destination, as in
`t = (None, 1)`, a `None` is `Obj` and the elements of an array literal must agree on their
own.

---

## Optionals as `Obj`

A `T?` converts to `Obj`, so it can be handed to anything that takes an `Obj` — most usefully
the print family:

```pkn
maybe: Node? = None;
println(maybe);        // None
o: Obj = maybe;        // o holds the None object
```

---

## Ternary

`if c then x else None` is a `T?` when `x` is a `T` (or a `T?`), and two optional branches
unify to the optional of their common ancestor:

```pkn
fn pick(c: bool) -> Str? { return if c then "yes" else None; }
```

---

## Memory

A `T?` of a reference type costs nothing extra: it is the same reference-counted box as a `T`,
with "no box" meaning `None`. A present optional primitive is one boxed `Int` / `Float` /
`Bool` / `Char` object; `None` allocates nothing. Retaining, releasing, moving and destroying an absent optional are no-ops, and
`mov` of an optional transfers the reference (or the absence) exactly like `mov` of a `T`. See
`08-memory-model.md`.

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| `optional enum types are not supported yet` | `Enum?` |
| `optional type 'void?' is not supported` | `void?` (at parse time, or a type argument `void`) |
| `nested optional type 'T??' is not supported` | `T??` |
| `cannot use optional 'T?' as 'T' without unwrapping (use match)` | narrowing on declaration / assignment / return / argument / field or array store; member access, method call, subscript, operator or unary on a `T?` |
| `cannot compare optional 'T?' with non-optional 'U'; compare against None or unwrap it with match` | `opt == present` |
| `operands of '==' have mismatched types 'A?' and 'B?'` | two optionals of unrelated types |
| `match on optional 'T?' requires type-name arms or 'None', not literal patterns` | a value literal arm on an optional subject |
| `type 'X' is not a subclass of 'T' (the match subject has type 'T?')` | an unrelated arm type |
| `match arm type 'T?' cannot be optional` | an optional arm type |
| `unreachable arm: the 'T' arm above already matches every non-None value` | a type arm after the `T` arm |
| `duplicate 'None' arm in match on 'T?'` | two `None` arms |
| `match arm type 'X[]' does not match the optional subject type 'T[]?'` | wrong array arm on an optional array |
| `match arm type 'float' does not match the optional subject type 'int?'` | an arm other than the primitive on an optional primitive |
| `non-void function 'f' does not always return a value` | a `T` arm without `None` / `_` as the last statement of a non-void function |

---

## Not in the prototype

Flow typing (`if (x != None) { x.foo(); }` does not narrow `x`), `if let`, the `??` default
and `?.` chaining operators, optional enums, and typing `None` in a literal that has no
declared destination (`xs = [None, n]`).

`None` itself keeps the static type `Obj`: it is accepted wherever a `T?` is expected, but an
`Obj` variable holding `None` is not (`n: Node? = someObj` is an error even when `someObj` is
`None` at run time). A `None` arm is accepted only on an optional subject; on an `Obj` subject
use `_`.

# PaykanLang — Optional Types (prototype)

> **Prototype.** Optional types are an experimental feature (issue #5). The syntax, the rules
> below and the diagnostics may change; see `proposals/optionals.md` for the design, the
> decisions behind it and what is deliberately left out.

An **optional type** `T?` holds either a value of the reference type `T` or `None`. It is how a
program says "this may be absent" without giving up the static type: a `next: Node?` field is a
`Node` or nothing — never a `Str`.

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
- Only **reference types** can be optional: classes, `Str`, arrays. `int?`, `float?`, `bool?`,
  `char?` and `Enum?` are rejected ("optional primitive types are not supported yet"), and so is
  `T??`.
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
  n: Node { println(StrInt(n.v)); }   // present — any Node, whatever its runtime subclass
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

Note that an array *literal* containing `None` (`[None, n]`) types as `Obj[]`, not `Node?[]`;
build such arrays with `push` (a known limitation of the prototype).

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

A `T?` costs nothing extra: it is the same reference-counted box as a `T`, with "no box" meaning
`None`. Retaining, releasing, moving and destroying an absent optional are no-ops, and
`mov` of an optional transfers the reference (or the absence) exactly like `mov` of a `T`. See
`08-memory-model.md`.

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| `optional primitive types are not supported yet` | `int?`, `float?`, `bool?`, `char?`, `void?`, `Enum?` |
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
| `non-void function 'f' does not always return a value` | a `T` arm without `None` / `_` as the last statement of a non-void function |

---

## Not in the prototype

Flow typing (`if (x != None) { x.foo(); }` does not narrow `x`), `if let`, the `??` default
and `?.` chaining operators, optional primitives, and array-literal typing with `None`
elements. Each is sketched in `proposals/optionals.md`.

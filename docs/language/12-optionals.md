# Optionals

> **Stable** (issue #5), optional primitives and optional-mode `match` included: the
> syntax and semantics below are covered by the compatibility promise for the 0.2 series (see
> [Stability](index.md#stability)).

Sometimes a value may be missing: a search finds nothing, a string is not a number, a task
has no deadline. PaykanLang expresses this with **optional types**. A `T?` holds either a
value of type `T` or `None`, without giving up the static type: a `next: Node?` field is a
`Node` or nothing, never a `Str`, and a `count: int?` is an `int` or nothing. The compiler
makes sure you check which before using the value. This chapter shows how to declare,
unwrap and compare optionals, and `tasks` learns to look tasks up and to give them an
optional deadline; then it gives the precise rules.

## Optional types

Add `?` to a type to make it optional: `Str?`, `int?`, `Task?`. Any class, `Str`, `int`,
`float`, `bool`, `char` and the sized numeric types can be optional, and so can enums,
tuples, structs and function values. `None` is the absent value:

```pkn
import ::io;

fn main() -> int {
  nickname: Str? = None;
  age: int? = 36;          // an int converts to int? automatically
  io::println(nickname);
  io::println(age);
  nickname = "Gracie";
  io::println(nickname);
  return 0;
}
```

Output:

```
None
36
Gracie
```

A `T` converts to `T?` automatically, so functions returning `T?` simply `return` a `T` or
`None`. The other direction never happens implicitly: a `T?` cannot be used as a `T`, not in
arithmetic, not to call a method, not to pass to a parameter of type `T`:

```pkn
fn main() -> int {
  age: int? = 36;
  nextYear = age + 1;
  return 0;
}
```

Error:

```
error: cannot use optional 'int?' as 'int' without unwrapping (use match)
```

## Getting the value out with match

`match` on an optional has an arm for the present case, which names the wrapped type and
binds the value, and an arm for `None`. Together the two are exhaustive:

```pkn
import ::io;

fn half(n: int) -> int? {
  if n % 2 != 0 { return None; }
  return n / 2;
}

fn describe(n: int) -> Str {
  match half(n) {
    h: int { return Str(n) + " halves to " + Str(h); }
    None   { return Str(n) + " is odd"; }
  }
}

fn main() -> int {
  io::println(describe(10));
  io::println(describe(7));
  return 0;
}
```

Output:

```
10 halves to 5
7 is odd
```

Inside the first arm, `h` is a plain `int`. With a class type the present arm matches every
object of that class *or any subclass*, unlike a class `match` on a non-optional value,
which compares exact classes.

To just test for presence, compare with `None`: `x == None` and `x != None` work on any
optional. But a test does not unwrap: after `if x != None` the type of `x` is still `T?`,
so you still need a `match` to use the value. Two optionals can be compared with `==` as
well; they are equal when both are `None` or both hold equal values (see
[Comparing with `None`](#comparing-with-none)).

## Fields that start as None

A field whose type is optional starts out as `None`, so `__init__` does not have to assign
it. This is the natural way to model something that is set later, or not at all:

```pkn
import ::io;

class Person {
  name: Str;
  email: Str?;

  view fn contact() -> Str {
    match self.email {
      e: Str { return self.name + " <" + e + ">"; }
      None   { return self.name + " (no email)"; }
    }
  }
}

impl Person {
  fn __init__(name: Str) { self.name = name; }
}

fn main() -> int {
  p = Person("Ada");
  io::println(p.contact());
  p.email = "ada@example.org";
  io::println(p.contact());
  return 0;
}
```

Output:

```
Ada (no email)
Ada <ada@example.org>
```

## Linked structures

Optionals make linked structures easy to express, because "no next node" is just `None`.
Here is a minimal linked list with a search that returns `Node?`:

```pkn
import ::io;

class Node {
  value: int;
  next: Node?;
}

impl Node {
  fn __init__(v: int, next: Node?) { self.value = v; self.next = next; }
}

fn find(head: Node?, wanted: int) -> Node? {
  cur = head;
  while cur != None {
    match cur {
      n: Node {
        if n.value == wanted { return n; }
        cur = n.next;
      }
      None { }
    }
  }
  return None;
}

fn main() -> int {
  list = Node(1, Node(2, Node(3, None)));
  match find(list, 2) {
    n: Node { io::println("found " + Str(n.value)); }
    None    { io::println("not found"); }
  }
  io::println(Str(find(list, 9) == None));
  return 0;
}
```

Output:

```
found 2
True
```

## Things to know

- `None` on its own has the type `Obj`, the root class. So `x = None;` declares an `Obj`
  variable, not an optional: write the type, as in `x: Task? = None;`. The same goes for
  an array literal holding `None`: `ts: Task?[8] = [None; 8]` needs its annotation.
- `T?[n]` is an array whose elements may be `None` ([Arrays](#arrays)); an array cannot
  itself be optional.
- Enums, tuples and structs have optionals too (`Direction?`, `(int, Str)?`, `Point?`),
  unwrapped with an arm naming the type: `d: Direction { ... }`. Such an optional holds its
  value in place, at most 64 bytes of it ([Optional Enums, Tuples and
  Structs](#optional-enums-tuples-and-structs)).
- There is no `?.`, `??` or `if let` shorthand in this release; `match` is the way to unwrap.
- The standard library says "nothing" with optionals too: a file read returns a result
  whose `value` is a `Str?`, `None` at the end of the file. [Input and
  output](../stdlib/io.md) shows how to `match` on those.

## Project: finding tasks and optional deadlines

Two changes to `tasks`. A deadline becomes an optional field instead of a subclass, so any
task can get one later. And `TaskList.find` looks a task up by its title, returning `None`
when there is none, which lets the caller decide what to do. It isn't a `view fn`, because
the caller may change the task it returns:

```pkn
import ::io;

class Task {
  title: Str;
  done: bool;
  due: Str?;

  view fn toString() -> Str {
    text = (if self.done then "[x] " else "[ ] ") + self.title;
    match self.due {
      d: Str { return text + " (due " + d + ")"; }
      None   { return text; }
    }
  }
}

impl Task {
  fn __init__(title: Str) {
    self.title = title;
    self.done = False;
  }
}

class TaskList {
  tasks: Task[8];
  count: int;

  fn add(title: Str) -> bool {
    if self.count == self.tasks.len() { return False; }
    self.tasks[self.count] = Task(title);
    self.count = self.count + 1;
    return True;
  }

  fn find(title: Str) -> Task? {
    i = 0;
    while i < self.count {
      if self.tasks[i].title == title { return self.tasks[i]; }
      i = i + 1;
    }
    return None;
  }
}

impl TaskList {
  fn __init__() {
    self.tasks = [Task(""); 8];
    self.count = 0;
  }
}

fn setDue(list: TaskList, title: Str, due: Str) {
  match list.find(title) {
    t: Task { t.due = due; io::println(t); }
    None    { io::println("no task called '" + title + "'"); }
  }
}

fn main() -> int {
  list = TaskList();
  list.add("file taxes");
  list.add("buy milk");
  setDue(list, "file taxes", "April 15");
  setDue(list, "walk the dog", "today");
  io::println(list.tasks[1]);
  return 0;
}
```

Output:

```
[ ] file taxes (due April 15)
no task called 'walk the dog'
[ ] buy milk
```

---

## Quick Summary

- `T?` is written as a suffix and is allowed in every type position: variables, fields,
  parameters, return types, tuple elements, type arguments and array element types
  (`Node?[3]`). A match arm names the type without the `?`.
- Reference types (classes, `Str`), the primitives `int`, `float`, `bool`, `char` and
  the sized numeric types (`u8?`, `f32?`, …), enums, tuples, structs and function values
  (`((int) -> int)?`, see [Function Values Are Values](14-function-types.md#function-values-are-values))
  can be optional (see [Optional Primitives](#optional-primitives) and [Optional Enums,
  Tuples and Structs](#optional-enums-tuples-and-structs)). `void?`, a written `T??` and optional
  fixed-size arrays (`int[3]?`) are rejected.
- `None` is the absent value. A `T` converts to `T?` implicitly; a `T?` **never** converts back
  to `T` — unwrap it with `match`.
- On a `T?` you may: assign it, pass and return it, store it in fields and arrays,
  compare it with `==`/`!=` against `None` or another optional, call `toString()` on it
  (`"None"` or the value's text), and `match` on it. Everything else (fields, other methods,
  subscripts, `+`) needs a `match` first.
- A field of type `T?` is `None` unless `__init__` assigns it.
- A `T?` is not an object: it never becomes an `Obj`, but an optional of a `Show` type is
  `Show` itself, so `io::println(maybe)` prints `None` or the value.

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
  n: Node { io::println(Str<int>(n.v)); }   // present — any Node, whatever its runtime subclass
  None    { io::println("nothing"); }     // absent
}
```

- The arm naming the wrapped type `T` matches **every** present value and binds it as `T`. This
  differs from a class-typed subject, where a type arm is an exact-runtime-type test: for an
  optional, the `T` arm is the "present" pattern.
- An arm naming a **strict subclass** of `T` matches on exact runtime type, as in type mode,
  and must come before the `T` arm (an arm after the `T` arm is unreachable and rejected):

  ```pkn
  match maybe {
    l: Leaf { io::println("a leaf"); }
    n: Node { io::println("some other node"); }
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
  call result, an array element.

Matching on an `Obj` subject is unchanged, and a `None` arm is only valid on an optional
subject. A read's result is a `Result`, an enum, not an optional: `match io::stdin().readln() {
line: Ok { … } _ { … } }` is a variant-mode match (see [Input and output](../stdlib/io.md)).

---

## Comparing with `None`

`x == None` and `x != None` (in either order) test for absence directly and never call
`equals`:

```pkn
while cur != None { … }
if find(head, 9) == None { io::println("not found"); }
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
  if n % 2 != 0 { return None; }
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
  `float<Str>(s)` a `float?` and `bool<Str>(s)` a `bool?`, `None` for an invalid string
  (see [Conversions](04-strings-and-conversions.md#conversions)).
- A primitive widens to its optional implicitly (`x: int? = 5`). An `int` also widens to
  `float?`, after the usual `int` -> `float` promotion (`f: float? = 3`).
- An `int?` does **not** convert to a `float?` (or any other optional). `int[2]` is not an
  `int?[2]` either. An array or tuple literal still works where its elements need widening
  (`xs: int?[2] = [1, 2]`, `p: (int?, Str) = (1, "a")`), but destructuring does not widen:
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
  generic class or function instantiated with `T = int` is an `int?`, and so is one
  instantiated with `T = int?`: a template's `T?` of a `T` that is already optional is `T`
  itself, never a nested optional ([Generics](13-generics.md)).
- An optional primitive is not an object (see [Optionals are not
  objects](#optionals-are-not-objects)): `o: Obj = x` is an error. `io::println(x)` prints `5` or
  `None`.

At runtime an `int?` is a value: the `int` and a presence flag, held inline in a variable, a
field, a parameter, an array element or a tuple element and copied like an `int`, with no heap
allocation and no reference count. `None` is the flag being clear.

---

## Optional Enums, Tuples and Structs

An enum, a tuple or a struct has an optional too, and like an optional primitive it is a
value, held in place with no heap allocation of its own. A `Suit?` is the enum's ordinal
and a presence flag. A `(int, Str)?` or a `Point?` is a presence flag followed by the
value, laid out like the tuple `(bool, T)`; `None` is all zeros. Copying one copies the
value, retaining the references it holds, except at a variable's last use, where an
assignment, a store or a `return` moves them ([Memory](16-memory.md#ownership-transfers)).
The value is at most 64 bytes: an optional of a
bigger tuple or struct is rejected (`'Big?': an optional holds a value of up to 64 bytes in
place, and 'Big' is 72 bytes; use 'Box<Big>?' (memory::Box) instead`), and the value goes in
a `memory::Box`, whose optional is a reference. A nested tuple literal widens into an
optional slot as a call's result does: `a: (int, (Str, Str)?) = (1, ("x", "y"));`.

They work as an optional primitive does: a present value widens implicitly
(`p: Point? = Point(1, 2)`), `None` is absent, `== None` / `!= None` test it, two of the
same type are equal when both are `None` or both are present and `==`, and a `match`
unwraps one with an arm naming the inner type (`q: Point`, `t: (int, Str)`, `s: Suit`)
plus `None`. They can be fields (`None` in a new object), array elements (`Point?[2]`),
tuple elements, parameters, results and ternary branches (`if c then p else None`). An
optional tuple, struct or enum prints `None` or the value's text (an enum's is its variant's
name).

```pkn
import ::io;

enum Suit { Hearts, Spades }
struct Point { x: int; y: int; }

fn find(n: int) -> Point? { return if n > 0 then Point(n, n * 2) else None; }

fn main() -> int {
  ps: Point?[2] = [find(3), find(0)];
  io::println(ps[0].toString() + " " + ps[1].toString());
  io::println(ps[0] == find(3));
  t: (int, Str)? = (2, "two");
  match t {
    v: (int, Str) { io::println(v.1); }
    None          { io::println("nothing"); }
  }
  s: Suit? = None;
  match s {
    k: Suit { io::println(k == Suit::Spades); }
    None    { io::println("no suit"); }
  }
  return 0;
}
```

Output:

```
Point(x: 3, y: 6) None
True
two
no suit
```

---

## Optional Fields

A field of type `T?` need not be assigned in `__init__`; it starts as `None` (as `email`
does in [Fields that start as None](#fields-that-start-as-none)).
Non-optional fields keep the usual rule (assigned on every path through `__init__`).
Assigning `None` to an optional field releases whatever it held.

---

## Arrays

An **array of optionals**, `Node?[n]`, may hold `None` in any slot: `xs[i] = None` works,
`xs[i]` is a `Node?`, and the array releases only its present elements. An array itself
cannot be optional for now (`int[3]?` is an error).

```pkn
xs: Node?[2] = [None, Node(1)];
xs[0] = Node(2);
xs[1] = None;
match xs[0] { n: Node { … } None { … } }
```

An array literal containing `None` takes its destination's element type
(`xs: Node?[2] = [None, n]`, and `slots: Node?[8] = [None; 8]` is eight empty slots).
Without a declared destination a bare `None` is `Obj`: `xs = [None, Node(1)]` is an
`Obj[2]`, and `xs = [None, n]` (with `n: Node?`) is an error. Annotate the variable.

---

## Literals take their destination's types

An array or tuple literal flowing into a typed slot (a declaration, an assignment to an
existing variable, an argument, a return, a field or element store) takes the slot's
element types where its own elements need them, including in nested literals:

```pkn
a: (int?, Str) = (None, "a");             // None takes the slot's int?
b: (Node?, int) = (None, 1);              // ... or Node?
nb: int? = None;
tbl: (Str, int?)[3] = [("a", 1), ("b", nb), ("c", None)];   // int and int? elements
nodes: Node?[2] = [Node(1), None];
nested: (Str, (int?, Node?))[1] = [("a", (1, None))];
```

A literal still has to fit: `(None, "a")` is not an `(int, Str)`, and
`[("a", 1), ("b", nb)]` is not a `(Str, int)[2]` ("array literal element of type '(Str, int?)'
does not match the element type '(Str, int)' of '(Str, int)[2]'"). There is no promotion inside a tuple
literal either (`(1, 2)` is not a `(float, int)`). Where there is no destination, as in
`t = (None, 1)`, a `None` is `Obj` and the elements of an array literal must agree on their
own.

---

## Optionals are not objects

An optional is a value that may be absent, not an object: a `T?` never converts to `Obj`, so
`o: Obj = maybe` (and passing, returning or storing it where an `Obj` is expected) is an error,
`cannot use optional 'Node?' as 'Obj': an optional is not an object; unwrap it with match`. It
still shows itself: an optional of a `Show` type implements `Show` (its `toString()` is
`"None"` or the value's text), so `io::println` and any other `T: Show` function take one as
it is:

```pkn
maybe: Node? = None;
io::println(maybe);        // None
n: int? = 5;
s: Str = n.toString();     // "5"
```

A `view` is never converted either: a `view T` cannot be passed to a `view T?` parameter or
bind a `view T?` local (`'v' is a 'view' parameter; a 'view' is not converted, and parameter 1
of 'take' is a 'view int?'`). A value that is not a `view` still widens (`take(5)`).

---

## Ternary

`if c then x else None` is a `T?` when `x` is a `T` (or a `T?`), and two optional branches
unify to the optional of their common ancestor:

```pkn
fn pick(c: bool) -> Str? { return if c then "yes" else None; }
```

---

## Memory

A `T?` of a reference type costs nothing extra: it is the same reference to a
reference-counted object as a `T`, with a null reference meaning `None`. An optional
primitive is a value, the primitive and a presence flag: it allocates nothing and is never
counted. An optional enum, tuple or struct is a value held in place in the same way, and an
optional function value is the function value's 16 bytes. An `int?[3]` holds its elements inline.
Retaining, releasing and destroying an absent optional are no-ops. See [Memory](16-memory.md).

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| `optional type 'void?' is not supported` | `void?` (at parse time, or a type argument `void`) |
| `nested optional type 'T??' is not supported` | a written `T??` |
| `a fixed-size array ('int[3]') can only be a local variable, a parameter, a result or a class field for now` | `int[3]?`: a fixed-size array cannot be optional |
| `cannot use optional 'T?' as 'T' without unwrapping (use match)` | narrowing on declaration / assignment / return / argument / field or array store; member access, method call, subscript, operator or unary on a `T?` |
| `cannot compare optional 'T?' with non-optional 'U'; compare against None or unwrap it with match` | `opt == present` |
| `operands of '==' have mismatched types 'A?' and 'B?'` | two optionals of unrelated types |
| `match on optional 'T?' requires type-name arms or 'None', not literal patterns` | a value literal arm on an optional subject |
| `type 'X' is not a subclass of 'T' (the match subject has type 'T?')` | an unrelated arm type |
| `match arm type 'T?' cannot be optional` | an optional arm type |
| `unreachable arm: the 'T' arm above already matches every non-None value` | a type arm after the `T` arm |
| `duplicate 'None' arm in match on 'T?'` | two `None` arms |
| `match arm type 'float' does not match the optional subject type 'int?'` | an arm other than the primitive on an optional primitive |
| `cannot use optional 'T?' as 'Obj': an optional is not an object` | a `T?` where an `Obj` is expected (declaration, assignment, argument, return, field or array store) |
| `'v' is a 'view' parameter; a 'view' is not converted, and parameter 1 of 'f' is a 'view T?'` | a `view T` passed to a `view T?` parameter, or bound to a `view T?` local |
| `non-void function 'f' does not always return a value` | a `T` arm without `None` / `_` as the last statement of a non-void function |

---

## Not supported yet

Flow typing (`if x != None { x.foo(); }` does not narrow `x`), `if let`, the `??` default
and `?.` chaining operators, optional fixed-size arrays, and typing `None` in a literal that has no
declared destination (`xs = [None, n]`).

`None` itself keeps the static type `Obj`: it is accepted wherever a `T?` is expected, but an
`Obj` variable holding `None` is not (`n: Node? = someObj` is an error even when `someObj` is
`None` at run time). A `None` arm is accepted only on an optional subject; on an `Obj` subject
use `_`.

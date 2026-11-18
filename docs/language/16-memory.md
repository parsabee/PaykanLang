# Memory

PaykanLang has no garbage collector and no `free`. Object lifetimes are managed by
**automatic reference counting** (ARC): every heap object carries a count of the references
to it, and the object is destroyed the moment the last one goes away, immediately and
deterministically. The compiler inserts the counting for you. What it counts is decided by
**ownership**: an object has one owner, the variable, field or element that holds it,
unless its reference is `shared`, and a value that holds one is handed on by `move`, copied
by `clone()` or lent to a borrow, never copied behind the program's back. This chapter
describes what the model guarantees, the ownership rule, how to check a program for leaks,
the one kind of leak ARC cannot prevent and how `weak` references, the `shared` references
that own nothing, avoid it, and then the details.

## Value Types vs Reference Types

Types come in two kinds, and the kind decides how a value is stored and whether ARC applies:

- **Value types**: `int`, `float`, `bool`, `char`, the sized numeric types (`u8`, `f32`, …),
  enums, arrays (`int[3]`), tuples (`(int, Str)`), structs and function values, and the
  optionals of the primitives, enums, tuples and structs. They live in their variable or
  field, so there is nothing to manage, and they are copied on assignment when every part of
  them is [Copyable](#ownership-transfers). An array, a tuple or a struct of objects or
  strings holds references to them, as a variable would, and owns them: it moves, as they
  do. A function value holds a reference to its closure, the object that keeps its
  captures, and is copied: the copies share the closure, which counts them (a function
  that captures nothing has no closure, and is just the function's address).
- **Reference types**: class instances (an `io::File` too), `Str`, and optional values of
  them. They live on the heap, and variables, fields, array elements and tuple elements hold
  references to them: one reference each, its owner's, unless the reference is `shared`.

In detail:

| Category | Types | Storage | On assignment / passing |
|----------|-------|---------|--------------------------|
| **Value** | `int`, `float`, `bool`, `char`, the sized numeric types (`u8`, `f32`, …, each in an 8-byte slot), `enum`, and their optionals `int?`, `float?`, `bool?`, `char?`, `u8?` | Stack / register (inline in a field) | Copied bit-for-bit; independent thereafter |
| **Array** | `T[n]` of any element type | Inline in the variable, the field or the enclosing array | Elements copied when Copyable (`shared` elements, and the `shared` references their structs hold, retained); moved otherwise |
| **Tuple** | `(T1, T2, ...)` of any element types | Inline in the variable, the field or the enclosing array or tuple | Elements copied when Copyable (`shared` elements, and the `shared` references their structs hold, retained); moved otherwise |
| **Struct** | `struct Name { ... }` of any field types | Inline, like a tuple of its fields | Fields copied when Copyable (`shared` references retained); moved otherwise |
| **Function value** | `(T1, T2) -> R`: a lambda or a named function | Inline, 16 bytes: the code's address and a reference to its closure, a heap object holding the captured variables (copied when the lambda is made; none when nothing is captured) | Moved: the code and the closure go together; the captures are released with the closure |
| **Reference** | class instances (an `io::File` among them), `Str`, optionals `T?` of these | Heap | Moved (`move`), cloned (`clone()`) or lent to a borrow; a `shared` reference copied, its count adjusted |

A value type has no identity beyond its bits: assigning one `int` to another produces two
independent values. A reference type has identity, and one owner: `q = move p` hands the
object to `q`, and `p` cannot be read again, while `q = p` is an error
([Ownership transfers](#ownership-transfers)). Two names refer to the **same** object only
when its reference is `shared` (or `shared mut`, to change it): then assigning one variable
to another makes both names refer to it, and a change through one is visible through the
other. A reference is **immutable unless it is written `mut`**: through a plain `Point` the
object's fields are read and its `view fn`s called, and nothing is assigned; a `mut Point`
is the reference that changes it. A `mut` reference is handed on as a plain one freely, and
a plain one becomes `mut` only by `move` ([Objects are
references](05-classes.md#objects-are-references); [Concurrency](19-concurrency.md) has the
qualifiers in full).

```pkn
a: int = 1;
b: int = a;      // independent copy
b = 2;           // a is still 1

p: mut Point = Point(1, 2);
q = move p;      // the same object, now q's: p is gone

s: shared mut Point = Point(1, 2);
t = s;           // same object — both names share it
s.x = 99;        // t.x is now 99 too
```

An array of numbers is a value: `ys = xs` copies its elements, so a later `ys[0] = 99` leaves
`xs[0]` as it was. A tuple is one too: `u = t` copies the elements of a `t` of numbers, and
`u` keeps them when `t` is assigned a new tuple.

## The Reference Counts

Each heap object carries a **reference count**. A unique object has one owner, and handing
it on with `move` changes no count: the reference itself moves. Copying a `shared` reference
(into a variable, a field, an array or a tuple) increments the count, and so does passing one
to a function that keeps it (see [Borrowed parameters](#borrowed-parameters)), or a
reference the compiler takes of its own while code it runs could let the object go; when a
reference goes away (a variable goes out of scope, a field or element is overwritten, an
object holding it is destroyed) the count is decremented. When it reaches zero, the object
is destroyed:
its own references are released, which may destroy further objects in turn, and its memory
is freed. You never write any of this; the compiler inserts the counting.

Every reference-counted object carries its counts in its own header, so an object is one heap
allocation and a reference to it is simply its address:

```
object: { vtable pointer, strong count, weak count, fields… }
```

The header is 16 bytes: the 8-byte vtable pointer and two 32-bit counts. An object can have up to
2,147,483,646 strong and 2,147,483,647 weak references; one more stops the program with `paykan: too many strong
references to one object (the limit is 2147483647)` rather than corrupting the count.

Two operations maintain the strong count:

- **retain** — increments the count when a new reference to the object is created (a copy of a
  `shared` reference in a variable, a field, an array or a function that keeps it).
- **release** — decrements the count when a reference goes away (a variable leaves scope, a field
  or slot is overwritten, a temporary is discarded).

Each is a few instructions in the compiled code, with no call: a retain adds one to the count in
the object's header and a release subtracts one. Only a release that brings the count to zero,
which destroys the object, and a retain at the limit, which stops the program, call into the
runtime.

The weak count is the number of `weak` variables and fields that refer to the object (see
[Weak References](#weak-references)); it never keeps the object alive.

The object is reached the same way regardless of its concrete class because every heap object
begins with a pointer to its **vtable** — the table of its methods, including `destroy`,
`toString`, and `equals` — followed by its two counts. Because the counts live in the object,
every alias of it (`self`, a `match` binding, a field, an array element) retains the same
count: the strong count is a single source of truth, and a new owning reference taken through
any alias is always right.

## Destruction

When a release drops the count to zero:

1. The object's `destroy` method runs. The compiler generates `destroy` to run the class's
   `__deinit__` and then each superclass's, where they declare one
   ([Classes](05-classes.md#deinitializers)), and then to **release every
   reference the object holds** — its reference-typed fields (the `weak` ones dropped), and
   the references inside its array, tuple, struct and function-value fields — so destruction cascades
   transitively.
2. The object's memory is freed. If `weak` references to the object remain, its memory is
   kept, empty, until the last of them goes away; every one of them reads as `None` from now
   on.

This is **deterministic**: an object is destroyed at the exact point its last reference is
released, in a well-defined order, with no background collector and no pause. An `io::File`, for
example, is closed as soon as the last reference to it goes away.

Destruction is **iterative**: deep chains are freed without stack growth. A linked list of a
million nodes, a deeply nested tree, or a long chain of objects linked through array or tuple
fields are destroyed in constant C stack space, so dropping them cannot overflow the stack. The
release that drops the last reference destroys its object; every object that `destroy` releases to zero is set aside
rather than destroyed on the spot, and the same release then destroys the set-aside objects one
by one until none remain. The order is fixed: a depth-first pre-order walk of the objects that
die, each object's references taken in field order (index order in an array or a tuple) — the
order in which a recursive destruction would start them — except that an object's `destroy`
finishes before the objects it released are destroyed. All of them are destroyed and freed before
that release returns.

`destroy` is **final**. A class cannot declare its own `destroy`, override it, or call
`obj.destroy()` explicitly — the compiler owns it entirely (see [Classes](05-classes.md)).
Cleanup that must happen at end of life goes in the class's `__deinit__`, which `destroy`
runs before it releases the fields; what the fields hold is released automatically after it.

A struct can declare a `__deinit__` too
([Structs](11-structs.md#running-code-when-a-value-is-destroyed-__deinit__)). A value has
no count: one with a `__deinit__` is move-only, so it is in one place at a time, and its
`__deinit__` runs where that place ends: the scope of the variable holding it, the object
or the value it is a part of, the assignment that replaces it. A value that was moved runs
nothing where it was.

When a scope ends, its variables are let go in the reverse of the order they were declared
in, references and values alike: the last one declared goes first.

## Assignment, Passing, and Returning

For reference types, the counting is balanced so an object stays alive exactly as long as some
reference names it:

- **Assignment** `x = move y` hands `y`'s reference to `x`, and `x = s` of a `shared` reference
  retains it; either way whatever `x` referred to before is released, so reassigning a
  variable cannot leak the previous value.
- **Passing to a plain parameter** hands the function a reference of its own: a temporary, a
  moved variable or a `clone()`, which keeps the object alive while the function holds it,
  until it returns or until it hands the reference on in turn. A `shared` argument the caller
  keeps alive for the whole call is lent instead, and the function takes a reference of its
  own to it only where it keeps it (see [Borrowed parameters](#borrowed-parameters)). A `view`
  or `inout` parameter is a borrow: the caller keeps the object, and no count changes.
- **Returning** transfers ownership of the result to the caller; `return x` of a local
  moves it.
- **Storing into a field or array slot** takes the stored reference (moved, or a retained
  copy of a `shared` one); overwriting a slot releases the value it previously held. A
  field's first assignment in `__init__` overwrites nothing: the object is new.

You do not write any of these retains and releases — they are emitted by the compiler. The
guarantee you can rely on is that an object is alive for as long as it is reachable through a live
reference, and is destroyed promptly once it is not.

## Checking for leaks: `--track-heap`

`paykan --track-heap` runs a program with a counting allocator and, when it exits, prints
statistics to standard error:

```text
$ paykan --track-heap hello.pkn
Hello, world!
paykan heap stats:
  total allocations : 6
  total frees       : 6
  total reallocs    : 0
  live blocks       : 0
  live bytes        : 0
  peak bytes        : 138
```

`live blocks` is the line to read: it counts the allocations that were never freed, and is
`0` for a program that does not leak. (The other numbers depend on the program and the
compiler version.) The test suite runs every example in this documentation, and every sample
program in the repository, this way.

`PAYKAN_TRACK_HEAP`, set to any value, does the same as `--track-heap` for `paykan run`, and
it is how an executable `paykan build` wrote is checked (`PAYKAN_TRACK_HEAP=1 ./hello`):
`--track-heap` applies to `run`, so `build` rejects it.

## Reference Cycles

Reference counting has one blind spot. It reclaims an object when its count reaches zero,
but if two or more objects refer to each other — a parent holding a child that holds the
parent back — each keeps the other's count above zero, so the group stays alive even after
the program has dropped every reference to it. Unique references cannot form one: an object
has one owner, so the objects form a tree, and no object holds its own owner. A cycle needs
`shared` references, several owners of one object, and **such cycles are not collected and
will leak**:

```text
import ::io;

class Person {
  name: Str;
  friend: shared mut Person?;
}

impl Person {
  fn __init__(name: Str) { self.name = move name; }
}

fn main() -> int {
  let a: shared mut Person = Person("Ada");
  let b: shared mut Person = Person("Grace");
  a.friend = b;
  b.friend = a;          // a -> b -> a: a cycle
  io::println(a.name + " and " + b.name + " are friends");
  return 0;
}
```

```text
$ paykan --track-heap friends.pkn
Ada and Grace are friends
paykan heap stats:
  ...
  live blocks       : 6
  live bytes        : 138
  ...
  ** LEAK: 6 block(s) / 138 byte(s) not freed **
```

PaykanLang has no cycle collector. The fix is a **weak reference**: a field (or a local)
whose type is qualified `weak` is a `shared` reference that owns nothing. It refers to a
shared object without counting as a reference to it, so it never keeps the object alive.
Its type is an optional, because it reads as `None` once the object is gone. Make the back
link weak and the cycle is no cycle at all; the people are `shared mut`, so the link is
`weak mut`, the form that names a `shared mut` object:

```pkn
import ::io;

class Person {
  name: Str;
  friend: weak mut Person?;
}

impl Person {
  fn __init__(name: Str) { self.name = move name; self.friend = None; }
  view fn friendName() -> Str {
    match self.friend {
      f: Person { return f.name.clone(); }
      None { return "nobody"; }
    }
  }
}

fn main() -> int {
  let a: shared mut Person = Person("Ada");
  b: shared mut Person = Person("Grace");
  a.friend = b;
  b.friend = a;
  io::println(a.name + " and " + a.friendName() + " are friends");
  b = Person("Alan");    // Grace was held only by b: she is freed
  io::println(a.name + " and " + a.friendName() + " are friends");
  return 0;
}
```

Output:

```
Ada and Grace are friends
Ada and nobody are friends
```

This version ends with `live blocks : 0`. Make the back link (the child's `parent`, an
observer's `owner`, a closure's reference to the object that stores it) the `weak` field or
variable, and the objects are freed with their last outside reference. Where no link is
naturally the weak one, you can still break a cycle by hand, overwriting one of the linking
fields with `None` before the objects go out of scope. Structures where links only point one
way, such as a list where each node refers to the next, or a tree where parents refer to
children but not back, never form cycles and need no care.

A closure is an object too, and its captures are its fields: a lambda that captures an object
(a `shared` reference, since a capture is a copy) and is then stored in that object (`h.cb =
fn() -> int { return h.n; };` with `h` a `shared mut Holder`) forms a cycle like any other,
and leaks the same way unless the capture is a `weak` variable (`me: weak mut = h; h.cb =
fn() -> int { match me { ... } };`) or the field is overwritten before `h` goes out of
scope.

## Weak References

A **weak reference** is a `shared` reference that owns nothing: it refers to a shared
object without being counted as one of its owners, so it never keeps the object alive, and
it reads as `None` once the object is gone. `weak` qualifies the type of a local or a class
field, after the colon, in the two forms a shared object has:

- `x: weak T?` names a `shared` object, and reads as a `shared T?`: the object is read
  through it;
- `x: weak mut T?` names a `shared mut` object, and reads as a `shared mut T?`: the object
  is changed through it too.

Its type is `T?` for a class `T` (a user class, `Str` or `Obj`). It may be left off a local
declaration that has an initializer (`u: weak mut = a;`), and a local with no initializer
starts as `None` (`e: weak Node?;`). Here a tree's nodes are held `shared mut`, by `main`
and by their parents, and each child's back link to its parent is `weak mut`, so the tree
holds no cycle:

```pkn
import ::io;

class Node {
  name: Str;
  parent: weak mut Node?;   // a back link that does not own the parent
  first: shared mut Node?;
  next: shared mut Node?;
}

impl Node {
  fn __init__(name: Str) {
    self.name = move name;
    self.parent = None;
    self.first = None;
    self.next = None;
  }
  shared fn add(child: shared mut Node) {   // stores self: a 'shared fn'
    child.parent = self;
    child.next = self.first;
    self.first = child;
  }
  view fn path() -> Str {
    match self.parent {
      p: Node { return p.path() + "/" + self.name; }   // p reads the parent
      None { return self.name.clone(); }               // a string of its own
    }
  }
}

fn main() -> int {
  root: shared mut Node = Node("root");
  let a: shared mut Node = Node("a");   // held by main and by root
  root.add(a);
  w: weak mut Node? = root;   // reads as root while something holds it
  u: weak mut = a;            // inferred Node?
  io::println(a.path() + " " + Str(w == None));
  root = Node("other");       // the first root dies: it was held by nothing else
  io::println(a.path() + " " + Str(w == None));
  match u {
    n: Node { n.name = "b"; io::println(a.name); }   // n is a 'shared mut Node'
    None { io::println("gone"); }
  }
  return 0;
}
```

Output:

```
root/a False
a True
b
```

Reading a weak variable or field gives the object while it is alive and `None` once it has
been destroyed, as the optional of its form: a `weak` one reads as a `shared T?`, a `weak
mut` one as a `shared mut T?`. The value read is an ordinary optional of that form,
matched with `match`, compared with `== None`, passed (to a `view` parameter or one of that
form), returned, or stored in a strong variable or field, which then keeps the object
alive. The `T` a `match` arm binds from it has the same form: through a `weak mut`
reference the object is changed (`n.name = "b"` above), and through a `weak` one it is only
read (`'n' is immutable (a 'shared Node'; write 'shared mut Node' to change it); cannot
assign to its field 'name'`).

Storing makes a weak variable or field refer to an object without retaining it, and
assigning `None` makes it refer to nothing. What is stored has exactly the weak reference's
form: a `shared T` (or `shared T?`) in a `weak` one, a `shared mut T` in a `weak mut` one,
and `None` in either. Anything else is an error, and so is passing it to the parameter of a
generated `__init__` that fills a weak field:

```pkn
class Node {
  name: Str;
  next: shared mut Node?;
  tail: weak mut Node?;
}

impl Node {
  fn link(n: shared mut Node) { n.tail = self; }
}

fn main() -> int {
  let a: mut = Node("a", None, None);
  let b: shared = Node("b", None, None);
  let c: shared mut = Node("c", None, None);
  c.tail = a;
  c.tail = b;
  w: weak = c;
  let d = Node("d", None, b);
  return 0;
}
```

Error:

```
cannot store 'self' (a 'mut Node') in field 'tail', a 'weak mut Node?': a 'weak mut' reference names a 'shared mut' object; a 'shared fn' has a 'shared mut' 'self'
cannot store 'a' (a 'mut Node') in field 'tail', a 'weak mut Node?': a 'weak mut' reference names a 'shared mut' object; declare 'a' 'shared mut Node'
cannot store 'b' (a 'shared Node') in field 'tail', a 'weak mut Node?': a 'weak mut' reference names a 'shared mut' object
cannot store 'c' (a 'shared mut Node') in variable 'w', a 'weak Node?': a 'weak' reference names a 'shared' object
cannot pass 'b' (a 'shared Node') to parameter 'tail' of 'Node', a 'weak mut' field's: a 'weak mut' reference names a 'shared mut' object
```

So **a unique object is never weakly referenced**. It has one reference, its owner's, and
unique references form a tree, which has no cycle to break; a weak reference to one would be
a second name for an object its owner believes it alone holds. A weak reference names an
object that already has several owners, and a back link needs its object shared: the
nodes of a tree held `shared mut`, as above, and a method that stores `self` in a weak
field declared with a shared `self`. A plain method borrows a unique receiver, so its
`self` cannot be stored (`link` above); a `shared fn`, whose `self` is `shared mut`, stores
it in a `weak mut` field or local (`add` above), and a `shared view fn`, whose `self` is
`shared`, in a `weak` one. Each is called on a receiver held in exactly that form, or on a
temporary ([Methods that hand on self](05-classes.md#methods-that-hand-on-self-shared-fn)).
A store takes nothing from the object's owners: `u: weak mut = a;` above, and `child.parent
= self` in `add`, leave `a` and `self` as they were.

A `weak` local written without a type takes the class of its initializer (`u: weak mut =
a;` above is a `Node?`) and the form written, `weak` or `weak mut`. `None` has no class:
`a: weak = None;` is an error (`cannot infer the class of weak local 'a' from None; write
'a: weak T? = None'`), and `a: weak Node? = None;` starts it as nothing. An `Obj` that holds
`None` is the `None` value, not nothing: `a: weak = x;` with `x` a `shared Obj` after `x =
None;` refers to it and never reads `None` ([Optionals](12-optionals.md#not-supported-yet)).

A lambda that captures a `weak` variable captures it weakly, so a closure can refer to the
object that stores it without forming a cycle (see
[Captures](14-function-types.md#captures)). So does a lambda written inside that lambda,
however deeply nested: a closure that an outer closure returns (`let outer = fn() -> () ->
Str { return fn() -> Str => if me == None then "gone" else "alive"; };`) holds the object
weakly too, and reads `None` once it is gone. Inside the lambda the captured variable is
read-only, as any capture is; the object a `weak mut` capture reads is changed through it as
through any `shared mut` reference.

A weak reference is not a place a strong reference could be written to, so it cannot be
passed to an `inout` parameter, named by an `inout` local, or bound by an `inout` match arm
(`'w' is a 'weak' reference and cannot be passed to 'inout' parameter 'n'`). It can be
`view`ed, since a view only reads it. `weak` takes `mut` and nothing else (`'weak' takes
'mut' or nothing: 'weak Node?' names a 'shared' object, 'weak mut Node?' a 'shared mut'
one`), and a weak local is never `let` (`a 'weak' local cannot be 'let': ...`): it is
assigned as what it names changes. For now `weak` applies to locals and class fields: not
to struct fields, tuple or array elements, parameters or results. The spelling of earlier
releases, `weak parent: Node?;` and `weak w = x;`, is an error that writes the declaration
with the qualifier (`'weak' goes after the colon, before the type: write 'parent: weak
Node?' ...`).

A `weak` local may name what a `view` names: a `view` parameter, or part of `self` in a
`view fn`. It takes no reference of its own, so the view is not copied. What it reads is the
view's object, so its reads are `view`s too: a `match` arm's name for it is bound to a `view`,
which is read, or passed on to a `view` parameter, but never changed, stored, returned (`'n'
is bound to a 'view'; it cannot be returned`) or captured by a function (`'cur' is a 'weak'
local of a 'view'; a function cannot capture a borrow`). It may be assigned another `view`.
That is how a `view fn` walks a chain of shared objects reached from `self` without copying
a reference out of the view (a plain local could not hold one): with `head` and each node's
`next` a `shared mut` field, `cur: weak mut = self.head;` then `match cur { n: Node { ...;
cur = n.next; } None { } }`. A method that may change `self` walks one the same way and
returns what it finds, since there `self` is not a `view`. The standard library's
[`LinkedList`](../stdlib/collections.md#linkedlist) is such a chain: its nodes are `shared
mut`, each owning the next and reaching the one before it by a `weak mut` reference.

The object's memory is kept, empty, while `weak` references to it remain (its own references
are released when it is destroyed), and freed with the last of them — so a program that drops
its weak references frees everything, as `--track-heap` shows.

## Ownership transfers

Every value has an owner: the variable, parameter, field or element that holds it. A value
is **Copyable** or **move-only**, and which it is decides what reading it from one place into
another does.

- **Copyable** values copy freely: numbers, `bool`, `char`, `shared T` and `shared mut T`
  references (a copy is another owner of the same object, and is counted), `atomic`,
  `atomic shared` and `locked` handles, function values (a copy shares the closure and
  counts it; one with no captures is just a function's address), and a struct, a tuple, a
  fixed-size array, an optional or an enum made only of those
  ([Copyable](06-traits.md#copyable)).
- **Move-only** values are a unique reference (`T` or `mut T`, a class's reference without
  `shared`), a `Str`, a container (`collections::List<T>`, `memory::Buffer<T>`, of numbers
  too), a task handle, and every struct, tuple, array, optional or enum holding one.

A move-only value read from a place (a variable, a parameter, a field, an element) goes into
an **owning slot**, a variable's initialisation or assignment, a plain (by-value) parameter
(an imported function's or constructor's too, and an enum variant's field,
`Shape::Named(move t)`), a field or an element it is stored in, a literal's element, a
`return`, only by one of:

- `move x`, with `x` a local variable (a `let` one too) or a by-value parameter, which ends
  `x`: reading it again is an error that names the move. `move x.f` takes a unique field out
  of such a variable and ends `x` ([`move`](19-concurrency.md#move-an-explicit-transfer)).
- `x.clone()`, a new value, when the type is [Cloneable](06-traits.md#cloneable): a `Str`,
  an aggregate of Cloneable parts, a class with an `impl Cloneable for` block.
- A borrow instead of an owning slot: a `view` parameter, which only reads, or an `inout`
  one, which changes the caller's storage in place
  ([Parameter modes](03-functions.md#parameter-modes-view-and-inout)).
- `shared`: a reference declared `shared` (or `shared mut`) from the start is Copyable, for
  a program in which several names hold one object.

Anything else is an error that names the ways out, for a variable, a value holding a
move-only part, a Cloneable type and a field:

```pkn
class Node {
  name: Str;
}

struct Person {
  name: Str;
  age: int;
}

class Holder {
  node: Node;
}

fn keep(n: Node) -> int { return n.name.len(); }

fn main() -> int {
  let a = Node("a");
  let b = a;
  let p = Person("Ann", 30);
  let q = p;
  let s = "text";
  let t = s;
  let h = Holder(Node("h"));
  let n = h.node;
  let c = Node("c");
  let k = keep(move c);
  let j = c.name;
  return 0;
}
```

Error:

```
cannot copy 'a' (a unique 'Node'); write 'move a' to transfer it, or declare it 'shared Node'
cannot copy 'p': 'Person' holds unique field 'name' ('Str'); write 'move p' to transfer it, or 'p.clone()' to copy it
cannot copy 's' (a unique 'Str'); write 'move s' to transfer it, 's.clone()' to copy it, or declare it 'shared Str'
cannot copy 'h.node' (a unique 'Node'); a field leaves by 'mem::replace' or 'mem::take', or is declared 'shared Node'
'c' was moved at 26:16; it cannot be used after the move
```

A temporary needs none of this: a call's result, a constructor, a literal, an operator's
result and `move x` itself are owned already, and go into any slot. So does a Copyable value
read from anywhere. Each way out keeps one owner per object, or says that there are several:

```pkn
import ::io;

class Node {
  name: Str;
}

impl Cloneable for Node {}

fn make(name: Str) -> Node {
  let n = Node(move name);           // the parameter's string becomes the node's
  return n;                          // a return moves n
}

fn show(n: view Node) -> Str { return n.name.clone(); }   // a borrow: the caller keeps n
fn keep(n: Node) -> Str { return move n.name; }            // owns n, and gives up its name

fn main() -> int {
  let a = make("a");
  let b = move a;                    // b owns the node; a is gone
  let c = b.clone();                 // a second node, with a name of its own
  io::println(show(b) + " " + keep(move c));
  let s: shared Node = make("s");
  let t = s;                         // shared: two owners of one node
  io::println(show(s) + " " + s.name + t.name);
  return 0;
}
```

Output:

```
a a
s ss
```

`move n.name` in `keep` takes a field out of a variable the function owns, which ends the
variable. An object that runs a `__deinit__` ([Classes](05-classes.md#deinitializers)) gives
up no field this way, since its `__deinit__` runs on the whole object: `cannot move 'c.name'
out of 'c': a 'C' has a '__deinit__', which runs on the whole object; write
'mem::replace(c.name, v)'`. That is so when the object's class declares one, when a
superclass does, and when a class the module derives from it does (the object may be one of
those). A class that another module derives is not known where the field is moved, so its
`__deinit__` is not to read a field that the module of its superclass moves out.

### `return` moves

A `return` is always the last use of what it returns, so `return n` of a local variable or a
by-value parameter moves it, with no `move` written: `make` above returns its node so. What
`return` gives back from a field or an element is not a variable, and follows the rule: in a
method, `return self.name;` of a `Str` field is `cannot copy 'self.name' ...`, and the method
returns a `clone()`, or a `view` of the field (`view fn name() -> view Str`,
[Borrowed results](03-functions.md#borrowed-results)). A literal returned is an owning slot
for each of its elements: `return (left, right);` of two `Str` locals writes `return (move
left, move right);`.

A literal's element takes its slot's form, or with none its own `shared` one: `(1, s)` with
`s` a `shared Node` is an `(int, shared Node)` and `[s; 3]` a `shared Node[3]`, while into
a `Node` element it is `cannot store a 'shared Node' in an element of the tuple, a 'Node': a
'shared' reference, a frozen object and a cell convert only as they are`. A frozen object or
a cell goes in a struct's field, not an element ([Qualifiers in a
type](19-concurrency.md#qualifiers-in-a-type)).

### Fields and elements: `mem::replace`, `mem::take` and `mem::swap`

`move` takes a variable. A move-only value in a field leaves it only in exchange for
another, by the functions of the [`::memory`](../stdlib/memory.md) module (`import ::memory
as mem;`):

- `mem::replace(slot, v)` returns what `slot` holds and stores `v` there;
- `mem::take(slot)`, for a `T?` slot, returns what it holds and leaves `None`;
- `mem::swap(a, b)` exchanges the values of two places.

Each takes its places as `inout` parameters, so a field is one of a `mut` object, and a
variable is one that is not `let` (a `mut` one when it holds an object or a string). An array
element cannot be passed to an `inout` parameter yet, so these do not reach one: an element
of a fixed-size array is assigned, and copied out with `clone()`.

```pkn
import ::io;
import ::memory as mem;

class Node {
  name: Str;
}

class Holder {
  node: Node;
  spare: Node?;
}

fn main() -> int {
  let h: mut = Holder(Node("first"), Node("spare"));
  let old = mem::replace(h.node, Node("second"));   // h keeps a node: the new one
  let spare = mem::take(h.spare);                   // h.spare is None now
  a: mut = "left";
  b: mut = "right";
  mem::swap(a, b);
  io::println(old.name + " " + h.node.name + " " + Str(h.spare == None) + " " + a + " " + b);
  match spare {
    n: Node { io::println(n.name); }
    None { }
  }
  return 0;
}
```

Output:

```
first second True right left
spare
```

### `match` bindings borrow

On a variable, a parameter or a field, a `match` arm's binding of a move-only part is a
borrow of the subject, a `view`: the subject keeps the object, and the binding reads it and
is passed to `view` parameters, but is not stored, moved, returned or written through (`'n'
is a 'match' binding, a borrow of 'x' ('match move x' binds what it owns); it cannot be
stored, only read or passed to a 'view' parameter`).
`match move x { n: Node { ... } }` binds owned parts instead, and moves `x`; an `n: inout
Node` arm writes through to the subject; the binding of a temporary (`match find() { ... }`)
is owned, as a local is; and a Copyable part is copied ([Bindings](08-match.md#bindings)).

### When an object is destroyed

A move releases nothing early: the destination holds the reference and releases it at the end
of *its* scope: a parameter when its function returns, a variable when its scope ends, a field
when its object is destroyed or the field is assigned again. What a move changes is *when*
an object is destroyed: it lives as long as its last holder, which after a move is the
destination, so it may be destroyed before the variable's scope would have ended. Two things
can see that. A [`weak` reference](#weak-references) elsewhere reads `None` as soon as the
destination lets go: the object is a `shared` one then, the only kind a weak reference
names, and `move` hands on a `shared` variable's reference as it does a unique one's. And
what destroying the object does happens then: an
[`io::File`](../stdlib/io.md) moved into a function is closed, and what was written to it is
on disk, when that function returns (unless it keeps the file), so a later `open` of the
same path in the caller reads it back.

```pkn
import ::io;

class Node {
  name: Str;
}

class Watcher {
  target: weak Node?;
}

fn consume(n: shared Node) -> Str {
  return n.name.clone();
}

fn report(w: view Watcher) {
  match w.target {
    t: Node { io::println("alive: " + t.name); }
    None { io::println("gone"); }
  }
}

fn main() -> int {
  let n: shared = Node("first");
  let w = Watcher(n);
  report(w);
  io::println(consume(move n));   // the move: consume holds the only reference
  report(w);                      // consume released it when it returned
  return 0;
}
```

Output:

```
alive: first
first
gone
```

### Transfers at a last use

What is copied still costs a count: a `shared` reference, and a value holding one, is
retained when it is copied and released when the copy goes away. Where a variable is not
read again after such a copy, the compiler makes the copy a **transfer**, an optimisation
that the rule above does not depend on: the reference moves to the destination without a
retain, and the variable releases nothing at the end of its scope, as a `move` would have
done. The program may do exactly what it may do without it. What a transfer can change is
when an object is destroyed, as a move can ([When an object is
destroyed](#when-an-object-is-destroyed)): the destination may be its last holder, and a
`weak` reference to it reads `None` once the destination lets go.

A copy the variable is read after stays a copy: later in the function, or on the next
iteration of a loop (a variable assigned a new value before it is read again is not). So
does one the function follows, before the variable's scope ends, with a call or the release
of a reference (the one an assignment replaces, say) while it uses an object it holds no
reference to: a field's value reached through another variable, or anything reached through
`self`. Where one branch of an `if` or one arm of a `match` hands the variable on and
another does not, the other releases it when its scope ends. A copy of the variable while a
`weak` local made from it (`u: weak = n;`) is in scope stays a copy, whether or not that local
is read again, so the local reads the object until the variable's scope ends. `self`, `inout`
parameters and locals, fields, array elements and the members of a tuple or struct, and
`view` parameters and variables, are never transferred, and a lambda's captures are always
copies. The compiler's `--stats` option counts the transfers it made ([docs/pir.md
§12](../pir.md#12-passes) has the details).

## Reference counts the compiler removes

The compiler also removes a retain together with the releases that undo it, wherever
something else keeps the object alive in between, so that no count changes where something
could see the difference. A `match` on a field takes a reference of its own for the whole
match (an arm may assign the field), and its binding takes another; the binding's goes, since
the match's keeps the object alive until the match ends. The match's own goes too when
nothing in it can let go of a reference (no function or method call, no assignment of a
reference): then the field it read keeps the object. A reference read from a variable for one
operation (an operand of `==`, say) needs none of its own while nothing reassigns the
variable, and one read from a field none while nothing can let go of a reference.

The compiler keeps a reference whenever code that may let the object go runs while it is in
use: the match's when an arm calls code, which may assign the field the match read and drop
the object the arm is using, and every reference a read through an object takes for a call
([Reads a call may replace](../pir.md#7-what-the-lowering-makes-explicit-the-ownership-rules)).
Here `clear` sets `l.head` to `None` while the arm still uses `n`, the node `l.head` held:

```pkn
import ::io;

class Node {
  name: Str;
}

class List {
  head: Node?;
}

fn clear(l: shared mut List) -> int {
  l.head = None;
  return 0;
}

fn main() -> int {
  let l: shared mut List = List(Node("first"));
  match l.head {
    n: Node {
      let k = clear(l);
      io::println(n.name + " " + Str(k));   // n lives until the match ends
    }
    None { }
  }
  return 0;
}
```

Output:

```
first 0
```

Nothing else changes: an object is destroyed at the same point as without these removals, a
`weak` reference reads what it would, and the program frees the same objects. `--stats`
counts the retains and releases removed ([docs/pir.md §12](../pir.md#12-passes) has the
rules).

## Borrowed parameters

A plain parameter owns its argument, and what the caller gives it decides what that costs. A
temporary, a moved variable and a `clone()` are handed over: the function releases them when
it is done, or hands them on. A `shared` argument, which the caller may go on using, is
**lent** when the caller keeps a reference of its own to the object for the whole call: a
variable it reads again afterwards, a parameter of its own, or a field nothing in the call
can replace. A lent argument costs no count at all where the function only reads it or
passes it on: neither the caller nor the function retains it, and the function does not
release it. Where the function keeps it (stores it in a field or a variable, returns it), it
retains it there, so the place it is kept in has a reference of its own. Which way an
argument goes is decided at each call; nothing in the function's declaration says it. A
`view` or `inout` parameter is a borrow by its declaration: it owns nothing, and costs no
count either way.

```pkn
import ::io;

class Node {
  name: Str;
}

class Watcher {
  target: weak Node?;
}

fn state(w: view Watcher) -> Str {
  match w.target {
    Node { return "alive"; }
    None { return "gone"; }
  }
}

// Only reads its parameter: lent, its count never changes.
fn show(n: shared Node) {
  io::println(n.name);
}

class Holder {
  n: shared Node?;
}

// Keeps its parameter: the field takes a reference of its own.
fn keep(h: inout Holder, n: shared Node) {
  h.n = n;
}

// Given its parameter by 'move': handed over, it dies when consume returns.
fn consume(n: shared Node) {
  io::println(n.name);
}

fn main() -> int {
  let a: shared Node = Node("a");
  let w = Watcher(a);
  show(a);                    // lent: main reads a again below
  h: mut = Holder(None);
  keep(h, a);                 // lent too: the field takes its own
  h.n = None;
  io::println("after the field let go: " + state(w));
  show(a);
  let b: shared = Node("b");
  let v = Watcher(b);
  consume(move b);            // handed over: b dies in consume
  io::println("after consume: " + state(v));
  return 0;
}
```

Output:

```
a
after the field let go: alive
a
b
after consume: gone
```

A function that assigns its parameter (`n = Node("b")`, `s += "!"`, passing it to an `inout`
parameter) owns it from its start instead, as every function did before: then the parameter
is its own variable. Nothing else changes: an object is destroyed at the same point whether
it is lent or not, a `weak` reference reads what it would, and the program frees the same
objects. `--stats` counts the arguments lent and the parameters
([docs/pir.md §12](../pir.md#12-passes) has the rules).

## Strings, Arrays and Tuples

`Str` is a heap, reference-counted class. String literals and the results of `+` concatenation
are reference-counted objects like any other, and a `Str` is move-only as every unique object
is: it is handed on by `move`, copied by `s.clone()` (a new string) or shared as a `shared
Str`. A string is an object, so it is written through
a `mut Str` only: `concat` appends to it in place (`let s = "a"; s.concat("b");` is `'s' is
immutable (a 'Str'; write 'mut Str' to change it); 'concat' is not a 'view fn'`), where
`s + "b"` reads `s` and makes a new string.

An array `T[n]` is a value, not an object: it has no box and no count of its own, and lives
where its variable or field does, inline in the stack frame or in the object. How it treats its
**elements** depends on the element type:

- An array of a **value type** (`int[3]`, `float[3]`, `bool[3]`, an enum array) stores the
  scalar values directly; there is nothing to reference-count per element.
- An array of a **reference type** (`Str[3]`, `Point[3]`, …) stores references and owns
  each element it holds. Overwriting a slot releases the old element, and the array releases
  all its elements when its variable goes out of scope or the object holding it is destroyed.

Assigning an array, or passing it to a plain parameter, copies it when its elements are
Copyable: a copy of an array of `shared` references (`shared Node[4]`), or of structs
holding them, retains each one's object, which the two arrays then share
([Qualifiers in a type](19-concurrency.md#qualifiers-in-a-type)). An array of move-only
elements is moved instead, its references with it, and copied only by `clone()`
([Ownership transfers](#ownership-transfers)). See [Arrays](09-arrays.md).

A tuple is a value in the same way: it has no box and no count, and its elements are laid out
in place, one after another like a C struct's fields, in its variable, its field or the array
element that holds it. A tuple of value types (`(int, float)`) allocates nothing; a reference
element (`Str`, a class, `Node?`) is a reference to the object, which the tuple holds. A
tuple of Copyable elements is copied (an assignment, a plain parameter, a result, an array
element), and each copy retains the `shared` references it and its structs hold once more
(`(int, shared Node)`); one holding a move-only element moves, its references with it, and
the variable it leaves releases none.
A variable's references are released when it goes out of scope, a field's when its object
is destroyed, an array element's with its array (see
[Ownership transfers](#ownership-transfers)). See [Tuples](10-tuples.md).

A struct is a value in exactly the same way: it is laid out like the tuple of its fields and
retains and releases the references it holds as a tuple does. Assigning a field in place
(`p.name = move s`) takes the new reference and releases the one it replaces. A method's `self`
is the receiver's storage, so a method allocates nothing to be called. See [Structs](11-structs.md).

A tagged union is held in place too, its tag followed by every variant's fields, only the
current variant's set ([Held in place](07-enums.md#held-in-place)), and it retains and releases
the references its current variant holds. A value of one of these types is released in place
at the end of its scope. At a `return`, `break` or `continue` leaving it, and as a statement's
temporary, one whose references take more than four releases (an array's elements counting
once, for the loop that visits them) is released in place too until its function has written
512 such releases, and after that by one call of a function the compiler writes once for the
type, which releases a tagged union's references by its tag: a function's code does not grow
with its ways out times the references its values' types can hold.

An array, a tuple, a struct or a tagged union is passed by address: the callee copies a plain
parameter when the call starts, and reads a `view` parameter where it is. So a call with an
argument that may write that storage after it is read (`f(p, bump(p))` with `bump(p: inout
Point)`, `f(c.p, reset(c))` with `reset` storing into `c.p`) gives the callee a copy the caller
takes where the argument is evaluated, its references retained and released after the call,
as `==` does with its left operand: the value passed or compared is the one read
([Evaluation order](02-basics.md#evaluation-order)). For a `view` parameter, a slice and the
receiver of a struct's `view fn`, a later `inout` argument or method that may change it
naming that storage is an error instead ([Exclusivity](03-functions.md#exclusivity)), so
their copies are taken for a write through an object or a function value. A slice parameter
passed on to another names its caller's array, and is copied the same way when a later
argument may write the array: its elements, as many as the slice has when the program runs,
into memory allocated for the call and freed after it, which the compiler warns about where
the slice is passed ([Evaluation order](02-basics.md#evaluation-order)). Where no later
operand may write it, as in almost every call, nothing more is copied: a later call given
only values that reach no object, or whose code stores into nothing its caller holds,
cannot, and nor can one whose code stores only into objects' fields that cannot hold the
value (`f(c.p, counter.next())`, where `next` stores only a counter's `int` field). A call
through a function value runs code the caller does not know, so a value read before it is
copied.

## `None`

`None` is a singleton `Obj` representing the absence of a value. Because it is a shared global
rather than an allocation, retaining and releasing it is harmless and it is never destroyed.
`None` is what an optional holds when it holds nothing (the `msg` of an `Error` that has no
message, say), distinguished with `match` (see
[Optionals](12-optionals.md#unwrapping-with-match)). A read like `readln` at end of input
holds no `None`: it returns an `Err` whose code is `EndOfFile`
([End of file](../stdlib/io.md#end-of-file-failed-reads-and-buffering)).

### Optional types

A value of optional type `T?` of a reference type `T` (see [Optionals](12-optionals.md)) is the **same
reference** as a `T`, the object's address; the
absent value is represented by a *null reference* rather than by the `None` singleton. This
means an optional costs nothing extra: retaining, releasing, moving or destroying an absent
`T?` is a no-op, a `None` stored into an optional field or array slot releases the previous
value and allocates nothing, and `x == None` is a plain null test. A `T?` never becomes an `Obj`
(an optional is not an object); its `toString()` (`io::println(maybe)`) is `"None"` for an
absent value.

An optional primitive (`int?`, `float?`, `bool?`, `char?`, `u8?`, …) is a value instead (#96): the
primitive and a presence flag, copied like an `int` and never counted, inline in a variable, a
field, an array element or a tuple element. It is not an object, so it never becomes an
`Obj`. A `match` arm binding the `int` copies the
value out and owns nothing.

An optional enum, tuple or struct is a value held in place too: an enum's is its ordinal and
a presence flag, a tuple's or a struct's a presence flag followed by the value (at most 64
bytes; a bigger one goes in a `memory::Box`), copied with the references it holds retained. An optional function value is the function value's own 16 bytes, `None`
being the one with no code. See [Optional Enums, Tuples and Structs](12-optionals.md#optional-enums-tuples-and-structs).

## Thread Safety

The reference counting in this release is **not atomic**: it assumes a single thread of
execution, which is all the language offers today. Thread-safe ARC — counts that can be
shared safely across threads — is part of the language's stated direction toward safe
concurrency and heterogeneous execution, and is on the roadmap rather than in this release.
Do not assume that sharing a reference-counted object across threads is safe today.
The concurrency prototype's checks keep each counted reference on one thread at a time: a
plain or `mut` reference crosses by `move`; an object every thread reads is frozen first, as
an `atomic shared T`, and one several threads write is a `locked mut T`, written under its
lock; an `atomic` cell and a frozen object are not counted. [Concurrency](19-concurrency.md)
describes which references may cross a thread.

## What You Can Rely On

- Objects are freed **promptly and deterministically** at last release — no GC pauses, no
  non-determinism.
- You never write `free`, and you cannot double-free or use-after-free a value through ordinary
  language constructs — ownership is tracked by the compiler.
- A move-only value has one owner. It is never copied behind your back: it moves by `move`
  (and by a `return`), is copied only by `clone()`, and is lent to `view` and `inout`
  parameters; several names hold one object only when its reference is `shared`, so a change
  through one name is seen through another only then.
- A reference keeps its object alive for exactly as long as it is reachable.
- A reference changes its object only when it is written `mut`: through a plain `T` the
  object is read, its `view fn`s are called and it is never passed to an `inout` parameter,
  so an object no `mut` reference names is not changed
  ([Objects are references](05-classes.md#objects-are-references)).
- `destroy` runs automatically and releases everything an object owns, transitively.
- A `weak` reference never keeps an object alive and never dangles: it reads as `None` once
  the object is gone.
- Running out of memory stops the program with a panic, `paykan: out of memory allocating
  <n> bytes`, after what it printed is flushed: an allocation never goes on with a null
  reference. A `memory::Buffer` reports it instead, as `OutOfMemory`
  ([::memory](../stdlib/memory.md)).
- A borrowed result never outlives what it names. A `-> view T` result is held by its caller,
  and an `-> inout T` result can only point into storage its caller keeps alive while the
  result is used: `self`, or what an `inout` parameter names and the object a reference held
  there refers to. One in a separate object, reached through a field of another (`return
  self.c.k;`), is a compile error
  ([Where an `inout` result can point](03-functions.md#where-an-inout-result-can-point)).

The two things the model does **not** do for you today are collecting reference cycles (break
them with `weak`) and making reference counting safe across threads — both are known and on
the roadmap. Which references the concurrency prototype lets cross a thread, and the checks that
keep the counts on one thread, are in [Concurrency](19-concurrency.md).

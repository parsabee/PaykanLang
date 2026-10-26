# Memory

PaykanLang has no garbage collector and no `free`. Object lifetimes are managed by
**automatic reference counting** (ARC): every heap object carries a count of the references
to it, and the object is destroyed the moment the last one goes away, immediately and
deterministically. The compiler inserts the counting for you; this chapter describes what
it guarantees, how to check a program for leaks, the one kind of leak ARC cannot prevent and
how `weak` references avoid it, and then the details of the model.

## Value Types vs Reference Types

Types come in two kinds, and the kind decides how a value is stored and whether ARC applies:

- **Value types**: `int`, `float`, `bool`, `char`, the sized numeric types (`u8`, `f32`, …),
  enums, arrays (`int[3]`), tuples (`(int, Str)`), structs and function values, and the
  optionals of the primitives, enums, tuples and structs. They are copied on assignment and
  live in their variable or field, so there is nothing to manage. An array, a tuple or a
  struct of objects or strings holds references to them, as a variable would; a function
  value holds a reference to its closure, the object that keeps its captures.
- **Reference types**: class instances (an `io::File` too), `Str`, and optional values of
  them. They live on the heap, and variables, fields, array elements and tuple elements hold
  references to them.

In detail:

| Category | Types | Storage | On assignment / passing |
|----------|-------|---------|--------------------------|
| **Value** | `int`, `float`, `bool`, `char`, the sized numeric types (`u8`, `f32`, …, each in an 8-byte slot), `enum`, and their optionals `int?`, `float?`, `bool?`, `char?`, `u8?` | Stack / register (inline in a field) | Copied bit-for-bit; independent thereafter |
| **Array** | `T[n]` of any element type | Inline in the variable, the field or the enclosing array | Elements copied; reference elements retained |
| **Tuple** | `(T1, T2, ...)` of any element types | Inline in the variable, the field or the enclosing array or tuple | Elements copied; reference elements retained |
| **Struct** | `struct Name { ... }` of any field types | Inline, like a tuple of its fields | Fields copied; reference fields retained |
| **Function value** | `(T1, T2) -> R`: a lambda or a named function | Inline, 16 bytes: the code's address and a reference to its closure, a heap object holding the captured variables (copied when the lambda is made; none when nothing is captured) | Code copied, closure shared (retained); the captures are released with the closure |
| **Reference** | class instances (an `io::File` among them), `Str`, optionals `T?` of these | Heap | Reference shared; count adjusted |

A value type has no identity beyond its bits: assigning one `int` to another produces two
independent values. A reference type has identity: assigning one variable to another makes both
names refer to the **same** object, and a mutation through one name is visible through the other.

```pkn
a: int = 1;
b: int = a;      // independent copy
b = 2;           // a is still 1

p: Point = Point(1, 2);
q: Point = p;    // same object — both names share it
q.x = 99;        // p.x is now 99 too
```

An array is a value: `ys = xs` copies its elements, so a later `ys[0] = 99` leaves `xs[0]` as
it was. A tuple is one too: `u = t` copies `t`'s elements, and `u` keeps them when `t` is
assigned a new tuple.

## The Reference Counts

Each heap object carries a **reference count**. Storing a reference (in a variable, a field,
an array or a tuple) increments the count, and so does passing it to a function that keeps it
(see [Borrowed parameters](#borrowed-parameters)); when a reference goes away (a variable goes
out of scope, a field or element is overwritten, an object holding it is destroyed) the count
is decremented. When it reaches zero, the object is destroyed:
its own references are released, which may destroy further objects in turn, and its memory
is freed. You never write any of this; the compiler inserts the counting.

Every reference-counted object carries its counts in its own header, so an object is one heap
allocation and a reference to it is simply its address:

```
object: { vtable pointer, strong count, weak count, fields… }
```

The header is 16 bytes: the 8-byte vtable pointer and two 32-bit counts. An object can have up to
2,147,483,647 references of each kind; one more stops the program with `paykan: too many strong
references to one object (the limit is 2147483647)` rather than corrupting the count.

Two operations maintain the strong count:

- **retain** — increments the count when a new reference to the object is created (an assignment,
  passing the object into a place that keeps it, storing it in a field or array).
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

1. The object's `destroy` method runs. The compiler generates `destroy` to **release every
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
`obj.destroy()` explicitly — the compiler owns it entirely (see [Classes](05-classes.md)). There is no
user-defined finalizer; cleanup that must happen at end of life belongs in the fields the object
holds, which are released automatically.

## Assignment, Passing, and Returning

For reference types, the counting is balanced so an object stays alive exactly as long as some
reference names it:

- **Assignment** `x = y` retains the new object and releases whatever `x` referred to before, so
  reassigning a variable cannot leak the previous value.
- **Passing to a function** lends it the caller's reference when the caller keeps the object
  alive for the whole call, and gives it a reference of its own otherwise (a temporary, or a
  variable at its last use: see [Ownership transfers](#ownership-transfers)), which keeps the
  object alive while the function holds it: until it returns, or until it hands the reference
  on in turn. A function takes a reference of its own to what it was lent only where it keeps
  it (see [Borrowed parameters](#borrowed-parameters)). The reference the caller holds is
  unaffected, so an object the caller still holds outlives the call.
- **Returning** transfers ownership of the result to the caller.
- **Storing into a field or array slot** retains the stored object; overwriting a slot releases
  the value it previously held. A field's first assignment in `__init__` overwrites nothing: the
  object is new.

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
the program has dropped every reference to it. **Such cycles are not collected and will
leak**:

```text
import ::io;

class Person {
  name: Str;
  friend: Person?;
}

impl Person {
  fn __init__(name: Str) { self.name = name; }
}

fn main() -> int {
  a = Person("Ada");
  b = Person("Grace");
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
declared with `weak` refers to an object without counting as a reference to it, so it never
keeps the object alive. Its type is an optional, because it reads as `None` once the object
is gone. Make the back link weak and the cycle is no cycle at all:

```pkn
import ::io;

class Person {
  name: Str;
  weak friend: Person?;
}

impl Person {
  fn __init__(name: Str) { self.name = name; self.friend = None; }
  fn friendName() -> Str {
    match self.friend {
      f: Person { return f.name; }
      None { return "nobody"; }
    }
  }
}

fn main() -> int {
  a = Person("Ada");
  b = Person("Grace");
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
and is then stored in that object (`h.cb = fn() -> int { return h.n; };`) forms a cycle like any
other, and leaks the same way unless the capture is a `weak` variable (`weak me = h; h.cb =
fn() -> int { match me { ... } };`) or the field is overwritten before `h` goes out of scope.

## Weak References

A variable or a class field declared with `weak` refers to an object **without keeping it
alive**. Its type is `T?` for a class `T` (a user class, `Str` or `Obj`),
and it may be left off a local declaration that has an initializer:

```pkn
import ::io;

class Node {
  name: Str;
  weak parent: Node?;   // a back link that does not own the parent
  first: Node?;
  next: Node?;
}

impl Node {
  fn __init__(name: Str) {
    self.name = name;
    self.parent = None;
    self.first = None;
    self.next = None;
  }
  fn add(child: Node) {
    child.parent = self;
    child.next = self.first;
    self.first = child;
  }
  fn path() -> Str {
    match self.parent {
      p: Node { return p.path() + "/" + self.name; }
      None { return self.name; }
    }
  }
}

fn main() -> int {
  root = Node("root");
  a = Node("a");
  root.add(a);
  weak w: Node? = root;   // reads as root while something holds it
  weak u = a;             // inferred Node?
  io::println(a.path() + " " + Str(w == None));
  root = Node("other");   // the first root dies: it was held by nothing else
  io::println(a.path() + " " + Str(w == None));
  match u {
    n: Node { io::println(n.name); }
    None { io::println("gone"); }
  }
  return 0;
}
```

Output:

```
root/a False
a True
a
```

Reading a `weak` variable or field gives a `T?`: the object while it is alive, `None` once it
has been destroyed. The value read is an ordinary optional — matched with `match`, compared
with `== None`, passed, returned, or stored in a strong variable or field, which then keeps the
object alive. Assigning a `T` or a `T?` to a `weak` variable or field makes it refer to that
object (or to nothing, for `None`) without retaining it. A lambda that captures a `weak`
variable captures it weakly, so a closure can refer to the object that stores it without
forming a cycle (see [Captures](14-function-types.md#captures)).

A weak reference is not a place a strong reference could be written to, so it cannot be passed
to an `inout` parameter, named by an `inout` local, or bound by an `inout` match arm. It can be
`view`ed, since a view only reads it. For now `weak` applies to locals and class fields: not to
struct fields, tuple or array elements, parameters or results.

A `weak` local may name what a `view` names: a `view` parameter, or part of `self` in a
`view fn`. It takes no reference of its own, so the view is not copied. What it reads is the
view's object, so its reads are `view`s too: a `match` arm's name for it is bound to a `view`,
which is read, or passed on to a `view` parameter, but never changed, stored, returned (`'n'
is bound to a 'view'; it cannot be returned`) or captured by a function (`'cur' is a 'weak'
local of a 'view'; a function cannot capture a borrow`). It may be assigned another `view`.
That is how a `view fn` walks a chain of objects reached from `self` without copying a
reference out of the view (a plain local could not hold one): `weak cur = self.head;` then
`match cur { n: Node { ...; cur = n.next; } None { } }`. A method that may change `self`
walks one the same way and returns what it finds, since there `self` is not a `view`.

The object's memory is kept, empty, while `weak` references to it remain (its own references
are released when it is destroyed), and freed with the last of them — so a program that drops
its weak references frees everything, as `--track-heap` shows.

## Ownership transfers

You never hand a reference on by hand. Assigning or passing a reference shares the object:
`t = s` retains it, and `s` and `t` are both live, and readable, until their scopes end. There
is no way to *consume* a variable by hand.

Where a variable is not read again after it is handed on, the compiler makes the hand-off a
**transfer**: the reference moves to the destination without a retain, and the variable
releases nothing at the end of its scope. At a variable's last use:

- **Assigning** it to another variable, **storing** it in a field or an array element, and
  **returning** it are transfers, whatever it holds: an object, a string, or a tuple, a struct,
  an enum, a function value, a fixed-size array of up to four elements or an optional of one,
  all of whose references move together.
- **Passing** it to a function is a transfer when it holds an object or a string, or an
  optional of one: the function holds the reference, and releases it when it returns. A tuple,
  a struct, an enum, a function value or a fixed-size array is passed as its contents, of which
  the function keeps a copy of its own, so passing one stays a copy, and the variable releases
  its references when its scope ends.
- A `view` parameter or variable is never handed on: it names what another variable holds, and
  the object stays alive until the view's own scope ends. Passing a variable to a `view`
  parameter at the variable's last use is a transfer like any other.

A hand-off the variable is read after stays a copy: later in the function, or on the next iteration
of a loop (a variable assigned a new value before it is read again is not). So does one the
function follows, before the variable's scope ends, with a call or the release of a reference (the
one an assignment replaces, say) while it uses an object it holds no reference to: a field's value
reached through another variable, or anything reached through `self`.
In `x = h.node; take(x); s = h.node.name + label(1);` the hand-off to `take` stays a copy:
`label` runs while the name read from `h.node` is still to be used, and `x` may be what keeps that
node alive (a transfer treats any call as one that may release a reference). A read that a call
could replace keeps what it read alive itself until it is used, which the compiler decides from
what the call is given and, when it compiles the function called, from what that function does:
with `replace(h)`, which is given `h` and assigns `h.node`, in place of `label(1)`, the name has a
reference of its own, and the hand-off is a transfer. In a method, a hand-off therefore stays a
copy when `self` is
used by or after a call or a release that follows it, the call it is passed to included (not in
`__init__`, whose object the constructor holds). Where one branch of an `if` or one arm of a `match` hands the
variable on and another does not, the other releases it when its scope ends, as before. In
`f(a, a)` the first `a` is a copy and the second a transfer. `self`, `inout` parameters and locals,
fields, array elements and the members of a tuple or struct, and `view` parameters and variables,
are never transferred, and a lambda's captures are always copies. The compiler's `--stats` option
counts the transfers it made ([docs/pir.md §12](../pir.md#12-passes) has the details).

A transfer releases nothing early: the destination holds the reference and releases it at the
end of *its* scope, as it would a copy: a parameter when its function returns, a variable when
its scope ends, a field when its object is destroyed or the field is assigned again. A program
frees the same objects either way. What a transfer can change is *when* an object is destroyed:
it lives as long as its last holder, which after a transfer is the destination, so it may be
destroyed before the variable's scope would have ended. Two things can see that. A
[`weak` reference](#weak-references) elsewhere reads `None` as soon as the destination lets go.
And what destroying the object does happens then: an [`io::File`](../stdlib/io.md) passed to a
function at its variable's last use is closed, and what was written to it is on disk, when
that function returns (unless it keeps the file), so a later `open` of the same path in the
caller reads it back.

```pkn
import ::io;

class Node {
  name: Str;
}

class Watcher {
  weak target: Node?;
}

fn consume(n: Node) -> Str {
  return n.name;
}

fn report(w: view Watcher) {
  match w.target {
    t: Node { io::println("alive: " + t.name); }
    None { io::println("gone"); }
  }
}

fn main() -> int {
  n = Node("first");
  w = Watcher(n);
  report(w);
  io::println(consume(n));   // the last use of n: consume holds the only reference
  report(w);                 // consume released it when it returned
  return 0;
}
```

Output:

```
alive: first
first
gone
```

A hand-off of the variable while a `weak` local made from it (`weak u = n;`) is in scope stays a
copy, whether or not that local is read again, so the local reads the object until the
variable's scope ends.

Earlier releases had a `mov` keyword for this. It was removed in v0.2.0: `mov` is still a
reserved word, and writing `t = mov s` is an error:

```
error: 'mov' was removed in v0.2.0; ownership transfers are inferred
```

Drop the keyword: `t = mov s;` becomes `t = s;`. The program prints the same output and frees
the same objects. The variable `s` simply stays readable afterwards.

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

fn clear(l: List) -> int {
  l.head = None;
  return 0;
}

fn main() -> int {
  l = List(Node("first"));
  match l.head {
    n: Node {
      k = clear(l);
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

A function is **lent** an argument when the caller keeps a reference of its own to the object
for the whole call: a variable it reads again afterwards, a parameter of its own, or a field
nothing in the call can replace. A lent argument costs no count at all where the function
only reads it or passes it on: neither the caller nor the function retains it, and the function
does not release it. Where the function keeps it (stores it in a field or a variable, returns
it), it retains it there, so the place it is kept in has a reference of its own. A temporary,
and a variable at its last use, are handed over as before: the function releases them when it
is done, or hands them on, so they die where they always did. A variable at its last use is
lent all the same where another reference the caller holds outlives the call, so that the
function's release could never be the last: a `match` binding passed on in its arm, whose
object the matched value holds until the `match` ends (`x: Node { c = c + count(x); }` costs
no count but the matched value's), or a copy of a variable the caller reads again (`u = t;
count(u);`). Which way an argument goes is decided at each call; nothing in the function's
declaration says it.

```pkn
import ::io;

class Node {
  name: Str;
}

class Watcher {
  weak target: Node?;
}

fn state(w: view Watcher) -> Str {
  match w.target {
    Node { return "alive"; }
    None { return "gone"; }
  }
}

// Only reads its parameter: lent, its count never changes.
fn show(n: Node) {
  io::println(n.name);
}

class Holder {
  n: Node?;
}

// Keeps its parameter: the field takes a reference of its own.
fn keep(h: Holder, n: Node) {
  h.n = n;
}

fn main() -> int {
  a = Node("a");
  w = Watcher(a);
  show(a);                    // lent: main reads a again below
  h = Holder(None);
  keep(h, a);                 // lent too
  h.n = None;
  io::println("after the field let go: " + state(w));
  show(a);                    // a's last use: handed over, a dies in show
  io::println("after show: " + state(w));
  return 0;
}
```

Output:

```
a
after the field let go: alive
a
after show: gone
```

A function that assigns its parameter (`n = Node("b")`, `s += "!"`, passing it to an `inout`
parameter) owns it from its start instead, as every function did before: then the parameter
is its own variable. Nothing else changes: an object is destroyed at the same point whether
it is lent or not, a `weak` reference reads what it would, and the program frees the same
objects. `--stats` counts the arguments lent and the parameters
([docs/pir.md §12](../pir.md#12-passes) has the rules).

## Strings, Arrays and Tuples

`Str` is a heap, reference-counted class. String literals and the results of `+` concatenation
are reference-counted objects like any other.

An array `T[n]` is a value, not an object: it has no box and no count of its own, and lives
where its variable or field does, inline in the stack frame or in the object. How it treats its
**elements** depends on the element type:

- An array of a **value type** (`int[3]`, `float[3]`, `bool[3]`, an enum array) stores the
  scalar values directly; there is nothing to reference-count per element.
- An array of a **reference type** (`Str[3]`, `Point[3]`, …) stores references and **retains**
  each element it holds. Overwriting a slot releases the old element, and the array releases
  all its elements when its variable goes out of scope or the object holding it is destroyed.

Assigning an array, or passing it to a plain parameter, copies it: the copy retains each element's object, which the two
arrays then share. (Assigning a variable holding up to four elements at its last use moves them
instead: see [Ownership transfers](#ownership-transfers).) See [Arrays](09-arrays.md).

A tuple is a value in the same way: it has no box and no count, and its elements are laid out
in place, one after another like a C struct's fields, in its variable, its field or the array
element that holds it. A tuple of value types (`(int, float)`) allocates nothing; a reference
element (`Str`, a class, `Node?`) is a reference to the object, which the tuple **retains**. Each copy of a tuple
(an assignment, a plain parameter, a result, an array element) retains the references in it
once more; a variable's are released when it goes out of scope, a field's when its object is
destroyed, an array element's with its array. At a variable's last use, assigning, storing or
returning it moves its references instead, and the variable releases none (see
[Ownership transfers](#ownership-transfers)). See [Tuples](10-tuples.md).

A struct is a value in exactly the same way: it is laid out like the tuple of its fields and
retains and releases the references it holds as a tuple does. Assigning a field in place
(`p.name = s`) retains the new reference and releases the one it replaces. A method's `self`
is the receiver's storage, so a method allocates nothing to be called. See [Structs](11-structs.md).

A tagged union is held in place too, its tag followed by every variant's fields, only the
current variant's set ([Held in place](07-enums.md#held-in-place)), and it retains and releases
the references its current variant holds. A variable of one of these types whose references
take more than a few releases is released, when it goes out of scope (at the scope's end, or at
a `return`, `break` or `continue` leaving it), by one call of a function the compiler writes
once for the type, which releases a tagged union's references by its tag: a function's code
does not grow with its ways out times the references its variables' types can hold.

An array, a tuple, a struct or a tagged union is passed by address: the callee copies a plain
parameter when the call starts, and reads a `view` parameter where it is. So a call with an
argument that may write that storage after it is read (`f(p, bump(p))` with `bump(p: inout
Point)`, `f(c.p, reset(c))` with `reset` storing into `c.p`) gives the callee a copy the caller
takes where the argument is evaluated, its references retained and released after the call,
as `==` does with its left operand: the value passed or compared is the one read
([Evaluation order](02-basics.md#evaluation-order)). Where no later operand may write it, as in
almost every call, nothing more is copied: a later call given only values that reach no object,
or whose code stores into nothing its caller holds, cannot.

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

## What You Can Rely On

- Objects are freed **promptly and deterministically** at last release — no GC pauses, no
  non-determinism.
- You never write `free`, and you cannot double-free or use-after-free a value through ordinary
  language constructs — ownership is tracked by the compiler.
- A reference keeps its object alive for exactly as long as it is reachable.
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
the roadmap.

# Concurrency and ownership (v0.4)

Status: proposal, on the owner's decisions of 2026-10-28 (32 numbered
decisions and the defaults after them), of 2026-10-29 (decisions 33-35,
which revise several earlier ones) and of 2026-11-02 (decisions 36 and 37,
which revise 33): decisions 1-37 in all.  It supersedes issue #26.  Line
references are to the v0.3.0 tree.  A detail the decisions leave open is
this design's choice: it is marked "(proposed; owner may change)" and listed
under [Open questions](#open-questions).  The qualifiers themselves are the
owner's: `atomic` and `locked` in place of the first draft's cross-thread
qualifier and types, with no `locked view` (33); `const` and `shared
const`, apart from `let` and `view` (36); `atomic shared` for read-only
sharing across threads, with no `atomic shared` and `atomic` only on
numbers (37); and, on 2026-11-03, the mut model: the draft's `const` is
dropped, the bare type is immutable and `mut` buys the writes ([Syntax](#syntax)), with a
sentence marked "(the mut model, owner 2026-11-03: ...)" wherever a `const`
form below reads differently now.  The rules recorded under those decisions come from the
discussion, so every rule cited as decision 33, 36 or 37 beyond that is
proposed; owner may change.  A prototype of the qualifier, `move`, send,
`atomic`, `locked` and `parallel for` sections is on branch
prototype/concurrency (#361; samples/concurrency/README.md lists what it
cuts), and a sentence marked "(prototype, 2026-11-03)" records what it
decided where that differs from, or settles, what is written here
(proposed; owner may change).  Snippets are independent: each declares or
names the classes it uses.

v0.4 gives Paykan one model for concurrent programs.  Every reference is
unique (the default) or `shared` (aliased on one thread, plain counts),
either may be `const` (the object never changes), and ownership moves with
an explicit `move` (the mut model, owner 2026-11-03: either is immutable
unless written `mut`).  Across threads a value moves, is copied, is shared
read-only as `atomic shared` (counted atomically on the object, its interior
frozen so reads inside count nothing), or sits in a cell whose copies are
handles: `atomic` for numbers, `locked` for state reached in `lock` blocks.
Tasks are `async fn`s on one event loop per thread, talking through bounded
channels; data parallelism is a verified `parallel for`.  Programs that
compile have no data races, and objects of user classes keep plain counts
unless their type says `atomic shared`.  Two settled rules are reversed:
`move` returns, and a copy of an unmarked container is no longer the same
container.  `paykan migrate` carries programs across.

## Summary of the decisions

| Topic | Decision |
|---|---|
| Qualifiers | unique (unwritten), `const`, `shared`, `shared`, `atomic`, `atomic shared` or `locked`; a unique value converts one way into any, a `const` one into the other `const` forms, none back but `sync::take` (the mut model, owner 2026-11-03: `T`, `mut T`, `shared T`, `shared mut T`, `atomic T`, `atomic shared T` and `locked mut T`; `mut T` flows into `T`, `T` gains `mut` only by `move`, and `shared` and `shared mut` never convert) |
| Transfer | `move x`, `return move x` included; `b = a` of a unique value is an error |
| Borrows | `view` and `inout`, never counted; exclusivity exact on owned paths, held elsewhere |
| Crossing threads | the send check, handles copied, a run-time check on values moved out of borrowed storage |
| Generics | `T` is move-only unless `T: Copyable` |
| Tasks | `async fn`, `await`, `task::spawn`, `task::spawnOn`; every task handle awaited, joined, cancelled or detached |
| Scheduling | one event loop per thread, tasks pinned; work stealing in v0.5 |
| Cancellation | cooperative, at waits and `task::cancelled()`, propagated to started tasks |
| Communication | bounded `sync::Channel<T>`, `lock` blocks on `locked` cells, `atomic` operations |
| Data parallelism | verified `parallel for`, reductions reproducible on every machine |
| Runtime | thread-safe C11, atomic counts only on `atomic shared` objects, handles and task records, kqueue and epoll loops, timers, files on helper threads, TSan |
| Migration | `paykan migrate <paths>` writes `shared` and `move`; one breaking release |

Decisions 33-35 replaced the first draft's `thread_shared` qualifier,
`sync::Locked<T>` and `sync::Atomic<T>` types, `@par` attribute and v0.4
attributes (#171); decision 36 added `const` (gone since the mut model,
owner 2026-11-03), and decision 37 replaced
decision 33's `atomic view` cell with `atomic shared`, counted on the object.

## Goals and non-goals

Goals:

* One model for data parallelism, host concurrency and, later, accelerators
  (decision 1), layered: explicit tasks and loops now, an opt-in dataflow
  layer later (decision 2).
* No data races in programs that compile, with diagnostics naming the path.
* Confinement by default and plain counts in single-threaded code
  (decision 3); atomic counts only on `atomic shared` objects, chosen by
  type, and on the cells, channels, loops and task records (decisions 33,
  37).  Value structs are the encouraged way into a task: they are copied,
  cross threads freely and need no qualifier.
* Immutability in the type: a `const` object never changes, so reads
  through it need no holds (decision 36).  (The mut model, owner
  2026-11-03: the bare type is the immutable one, and `mut T` the object
  that changes.)
* Every backend: the JIT, native builds, strict C11 from `--emit-c`, plugins.

Non-goals for v0.4: scoped task blocks (decision 6); CPU functions as tasks
and work stealing (decisions 14, 15); sockets, processes and Windows
(decision 27); the dataflow layer, tensors and accelerators (decisions 2,
13); unbounded channels (decision 30), `select` and async lambdas;
attributes (decision 35); `parallel` on anything but `for` (decision 34).

## Ownership

### The qualifiers

| Written | Reached by | Counts | Crosses threads | Made from | Used |
|---|---|---|---|---|---|
| `mut T` (was: nothing, unique) | one owner | plain, often none | by `move` | a new value, or a `T` by `move` | freely |
| nothing (`T`; was `const T`) | one owner | plain, often none | by `move`, when its parts can | a `mut T`, moved or new | read only |
| `shared mut T` (was `shared T`) | aliases on one thread | plain | never | a unique, moved or new | freely |
| `shared T` (was `shared const T`) | aliases on one thread | plain | never | a unique, moved or new | read only |
| `atomic T` | handles on any thread | the cell's, atomic | as a handle | a number, `bool`, `char` or fixed array of them | by atomic operations |
| `atomic shared T` (was `atomic shared const T`) | references on any thread | the object's, atomic; none inside | as a handle | a crossable unique, moved or new, then frozen inside | read only |
| `locked mut T` (was `locked T`; `locked T` alone warns) | handles on any thread | the cell's, atomic; plain inside | as a handle | a crossable value, moved or new | in `lock` blocks |

The first column is in the mut model's spelling (owner, 2026-11-03), the
earlier spelling in parentheses: unique is the bare `T`, immutable, or `mut
T`; the rows that are read only are the ones with no `mut`.

`atomic` and `locked` each make one heap cell with an atomic count, and a
copy is another handle to it (decision 33; proposed; owner may change).  An
`atomic shared` reference is the object's own pointer, its atomic count in
the object, as with Rust's `Arc` (decision 37).  In this design a
**handle** is a reference counted atomically: an `atomic shared` reference,
a handle to an `atomic` or `locked` cell, a `sync::Channel` or a
`task::Loop`, each Copyable and crossable.  A `task::Task` is a **task
handle**, unique and a different thing ([Task handles](#task-handles)).
"Read only" is `const` access ([`const`](#const); the mut model, owner
2026-11-03: the access through a reference not written `mut`).  Every reference type
takes the qualifiers (decision 21): classes, `Obj`, `Str`, `Box`, `Buffer`,
the containers and function values (in the prototype `Buffer` stays a
struct, a value, and takes none; [`Str` and the
containers](#str-and-the-containers); prototype, 2026-11-03).  Today no
type says how a reference is held (`ast::isRefType`, include/AST.h:2454).

### Syntax

docs/grammar.md changes first, then the frontends (docs/grammar.md:12-13):

```
typeAnnotation ::= qualifier* primaryType ( "[" "]" | "[" ( INT | "_" ) "]" | "?" )*
qualifier      ::= "mut" | "shared" "mut"? | "atomic" "shared"? | "locked" "mut"?   -- the mut model (owner, 2026-11-03)
unary          ::= ( "!" | "-" | "~" | "move" | "await" ) unary | postfix
funcDecl       ::= "async"? receiver? "fn" IDENT typeParams? "(" paramList? ")"
                   ( "->" ( "view" | "inout" )? typeAnnotation )?
                   ( block | "=>" expression ";" )
receiver       ::= "view" | "shared" "view"?
classMember    ::= ... | "async"? receiver? "fn" ...   -- pure virtual, as today after "fn"
traitMember    ::= "async"? receiver? "fn" ...         -- as today after "fn"
nativeFuncDecl ::= "native" "async"? "fn" ...           -- as today after "fn"
implMember     ::= funcDecl | "static" "async"? "fn" ...
implDecl       ::= "impl" ( typeAnnotation "for" )? modulePath
                   ( "<" implParam ( "," implParam )* ">" )? "{" implMember* "}"
implParam      ::= IDENT ( ":" typeAnnotation ( "+" typeAnnotation )* )?
statement      ::= ... | qualDecl | lockStmt
qualDecl       ::= IDENT ":" qualifier "=" expression ";"
letDecl        ::= "let" IDENT ( ":" ( typeAnnotation | qualifier ) )? "=" expression ";"
lockStmt       ::= "lock" expression "as" IDENT ":" ( "view" | "inout" ) block
forStmt        ::= "parallel"? "for" ...                -- as today after "for"
```

**The mut model (owner, 2026-11-03).**  The bare type is the cheapest form
and immutable: a `T` has one owner, and through it no field is assigned,
no method but a `view fn` is called and nothing is passed to an `inout`
parameter, at zero overhead.  Every added word buys one thing at one cost:
`mut T` the writes through the owner; `shared T` several owners on one
thread, immutable, and `shared mut T` several owners with writes; `atomic
shared T` an object frozen for every thread (the form once spelled `atomic
const`, then `atomic shared const`); `locked mut T` an object written under
a lock, inside `lock c as v: inout { }`, and `locked T` alone is accepted
with a warning, since it locks what nothing writes; `atomic T` stays the
cell of a number, `bool` or `char`.  The language has no `const`: the
draft's `const T`, `shared const T` and `atomic shared const T` are the bare
`T`, `shared T` and `atomic shared T`.  A value takes no
`mut`: for a number, `bool`, `char`, struct, tuple or fixed array `let`
alone decides whether its storage is rewritten (and so for a function
value, in the prototype).  A borrow takes none: `view T` reads and `inout
T` writes what it names, the `inout` being the borrow's writes, on a
parameter, a local or a `match` binding; a plain `match` binding takes no
qualifier and is a copy of its subject in the subject's form.  No `mut`
goes on a cell or a frozen object: `atomic mut T` and `atomic shared mut
T` are errors, and `mut shared` and `mut locked` are spelled `shared mut`
and `locked mut`.  `self` is mutable in every method but a `view fn`
(`__init__` builds it); a unique field follows its root, writable under a
`mut` object and immutable under an immutable one, so it is never written
`mut`, while a `shared mut` field is the object's own form and keeps its
writes through any root (it is written through every alias); an object held in a value (a
struct field, an array or tuple element) follows the variable's `let`, not
`mut`.  Conversions: `mut T` flows into `T` freely; `T` gains `mut` only
by `move` (or straight from a temporary, such as a constructor call); `T`
or `mut T` moves into `atomic shared T`, frozen then; `shared T` and
`shared mut T` never convert into each other, and a frozen object and a
cell convert only as they are.  Declarations: `let n = Node(1)` is
immutable; `let n: mut Node = Node(1)`, `n: mut = Node(1)` (a first
assignment, the type inferred) and `n: mut Node = Node(1)` are mutable;
`fn f(n: mut Node)`, `-> mut Node`, `next: shared mut Node`.

* `const`, `shared` and `shared` (the mut model, owner 2026-11-03: `mut`,
  `shared` and `shared mut`) bind to the primary type (`shared
  Node[3]` is three references); `atomic`, `locked` and `atomic shared`, each
  one object, cover the whole type (`atomic int[1024]`, decision 33;
  `locked Node?`; `atomic shared Node[3]`, one boxed array), except that
  `atomic shared` leaves a trailing `?` outside: `atomic shared Str?` is an
  optional reference (proposed; owner may change).  A mode and a
  qualifier never meet: a borrow is written `n: view Node` or `n: inout
  Node` (owner, 2026-11-02).  In the prototype a struct, tuple or fixed
  array takes no qualifier of its own: `shared Node[3]` is three shared
  references, as above (#368), while `atomic shared Node[3]` is an error
  that points to `mem::Box<Node[3]>`, and `shared int[3]` is `'shared'
  qualifies a reference; 'int[3]' is a value` ([`const`](#const);
  prototype, 2026-11-03).
* `x: shared = e;` and `let x: const = e;` infer the type, as a borrow
  declaration does (docs/grammar.md:385) (decision 36 for `const`; proposed;
  owner may change).  There is no `const x = e;` (decision 36).  (The mut
  model, owner 2026-11-03: `n: mut = Node(1)`, a first assignment with the
  type inferred, likewise.)  A receiver
  prefix other than `view` is for methods only.
* `move`, `shared`, `atomic`, `locked`, `mut` (the mut model, owner
  2026-11-03: in place of the draft's `const`), `async`, `await`, `lock`
  and `parallel` become keywords (include/paykan/Keywords.h:21-25); `view`
  is one already.  `unique` is never written.
* Recovery (docs/grammar.md:672-678) adds `async fn`, `shared fn` and
  `shared view fn` to the member and top-level points; `lock` and `parallel
  for` recover like other statements.
* The transfer is spelled `move`.
* `shared int` is `'shared' qualifies a reference; 'int' is a value`, and
  `const int` is `'const' applies to objects; 'int' is a value, which 'let'
  fixes` (decision 36).  A type takes one qualifier: the parser reads a run
  of them and reports `locked atomic shared Config`; a class holding the
  handle can be `locked` (proposed; owner may change).  There is no `locked
  view` (decision 33), `locked const` or `atomic shared` (decision 37), and
  the parser reports each by name.  The frozen form is spelled `atomic
  shared`, each word saying one thing (`atomic`: the reference
  crosses threads; `shared`: the object has several owners, one on each
  thread at least; `const`: nothing writes it), and the parser rejects the
  two spellings that leave a word out, `atomic const N` with
  `'atomic const' is 'atomic shared': an object read from several
  threads has several owners; write 'atomic shared T'` and `atomic
  shared N` with `'atomic shared T' would be written from several threads
  with no lock; write 'atomic shared T' to freeze it, or 'locked T'
  to change it under a lock` (owner, 2026-11-03).  (The mut model, owner
  2026-11-03: `mut int` is `'mut' applies to objects; 'int' is a value: its
  storage is rewritten by assignment, which 'let' fixes`; the frozen form is `atomic shared
  T`, two words, immutability being the bare type's, and `atomic shared
  mut N` is `'atomic shared mut T' would be written from several threads
  with no lock; write 'atomic shared T' to freeze it, or 'locked mut T' to
  change it under a lock`; `atomic mut N` is `an 'atomic' cell takes no
  'mut': it changes by its operations; an object crosses threads as
  'atomic shared T' (frozen) or 'locked mut T'`; `mut shared N` is `'mut
  shared' is spelled 'shared mut': the form first, then the writes`; and
  two qualifiers are `a type takes one qualifier: 'shared mut shared' is
  two`.)

### Copies and `move`

A type is **Copyable** when it is a number, `bool`, `char`, an enum with
Copyable payloads, a tuple, struct, fixed array or optional of Copyable
parts, a `shared` or `shared` reference, or a handle, `atomic shared`
included (decisions 33, 36, 37).  Everything else is move-only: unique
classes, `Str`, containers and function values, their `const` forms (the
mut model, owner 2026-11-03: bare or `mut`), task
handles, and every aggregate holding one.

A move-only value read from a place (a variable, parameter, field, element,
borrow or `self`) cannot flow without `move` into an owning slot: a
declaration, assignment or store, a plain argument, a `return`, a literal's
element, a plain `match` binding or a capture (decision 18):

```pkn
class Node { id: int; }
let a = Node(1);
let b = a;        // error: cannot copy 'a' (a unique 'Node'): write 'move a' to transfer it, or declare it 'shared Node'
let c = move a;   // a is unusable from here
struct Person { name: Str; age: int; }
let p = Person("Ann", 30);
let q = p;        // error: cannot copy 'p': 'Person' holds unique field 'name' ('Str')
let k: atomic int = 0;
let j = k;        // another handle to the same cell
```

A function returning a unique local writes `return move n;`; `return n;` is
the copy error.  Temporaries are owned already: calls, constructors,
literals, operators and `move x`.  `move` before a temporary is accepted and
does nothing, so decision 23's `move Config("w.bin")` compiles (proposed;
owner may change).

`move` takes an owned binder: a local (a `let` one too), a by-value parameter
or an owned `match` binding.  Fields and elements leave through
`mem::replace<T>(slot: inout T, v: T) -> T` and
`mem::take<T>(slot: inout T?) -> T?` (`import ::memory as mem;`).  The one
partial move is `move b.f` of an owned binding `b` of an enum variant: it
moves a payload field out and ends `b`, which is how a unique value leaves a
`Result` (proposed; owner may change).

```pkn
let x = move h.node;  // error: cannot move 'h.node': 'move' takes a local variable, a by-value parameter or a 'match' binding; write 'mem::replace(h.node, v)'
let y = move self;    // error: cannot move 'self': a method borrows its receiver
let z = move n;       // n: view Node -- error: cannot move 'n': it is a 'view' parameter, a borrow
let v = n;            // error: cannot copy 'n': it is a 'view' parameter, a borrow
let u = move w;       // weak w -- error: cannot move 'w': a 'weak' variable holds no strong reference
```

`move` of a Copyable value is allowed and ends the source, so generic code
can write `move v` for any `T` (proposed; owner may change).  Moves at a last
use stay as an optimisation (the owner's default).

### Use after move

A new pass, SemaMoves, runs a forward "maybe moved" set over each body,
modelled on the definite assignment of fields in `__init__`
(src/Sema/SemaClass.cpp:261-410).

```pkn
let job = Job("a");                     // class Job { name: Str; }
let t = task::spawn(run(move job));
io::println(job.name);   // error: 'job' was moved at 2:25; it cannot be read after the move
```

* After a branch that moves: `'a' may have been moved (by the 'if' at 10:3)`.
* A move in a loop of an outer variable not assigned again on every path:
  `'job' is moved in the loop at 4:3 and not assigned again before the next
  iteration`.
* An assignment revives a moved non-`let` variable (proposed; owner may
  change).
* Moving a borrowed variable is checked like an `inout` use (checkNotBorrowed,
  src/Sema/SemaBorrows.cpp:302): `'l' is borrowed by 'view' local 'x' until
  'x' is last used; it cannot be moved`.

### Owned paths and borrowed roots

An **owned root** is a local, a by-value parameter or an owned `match`
binding.  An **owned path** is an owned root followed only by unique (or
`const`; the mut model, owner 2026-11-03: unique, bare or `mut`) fields and fixed-array elements.  Every other root is **borrowed**: `self`, `view`
and `inout` parameters and locals, `view` and `inout` match bindings,
borrowed results, every `shared`, `shared` and `atomic shared`
reference and the contents of every cell.  Whatever its declared
qualifier, a borrowed root may be reached through a shared alias: `view T`
accepts every form of `T`, and an `inout Node` may name the unique field of
a shared object.

```pkn
class Node { name: Str; }
class Holder { child: Node; }
fn look(n: view Node, h: shared Holder) -> int {
  h.child = Node("b");       // releases the Node that 'n' names
  return n.name.len();
}
fn g(h: view Holder, s: shared Holder) -> int {
  return look(h.child, s);   // called as g(s, s): 'h.child' must stay held
}
```

The lowering holds `h.child` across `call @look` today (a probe's PIR:
`retain nonnull %child.9`) and keeps doing so.  Only owned paths get the
count savings ([Counting](#counting)), operand conflicts in place of holds,
and exact exclusivity.  Borrowed roots keep today's holds and snapshots
(BorrowedReads and SnapshotReads, src/Lowering/LoweringHolds.cpp:62-152),
except for reads inside a `const` region ([`const`](#const); the mut
model, owner 2026-11-03: a region under an immutable root, a bare `T`).

A hold of a handle is an atomic retain of its cell or object.  An object
frozen inside an `atomic shared` lives as long as its root, and a hold of one
writes nothing, so a hold whose path passes through a handle holds the
handle, unless the handle sits in an owned local, in a `const` region (an
immutable root's, in the mut model) or,
for a `parallel` body, in the blocked parent's frame (proposed; owner may
change).

### `const`

(The mut model, owner 2026-11-03: this section's draft forms read as
follows.  `const T` is the bare `T`, `shared const T` is `shared T`
and `atomic shared const T` is `atomic shared T`; the forms that change are
`mut T`, `shared mut T` and `locked mut T`.  The rules below hold with
those spellings, and what differs is noted in place.)

`const` says an object never changes, through any reference, for its whole
life (decision 36).  `const T` has one owner, `shared T` aliases on
one thread with plain counts, and `atomic shared T` is shared across threads
([`atomic shared`](#atomic-shared)).  `let` stays reassignment only, shallow
for a reference and deep for a value (docs/language/02-basics.md:149-154),
and `view` stays a borrow mode.  Decision 36's rules (proposed; owner may
change):

* **Deep.**  Everything reached through a `const` root follows `view`'s
  rules (src/Sema/SemaViews.cpp:3-16): nothing is assigned, moved out or
  lent `inout`, only `view fn`s are called, and no function value is called
  ([Lambdas](#lambdas)).  `atomic` and `locked` fields, channels and
  the objects behind `shared` references stay usable: the reference is
  fixed, what it names may change.  Decision 36 states the `shared`
  exception for `shared`; a `const T` may hold `shared` references
  too (decision 20) and gets the same rule (proposed; owner may change).
* **One-way.**  A unique value becomes `const` through its declared type,
  moved in or new; nothing converts back.  `shared T` never becomes `shared
  const T`, as other aliases may still write; `const T` moves into `shared
  const T` or `atomic shared T` without a copy.  A changed version is a new
  object.  (The mut model, owner 2026-11-03: `mut T` flows into `T`
  freely; `T` gains `mut` only by `move` or straight from a temporary,
  `cannot store a 'Node' in variable 'v', a 'mut Node': a reference gains
  'mut' only by 'move'`; `shared T` and `shared mut T` never convert into
  each other, `cannot store a 'shared mut Node' in variable 'v', a 'shared
  Node': a 'shared' reference, a frozen object and a cell convert only as
  they are`.)
* **Values.**  `const` on a plain value is an error.  On a struct, tuple or
  fixed array holding references it makes them `const`, and the value is
  read only, though a variable not declared `let` may still be assigned
  whole (proposed; owner may change).  The prototype goes further: a
  struct, tuple or array is a value wherever it is and takes no qualifier
  of its own, `const` included (`shared Node[3]` qualifies the elements,
  #368); one is shared or frozen through `mem::Box<T>`, an
  object, and Sema says so: `'const' applies to objects; 'Pair' is a
  value: box it ('mem::Box<Pair>') to share what it holds` (prototype,
  2026-11-03).  (The mut model, owner 2026-11-03: `mut` never goes on a
  value either, `'mut' applies to objects; 'int' is a value: its storage
  is rewritten by assignment, which 'let' fixes`, and in the prototype a
  function value counts as one; an object held in a struct, tuple or array
  follows the variable's `let`.)
* **Borrows.**  A `view` parameter accepts a `const` argument and an
  `inout` one does not (decision 36).  A borrow is written with no
  qualifier: `view const T`, `inout const T`, `view shared T` and `inout
  shared T` are errors (a `view` is read-only already, and an `inout` of
  a `const` object could not change it).  A `view T` takes every form of
  the object, an `inout T` a unique or `shared` one, with no retain or
  release (owner, 2026-11-02).  (The mut model, owner 2026-11-03: a borrow
  still takes no qualifier; `inout` is the borrow's writes, so an `inout`
  parameter, local or `match` binding writes what it names, and it takes
  a `mut T` or a `shared mut T`: `'x' is immutable (a 'T'; write 'mut T'
  to change it); it cannot be passed to 'inout' parameter 'p'`.)
* **Crossing.**  `const T` crosses by `move` when its parts can; `shared
  const` never crosses.  No `const` value holds a `task::Task`: it could
  never be awaited, joined, cancelled or detached (proposed; owner may
  change).  (The mut model, owner 2026-11-03: a `T` or a `mut T` moves
  into `atomic shared T`.)

```pkn
class Node { name: Str; next: Node?; }
let a: const Node = Node("a", None);
a.name = "b";                        // error: cannot assign 'a.name': 'a' is 'const', which never changes; a field that changes is 'atomic' or 'locked'
let s: shared Node = move a;   // no copy
let t: shared Node = Node("t", None);
let u: shared Node = t;        // error: no conversion from 'shared Node' to 'shared Node': other references may still change it
```

In the mut model (owner, 2026-11-03) the same program reads:

```pkn
class Node { name: Str; next: Node?; }
let a = Node("a", None);
a.name = "b";                        // error: 'a' is immutable (a 'Node'; write 'mut Node' to change it); cannot assign to its field 'name'
let s: shared Node = move a;         // no copy
let t: shared mut Node = Node("t", None);
let u: shared Node = t;              // error: cannot store a 'shared mut Node' in variable 'u', a 'shared Node': a 'shared' reference, a frozen object and a cell convert only as they are
```

**Reads through `const`.**  Nothing in a `const` object can be replaced, so
a read of a field of an object in a **`const` region** takes no hold and no
snapshot (decision 36; with plain counts on one thread).  A region is a
`const` root, the objects it owns through unique fields, elements and
value parts, and the objects behind its `const`, `shared` and `atomic shared
const` references, each in turn.  It stops at a `shared` or weak reference,
whose object other aliases may change, and at a cell's contents.  The root
is kept alive by what reached it: an owned binder, a counted local, a `view
const` parameter, or, for `h.cfg` of a `shared Holder`, the hold on the
read of `h.cfg`, a field another alias may replace, and nothing past it.  A
`view T` parameter given a `const` argument does not know it, so its body
keeps today's holds, sound but without the saving (proposed; owner may
change).  (The mut model, owner 2026-11-03: a region's root is an
immutable reference, a bare `T`, a `shared T` or an `atomic shared T`.)

### Parameters, borrows and exclusivity

| Parameter | The caller writes | The callee gets |
|---|---|---|
| `n: Node`, `n: const Node` (the mut model, owner 2026-11-03: `n: mut Node`, `n: Node`) | `move a`, or a temporary (a `mut` one converts to the bare form) | ownership (decision 22) |
| `n: shared Node`, `n: shared Node` (the mut model: `shared mut Node`, `shared Node`) | `s` (a copy), `move s`, or `move a` (converts) | a new shared reference |
| `n: atomic shared Node`, and `atomic`, `locked` | `h` (a copy), `move h`, or `move a` (converts) | another handle |
| `n: view Node` | `a` in any form, or a temporary | a borrow, never counted |
| `n: inout Node` | `a`, a unique or `shared` `Node` (the mut model: a `mut` or `shared mut` one) | the caller's storage |

```pkn
keep(move a);   // fn keep(n: Node): a is unusable afterwards
grow(b);        // fn grow(n: inout Node): a borrow, no 'move'
show(move c);   // fn show(n: view Node): c ends; the Node is dropped after the call
grow(s);        // s: shared Node -- fine: a borrow counts nothing, whatever the qualifier
```

* A plain move-only parameter is always handed over, so lending skips its
  prologue (docs/pir.md:211-268).
* `view T` accepts every form of `T`, and `inout T` a unique or `shared`
  one, both counted plainly, so a store through it releases by the right
  kind (decision 36 for `const`; owner, 2026-11-02).  (The mut model,
  owner 2026-11-03: `inout T` takes a `mut T` or a `shared mut T`.)
* `move x` is a write for exclusivity (callConflict,
  src/Sema/SemaParamModes.cpp:53).  On an owned path, an earlier operand's
  read of a non-Copyable value conflicts with a later `inout` or `move` of
  the same root, in place of holding the read
  (docs/language/03-functions.md:525-545).

```pkn
let t = s + shout(s);            // fn shout(x: inout Str) -- error: 's' is read by the left operand of '+'; a later operand cannot pass it to 'inout' parameter 'x' of 'shout'
let u = j.name + send(move j);   // error: 'j' is read by the left operand of '+'; a later operand cannot move it
```

Exclusivity becomes exact for owned paths: two owned paths to one object
(docs/language/03-functions.md:445-447) need a `shared` alias.  Borrowed
paths stay syntactic and held, and `parallel` loops check them at run time.

A plain `match` binding of a move-only part of storage (a place, a call's
`inout` result, a conditional with a branch that is one) is a borrow of it,
a `view`: `'n' is a 'match' binding, a borrow of 'x' ('match move x' binds
what it owns)`; a temporary subject is owned by its binding.  A downcast
binding keeps its subject's qualifier: on a `shared Obj`, `p: Node` binds a
`shared Node`, and a written qualifier must match, on an arm with no
binding too: `'p' binds part of a 'shared Obj': its type is 'shared Node',
not 'atomic shared Node'`.  A conditional's branches each have that form,
a `None` or a new object becoming `shared` aside.  An `inout` arm of a
`shared mut` subject names `shared mut` storage, and a `view` or `inout`
arm takes no written qualifier (#368).

A type arm's `view` of a subject that is a borrow itself, `self` or a
`view` parameter, local or binding (a `lock` block's `as v: view` binding
among them), is another name for the subject's variable (#368).  It emits
no `box`, and it is lent on wherever the subject is.  A binding that is a
copy is counted: a `shared` part, a `shared fn`'s `self` among them, may be
assigned, moved or passed `inout`.  So is a binding over an `inout`
parameter or local, or an `inout` block's binding, whose subject the arm
may assign while it is live.  Over a `shared` or owned subject the binding
is counted by its type, atomically for a handle (proposed; owner may
change).  The borrow
rule is also what keeps `atomic shared` sound: a `view Obj` may borrow an
`atomic shared` object, and a `box` there would count it plainly
([Counting](#counting)).

### Conversion by declared type

The declared type converts (decision 23), in checkAssignable, the funnel for
every declaration, argument, return and store (src/Sema/Sema.cpp:343-366).

| Into, from | new, or `move` of a unique (`mut T`) | `move` of a `const` (a bare `T`) | a move-only place | `shared` (`shared mut`) | `shared` (was `shared const`) | `atomic shared` | `atomic`, `locked` |
|---|---|---|---|---|---|---|---|
| unique (`mut T`) | moves | error (the mut model: moves) | copy error | error | error | error | error (`atomic` loads into a `T`) |
| `const` (a bare `T`) | converts | moves | copy error | error | error | error | error |
| `shared` (`shared mut`) | converts | error (the mut model: moves) | copy error | copies | error | error | error |
| `shared` (was `shared const`) | converts | converts | copy error | error | copies | error | error |
| `atomic shared` | converts after the send check, frozen inside | the same | copy error | error | error | copies | error |
| `atomic`, `locked` | converts into a new cell (`locked` after the send check) | error | copy error | error | error | error | copies, same qualifier |

(The mut model, owner 2026-11-03: the spellings in parentheses; a bare `T`
moves into `mut T` and `shared mut T`, since `mut` is gained by `move`,
and `mut T` flows into `T` with no move.)

```pkn
class Config { path: Str; hits: atomic int; }
let s: shared Node = move a;                         // converts
let cfg: atomic shared Config = Config("w.bin", 0);   // a new value converts and is frozen inside
let n: Node = s;               // error: no conversion from 'shared Node' to 'Node'
let v: atomic shared Node = s;  // error: no conversion from 'shared Node' to 'atomic shared Node': a shared object stays on its thread
let w: Config = cfg;           // error: no conversion from 'atomic shared Config' to 'Config'
let o: Obj = cfg;              // error: no conversion from 'atomic shared Config' to 'Obj': write 'atomic shared Obj'
```

(The mut model, owner 2026-11-03: the prototype words these `cannot store
a 'shared Node' in variable 'n', a 'Node': a 'shared' reference, a frozen
object and a cell convert only as they are`.)

A Copyable value converts by copy.  Upcasts keep the qualifier: an `atomic shared
const Config` becomes an `atomic shared Obj`, never a plain `Obj`, which
would be counted plainly ([Counting](#counting)), and a `const Node` a
`const Obj` (the mut model, owner 2026-11-03: a `Node` an `Obj`, a `mut
Node` a `mut Obj`; proposed; owner may change).  `sync::Channel` and `task::Loop`
are final and convert only to themselves: as an `Obj` either would be
counted plainly, so `'sync::Channel<int>' is a handle: it cannot become an
'Obj'`, though a `view Obj` may borrow one (proposed; owner may change).
Unique to `const`, `shared` or `shared` (the mut model: `mut T` to `T`,
`shared T` or `shared mut T`) costs nothing at run time; to
a cell, one allocation; to `atomic shared`, one walk of the value, plus an
allocation only to box a value type.

### What each qualifier may hold

| Holder | unique part | `shared`, `shared mut` part | handle | value |
|---|---|---|---|---|
| `mut` or `shared mut` object (the mut model, owner 2026-11-03; was unique or `shared`) | yes | yes (it cannot be sent) | yes | yes |
| bare `T` or `shared T` object, immutable (was `const` or `shared const`) | yes, immutable | yes, usable (it cannot be sent); a `shared mut` part reads immutable | yes, usable | yes, read only |
| `atomic shared` object | yes, frozen | no | yes, usable | yes, read only |
| `locked` cell (`locked mut`; `locked T` alone is read only) | yes | no | yes | yes |
| `atomic` cell | no | no | no | numbers, `bool`, `char`, fixed arrays of them |

A unique object may hold anything (decision 20), and so may a `const` one,
its unique parts `const` (decision 36).  (The mut model, owner 2026-11-03:
a unique field follows its root, writable under a `mut` object and
immutable under a bare one, so it is never written `mut`; a `shared mut`
field is the object's own form and keeps its writes through any root,
while a `shared` field is immutable by its declaration: `field 'f' is
immutable (a 'shared T'; write 'shared mut T' to change it); cannot assign
to its field 'g'`.)  A `locked` cell or an `atomic shared
const` object holds any `T` that passes the send check (decisions 33, 37;
proposed; owner may change), checked at every conversion and per generic
instance (`locked Holder<(Str, int)>`):

```pkn
class Settings { path: Str; cache: shared Cache; }
let st: atomic shared Settings = Settings("w.bin", c);   // c: shared Cache
// error: cannot make 'Settings' 'atomic shared': field 'cache' is shared
```

No `const` value (the mut model: no immutable one) holds a `task::Task` ([`const`](#const)); for `atomic shared
const` the freeze would also write a count the runtime still uses.  A
`locked` cell may hold one, taken out under the lock (proposed; owner may
change).

No other rule is needed, though methods keep plain holds under `self`:
`locked` contents are touched only under the mutex, holds released before
the unlock, and an `atomic shared` object's interior is frozen, its root
counted only where its type is known ([Counting](#counting)).  A value
leaves either by copy, or from a `locked` cell by a checked move out of
borrowed storage.

### The send check

The send check (decision 20) runs at `task::spawnOn`'s arguments, receiver
and result, at every conversion to `atomic shared` or `locked`, and at a
channel's `T`.  The prototype has no `spawnOn`, and its tasks run on a
pool, so it runs the check at every async call's arguments, at every
conversion to `atomic shared` or `locked`, and at the declaration of an
`async fn`'s result, where a result that cannot cross is reported once,
at the function: `the result of 'async fn' 'give' (a 'shared Job') cannot
cross to the task's awaiter: is 'shared Job': a 'shared' reference is
aliased on its own thread` (prototype, 2026-11-03).

| Type | Crossable |
|---|---|
| numbers, `bool`, `char`, payload-free enums, unique and `const` `Str` (the mut model: `Str` and `mut Str`) | yes |
| unique or `const` class (the mut model: bare or `mut`) | when all its fields are, own and inherited |
| `Buffer<T>`, `task::Task<T>` | when `T` is |
| structs, tuples, fixed arrays, optionals, payloads | when their parts are |
| handles: `atomic shared` references (`atomic shared Obj` too), `atomic` and `locked` handles, `sync::Channel<T>`, `task::Loop` | yes: the contents were checked when the object, cell or channel was made |
| `shared T`, `shared T`, and weak references | no: they count plainly, and a weak load is a plain retain |
| function values | no: a function type does not show its captures |
| `Obj` | no: it may be any class |

```pkn
class Job { name: Str; cache: shared Cache; }
let t = task::spawnOn(lp, run(move job));   // error: cannot send 'Job': field 'cache' is shared
```

Deeper paths are dotted (`field 'a.b.cache'`); elements are named by type
(`field 'cache' of 'Job'`) (proposed; owner may change).  Templates are
checked per instance, so the check sees concrete types.

A subclass could hide a shared field (importers subclass,
docs/language/06-traits.md:266-268).  So a class's crossable flag covers its
subclasses in its own module, and a subclass in another module keeps it
(proposed; owner may change).  `Obj` has none.  The flag goes in the `.pkm`
class record (docs/design/pkm.md:363).

```pkn
class Button : ui::Widget { onClick: shared () -> void; }
// error: class 'Button' cannot hold 'shared' field 'onClick': it extends 'Widget', which can be sent to another thread
```

**Moving out of borrowed storage.**  A hold gives a unique object a transient
count of 2.  Moved out through another path to the same storage while the
hold is live, it would cross threads with this thread's reference on it:

```pkn
class Job { name: Str; }
class Queue { jobs: cl::List<Job>; }
fn hand(j: view Job, q: shared Queue, lp: task::Loop) -> int {
  let k = q.jobs.pop();                        // the Job that 'j' names
  let t = task::spawnOn(lp, consume(move k));  // async fn consume(j: Job) -> err::Error
  t.detach();
  return j.name.len();
}
// hand(q.jobs.at(0), q, lp): the caller holds the Job, so its count is 2 at the pop
```

So a move out of storage the frame does not own is checked at run time
(#368; src/Lowering/LoweringMoveOut.cpp).  `mem::replace`, `mem::take` and the Buffer
take under `pop` and `removeAt` are compiler-known, and a native with an
`inout` parameter and a move-only result is MovesOut by rule.  A function
that moves out of its `self` or an `inout` parameter, by them or by calling
such a function, carries a MovesOut summary bit and leaves the check to its
caller: `pop`, `removeAt`, `take`, `popFront`, `popBack`, and `trySend` for
its slot.  A call whose slot is on an owned path needs none.  Otherwise the
call walks the moved value's unique part ([Runtime](#runtime)), stopping at
handles, `shared` and weak links and function values, skipping immortals,
and panics unless every strong count is 1: `a value moved out of 'q.jobs' is
still borrowed`.  It runs whatever the type's crossability: a held `Str` in
an uncrossable `Job { name: Str; cache: shared Cache; }` could otherwise
leave later through an owned path, unchecked.  An unresolved virtual call
returning a move-only value counts as MovesOut, as does a call through a
function value and one of another module's function, which has no summary in
its `.pkm`.  A slot is `self` or an `inout` parameter, through unique parts
only: past a handle the function checks what it takes itself, since its
caller's path to the handle may be owned, and so does a function whose result
is of another form, which no caller checks.  A call that may exchange a
borrowed `inout` place with an owned one (`mem::swap`) is checked in the owned
place when evaluating that place again runs nothing.  `move h.f` through a handle
is an error, so a field never reads as empty.  A hold that would be seen as
a second owner is not left behind: a `view` or `inout` local's ends with its
last use, an arm that binds nothing lets go of the subject as it begins, `move v` of a value held in memory
moves its bytes and a by-value parameter takes over a move-only value's
references.  The cost is one load per
object moved.  So every unique object reachable from an owned root has count
1, and a value that crosses (or converts to `locked` or `atomic shared`) is
new, on an owned path, or checked as it left; the crossing points need no check of their
own, and the freeze's check is a backstop.  The walk also catches a
write-back `inout` arm (docs/language/08-match.md:470-476) whose payload is
moved out through an alias.

### Generics and `Copyable`

Qualifiers are part of type arguments (decision 24): `List<Node>`, `List<const
Node>`, `List<shared Node>` and `List<atomic shared Node>` are separate
instances, each counting by its qualifier.  In a template a qualifier
written on `T` joins `T`'s own, as `T?` collapses
(docs/language/13-generics.md:503-505): `const T` with `T` = `shared Node`
is `shared Node`, and with `T` = `atomic shared Config` is itself;
`const T` on a plain value is the value.  A join that is no qualifier
(`shared` with `atomic shared`, `const` with `locked`, `shared` on a value)
is an error, and each instance is checked whole, so a conversion the join
implies (a `shared Node` into a `shared Node`) is still an error where
it happens.  `atomic T` and `locked T` take no qualified `T`, and `atomic shared
const T` and `locked T` send-check `T` per instance (proposed; owner may
change).  `Copyable` holds for `shared` and `atomic shared` references
and not for `const T`, which stays move-only.  (The mut model, owner
2026-11-03: `const` reads as the bare form and `mut` is the qualifier that
joins, `List<Node>` and `List<mut Node>`.  The prototype implements this
(#368): `mut T` with `T` = `shared mut Node` is `shared mut Node`, `T?`
with `T` = `shared Node` is `shared Node?`, and `mut T` with `T` =
`shared Node` is an error, since a `shared` reference is never written
through, as is `shared T` with `T` = `shared mut Node`; a by-value `T`
binds to a `shared` argument's form, which meets `T: Copyable`, and a
`locked` type argument is not taken yet.)

Inside a template `T` is move-only unless declared `T: Copyable`, a
structural built-in bound beside `ValueType`
(src/Sema/SemaTraits.cpp:94-100, 306-320).  `ValueType` no longer implies
Copyable, so `twice<T: ValueType>` (docs/language/06-traits.md:288) becomes
`T: Copyable`, and function types leave `ValueType`
(src/Sema/SemaTraits.cpp:313-317).

```pkn
fn twice<T: Copyable>(x: T) -> (T, T) { return (x, x); }
fn dup<T>(x: T) -> (T, T) { return (x, x); }   // error: 'T' is not 'Copyable': declare 'T: Copyable' to copy it
impl Copyable for Job {}   // error: 'Copyable' is built in: a type is Copyable by what it holds, and cannot be implemented
```

Templates are checked per instantiation (docs/language/13-generics.md:105-145),
so one used only with `int` would hide a missing bound.  An archetype check
runs each template once more with a synthetic move-only class for every
unbounded parameter, reporting only copy errors (#368, src/Sema/
SemaArchetypes.cpp).  It runs when the module has no other error, on the
module's own templates (functions, classes, structs, enums).  A copy is the
copy rule's, a value read out of a borrow, `[v; n]`, a capture, or a `T:
Copyable` bound the archetype fails: `'T' is not 'Copyable': declare 'T:
Copyable' to copy it`.  An archetype meets every bound but `Copyable`, so a
parameter bounded otherwise is an archetype too; one bounded `Copyable`
takes `int`, `float`, `bool` or `Str` when one meets its bounds, and a
template none meets is not checked.  The archetypes (`%T`) and what they
instantiate reach neither the lowering nor the module's interface.

A method has no bounds of its own (docs/grammar.md:243-247), so methods that
copy go in a conditional `impl` block (`implParam`), whose methods exist only
on instances meeting its bounds (proposed; owner may change):

```pkn
impl List<T: Copyable> {
  fn get(i: unsigned) -> T { ... }
  view fn copy() -> List<T> { ... }
}
nodes.get(0);   // nodes: List<Node> -- error: 'get' needs 'T: Copyable', and 'Node' in 'List<Node>' is not Copyable
```

### Weak references

A weak reference refers only to a `shared` object and never crosses threads
(decisions 33 and 37, revising 25).  It is the `shared` reference that owns
nothing (the owner's decision of 2026-11-07, #369): `weak` is a qualifier,
`u: weak Node?` naming a `shared` object and reading as a `shared Node?`,
`u: weak mut Node?` naming a `shared mut` one and reading as a `shared mut
Node?`.  A store takes exactly that form (Sema's checkWeakStore), and the
generated `__init__`'s parameter of a weak field too.

```pkn
w: weak = a;   // a: Node -- error: cannot store 'a' (a 'Node') in variable 'w', a 'weak Node?': a 'weak' reference names a 'shared' object; declare 'a' 'shared Node'
```

Handles can form cycles: a `locked Registry` holding an `atomic shared
Config` that holds the registry's handle, or a cell holding a handle to
itself.  No weak reference can break one, so it leaks, frozen graphs
included, until a field is cleared; `--track-heap` reports live cells and
`atomic shared` objects at exit (proposed; owner may change).

### `Str` and the containers

`Str` keeps in-place `concat` (src/Runtime/String.c:317-331).  A unique `Str`
moves, `shared Str` aliases as today, `const Str` and `shared Str`
never change (`concat` is no `view fn`), `atomic shared Str` is shared across
threads with nothing inside to freeze, and a `locked Str` changes in place
inside `lock` blocks.  (The mut model, owner 2026-11-03: a bare `Str` and
a `shared Str` never change, `let s = "a"; s.concat("b")` being `'s' is
immutable (a 'Str'; write 'mut Str' to change it); 'concat' is not a 'view
fn'`, while `s + "b"` makes a new string and is fine; `mut Str` and
`shared mut Str` change in place, and a `locked mut Str` in `lock`
blocks.)

```pkn
let s = "abc";
let t = s;             // error: cannot copy 's' (a unique 'Str'): write 'move s' to transfer it, or declare it 'shared Str'
let u = s.toString();  // an explicit copy
let a: shared Str = "x";
let b = a;
b.concat("y");         // a reads "xy" too
```

(The mut model, owner 2026-11-03: `a` is a `shared mut Str` for
`b.concat("y")` to compile.)

`Str(s)` of a `Str` always makes a new unique string; the identity is just
`s`.  Today it returns `s` (docs/language/03-functions.md:283-284), which
would make `Str(sh)` a unique alias of a shared string; `paykan migrate`
rewrites `Str(s)` of a shared string to `s` (proposed; owner may change).

Only `shared` containers are "the same container" when copied (decision 21);
today every container is (stdlib/collections.pkn:14-17,
docs/stdlib/collections.md:49-60).  The containers and `Buffer` become
classes (proposed; owner may change):

* `Buffer<T>` is compiler-known, its object the runtime buffer; `List` holds
  one unique Buffer, an allocation more than today, to be measured.
* `HashMap`, `HashSet` and the priority queues become the classes they wrap
  (stdlib/collections.pkn:563-566, 898-903).
* `LinkedList` keeps node objects, `shared mut`, linked by a `shared mut`
  `next` and `weak mut` `prev` and tail, and the hash table chains
  `shared mut` entries (the owner's decision, 2026-11-07): their shared
  parts make them uncrossable, and `Stack` and `Queue`, on a LinkedList,
  with them.
* Stores take ownership (`push(v: T)`); the eleven copying getters move to
  `impl X<T: Copyable>` blocks; `at(i) -> view T` borrows; `pop`, `removeAt`
  and `take(i)` move.

The prototype keeps `mem::Buffer<T>` a struct whose one field is the
runtime's buffer object, so it takes no qualifier of its own; it crosses
when `T` does, as the send table says, since the object's walk freezes
the elements, and `let xs: atomic shared mem::Box<mem::Buffer<float>> =
mem::Box(move b)` freezes the box, the buffer and its elements.  A
`parallel for` body then reads it through the box's `value()` and the
buffer's `at`, the prototype's route to data larger than a fixed array
(benchmarks/README.md; prototype, 2026-11-03).  The lowering emits a
buffer's `at`, `get` and `len` on a number type in place, as a fixed
array's subscript: a bounds check and a load, no call into the memory
module and no count on the buffer's object, and through a box's
`value()` or `get()` the box's field itself rather than the copy a `view`
result of a struct is (prototype, 2026-11-03).

Threads change a `locked cl::HashMap<Str, int>` and read an `atomic shared
cl::List<Str>`; one thread reads a `shared cl::List<Str>` through any
alias with no holds.  (The mut model, owner 2026-11-03: `locked mut
cl::HashMap<Str, int>`; a container's writing methods, `push`, `set`,
`grow`, `insert`, `remove` and `clear`, are not `view fn`s, so a container
that is written is `mut` once the containers are classes, while in the
prototype `cl::List<T>` is a struct, a value, and a written one is simply
not `let`: `'l' is declared with 'let'; its value cannot change, and 'push'
is not a 'view fn'`; `len`, `at`, `get` and `contains` read through any
form; `mem::Box`'s `get()` returns an `inout T` and needs a `mut` box,
`value()` being the `view fn`.)  Structs whose `shared S` shares their contents are
unsound: a method could move a field out and send it.

### Values that hold unique references

A struct, tuple, array or optional holding a unique reference is moved whole,
never copied (decision 21).  `[Node(); 3]` evaluates `Node()` for each
element, so it holds three nodes (the owner's decision of 2026-11-07, #369):
a non-Copyable value in `[v; n]` is no copy, and `[n; 3]` of a variable `n`
is `'[v; n]' evaluates its value for each of its 3 elements, and 'n' (a
'Node') is not Copyable`.  The lowering's hidden copies for evaluation order
(docs/language/16-memory.md:663-682) never copy a move-only value: on owned
paths they become the exclusivity errors above, elsewhere holds released
after the call.  `err::Error.msg` (stdlib/error.pkn:49-52) becomes `atomic shared
const Str?` (the mut model: `atomic shared Str?`), so `Error` and `res::Result<T, err::Error>` stay Copyable when
`T` is and cross threads (decision 37).

### Lambdas

Copyable values are captured by copy, as today, a handle by an atomic
retain, and a `shared` one by a plain retain; a `const` value (the mut
model: a bare `T`), being
move-only, only by `move`.  `move j` in a lambda body captures `j` by
transfer when the lambda is made (decision 32):

```pkn
let job = Job("a");                                   // class Job { name: Str; }
let score = fn() -> int { return rank(move job); };   // fn rank(j: view Job) -> int
io::println(job.name);   // error: 'job' was moved into a lambda at 2:39; it cannot be read after the move
```

A capture is read-only (docs/design/function-types.md:71-98) and a lambda
may be called again, so each call borrows it.  In the body, `move j` and
every other mention of `j` name the capture, and `move j` may stand only
where a read-only borrow may: a `view` argument, a `view fn` receiver, or an
operand that reads.  Passing it on (to `task::spawn`, a channel or an owning
parameter) is `'job' is captured by the lambda: each call borrows it, so it
cannot be moved out`.

A function type does not record its captures, so decision 21's "function
values that capture objects" cannot be told by type.  Every function value
is Copyable (the owner's decision of 2026-11-07, #369, replacing unique
function values): a lambda captures only Copyable values and `weak` locals,
read-only, so its closure never changes, and its copies share it and count
it; one with no captures has no closure and is just the code's address.  A
function type takes no qualifier of its own; its parameters and result may
(`(shared Node) -> int`, #368).  Since a closure's count is a plain one,
no function value crosses threads in v0.4; a callback for another thread is
a class with a `view fn`, held as `atomic shared`.  A call may change the
objects its `shared mut` captures share (docs/design/function-types.md:84-86),
so one reached through a `const` root is not called, as a method other than
a `view fn` is not.  `const` on a function type is an
error for the same reason (proposed; owner may change).  (The mut model,
owner 2026-11-03: the root is an immutable one, a bare `T`; in the
prototype `mut` on a function type is `'mut' applies to objects; '() ->
int' is a value: its storage is rewritten by assignment, which 'let'
fixes`.)

`self` is a borrow, so only a `shared fn`'s lambda may capture it:
`cannot capture 'self': a method borrows its receiver; declare the method
'shared fn' to keep a reference to it`, reversing
docs/design/function-types.md:89-94 for class methods.  The rules of this
section beyond decision 32 are this design's (proposed; owner may change).

### Methods and `self`

`self` is a borrow of any qualifier (the owner's default); the lowering never
counts it (src/Lowering/LoweringInternal.h:272-275), and Sema now rejects
copying, storing, capturing or `weak` of `self` or of a `view` parameter.  A
method that needs a counted `self` is a `shared fn` (or `shared view fn`),
callable only through a shared reference.  Overrides keep the prefix, as
they keep modes (src/Sema/SemaParamModes.cpp:301): `'add' overrides a
'shared fn'; it must be 'shared fn'` (proposed; owner may change).

```pkn
class Node { parent: weak Node?; first: shared Node?; next: shared Node?; }
impl Node {
  fn adopt(c: shared Node) { c.next = self; }   // error: cannot copy 'self': a method borrows its receiver; declare the method 'shared fn' to keep a reference to it
  shared fn add(child: shared Node) {
    child.parent = self;
    child.next = self.first;
    self.first = child;
  }
}
root.add(n);   // root: Node -- error: 'add' is a 'shared fn': call it through a 'shared' reference ('root' is a unique 'Node')
```

Through a `const`, `shared` or `atomic shared` reference (the mut model,
owner 2026-11-03: a bare `T`, a `shared T` or an `atomic shared T`;
`self` is mutable in every method but a `view fn`, and `__init__` builds
it) only `view
fn`s can be called (decisions 36, 37), and not a `shared view fn`, whose
`self` is a `shared` reference that could be stored and written through
(proposed; owner may change).  `equals` becomes `view fn equals(other:
view Obj) -> bool` (docs/language/05-classes.md:395), so `==` borrows its
operand in any form, the runtime's drop of it
(src/Runtime/RuntimeInternal.h:128) goes, and a native's plain reference
parameter owns its argument (proposed; owner may change).

A native whose result may alias an object other code holds declares it
`shared` or borrowed (`-> view T`), and only a borrowed result aliases part
of a `view` argument, so nothing comes to own a frozen or `const` (the mut
model: immutable) object or
count an `atomic shared` one plainly; otherwise the runtime object it
returns is immutable or locks itself.  `io::stdin()`
is `File(_stdin())` (stdlib/io.pkn:121-122), a new unique `File` around the
one immortal `PaykanFile_Stdin` (src/Runtime/File.c:248-254, 345-346) on
every call and thread, so the runtime locks it (proposed; owner may change).

### Counting

Plain or atomic is chosen by the count itself (#368: its range, from
`PAYKAN_REF_ATOMIC` up, tells it, which costs the one compare the fast paths
already make, so no count site has to know the form of what it counts; this
section's draft chose it by type, since a header bit would
branch on every plain count): an `atomic shared` object,
a cell, a channel, a loop and a task record count atomically, every other
object plainly, and a frozen or immortal count is never written (decision
37).  Atomic counts on user objects are sound only if every retain or
release of an `atomic shared` object happens where the static type says
`atomic shared` (proposed; owner may change):

1. Nothing converts an `atomic shared T` to another qualifier or to a plain
   `Obj`; an upcast keeps it ([Conversion by declared
   type](#conversion-by-declared-type)).
2. A borrow never counts its root: the lowering never retains, releases or
   holds `self`, a `view` parameter or a `view` local, and Sema rejects
   copying, storing, capturing or `weak` of one, so code compiled once for
   `view Node` never touches an `atomic shared` root.  An `inout` parameter
   takes a unique or `shared` object, both counted plainly, so its stores
   count by the right kind (owner, 2026-11-02).
3. A hold or copy is of a place whose declared type the site knows (a
   local, a field, an element), so one of an `atomic shared` place is atomic.
   Under a borrowed root, a hold or snapshot that reaches into an `atomic shared
   const` object meets frozen objects and writes nothing.
4. Templates are instantiated per qualifier, and the runtime helpers that
   count through a `PaykanType` read the kind from the reference word
   ([Runtime](#runtime)).
5. A type arm over a borrow emits no `box`; over an owned or counted
   subject the `box` takes the flag from the binding's type ([Parameters,
   borrows and exclusivity](#parameters-borrows-and-exclusivity)).
6. A native returns part of a `view` argument only as a borrowed result
   ([Methods and `self`](#methods-and-self)).

Unique references skip counting where the owner is visible (the owner's
default), on owned paths only, and reads in a `const` region (an immutable
root's, in the mut model) skip holds
([`const`](#const)):

1. no copies, so no retain at a hand-off, and no prologue retain;
2. keep-alives (src/Lowering/LoweringInout.cpp:137,
   src/Lowering/LoweringHolds.cpp:997) are skipped under an owned path and
   kept under every borrowed root;
3. a `view` local of an owned path goes uncounted;
4. releases stay a decrement and test, flagged `release unique`.

Lock and unlock, `Paykan_parallel_for`'s entry and exit, an await's split
points, and the freeze and thaw are barriers: unknown-effect calls, with a
lock block's holds on the contents and temporaries released before the
unlock and the operand's hold on the cell after it, so
rc-elimination, moves-at-last-use and lending never move a count on
`locked` contents outside the mutex, or onto a value being frozen.  New
`--stats` counters by qualifier and a benchmark measure the savings first.

## Tasks

### `async fn` and `await`

A function that can wait is an `async fn` (decision 10): a free function,
method, `static`, generic or `native` one, compiled to a stackless state
machine that works on the strict-C path.  `await e` is a prefix operator on
an async call written in place or a `task::Task<T>`; anything else is
`'await' takes a call of an async function or a 'task::Task'`.  An async call
must be awaited or spawned, ordinary code cannot `await`, async code calls
ordinary code freely (except code that may `join`), and an override keeps
its slot's colour.  In v0.4 an async function is not a value, `await` cannot
appear in a lambda, a `parallel` body or a `lock` block, `__init__` cannot be
async, and the result is a value: `an 'async fn' returns a value: its result
cannot be 'view' or 'inout'` (proposed; owner may change).  `main` may be
`async fn main() -> int` (decision 11; src/Sema/Sema.cpp:4088).

### Starting tasks

```pkn
import ::io;
import ::task;
import ::sync;
import ::error as err;

async fn produce(ch: sync::Channel<int>, n: int) -> err::Error {
  for i in 0..n {
    let e = await ch.send(i * i);
    if !e.isOk() { return e; }
  }
  ch.close();
  return err::none();
}

async fn main() -> int {
  let ch = sync::Channel<int>(8);
  let p = task::spawn(produce(ch, 100));
  total = 0;
  done = False;
  while !done {
    match await ch.recv() {
      v: Ok { total += v.value; }
      e: Err { done = True; }      // EndOfFile once closed and drained
    }
  }
  let pe = await p;
  io::println(Str(total));
  return if pe.isOk() then 0 else 1;
}
```

* `task::spawn(f(args)) -> task::Task<R>`, a compiler-known function, runs
  the call on this thread's loop, `task::spawnOn(lp, f(args))` on `lp:
  task::Loop` (the owner's default); the argument is an async call written
  in place (proposed; owner may change).
* `task::spawn` never runs the new task before it returns; the task first
  runs when its thread next runs the loop (an `await` that parks, a `join`).
* Inputs are owned (decision 6): unique and `const` values moved, Copyable
  values copied, `shared` and `shared` references for `spawn` only.
  Decision 6's "transferred at their last use" is written `move` (decision
  18).  `spawnOn` send-checks its arguments, receiver and `R`: what crosses
  is moved with the frame or, if Copyable, copied, a handle by an atomic
  retain.  A callee with `view` or `inout` parameters cannot be spawned; a
  spawned method's receiver is kept by the task: `shared`, `shared` or
  `atomic shared` for `spawn`, `atomic shared` for `spawnOn`.
* A spawned function returns `res::Result<T, err::Error>` or `err::Error`
  (decision 8), so a cancelled wait is a value (proposed; owner may change).
* `task::newLoop() -> res::Result<task::Loop, err::Error>` starts a thread
  whose loop runs until exit; making a thread can fail (proposed; owner may
  change).

### Task handles

A task may outlive the function that started it (decision 5): its task
handle can be returned, passed or stored, and a detached task runs on alone.
`task::Task<T>` is unique and passed by value only (proposed; owner may
change).  Its operations are `await t`, `t.join()`, `t.cancel()`,
`t.cancelAfter(ms)`, `t.detach()` and `t.isDone()`; `await`, `join` and
`detach` spend it.  A task handle must be decided (decision 7), and a
typestate check in SemaMoves follows one in a local through every exit:

```pkn
fn start() {
  let t = task::spawn(tick());
}   // error: task 't' is never joined, cancelled or detached
```

`cancel()` decides a task handle without spending it, and dropping a
cancelled one detaches it (proposed; owner may change).  One stored in a field
or a container (a `List<task::Task<err::Error>>` of workers) leaves the
compile-time check: it panics if dropped undecided (`task handle dropped
without join, cancel or detach`), and at exit `Paykan_tasks_finish` reports
every undecided task handle still alive in a leaked cycle (`paykan: 2 task
handles were never joined, cancelled or detached`).  Both soften decision
7's compile error (proposed; owner may change).

### Ordinary and async code

| | Ordinary code | Async code |
|---|---|---|
| Call an async function | only inside `task::spawn` or `spawnOn` | `await f(x)`, or spawn it |
| Wait for a task | `t.join()`, blocking the thread | `await t`, parking the task |
| `t.join()` | yes | error, also through calls |
| `lock`, `parallel for` | yes | yes, blocking the thread for their length |

`t.join()` in async code is a compile error (decision 11), also through a
known call (the Joins summary); an indirect call that joins inside a task
panics: `'join' called inside a task: use 'await'` (proposed; owner may
change).  An ordinary `main` never yields, so its tasks run only while it
waits in `join()`; a warning says so for a task it detaches (proposed; owner
may change).

`t.join()` runs this thread's loop while it waits, so other tasks run inside
the call.  `join`, and every runtime entry that may run this thread's tasks,
is Unknown in effectOf, where a native without `inout` arguments otherwise
does nothing (src/Lowering/LoweringHolds.cpp:1750-1752); a function with the
Joins summary is Unknown to its callers.  Every rule for `await` applies to
a call with the Joins summary: holds, snapshots and the next section.

### Borrows across a wait

An `inout` borrow (a local, an argument of an awaited call, or an `inout`
arm) may be live across an `await`, or a call with the Joins summary, only
when it names storage the waiting frame owns: an owned path, or the frame's
own `inout` parameter, which its caller checked.  Reached through `self`, a
borrowed parameter's object, a `shared` reference, or a borrowed result, it
is an error naming the root, since another task may write there meanwhile
and the update would be lost:

```pkn
class Counter { n: int; }
impl Counter {
  async fn add(k: int) -> err::Error {
    c: inout = self.n;   // error: 'self.n' is reached through 'self'; it cannot be borrowed 'inout' across an 'await': another task may change it there
    let e = await time::sleep(10);
    c = c + k;
    return e;
  }
}
```

A by-address `view` argument (a struct, tuple or slice) of an awaited call
not on an owned path is snapshotted into the frame when Copyable, and is
otherwise `'s.meta' is reached through 'shared' reference 's' and is not
Copyable; it cannot be lent to an awaited call: another task may change it
there` (proposed; owner may change).  A part of a `const` region needs
neither, as it never changes.  Holds keep all of this memory-safe; the rule
keeps it correct.

### Failures and panics

Expected failures are values that `await t` and `t.join()` hand back
(decision 8).  A panic in a task aborts the process, as today; the JIT's
SIGABRT handler is process-wide (src/Backends/LLVM/JIT/JIT.cpp:165-192).
Results of detached tasks are dropped silently (proposed; owner may change).

### Cancellation

Cancellation is cooperative (decision 9).  `t.cancel()` sets a sticky flag in
the task record by an atomic store, under the record's striped lock, then
posts a wake to the task's loop; propagation and the exit rule do the same.
`task::cancelled()` and every cancellation point read the flag atomically, so
a CPU-bound task sees a cancel from another thread at its next check.  On a
pinned thread a CPU-bound task starves its own loop, so only another thread
or exit can cancel it.  The cancellation points are channel `send` and
`recv`, `time::sleep`, file operations and `await t`.  They return the new
`err::ErrorCode::Cancelled`, after `OutOfRange` and before `Other`
(stdlib/error.pkn:44-45, src/Runtime/Runtime.h:373-395) (proposed; owner may
change).

```pkn
class Job { name: Str; }
fn handle(j: Job) { }
async fn serve(ch: sync::Channel<Job>) -> err::Error {
  while True {
    match await ch.recv() {
      j: Ok { handle(move j.value); }   // the payload moves out; 'j' ends
      e: Err { return e.error; }        // Cancelled, or EndOfFile once closed
    }
  }
  return err::none();
}
```

* The flag stays set, so cleanup that must wait is not possible in v0.4;
  `t.cancelAfter(ms)` makes the operations return `TimedOut` (proposed; owner
  may change).
* A wait that returns `Cancelled` first yields once, to the back of the
  ready queue, so siblings and the exit drain still run (proposed; owner may
  change).  A task that ignores `Cancelled` loops forever: `while True { let
  e = await ch.send(1); }`.
* A task always runs, to its first blocking operation if cancelled before it
  starts, so a frame ends only by completing.
* A joiner cancelled in `await t` gets `Err(Cancelled)`, and `t` is cancelled
  too (proposed; owner may change).
* Cancelling a task cancels the tasks it started, except detached ones; a
  parent that ends first hands its children to the grandparent (proposed;
  owner may change).
* A running `parallel` loop is not interrupted; `task::cancelled()` in its
  body reads the starting task's flag (proposed; owner may change).
* Synchronous blocking I/O (`io::stdin().readln()`) is no cancellation point
  and can delay exit until input arrives; in an `async fn` it warns:
  `'readln' blocks its thread; in an 'async fn' it is not a cancellation
  point and can delay exit` (proposed; owner may change).

### Scheduling: pinned loops

Scheduling is pinned (decision 15): each thread runs its own event loop, and a
task resumes on the thread that started it, so shared values may be held
across an `await` and counts stay plain.  Only the owning loop polls a task;
every cross-thread event (a `spawnOn`, a wake, a completion) is a message to
its inbox, and a cancel is the atomic flag plus a wake.  `spawnOn` moves the
frame with the message; the result comes back by a release store.

v0.5 work stealing is not blocked: frames carry no thread identity, every
wake goes through one runtime function, and Sema computes whether a frame
holds an uncrossable value across an `await` (a reserved frame-pinned bit).

### Program exit

When `main` returns, every task still running, detached ones included, is
cancelled, and the process exits once all have stopped (decision 31).  Every
`@main` calls `Paykan_tasks_finish()` before `ret`
(src/Lowering/Lowering.cpp:1129-1161, 1413-1447), so every backend gets the
rule (proposed; owner may change).  It cancels the remaining tasks, runs the
loops until empty, reports undecided task handles and joins the threads.  Tasks
spawned during exit start cancelled; a task that never reaches a
cancellation point delays exit.

## Communicating

### Channels

`sync::Channel<T>` is bounded (decision 30) and an always-crossable handle
class (decision 33), so its `T` must pass the send check (proposed; owner
may change): a `Channel<const Job>` (the mut model: `Channel<Job>`, bare
and immutable; `Channel<mut Job>` moves objects that change) moves values that never change, a
`Channel<atomic shared Config>` copies references.  The ring and the waiters
are runtime state behind the channel's mutex; on one thread the lock is
uncontended.

| Operation | Result |
|---|---|
| `sync::Channel<T>(n)`, `n: unsigned` | capacity `n >= 1` |
| `await ch.send(move v) -> err::Error` | waits while full; `BrokenPipe` after `close`; `Cancelled` |
| `await ch.recv() -> res::Result<T, err::Error>` | the next value; after `close` the rest, then `EndOfFile`; `Cancelled` |
| `ch.close()` | idempotent; wakes every waiter |
| `ch.tryRecv() -> res::Result<T, err::Error>` | the next value, or `WouldBlock` |
| `ch.trySend(slot: inout T?) -> err::Error` | takes the value from `slot` on success (MovesOut); `WouldBlock` |

Decision 9 replaces decision 4's `recv() -> T?`, and decision 30 its
unbounded channels.  A literal capacity 0 is an error, a computed 0 a panic.
A failed send drops its value; a delivered value is never dropped.  The try
operations work in ordinary code and, on a channel local, in `parallel`
bodies (proposed; owner may change).

### `locked` and `lock` blocks

`locked T` holds any crossable `T` in one cell with a mutex, reached only
inside `lock x as v: inout { }` or `lock x as v: view { }` (decision 33;
proposed; owner may change); there is no `locked view` (decision 33) and no
`locked const`, as a value that never changes needs no lock (decision 37).
Besides `atomic`'s numbers and channels, state that changes across threads
is `locked` (decision 37).  (The mut model, owner 2026-11-03: the cell
written in `lock x as v: inout { }` is `locked mut T`; `locked T` alone is
accepted with the warning `'locked T' locks an object nothing writes;
'atomic shared T' is read from every thread with no lock`, and an `inout`
lock block on it is `'x' is 'locked T', whose contents are never written;
declare it 'locked mut T' for a 'lock ... as v: inout' block`, and `mut
locked` is spelled `locked mut`.)

```pkn
class Counts { hits: int[16]; }
class Stats { counts: locked Counts; served: atomic int; }
impl Stats {
  view fn record(bucket: int) {
    lock self.counts as c: inout {
      c.hits[bucket] += 1;
    }
    self.served += 1;
  }
}
let st: atomic shared Stats = Stats(Counts([0; 16]), 0);
st.record(3);              // on any thread that has a handle
let h = st.counts.hits;    // error: 'st.counts' is 'locked': reach its contents in 'lock st.counts as c: view { }'
```

(The mut model, owner 2026-11-03: `counts: locked mut Counts`, as `record`
writes it.)

The operand (`lockStmt`) is a `locked` variable, field path or parameter,
borrowed, and held if reached through a borrowed root, until after the
unlock.  `c` is a local borrow of the contents (src/Sema/SemaBorrows.cpp:3-21)
ending at the closing brace.  Both modes take the cell's one non-recursive
mutex (proposed; owner may change), released on every exit; the unlock is a
count barrier ([Counting](#counting)).

The rules are strict (decision 29): `await`, a nested `lock`, `join` and a
`parallel` loop are errors in a lock block, also through calls, which
decision 29 left to be specified (proposed; owner may change):

* A resolvable call to a function with the Waits summary is an error.  Waits
  means it contains a `lock`, a `parallel` loop, a `join` or a blocking
  native (`io::File` reads and writes, stdin reads), or calls a function
  that does.  `await` needs no analysis: an ordinary callee cannot await.
* An unresolvable call (a function value, or a virtual call not resolved
  through an exact or closed class) is an error, as in a `parallel` body:
  `'f' may run code that takes a lock; a 'lock' block calls only functions
  it can resolve`.
* Native code calling back into Paykan code (`toString` from printing,
  `equals`) meets a per-thread held-lock depth, a run-time backstop: a nested
  acquire or a `join` panics, and `parallel_for` runs inline.

No thread waits, while holding a cell's mutex, for anything that may wait
for one, so `lock`, `join` and `parallel` cannot deadlock.  Other waits
still can: two tasks awaiting each other's task handles or channels, or a
lock block spinning on an `atomic` that another thread sets only after
taking the same lock.  A task awaiting its own task handle panics: `a task
cannot wait for itself` (proposed; owner may change).

### `atomic`

`atomic T` holds a number, `bool`, `char`, or a fixed array of them as one
cell of atomic elements (decision 33; proposed; owner may change); the
prototype's `atomic T` takes every number type, `bool` and `char`, and no
fixed array yet: `'atomic' holds a number, a 'bool', a 'char' or a fixed array of them, not
'Node': an object crosses threads as 'atomic shared Node' (frozen) or
'locked Node'` (prototype, 2026-11-03; the mut model: `'locked mut
Node'`).  A
read is an atomic load and `=` an atomic store; `+=`, `-=`, and on unsigned
types `&=`, `|=` and `^=`, are atomic read-modify-writes.  Integers wrap
like `+` (docs/language/02-basics.md:691); floats add by a compare-and-swap
loop, `f16` and `f32` rounding to the type inside it
(docs/language/02-basics.md:428-430).  `x.exchange(v)` returns the value
found.  `x.compareExchange(expected, desired) -> bool` stores `desired` when
the cell holds `expected` bit for bit, and otherwise writes the value found
into `expected`, an `inout` argument, as C11 does, so a float's `-0.0` or
NaN cannot fake a success (proposed; owner may change).  All are
sequentially consistent.  An array is used by element: `a[i] += 1`,
`a[i].exchange(v)`, `a.len()`.  (Owner, 2026-11-03: `let` follows its
rule here too: a `let` atomic is never assigned to and never changes
through its name, so `=`, `+=`, `-=`, `exchange` and `compareExchange` on
it are errors and a cell that changes is declared without `let`; a `let`
atomic is a warning, since an atomic matters only when threads change it;
`let m = n` is a read-only copy of the value, `m = n` another handle; a
cell field of a `let` object is the object's own, and changes.)

```pkn
fn bump(b: inout bool) { b = True; }
let hits: atomic int = 0;
let seen: atomic bool[64] = [False; 64];
hits += 1;
let was = seen[3].exchange(True);
hits = hits + 1;           // error: 'hits' is 'atomic': 'hits = hits + 1' reads and writes it separately; write 'hits += 1'
hits *= 2;                 // error: '*=' is not an atomic operation on 'hits': use '+=', '-=', 'exchange' or 'compareExchange'
let all: bool[64] = seen;  // error: 'seen' is an 'atomic bool[64]': it is read by element
bump(seen[3]);             // error: 'seen[3]' is an element of an 'atomic bool[64]': it has no handle and cannot be lent
```

Any read of an atomic place on the right of an `=` to that place
(`hits = m::max(hits, v)`, `self.n = self.n + 1`) is an error, and so are
the compound operators not listed (`*=`, `/=`, `%=`, shifts): each would
read and write separately; a `compareExchange` loop does them (proposed;
owner may change).

An atomic's value is never lent or bound by address: an `atomic` variable,
field or element passed to an `inout` or by-address `view` parameter, to
`mem::replace` or `mem::take`, or bound by a borrow local or an `inout` arm
is an error.  A scalar `view` argument is a load, and a whole `atomic` array
is never lent as an array or slice (proposed; owner may change).

A handle is bound by a declaration, an `atomic T` argument, a constructor's
field argument or a literal's element: there an `atomic` operand is the
handle, so `let h = hits;` shares the cell, while an element of an `atomic`
array, which has no handle, is an error; elsewhere it is loaded.  Outside
templates every later `=` to an `atomic` place stores, even through a
`view`, so an `atomic` variable is never rebound; a `locked` or `atomic shared
const` one is, like a `shared` one.  In a template a `T` that is `atomic int`
is an ordinary Copyable handle: a copy shares the cell and `=` rebinds the
slot, as the Buffer natives do, so `HashMap<Str, atomic int>.set` replaces a
counter's cell instead of storing into it; `b = a` of a struct holding an
`atomic` likewise rebinds `b`'s handles (proposed; owner may change).
`atomic` on a reference is an error: a reference shared across threads is
`atomic shared` (decision 37).  (The mut model, owner 2026-11-03: and
`atomic mut T` is `an 'atomic' cell takes no 'mut': it changes by its
operations; an object crosses threads as 'atomic shared T' (frozen) or
'locked mut T'`.)

### `atomic shared`

`atomic shared T` shares a value read-only across threads (decision 37).
There is no `atomic shared`: across threads, state that changes is `locked`,
and `atomic` alone stays numbers.  `T` passes the send check and
holds no `task::Task`; `atomic shared` of a number, `bool` or `char` is an
error, as the value can be copied.  Access is `const`
([`const`](#const)), so `atomic` and `locked` fields and channels inside
stay usable, being handles.  The spelling is the owner's, in place of
decision 37's `atomic const`: `atomic` says the reference crosses threads,
`shared` that the object has several owners, one on each thread at least,
and `const` that nothing writes it, so the two spellings that leave a word
out are parse errors, `atomic const N` (`'atomic const' is 'atomic shared
const': an object read from several threads has several owners; write
'atomic shared T'`) and `atomic shared N` (`'atomic shared T' would
be written from several threads with no lock; write 'atomic shared
T' to freeze it, or 'locked T' to change it under a lock`), `atomic N` of a
class is Sema's `'atomic' holds a number, a 'bool', a 'char' or a fixed array of them, not 'N':
an object crosses threads as 'atomic shared N' (frozen) or 'locked
N'`, and `shared T` stays the single-thread read-only form, which
still does not convert into `atomic shared` (owner, 2026-11-03).  (The mut
model, owner 2026-11-03: `atomic shared T` is the frozen form, two words,
`atomic` the crossing and `shared` the owners, immutability being the
bare type's; `atomic const T` and `atomic shared const T` are `'const' is
gone: a reference is immutable unless written 'mut T' ('shared mut T',
'locked mut T')`, and `atomic shared mut T` is `'atomic shared mut T'
would be written from several threads with no lock; write 'atomic shared
T' to freeze it, or 'locked mut T' to change it under a lock`; `shared T`
stays the single-thread immutable form and `shared mut T` the written
one, neither converting into `atomic shared`, which a `T` or a `mut T`
reaches by `move`.)

```pkn
class Config { path: Str; hits: atomic int; }
impl Config { fn rename(p: Str) { self.path = move p; } }
let cfg: atomic shared Config = Config("w.bin", 0);
cfg.path = "x";     // error: cannot assign 'cfg.path': 'cfg' is 'atomic shared', which never changes; a field that changes is 'atomic' or 'locked'
cfg.rename("x");    // error: 'rename' is not a 'view fn'; through 'atomic shared' reference 'cfg' only 'view fn's can be called
cfg.hits += 1;      // the cell changes, not 'cfg'
```

(The mut model, owner 2026-11-03: `'cfg' is frozen (an 'atomic shared
Config'); cannot assign to its field 'path'` and `'cfg' is frozen (an
'atomic shared Config'); 'rename' is not a 'view fn'`.)

**The object.**  The reference is the object's own pointer, and its count,
in the header, is atomic, with no cell between them (decision 37).  A
struct, tuple or fixed array given `atomic shared` is boxed automatically:
one object of a header and the value, read through the reference, so a
large table is shared without a copy (decision 37; the layout, and boxing
Copyable values too, proposed; owner may change).  Every count of the object is atomic,
and only where its type says so ([Counting](#counting)).  The prototype
boxes nothing by itself: a struct, tuple or array takes no qualifier, and
one is frozen by boxing it, `mem::Box(move v)` into an `atomic shared
mem::Box<T>` slot.  For that `mem::Box<T>` gained `view fn value() ->
view T`, the read of a `const` or frozen box; `get()` returns `inout T`,
which such a box cannot give, so through a `const` form it is `'bx' is
'atomic shared'; 'get' is not a 'view fn'` (prototype, 2026-11-03).  (The
mut model, owner 2026-11-03: `'bx' is frozen (an 'atomic shared
Box<int>'); 'get' is not a 'view fn'`, and through a bare box `'bx' is
immutable (a 'Box<int>'; write 'mut Box<int>' to change it); 'get' is not
a 'view fn'`: `get()` needs a `mut` box.)

**Freezing.**  (#368: as built, every object of the value is counted
atomically and none is a sentinel, so a hold of an object inside keeps it
alive however the root goes, and a site need not know that its path passes
through a handle; there is no thaw, and the last reference to each object
frees it.  A hold of an object inside therefore writes its count.  The rest
of this section is the draft.)  The conversion walks the value once ([Runtime](#runtime)):
every object's strong count must be 1 and its weak count 0; the root's
stays 1, now atomic, and each object inside becomes `PAYKAN_REF_FROZEN`, a
sentinel of its own beside the immortal one, so the thaw never touches None
or Stdin (refining decision 33's "immortal value"; proposed; owner may
change).  Otherwise the walk panics, `a value made 'atomic shared' is still
borrowed`, a backstop to the move-out check.  It stops at handles, other
`atomic shared` objects included, and skips immortals; a unique value is a
tree, so each object costs a load and a store.

**Reading.**  A copy of the reference is an atomic retain of the root.  A
borrow (a `view` argument, a receiver, an outer value of a `parallel` body)
counts nothing, and a hold or copy of an object inside is a plain retain of
a frozen object, which writes nothing, so readers on any thread share no
written cache line; a hold through the reference holds it ([Owned paths and
borrowed roots](#owned-paths-and-borrowed-roots)).  The runtime keeps an
invariant: no runtime object reachable from a crossable value writes when
read, so none keeps a lazy cache.  `io::File`, whose `view fn`s do I/O
(stdlib/io.pkn:86-99), holds an `Obj` (stdlib/io.pkn:76-77) and never
crosses; Stdin locks itself.

**Freeing.**  The thread that releases the last reference (an
acquire-release subtract, after every reader's last read) thaws the
interior, each count back to 1, and runs the ordinary destroy.

**Taking it back.**  `sync::take(move h)` is compiler-known, as
`task::spawn` is: on an `atomic shared T` it returns `res::Result<const T,
err::Error>` (the mut model, owner 2026-11-03: `res::Result<T,
err::Error>`, the bare immutable `T`; whether a thawed value may come back
`mut` is the owner's), the value thawed, when `h` is the last reference (decision
37), and on a `locked T` a `res::Result<T, err::Error>`, the cell's
contents; otherwise it releases `h` and returns `Err(Busy)` (proposed; owner
may change).  One acquire-release subtract decides: the call that brings
the count to zero takes the value, and every other gets `Busy`: of two
racing takes on the last two references, exactly one gets it.

**Swapping.**  v0.4 has no atomically swappable reference slots (decision
37).  A value replaced now and then is an `atomic shared` field of a
`locked` holder: a reader copies the reference under a short lock and reads
it with none; lock-free slots can come later.

```pkn
class Config { path: Str; }
class Current { cfg: atomic shared Config; }
fn current(cur: locked Current) -> atomic shared Config {
  lock cur as c: view { return c.cfg; }               // one atomic retain under the lock
}
fn reload(cur: locked Current, p: Str) {
  lock cur as c: inout { c.cfg = Config(move p); }    // readers keep the old one until they drop it
}
```

## Data parallelism: `parallel for`

### Syntax and meaning

```pkn
fn saxpy(a: float, xs: view float[], ys: inout float[]) {
  parallel for i in 0..xs.len() {
    ys[i] = a * xs[i] + ys[i];
  }
}

fn dot(xs: view float[], ys: view float[]) -> float {
  s = 0.0;
  parallel for i in 0..xs.len() {
    s += xs[i] * ys[i];
  }
  return s;
}
```

`parallel` is a keyword before `for` (decisions 12, 34).  The loop finishes
before the next statement, so it may borrow arrays (decision 6), and its
iterations run on a worker pool.  `..=` and `step` are allowed, since the
trip count is fixed at entry (src/Lowering/Lowering.cpp:1192-1294)
(proposed; owner may change).  A new SemaPar pass verifies that the
iterations are independent; anything it cannot verify is an error.

### What a body may use

A name is per-iteration (the loop variable included) or outer.  An outer name
may be (the owner's default, as decision 33 extends it; parameters, channels
and handles inside values are this design's, proposed; owner may change):

* a value of plain parts (numbers, `bool`, `char`, enums, handles, and
  optionals, tuples, structs and fixed arrays of them, so an `err::Error`
  too), read only, unless a reduction;
* a `view` or `inout` array or slice of plain elements;
* an `atomic`, `atomic shared` or `locked` local or parameter (an `atomic`
  array at any index), or a `sync::Channel` local for the try operations:
  borrowing a handle changes no count, and an `atomic shared` object's
  interior is frozen;
* a reduction variable.

An outer handle is owned by the blocked parent, so the body takes no holds
on it, and never assigns, moves or lends it `inout`.  An outer `atomic`
changes only by its atomic operations and is never a reduction (`hits += 1`
is an atomic add), and an atomic float sum is outside decision 28's
reproducibility (proposed; owner may change).  A frozen `atomic shared
mem::Box<mem::Buffer<T>>` is such a handle, and reading it through
`value()` and `at` is the prototype's route to data larger than a fixed
array ([`Str` and the containers](#str-and-the-containers); prototype,
2026-11-03).  A `mem::Buffer<T>` of numbers that the enclosing function
owns is an outer value a body may use as it uses an `inout` array: read
at any index (`at`, `get`, `len`) and written at the loop's variable only
(`set(i, v)`, the index the variable or a conversion of it, so no two
iterations write one element), and never reshaped (`grow`, `reserve` and
the rest are errors in the body: they could move the storage under the
other iterations); the prototype checks this at each call on the buffer
(prototype, 2026-11-03).  An outer `atomic` changed by a statement of the body
itself, under no `if` or `match`, changes on every iteration, and each
operation moves its cache line between cores, a thousand times a
reduction's cost on 16 threads (benchmarks/README.md); the prototype
warns and names the alternatives, a reduction on a plain variable or a
rarer update (prototype, 2026-11-03).

Unique, `const`, `shared` and `shared` references (the mut model: `T`,
`mut T`, `shared T` and `shared mut T`), weak variables
and function values are errors, since reading one may take a plain count
from several threads.  That holds for the `const` forms (the immutable
ones) too: a `view`
callee or a `view fn`, compiled without knowing its root is `const`, holds
plainly under it, so a value for a body to read is made `atomic shared`, a
walk for a `const T` (a bare `T`) (proposed; owner may change):

```pkn
parallel for i in 0..n { total += nodes.at(i).w; }
// error: 'nodes' (a 'List<Node>') cannot be used in a 'parallel' loop body: an iteration uses only values, arrays of values, and 'atomic', 'atomic shared' and 'locked' handles
```

### Writes and reductions

* An outer array is written, or passed `inout`, only through a path where
  each enclosing `parallel` loop's variable is a whole subscript, so
  `y[i][j]` and `y[j][i]` both pass (proposed; owner may change).
* An array the body writes is read only at the written paths, and a whole
  outer array is never passed `inout`.
* An outer scalar other than an `atomic` changes only by a recognised
  reduction, mentioned nowhere else in the body.

The reductions (proposed; owner may change) are `s += e`, `s -= e`,
`s *= e`, `s = m::max(s, e)` and `s = m::min(s, e)` on floats
(stdlib/math.pkn:83-84), and `s = if e > s then e else s` with `<`, `<=`, `>`
or `>=` and either operand order, on any numeric type.  `e` does not mention
`s`, and `s` is a local or an `inout` parameter.  `s += "x"` parses like a
numeric `+=` (docs/grammar.md:413-422), so reductions are numeric only.

### Reproducible chunking

Reductions are reproducible (decision 28).  A trip count of 0 runs no chunk;
otherwise `n` iterations split into `K(n) = min(n, 256)` chunks: with `q = n
/ K` and `r = n % K`, chunk `c` starts at `c*q + min(c, r)` and runs `q + (c
< r)` iterations (proposed; owner may change).  The thread count never
enters.  Each chunk starts from the identity (`+` from 0 or `-0.0`, `*` from
1, `m::max` and `m::min` from NaN, the conditional forms from the type's
bounds), and the parent folds the partials into `s` left to right.  For `s
-= e` each chunk accumulates `+e`; the parent adds the partials left to
right and subtracts the total from `s` once.  A parallel float sum may
differ from the sequential loop's, never between runs.  With
`-ffp-contract=off` for the emitted C and no `contract` flags in LLVM IR,
`parallel` adds no machine dependence beyond libm's, which sequential code
has too (proposed; owner may change).  Integer reductions are exact: `+` and
`*` wrap (docs/language/02-basics.md:691).

### Calls, control and nesting

* `break`, `return` and `await` are errors; `continue` ends the iteration.
  The prototype rejects `continue` as well: `'continue' cannot be used in a
  'parallel for' body (the prototype)` (prototype, 2026-11-03).
* Calls go only to par-safe functions: no printing or I/O, no native outside
  an allow-list (`::math`, the Str, Buffer, cell and channel runtime), no
  spawn or join, no function value, and no virtual call unresolved through
  an exact or closed class (src/Lowering/LoweringInternal.h:578-599).
* Locks, atomic operations and channel try operations are allowed, in
  unspecified order; reductions stay reproducible.
* Nested `parallel` loops cannot deadlock: with caller-runs scheduling, a
  thread waits only for chunks another thread has claimed.
* Unique elements at the loop's index (`ys[i] = move Node(i)`) are excluded
  in v0.4 (proposed; owner may change).

### Lowering of `parallel for`

No new PIR op and no MLIR change.  The prototype has both: a `parallel`
annotation on PIR's `while` (docs/pir.md section 6), the PIR ops `async
fn`, `async_call` and `await`, and the mlir backend's `pir.parallel`,
`pir.async_call` and `pir.await` ops, which the LLVM path lowers to an
`scf.parallel` and then OpenMP (`convert-scf-to-openmp`, the chunks
scheduled `dynamic, 1`) and to an `async.func` on MLIR's async runtime,
and the C path runs in order (prototype, 2026-11-03).  The body becomes
`<f>.par<N>(ctx: ptr, c: i64, lo: i64, hi: i64)`, outlined like a closure
(src/Lowering/LoweringClosure.cpp:188) with borrowed captures in a `mem`
context and a 256-slot partials array per reduction.  The parent calls
`Paykan_parallel_for(chunk, ctx, n, flags)`, a lent use of the whole context
whose entry and exit are count barriers, and folds.  Since borrowed paths are
checked only syntactically, the entry compares the byte ranges of written
borrowed parameters and written local borrows (`ys: inout = h.arr`) with the
others; on overlap the chunks run in order on the caller (proposed; owner
may change).  A light body, one with no call and no loop of its own, runs
inline on the calling thread below 4096 iterations, a trip count tested at
run time, where the fork and join cost more than the loop
(benchmarks/README.md).  The chunks are handed out dynamically, one at a
time: on a hybrid CPU a static split lets a thread on an efficiency core
set the loop's time, 16 threads then losing to 12 (benchmarks/README.md,
2026-11-02), and the result is the same whichever thread runs a chunk.
ForStmt (include/AST.h:662-691) gains a parallel flag, written
`(parallel)` in the interchange beside `(inclusive)`
(src/ASTInterchange/Writer.cpp:437-450).

## Attributes

v0.4 has none (decision 35): `parallel` is a keyword (decision 34), the lexer
keeps rejecting `@`, and #171's attributes come in v0.5.  The summaries the
compiler infers are `.pkm` flag bits, not FUNC's `Attrs`
(docs/design/pkm.md:445) (proposed; owner may change).

## Runtime

The runtime stays strict C11 (src/Runtime/CMakeLists.txt:20-26), and
`Runtime.h`, which the emitted C includes, never reaches `<stdatomic.h>` or
`<pthread.h>`.

**Thread safety (no language change).**

* The deferred-destroy state (src/Runtime/Shared.c:100-161) becomes one
  `_Thread_local` struct; a NULL `pending` means the inline array, since a
  thread-local's address is not an address constant (C11 6.6p9).
* Sentinels: `PAYKAN_REF_IMMORTAL = INT32_MAX` for None and Stdin
  (src/Runtime/Object.c:70, src/Runtime/File.c:248), which today start at 1
  and are never released (src/Runtime/Runtime.h:140-141);
  `PAYKAN_REF_FROZEN` one below; `PAYKAN_REF_MAX`
  (src/Runtime/Runtime.h:178) two below.  The fast paths stay one compare,
  retain when `(uint32_t)s < PAYKAN_REF_MAX` and release when
  `(uint32_t)s - 2u < PAYKAN_REF_FROZEN - 2u`
  (src/Runtime/Runtime.h:479-517, src/Backends/MLIR/ToLLVM/ToLLVM.cpp:693-700),
  and the slow paths never write either sentinel.  Nor do the other entries
  that count an existing object: `PaykanShared_new` (PIR's `box`, an
  acquire in a type arm, src/Runtime/Shared.c:42-49), `Paykan_weak_new` and
  `Paykan_weak_load` (src/Runtime/Shared.c:169-186); `box` takes the
  `atomic` flag from its binding's type.  The freeze skips immortals and
  the thaw resets only `PAYKAN_REF_FROZEN`, so None and Stdin never join a
  graph (proposed; owner may change).
* Mutexes around the tracking allocator (src/Runtime/Heap.c:61-264) and
  `strerror` (src/Runtime/Error.c:77); `toString` before `flockfile`, for
  line-atomic printing (src/Runtime/IO.c:20-42); a panic gate
  (src/Runtime/Object.c:74-84).
* Stdin operations hold `flockfile(stdin)` throughout, `lastOp`
  (src/Runtime/File.c:99-103) and `io_read_result`'s error check included.
* pthreads behind an internal layer (macOS has no `<threads.h>`), `-pthread`
  on every link (scripts/c_strict.py:194), and a checked target assumption
  (src/Runtime/Runtime.h:62-68, docs/c-backend.md:304-322): an `int32_t`
  count and an `int64_t` word may be accessed through lock-free `_Atomic`
  lvalues of the same size (proposed; owner may change).

**Atomic counts.**  `Paykan_retain_atomic` and `Paykan_release_atomic`
count `atomic shared` objects, cells, channels, loops and task records, and
nothing else.  Each first loads the count and returns at a sentinel, one
compare as on the plain path, so an immortal None in an `atomic shared Str?`
is never written; a live root's count never reaches one, as the retain
panics at `PAYKAN_REF_MAX`, as `count_overflow` does
(src/Runtime/Shared.c:34).  The release is an acquire-release subtract,
since TSan does not model a lone fence; at 1 it thaws an `atomic shared`
object's interior, then runs the destroy through the thread's own
deferred-destroy state, with plain counts, as nothing else refers to the
object.  Nothing weak refers to a handle, so the weak functions
(src/Runtime/Shared.c:169-191) count on one thread, testing the sentinels.
PIR's count operations gain an `atomic` flag set from the type
(src/Lowering/LoweringInternal.h:711-727), and rc-elimination pairs only
like kinds.  `PaykanType` reference words become `offset << 2 | atomic << 1
| nullable` (src/Runtime/Runtime.h:576-599), the bit set for a handle, so
`PaykanType_copy` and `PaykanType_drop` (src/Runtime/Type.c:25, 30),
`PaykanBuffer_set` (src/Runtime/Buffer.c:160-173), the buffer drops and the
tuple and struct `toString` and `equals` holds (src/Runtime/Tuple.c:163-165,
277-280) count each reference by its kind; tuple `equals` borrows its
argument.  A cell is the header and the value, plus a mutex for `locked`;
its operations are runtime calls the LLVM backend may inline, with no new
PIR op (proposed; owner may change).  The ABI version goes from 300 to 400
(src/Runtime/Runtime.h:46).  Every publication (`spawnOn`, a channel send,
an unlock) synchronises by release and acquire, so the plain writes that
built an `atomic shared` value happen before any other thread reads it.

Against decision 33's cell, the count on the object saves an allocation for
a class value and a load per read through the reference; a copy is the same
atomic add.  The header now shares a cache line with the object's first
fields, so frequent copies of one reference on many threads slow reads of
those fields, where the cell kept them apart; borrows count nothing, so only
code that copies such references across threads in a loop pays it, which the
benchmarks measure (proposed; owner may change).

**Walks.**  Every vtable gets a walk slot after `equals`
(src/Runtime/Runtime.h:154-158), generated as the destroy is
(src/Lowering/LoweringClass.cpp:423-426): it visits each unique reference,
own and inherited, through structs, tuples, arrays, optionals and payloads,
and skips handles, weak fields and function values; buffers and tuples
follow their `PaykanType`.  The String and File slots move up one.  One
walker, with an explicit stack like the pending stack
(src/Runtime/Shared.c:100-124), serves the move-out check, the freeze and
the thaw (proposed; owner may change).

**Worker pool.**  The first `parallel` loop with more than one chunk starts
it: `PAYKAN_THREADS - 1` workers (default: the online CPUs), with main's
stack size clamped to 8-64 MiB, since macOS gives threads 512 KiB (proposed;
owner may change).  The caller claims chunks through an atomic counter, and
an acquire-release counter orders writes before the fold.

**Event loop and timers.**  One per thread, made lazily: a ready queue, an
inbox (a mutex plus `EVFILT_USER` or `eventfd`) and a timer heap on the
monotonic clock.  Each turn drains the inbox, polls ready tasks, completes
finished ones and waits in `kevent` or `epoll_wait` until the next deadline;
a timer never fires early.  `time::sleep(ms: int) -> err::Error` takes
milliseconds, and `sleep(ms <= 0)` yields (proposed; owner may change).

**Async files.**  Four helper threads, started on first use, never touch
Paykan objects: a write's bytes are copied at submission, on the loop
thread; a read fills a buffer its operation owns, and the loop thread makes
the `Str`.  One operation per file runs at a time, and it owns the
descriptor: dropping an `fs::File` with an operation in flight closes it
when the operation completes, so an abandoned write never lands in a reused
descriptor.  The API is `await fs::open(path, mode) ->
res::Result<fs::File, err::Error>`, with async `read`, `readln`, `readbytes`
and `write` (proposed; owner may change).

**Task objects.**  A poll returns 0 (pending) or 1 (ready) through the first
slot after the object slots, and the next takes the result
(src/Runtime/Runtime.h:152-169); a woken task is re-polled from its root
(proposed; owner may change).  A task record (about 104 B) holds the atomic
cancel flag and the parent link under a striped lock.  Its two users, the
task handle and the frame, may be on two threads (a `spawnOn`, or a task
handle sent on), so it counts them atomically, the one runtime object
besides handles that does; and no code holds two stripes at once:
propagation and re-parenting collect the children under one stripe,
release it, then lock each child in turn (proposed; owner may change).  A
frame is 32 bytes plus its result, parameters and live locals (typically
64-160 B), a task handle 32 B, a channel about 150 B plus its ring.  A spawn
makes 3 allocations, an async call 1.

**Shutdown.**  `Paykan_runtime_shutdown()` runs after `main` and before the
heap dump at the three entry points
(src/Backends/LLVM/CodeGen/NativeBuild.cpp:61-110, 185-205;
src/Backends/MLIR/ToEmitC/ToEmitC.cpp:1070-1104) and joins the pool, which
restarts lazily for in-process JIT runs.

**TSan.**  A `tsan-linux` job like the ASan one
(.github/workflows/ci.yml:158-215), with `PAYKAN_TSAN`
(cmake/SanitizerSetup.cmake:63-68), runs the suite, C stress tests
(concurrent chain drops, as #26 asks; handle count contention; frozen reads
and `atomic shared` copies against the last release; stdin from two loops) and generated code through
the C path.  It does not block merges until green (proposed; owner may
change).

## Compiler architecture

| Piece | Where | Anchors |
|---|---|---|
| Grammar, frontend | the productions of Syntax, first in the grammar | docs/grammar.md:185-232, 358-400, 658-680 |
| Qualified types | a canonical `QualifiedType`, a form and a `const` bit (the mut model: a `mut` bit); unique is the bare type, and about 90 `ClassType` casts (`isa`, `dyn_cast`, `cast`, by grep over src/ and include/) and 57 `isRefType` calls see through it | include/ASTContext.h:179-196; include/AST.h:2443-2467; src/Sema/Sema.cpp:233-256 |
| Copies, conversions | checkAssignable, arguments, returns, literals, captures; `atomic` operands | src/Sema/Sema.cpp:343-366; src/Sema/SemaLambda.cpp:78 |
| Moves, task handles | new SemaMoves.cpp | src/Sema/SemaClass.cpp:261-410 |
| Exclusivity | `move` as a write; operand conflicts on owned paths | src/Sema/SemaParamModes.cpp:25-70, 350; SemaBorrows.cpp:302 |
| Holds | borrowed roots keep BorrowedReads and SnapshotReads, except in a `const` region (an immutable root's, in the mut model), whose storage nothing writes; holds through handles; type arms over borrowed subjects bind borrows; `join` Unknown | src/Lowering/LoweringHolds.cpp:62-152, 1750-1752 |
| `const`, cells, send check | `const` access (the mut model: the access through a reference not written `mut`) as `view` access, with its exceptions; `atomic` operators and lending rules, `locked` only in `lock`; `atomic shared` boxing of value types; handle classes final; the send check in new SemaSend.cpp; the class flag | src/Sema/SemaViews.cpp:43-168 |
| Generics | Copyable, conditional impls, archetypes, joined qualifiers | src/Sema/SemaTraits.cpp:68-100, 306-320; include/Sema.h:2004-2013 |
| Summaries | par-safe, Waits, Joins, MovesOut: a call-graph fixpoint in Sema | src/Lowering/LoweringInternal.h:537-620 |
| `parallel` verifier | new SemaPar.cpp | src/Sema/Sema.cpp:4423 |
| `move`, counts, walks | a load and a null store, as moves-at-last-use emits; count flags, atomic for every handle; the walk slot | src/Lowering/Lowering.cpp:1934-1973; LoweringInternal.h:711-727; LoweringClass.cpp:423-426 |
| Async lowering | frame, create, take, resume, outlined like closures; an await is an unknown-effect call for holds | src/Lowering/LoweringClosure.cpp:88-316; LoweringHolds.cpp:256-336; LoweringInternal.h:569 |
| Split | AsyncSplit at the end of `ModuleLowering::run`, before the code passes | src/Lowering/Lowering.cpp:535-543; src/Driver/main.cpp:84-92 |
| PIR | count flags; no counts on `view` parameters or `self`; `await` in text only | include/paykan/pir/PIR.h:316-356, 483-520; src/PIR/Verifier.cpp |
| PIR for plugins | the `atomic` flag in the text; the text version follows the release, so backend plugins see 400 and must honour it | include/paykan/pir/Version.h:16; docs/plugins/pir-for-backends.md:15, 82-87 |
| Backends | LLVM inlines atomic counts and may inline cell operations; C calls the runtime | ToLLVM.cpp:617-707; ToEmitC.cpp:312-323; PIRToStandard.cpp:1055-1061 |
| AST interchange | qualifiers, `move`, `await`, `async`, receiver prefixes, `lock` and `parallel` in the reader, writer and format document; the differential frontend tests | src/ASTInterchange/Reader.cpp, Writer.cpp:437-450; docs/plugins/ast-format.md; tests/Frontend/DifferentialTests.cpp |
| JIT | new runtime symbols | src/Backends/LLVM/JIT/JIT.cpp:38, 56-140 |
| `.pkm` | qualifiers in types; flag bits (par-safe, Waits, Joins, MovesOut, async, frame-pinned); the class flag; impl bounds | include/paykan/pkm/Interface.h:80, 94-99; docs/design/pkm.md:441-449 |

The split leaves ordinary PIR: a frame class (state, awaitable, result,
parameters, the locals live across an `await`), a create function under the
function's own name, a take function, and a resume function that is a
`while` over a `switch` of arms.  The split itself needs no change in the
passes, `CODE` (docs/design/pkm.md:995-1016) or the MLIR backends; a
hand-written frame of this shape runs on the JIT and as strict C11 today.
LLVM coroutines and the MLIR async dialect have no strict-C path.  A frame
owns its by-value reference parameters, since a lend ends when its call
returns (docs/pir.md:211-268); `view`, `inout` and `self` stay borrowed,
held by the awaiting statement, under [Borrows across a
wait](#borrows-across-a-wait).

## Migration

v0.4 is one breaking release (decision 26).  `paykan migrate <paths>` is the
automatic fixer: it writes the fixes; `--check` and `--diff` report without
writing (proposed; owner may change).  It uses the compiler as its oracle,
as LET-1 did (scripts/let_check.py:1-40):

1. Parse with the new grammar, renaming identifiers spelled like the new
   keywords, `parallel` (decision 34) and `const` (decision 36; the mut
   model, owner 2026-11-03: `mut` in its place) included,
   such as `shared` in samples/codegen/32_c_identifiers.pkn:47-56; no `.pkn`
   file in the repository spells `const` (by grep).
2. Run Sema in a legacy mode where every unqualified reference is `shared`:
   v0.3's meaning.
3. Infer qualifiers by union-find over the declared types: a copy whose
   source is used again makes both sides shared; `inout`, type arguments and
   fields tie qualifiers; weak targets are shared; the rest is unique.
4. Write `shared`, `move` (`return move x` included), `view` or `inout` on
   parameters, modes on `match` bindings, `equals` signatures, `view` on
   native parameters, `T: Copyable` where the archetype check asks, and `s`
   for `Str(s)` of a shared string.
5. Re-check, fall back to `shared` at the root of each remaining known error,
   and report the rest.

It prefers `shared` to inserted copies, so behaviour, in-place `concat`
included, is unchanged.  v0.3 programs are single-threaded, so it never
writes `atomic`, `atomic shared` or `locked`, and it writes no `const`, a
promise only the programmer can make (proposed; owner may change; the mut
model, owner 2026-11-03: it writes `mut` where the compiler reports a
write through an immutable reference, and nowhere else); the
drafts' spellings were never in a release, so nothing migrates from them.  The repository migrates
the same way, bottom-up, with the diff reviewed: 449 `.pkn` files (154
`self.f = x;` sites in 97 files), 329 docs blocks and about 1,690 programs
embedded in the C++ tests.  The tool runs on the stdlib first; the
container redesign (classes, `Copyable` impl blocks) is a reviewed hand edit
on top.  Samples keep their output and end with no live blocks under
`--track-heap`.  In a probe, 11 of 12 codegen samples alias somewhere, so
programs need `shared` as well as `move`.

**Two settled decisions are reversed:**

* **Transfers are explicit.**  v0.2.0 inferred every transfer (#145;
  docs/language/16-memory.md:371-414, 472-480).  Now a transfer of a
  unique value is written `move x`, and a plain `b = a` is an error; inferred
  moves stay only as an optimisation.
* **An unmarked container is no longer "copies are the same container".**
  stdlib/collections.pkn:14-17, stdlib/memory.pkn:5-7 and
  docs/stdlib/collections.md:49-60 change: a copy of a unique container is an
  error, and only `shared` containers alias.
  tests/Stdlib/container_struct_test.pkn:13-21 becomes an `inout` parameter.

Follow-ons: plain parameters take ownership instead of being lent
(docs/language/16-memory.md:135-141, 540-555); the `concat` example
(docs/language/04-strings-and-conversions.md:103-120) needs `shared Str`;
function values stay Copyable, their closures shared (decision 28); lambdas
capture `self` only in a `shared fn`
(docs/design/function-types.md:89-94); `Str(s)` always copies
(docs/language/03-functions.md:283-284); the `let` rule
(docs/language/02-basics.md:149-154) points to `const` for an object that
never changes (the mut model: to `mut` for an object that changes); and the thread-safety section
(docs/language/16-memory.md:716-722) is rewritten.

## Staging

**v0.4** (decisions 14, 27 and 33-37): the runtime, pool, TSan and
`parallel for`; async/await with timers, files, channels and joins; the
qualifiers, `const` among them (`mut`, in the mut model), with cells and `atomic shared` objects for
decision 14 (c)'s sync types and shared objects; the send check and
`paykan migrate`.

A chunk is about 6-7 hours of multi-agent work plus a CI round; S is a
quarter chunk, M a half, L one, XL two to three.

| Area | Pieces | Chunks |
|---|---|---|
| Ownership | syntax M; qualified types L; copies and `move` L; use after move M; owned paths and the move-out check M; exclusivity M; conversion M; `const` M (`mut`, in the mut model); `const` reads M; weak S; generics L; lowering L; count savings M; PIR M; backends M; interchange and frontends M; `.pkm` M; stdlib L; docs L; tests L | 13-14 |
| Migration | the tool XL; the repository XL | 4-6 |
| Runtime and `parallel` | thread safety M; pool M; stress tests M; TSan S; TSan for LLVM and JIT M; the `parallel` keyword S; interface bits S; summaries L; verifier L; lowering L; docs M | 6-7 |
| Async | syntax M; typing L; spawn M; task handles M; lowering L; holds M; split L; runtime L; timers M; files M; `::task` S; tests and docs L | 8-9 |
| Cells and channels | cells and handle counts M; `locked` and `lock` blocks L; `atomic` M; `atomic shared`: the freeze M, object counts and the type-descriptor bit S, boxing value types S; `Channel` L | 4-5 |
| **Total** | | **35-41, about 37** |

Decisions 33-35 took the first draft's 36 chunks to about 35: its
cross-thread qualifier (M), user-object atomic counts (L) and attributes
(M) went; the `parallel` keyword (S), the cells (M) and the freeze (M)
came.  Decisions 36 and 37 add about 1.5: `const` (M; `mut` now), mostly Sema on
`view`'s checks, and its hold-free reads (M), one lowering change; and for
`atomic shared`, the object counts (S) and the boxing of value types (S), on
the handle counts, type-descriptor bit and freeze already planned.  About
37 is still about twice decision 14's 13-17, which predates decisions
17-26: ownership adds about 18.

Order (proposed; owner may change): (1) runtime thread safety, and the
qualifier front end with the legacy mode as the default, so existing tests
keep passing; (2) the send check, generics, `const` (`mut`, in the mut model) and the cells; (3)
migration, flipping the default in one chunk; (4) `parallel for`; (5)
async, then channels.

Dependencies: **#171** is no longer one (decision 35).  **#353** (generic
methods) changes how `impl` methods are instantiated, as conditional `impl`
blocks do (include/Sema.h:1736-2013): land it first or together.  **#354**
(higher-order functions on the collections) is written against the
container classes and `Copyable` blocks, after the stdlib rewrite, or it is
migrated twice.

**v0.5:** work stealing with the hold-across-await check (decision 15);
ordinary functions as tasks on a compute pool (decisions 11, 14); sockets,
DNS, processes and pipes with #156 (decision 27); attributes (#171,
decision 35); relaxations such as unique elements at a `parallel` index.
**Later:** the dataflow layer (decision 2), tensors and packed element
storage (decision 13), accelerators with their own reduction answer
(decision 28), Windows.

## Decisions log

| # | Decision | Section |
|---|---|---|
| 1 | One model | Goals |
| 2 | Layered | Goals; Staging |
| 3 | Confinement, plain counts, value structs | Goals; The qualifiers; Counting |
| 4 | Channels, locks, atomics (revised by 33) | Communicating (as revised by 9, 30 and 33) |
| 5 | Free-standing tasks | Task handles |
| 6 | Owned inputs | Starting tasks; Data parallelism |
| 7 | Decided task handles | Task handles |
| 8 | Failures as values | Failures and panics |
| 9 | Cancellation | Cancellation; Channels |
| 10 | async/await | `async fn` and `await`; Compiler architecture |
| 11 | Two ways to wait | Ordinary and async code |
| 12 | `@par for` (spelling revised by 34) | Data parallelism |
| 13 | No tensors yet | Staging |
| 14 | v0.4 scope ((a)'s attributes revised by 35) | Staging |
| 15 | Pinned scheduling | Scheduling |
| 16 | `Shared<T>` | superseded by 17, then 33 |
| 17 | Qualifiers (`thread_shared` revised by 33) | The qualifiers |
| 18 | `move` | Copies and `move`; Use after move; Migration |
| 19 | Frozen fields (revised by 33, then 37) | `atomic shared` |
| 20 | Send check | The send check; What each qualifier may hold |
| 21 | Types covered | `Str` and the containers; Lambdas; Migration |
| 22 | Parameters | Parameters, borrows and exclusivity |
| 23 | Conversion (examples revised by 33) | Conversion by declared type |
| 24 | Generics | Generics and `Copyable` |
| 25 | Weak references (revised by 33 and 37) | Weak references |
| 26 | Migration | Migration |
| 27 | Async I/O | Runtime; Staging |
| 28 | Determinism | Reproducible chunking |
| 29 | Lock rules | `locked` and `lock` blocks |
| 30 | Bounded channels | Channels |
| 31 | Exit | Program exit |
| 32 | Lambda captures | Lambdas |
| 33 | `atomic`, `atomic view`, `locked` qualifiers (`atomic view` and its cell revised by 37) | The qualifiers; What each qualifier may hold; The send check; Weak references; Counting; Communicating; `atomic shared`; What a body may use |
| 34 | The `parallel` keyword | Data parallelism: `parallel for` |
| 35 | Attributes (#171) in v0.5 | Attributes; Staging |
| 36 | `const` and `shared`, apart from `let` and `view` (the mut model, owner 2026-11-03: `mut` in place of the draft's `const`) | The qualifiers; Syntax; `const`; Conversion by declared type; Parameters, borrows and exclusivity; Generics and `Copyable`; Methods and `self` |
| 37 | `atomic shared` in place of `atomic view`, counted on the object; no `atomic shared` | The qualifiers; `atomic shared`; Values that hold unique references; Counting; Runtime |

## Open questions

Each with the answer taken here.  Numbers are stable.  A question answered
by a proposal recorded under decision 33, 36 or 37 stays open for the
owner, as does every rule cited as one of them beyond the qualifiers
themselves.

**The owner's defaults, kept.**

1. **What may a `parallel` body use?**  Values (handles inside included),
   borrowed arrays and slices, and `atomic`, `atomic shared`, `locked` and
   channel locals and parameters, which count nothing (decision 33's
   locals; the rest proposed); no `const` or `shared` value (the mut
   model: no `T`, `mut T`, `shared T` or `shared mut T`)
   (question 105).
2. **What may `locked` and `atomic shared` hold?**  Answered by decisions 33
   and 37's proposals: any `T` that passes the send check; no `const` form
   (no immutable one, in the mut model) holds a `task::Task` (question 100).
3. **Is `self` borrowed?**  Yes; through the `const` forms (the immutable
   ones, in the mut model) only `view fn`s
   (decisions 36, 37).  `shared fn` (23) departs from this default.
4. **Which atomic types are there?**  **Answered by decision 37.**  Numbers,
   `bool`, `char` and their fixed arrays, sequentially consistent (33's
   proposal).  The prototype has the scalars, every number type, `bool`
   and `char`, and no arrays yet (prototype, 2026-11-03).
5. **Where do tasks run?**  `spawn` here; `spawnOn` with crossable inputs.
6. **Do last-use moves stay?**  As an optimisation.
7. **Do unique references skip counting?**  On owned paths, measured first.

**Ownership.**

8. **May a declaration name only the qualifier?**  **Answered by decision
   36.**  Yes: `let x: const = e;`, and `x: shared = e;` likewise (the mut
   model, owner 2026-11-03: `n: mut = Node(1)`).
9. **Is `move` before a temporary accepted?**  Yes, as decision 23 writes
    it; the alternative is an error.
10. **May `move` end a Copyable value?**  Yes, so generic code can write
    `move v`.
11. **Does an assignment revive a moved variable?**  Yes, a non-`let` one.
12. **Is there a partial move?**  Only a payload field of an owned variant
    binding, to empty a `Result`.
13. **Which qualifiers do borrows take?**  **Answered by decision 36.**
    `view` every form, `const` ones included, `inout` a unique or `shared`
    one (the mut model: a `mut` or `shared mut` one); a borrow is written
    with no qualifier at all (owner, 2026-11-02).
14. **May a plain binding copy a unique place?**  No; a temporary subject is
    owned by its binding.
15. **What qualifier does a downcast binding have?**  Its subject's; writing
    it is optional, so `m: Str` in stdlib/error.pkn still reads.
16. **How are send paths named?**  Dotted fields, elements by type.
17. **How are subclasses in other modules checked?**  By a flag fixed in the
    class's module; the alternative is a whole-program check at link.
18. **Is the move-out check worth its loads?**  Yes: without it a held
    unique object crosses threads.  A walk at each crossing point instead
    has no single point for stores inside a `lock` block.  It walks every
    type (question 90).
19. **How do qualified `T`s substitute?**  A qualifier on `T` joins `T`'s
    own: `const T` with `shared Node` is `shared Node` (the mut model:
    `mut T` is the qualifier that joins, its rule the owner's), and on a plain
    value is the value; a join that is no qualifier is an error; `atomic T`
    and `locked T` take none.  The alternative forbids qualified arguments
    to qualified parameters.
20. **How are missing `Copyable` bounds found?**  The archetype check; the
    fallback is CI instantiating every stdlib template with unique and
    shared classes.
21. **Where do copying methods live?**  In conditional `impl` blocks, as
    methods have no bounds.
22. **How does a method keep a counted `self`?**  `shared fn` or `shared
    view fn`; the alternative is a free function taking `shared Node`.
23. **How does `equals` take its operand?**  `view Obj`; a native's plain
    reference parameter owns its argument.
24. **Are the containers classes?**  Yes, with `Buffer`; Stack and Queue on
    a Buffer; LinkedList uncrossable.
25. **What does `Str(s)` of a `Str` do?**  Copies; the alternative keeps the
    identity with the argument's qualifier.
26. **What is `err::Error.msg`?**  **Answered by decision 37.**  `atomic shared
    const Str?` (the mut model: `atomic shared Str?`), so errors cross.
27. **How does a native return an alias?**  A `shared` or borrowed result,
    only the latter aliasing a `view` argument, or a runtime object that
    locks itself, as Stdin.
28. **Are function values unique?**  No (the owner, 2026-11-07): every
    function value is Copyable, its closure shared by its copies and a
    non-capturing one just a function's address; none crosses threads
    (question 86).
29. **Can a lambda pass its `move` capture on?**  No, each call borrows it.
    The alternative, a once-callable lambda with a run-time check, could.
30. **Can a lambda capture `self`?**  Only in a `shared fn`, reversing
    docs/design/function-types.md:89-94.
31. **Is `f(move a)` to a `view` parameter allowed?**  Yes; the value drops
    after the call.

**Tasks.**

32. **Is `spawn` a function or a keyword?**  A compiler-known function taking
    an async call in place.
33. **What may a spawned function return?**  `Result` or `Error`, so a
    cancelled wait is a value.
34. **What does `task::newLoop` return?**  A `res::Result`, since making a
    thread can fail; the thread runs until exit.
35. **What may a task handle be?**  Unique, by value, with `t.isDone()`.
36. **Does `cancel()` spend a task handle?**  No; it decides it, and
    dropping it then detaches.
37. **May a task handle live in a field or container?**  Yes, checked at
    drop and reported at exit.  This softens decision 7's compile error;
    forbidding `task::Task` outside locals, parameters and results keeps it
    at compile time but rules out a list of workers.
38. **Is a joining call a scheduling point?**  Yes, since `join` runs the
    loop.
39. **How is `join` through an indirect call caught?**  By a run-time panic.
40. **Should a task detached in an ordinary `main` warn?**  Yes: it runs only
    while `main` joins.
41. **Which borrows may cross a wait?**  `inout` of owned storage; a `view`
    argument elsewhere is snapshotted when Copyable.  The alternative is an
    error.
42. **Are detached results dropped silently?**  Yes.
43. **Where does `Cancelled` go?**  After `OutOfRange`.
44. **What do the new modules hold?**  `::task` task handles and loops,
    `::time` `sleep(ms: int)`, `::sync` channels and `take`, `::fs` files; a
    `time::Duration` can come later.
45. **Can cleanup wait after a cancel?**  Not in v0.4: the flag is sticky,
    with no shield; `cancelAfter` gives `TimedOut`, with no timeout
    combinator.
46. **Does a cancelled wait yield?**  Once, so its loop is not starved.
47. **What does a cancelled joiner do?**  Cancels the task it awaits.
48. **Where do orphaned children go?**  To the grandparent.
49. **Is a running `parallel` loop interrupted?**  No.
50. **Should blocking I/O in async code warn?**  Yes: it is no cancellation
    point.
51. **Where does exit run?**  `Paykan_tasks_finish` in every `@main`; the
    alternative is the entry points only.
52. **Are busy file helpers awaited at exit?**  No; operations own their
    descriptors.
53. **Is waiting for one's own task handle caught?**  By a run-time panic.
54. **What may an async function be?**  Not a value, lambda or `__init__`,
    no borrowed result; a `native async fn` returns an operation object.
55. **How are frames polled?**  From the root, with striped task locks.
56. **Where does the split run?**  Before the code passes; frames hold what
    is live at an await.

**Communicating and `parallel`.**

57. **What does `Channel<T>` require?**  A crossable `T`, as every channel
    is a cross-thread handle (decision 33); it always locks inside.
58. **Which channel errors?**  `BrokenPipe`, `EndOfFile`; a failed send
    drops its value; `tryRecv`, `trySend(slot)`.
59. **Do `view` lock blocks share the lock?**  No, one mutex for both modes.
    A reader-writer lock can come later, once `view` lock bodies take no
    counts on the contents, holds included.
60. **What may a lock block call?**  Resolvable functions without Waits;
    native callbacks meet a run-time backstop.
61. **Is deadlock impossible?**  Through `lock`, `join` and `parallel`, not
    through task handles, channels or spins; this scopes decision 29's
    wording, not its rules.
62. **Which atomic operations are there?**  Answered by decision 33's
    proposal: the operators, `exchange` and `compareExchange` (question 95).
63. **How are chunks cut?**  `K(n) = min(n, 256)`, free to change before
    1.0.
64. **What if written borrows overlap?**  Chunks run in order; the
    alternative is a panic.
65. **Which reductions?**  Five forms and the four comparisons; bitwise ones
    could join.
66. **Which nested writes pass?**  Every enclosing `parallel` variable a
    whole subscript.
67. **May a body write unique elements at its index?**  Not in v0.4.
68. **Are `..=` and `step` allowed?**  Yes.
69. **Is packed element storage in v0.4?**  No: decision 13 says "possibly";
    it comes with tensors.

**Runtime.**

70. **How are counts atomic in C?**  Runtime functions behind a target
    assumption, for handles (`atomic shared` objects included) and task
    records only; the alternative,
    `_Atomic int32_t` in the header, is to measure.  The sentinels are
    `INT32_MAX` (immortal) and one below (frozen), refining decision 33's one
    immortal value.  Cells are runtime calls, with no new PIR op.
71. **How many threads, how much stack?**  `PAYKAN_THREADS`; 8-64 MiB.
72. **What does the libm wording promise?**  No machine dependence beyond
    libm's.
73. **How do async files work?**  Four helpers; io_uring later.
74. **Are summaries `Attrs`?**  No, flag bits.
75. **Does TSan block merges?**  Not until green.

**Migration and staging.**

76. **Does `paykan migrate` write?**  Yes, as decision 26 says; `--check` and
    `--diff` are proposed.
77. **Is the legacy mode the default until the flip?**  Yes.
78. **In what order?**  Runtime and front end; the send check, generics,
    `const` (`mut`, in the mut model) and cells; migration; `parallel for`; async; channels.
79. **Is v0.4 about 37 chunks?**  Yes, decisions 36 and 37 adding about
    1.5.  TSan for LLVM and the JIT, the count savings and the `const` reads (an immutable root's)
    (about 1.5 chunks) can move to v0.5 without reversing a decision;
    moving async files would reverse decision 27.

**Added by decisions 33-35.**

80. **Do `atomic` and `locked` cover the whole type?**  Yes; `atomic
    int[1024]` is one cell, read by element.  `atomic shared` does too, but
    leaves a trailing `?` outside, so `atomic shared Str?` is an optional
    reference.  A type takes one qualifier; a class holding a handle can be
    `locked`.
81. **May `atomic` hold a reference?**  **Answered by decision 37.**  No: a
    value replaced now and then is an `atomic shared` field of a `locked`
    holder; lock-free reference slots can come later.
82. **What does an `atomic` operand mean?**  The handle where one is bound,
    else a load; a later `=` stores, so an `atomic` variable is never
    rebound, while `locked` and `atomic shared` ones are.
83. **What if `sync::take` gets a handle that is not the last?**  It
    releases it and returns `Err(Busy)`; one acquire-release subtract
    decides.  The alternative hands the handle back in the error, for a
    retry.
84. **What does freezing cost, and when does it panic?**  A load and a store
    per object inside, at the conversion and at the free, on whichever
    thread drops the last reference; it panics only on a held object.  One vtable walk slot
    and one walker serve the move-out check, the freeze and the thaw.
85. **What does an error message cost?**  Its string, counted atomically,
    with nothing inside to freeze and no cell; `err::none()` makes none.
86. **Which function values cross threads?**  None in v0.4; an `atomic shared
    const` lambda (`atomic shared`, in the mut model) written in place could come later.
87. **Will `parallel` apply to other statements?**  Not in v0.4; perhaps
    with the dataflow layer.
88. **Does `atomic shared` take a value type?**  **Answered by decision
    37.**  A struct, tuple or fixed array, boxed automatically; Copyable
    ones too, so a large table is shared without a copy (proposed); a
    number, `bool` or `char` is an error.  The prototype boxes nothing by
    itself: a value takes no qualifier, and one is frozen through
    `mem::Box<T>` (prototype, 2026-11-03).
89. **How do holds keep frozen objects alive?**  By holding the handle their
    path passes through, unless an owned local, a `const` region (an
    immutable root's) or a
    `parallel` body's parent holds it.

**Added in review.**

90. **Does the move-out check walk uncrossable values?**  Yes, every value
    moved out of borrowed storage, as a crossable part may leave later
    through an owned path; a native with an `inout` parameter and a
    move-only result is MovesOut.  A pop of a held element panics on one
    thread too.
91. **Does a type arm count its binding?**  Not over a borrowed subject,
    which it borrows; otherwise by type, a handle atomically.  `box` and the
    weak entries test the sentinels.
92. **May a `sync::Channel` or `task::Loop` become an `Obj`?**  No: both
    are final and convert only to themselves; a `view Obj` may borrow one.
93. **May an atomic's value be lent?**  Never by address, and an element is
    never a handle; a scalar `view` argument loads.
94. **What does `=` do on an atomic in a template or a struct copy?**  It
    rebinds the handle, as the Buffer natives do; it stores only on a place
    typed `atomic` outside templates.  The alternative forbids `atomic` type
    arguments and struct fields in v0.4.
95. **What does `compareExchange` return?**  A `bool`, writing the value
    found into an `inout` `expected` and comparing bits; the alternative
    restricts it to integers, `bool` and `char`.
96. **Which atomic updates are errors?**  A read of the place on the right
    of its own `=`, and `*=`, `/=`, `%=` and the shifts.
97. **What may a `parallel` body do with an outer handle?**  Use it without
    holds, never assign, move or lend it `inout`; an outer `atomic` is never
    a reduction, and an atomic float sum is not reproducible.  The
    prototype warns on an `atomic` changed on every iteration (prototype,
    2026-11-03).
98. **How do handle cycles end?**  They leak until a field is cleared;
    `--track-heap` reports live cells at exit.
99. **How is a task record counted?**  Atomically, by its task handle and
     its frame; no code holds two stripes at once.
100. **May an `atomic shared` hold a `task::Task`?**  No, nor may any
     `const` value (any immutable one, in the mut model): it could never be decided; `locked` may hold one.

**Added by decisions 36 and 37.**

101. **May an `atomic shared` become an `Obj`?**  Only an `atomic shared
     Obj`: an upcast keeps the qualifier, as a plain `Obj` would be counted
     plainly.  The alternative refuses the upcast.
102. **May a weak reference refer to a `shared`?**  Yes: it is a
     `shared` object on one thread, and a cycle can pass through its
     `shared` references; a load gives `shared T?`.  Decision 37
     names only `shared`.
103. **Are `shared` references inside a `const T` usable?**  Yes, as in a
     `shared T`, which decision 36 names: the reference is fixed, its
     object may change.  The alternative makes them read-only.  (The mut
     model, owner 2026-11-03: a `shared mut` field is the object's own
     form and keeps its writes under any root, a bare `T` included; a
     `shared` field is immutable by its declaration.)
104. **Where do reads skip holds?**  Only inside a `const` region (an immutable root's, in the mut model): fields of
     the `const` objects, of what they own and of the `const` objects their
     references name; not past a `shared` or weak reference or into a cell.
     A `view T` callee keeps its holds.
105. **May a `parallel` body read a `const` or `shared` value?**  No (nor a `T` or a `shared T` of the mut model):
     a `view` callee holds plainly under it; it is made `atomic shared`.  The
     alternative allows field reads with no calls.
106. **What is `const` on a struct?**  **Answered by the prototype,
     2026-11-03.**  An error: a struct, tuple or array takes no qualifier
     of its own, and one is shared or frozen through `mem::Box<T>`, an object
     (the mut model, owner 2026-11-03: `mut` on one is `'mut' applies to
     objects; 'Pair' is a value: its storage is rewritten by assignment,
     which 'let' fixes`).
     This design's answer was that its references become `const` and the
     value read only, a non-`let` variable still assigned whole, with the
     alternative leaving its value fields assignable.
107. **Can a `shared view fn` be called through `shared`?**  No: its
     `self` could be stored as a `shared` reference and written through.
     The alternative is a `shared fn` receiver.
108. **Is the count on the object slower than a cell?**  Not for reads,
     which save a load; frequent cross-thread copies share a cache line
     with the object's first fields, measured with the benchmarks.
109. **Can a function value be `const`?**  No (nor `mut`, in the
     prototype's mut model, where it counts as a value), and one reached through a
     `const` root (an immutable one) is not called: a call may change what its captures own.
     Callbacks there are `shared`.

## Diagnostics

Errors unless marked.  `12:9` stands for a source position.

| Situation | Message |
|---|---|
| copy of a unique place | `cannot copy 'a' (a unique 'Node'): write 'move a' to transfer it, or declare it 'shared Node'` |
| copy of a non-Copyable value | `cannot copy 't': 'task::Task<err::Error>' is not Copyable` |
| copy of a holder | `cannot copy 'p': 'Person' holds unique field 'name' ('Str')` |
| copy of a borrow | `cannot copy 'n': it is a 'view' parameter, a borrow` (`an 'inout' parameter`, `a 'view' local`) |
| `[v; n]` of a place | `'[v; n]' evaluates its value for each of its 3 elements, and 'n' (a 'Node') is not Copyable: write a value made for each` |
| read after `move` | `'a' was moved at 12:9; it cannot be read after the move` |
| after a branch | `'a' may have been moved (by the 'if' at 10:3)` |
| move in a loop | `'job' is moved in the loop at 4:3 and not assigned again before the next iteration` |
| moved into a lambda | `'job' was moved into a lambda at 2:39; it cannot be read after the move` |
| capture moved out | `'job' is captured by the lambda: each call borrows it, so it cannot be moved out` |
| `move` of a field | `cannot move 'h.node': 'move' takes a local variable, a by-value parameter or a 'match' binding; write 'mem::replace(h.node, v)'` |
| `move self` | `cannot move 'self': a method borrows its receiver` |
| `move` of a borrow | `cannot move 'n': it is a 'view' parameter, a borrow` (`an 'inout' parameter`, `a 'view' local`) |
| `move` of a weak | `cannot move 'w': a 'weak' variable holds no strong reference` |
| move while borrowed | `'l' is borrowed by 'view' local 'x' until 'x' is last used; it cannot be moved` |
| qualified value | `'shared' qualifies a reference; 'int' is a value` |
| qualified struct, tuple or array (prototype, 2026-11-03) | `'const' applies to objects; 'Pair' is a value: box it ('mem::Box<Pair>') to share what it holds` (also `'atomic shared' qualifies a reference; 'Node[3]' is a value: box it ('mem::Box<Node[3]>') to share what it holds`; since #368 `shared Node[3]` is three shared references, and `shared int[3]` is `'shared' qualifies a reference; 'int[3]' is a value`) (the mut model, owner 2026-11-03: `'mut' applies to objects; 'Pair' is a value: its storage is rewritten by assignment, which 'let' fixes`) |
| two qualifiers | `a type takes one qualifier: 'locked atomic shared Config'`; `write 'shared', not 'const shared'` (the mut model: `a type takes one qualifier: 'shared mut shared' is two`; `'mut shared' is spelled 'shared mut': the form first, then the writes`) |
| `locked view` | `there is no 'locked view': read 'x' in 'lock x as v: view { }'` |
| `locked const` | `there is no 'locked const': a value that never changes needs no lock; share it as 'atomic shared'` (the mut model, owner 2026-11-03: the language has no `const`; `locked T` alone is the warning `'locked T' locks an object nothing writes; 'atomic shared T' is read from every thread with no lock`, and `lock x as v: inout` on it `'x' is 'locked T', whose contents are never written; declare it 'locked mut T' for a 'lock ... as v: inout' block`) |
| `atomic shared` | `there is no 'atomic shared': across threads, share a value read only as 'atomic shared', or one that changes as 'locked'` (the parser, owner, 2026-11-03: `'atomic shared T' would be written from several threads with no lock; write 'atomic shared T' to freeze it, or 'locked T' to change it under a lock`) (the mut model, owner 2026-11-03: `atomic shared T` is the frozen form; `atomic shared mut T` is `'atomic shared mut T' would be written from several threads with no lock; write 'atomic shared T' to freeze it, or 'locked mut T' to change it under a lock`, and `atomic mut T` `an 'atomic' cell takes no 'mut': it changes by its operations; an object crosses threads as 'atomic shared T' (frozen) or 'locked mut T'`) |
| `atomic const` (owner, 2026-11-03) | `'atomic const' is 'atomic shared': an object read from several threads has several owners; write 'atomic shared T'` (the mut model: the language has no `const`) |
| `const` on a value | `'const' applies to objects; 'int' is a value, which 'let' fixes` (also `'Point' holds no reference`) (the mut model: `'mut' applies to objects; 'int' is a value: its storage is rewritten by assignment, which 'let' fixes`) |
| `const` declaration | `'const' is a qualifier: write 'let x: const = e;'` (the mut model: the language has no `const`, and `n: mut = e;` declares a mutable one) |
| `const` function type | `'const' takes no function type: a call may change what its captures own` (the mut model, the prototype: `'mut' applies to objects; '() -> int' is a value: its storage is rewritten by assignment, which 'let' fixes`) |
| qualified handle class | `'sync::Channel' is a handle: it takes no qualifier` (also `'task::Loop'`) |
| handle class to `Obj` | `'sync::Channel<int>' is a handle: it cannot become an 'Obj'` (also `'task::Loop'`) |
| conversions | `no conversion from 'shared Node' to 'Node'`; `no conversion from 'shared Node' to 'atomic shared Node': a shared object stays on its thread`; `no conversion from 'shared Node' to 'shared Node': other references may still change it`; `no conversion from 'const Node' to 'Node': a 'const' object never changes; make a new one`; `no conversion from 'atomic shared Config' to 'Config'`; `no conversion from 'atomic shared Config' to 'Obj': write 'atomic shared Obj'` (the mut model, owner 2026-11-03: `cannot store a 'T' in variable 'v', a 'mut T': a reference gains 'mut' only by 'move'`; `cannot store a 'shared mut T' in variable 'v', a 'shared T': a 'shared' reference, a frozen object and a cell convert only as they are`, also from a `shared T` into a `T` or an `atomic shared T`, and from an `atomic shared T` into a `T`) |
| borrow qualifier | `parameter 'n' is 'inout const': a borrow carries no qualifier; ... write 'inout T'` (the mut model: `'inout mut'` the same) |
| read, later `inout` | `'s' is read by the left operand of '+'; a later operand cannot pass it to 'inout' parameter 'x' of 'shout'` |
| read, later `move` | `'j' is read by the left operand of '+'; a later operand cannot move it` |
| binding a unique place | `'n' is a 'match' binding, a borrow of 'x' ('match move x' binds what it owns); it cannot be stored, only read or passed to a 'view' parameter` |
| binding qualifier | `'p' binds part of a 'shared Obj': its type is 'shared Node', not 'atomic shared Node'` |
| binding a conditional of two forms | `'n' binds part of a conditional whose branches have two forms, 'shared mut Base' and 'atomic shared Base': a binding holds one` |
| shared contents | `cannot make 'Settings' 'atomic shared': field 'cache' is shared` (also `'locked'`; paths dotted); `'Obj' cannot be 'atomic shared': it may be any class`; `a 'const' value cannot hold 'task::Task<err::Error>' (field 'worker'): it could never be decided; use 'locked'` (the mut model: an immutable value; the prototype's `cannot make 'Node(...)' (a 'Node') 'atomic shared': field 'next' is 'shared mut Node': a 'shared' reference is aliased on its own thread`, also `'locked mut'`) |
| cell type | `'atomic' holds a number, 'bool', 'char' or a fixed array of them; 'Node' is a reference: use 'locked Node' or 'atomic shared Node'`; `'atomic shared' shares an object; 'int' is a value: copy it` (the prototype: `'atomic' holds a number, a 'bool', a 'char' or a fixed array of them, not 'Node': an object crosses threads as 'atomic shared Node' (frozen) or 'locked Node'`, 2026-11-03; the mut model: `'locked mut Node'`) |
| atomic operations | `'hits' is 'atomic': 'hits = hits + 1' reads and writes it separately; write 'hits += 1'` (otherwise `use 'compareExchange' in a loop`); `'*=' is not an atomic operation on 'hits': use '+=', '-=', 'exchange' or 'compareExchange'`; `'seen' is an 'atomic bool[64]': it is read by element`; bitwise operators on an `atomic int` get today's `the bitwise operators take unsigned operands` |
| lending an atomic | `'seen[3]' is an element of an 'atomic bool[64]': it has no handle and cannot be lent`; `'hits' is 'atomic': its value cannot be lent 'inout' or bound by address` |
| through a `const` form | `cannot assign 'cfg.path': 'cfg' is 'atomic shared', which never changes; a field that changes is 'atomic' or 'locked'` (also `'const'`, `'shared'`); `cannot pass 'cfg.path' to 'inout' parameter 'p': 'cfg' is 'atomic shared', which never changes`; `cannot move 'c.name' out: 'c' is 'const', which never changes`; `'rename' is not a 'view fn'; through 'atomic shared' reference 'cfg' only 'view fn's can be called`; `'keep' is a 'shared view fn', whose 'self' could be stored and changed; through 'shared' reference 's' only 'view fn's can be called`; `cannot call 'w.onClick': it is reached through 'const' 'w', and a call may change what its captures own; hold it 'shared'` (the mut model, owner 2026-11-03: `'x' is immutable (a 'T'; write 'mut T' to change it); cannot assign to its field 'f'`, also `; 'm' is not a 'view fn'`, `; it cannot be passed to 'inout' parameter 'p'` and `; cannot assign to its elements`; `field 'f' is immutable (a 'shared T'; write 'shared mut T' to change it); cannot assign to its field 'g'`; `'x' is frozen (an 'atomic shared T'); cannot assign to its field 'f'`, also `; 'm' is not a 'view fn'`; a `let` variable keeps `'x' is declared with 'let' and cannot be passed to 'inout' parameter 'p'`) |
| `locked` outside a lock block | `'st.counts' is 'locked': reach its contents in 'lock st.counts as c: view { }'` |
| channel element | `'sync::Channel<shared Node>': a channel's values cross threads, and 'shared Node' cannot` |
| send check | `cannot send 'Job': field 'cache' is shared` (`field 'a.b.cache'`; `field 'cache' of 'Job'`) |
| sending a function value | `cannot send '() -> int': a function value's captures are not part of its type` |
| sending `Obj` | `cannot send 'Obj': it may be any class` |
| subclass flag | `class 'Button' cannot hold 'shared' field 'onClick': it extends 'Widget', which can be sent to another thread` |
| weak to unique | `a weak reference refers to a 'shared' or 'shared' object; 'a' is a unique 'Node' (unique objects form trees, with no cycle to break)` |
| copy of `T` | `'T' is not 'Copyable': declare 'T: Copyable' to copy it` |
| conditional method | `'get' needs 'T: Copyable', and 'Node' in 'List<Node>' is not Copyable` |
| `impl Copyable` | `'Copyable' is built in: a type is Copyable by what it holds, and cannot be implemented` |
| qualified `T` | `'shared T' with 'T' = 'int': 'shared' qualifies a reference; 'int' is a value`; `'locked T' with 'T' = 'atomic int': a type takes one qualifier`; `'const T' with 'T' = 'locked Job': there is no 'locked const'` (the mut model: the language has no `const`, and a `mut T` join is the owner's); `'locked T' with 'T' = 'Job': field 'cache' is shared` |
| storing `self` | `cannot copy 'self': a method borrows its receiver; declare the method 'shared fn' to keep a reference to it` |
| capturing `self` | `cannot capture 'self': a method borrows its receiver; declare the method 'shared fn' to keep a reference to it` |
| `shared fn` call | `'add' is a 'shared fn': call it through a 'shared' reference ('root' is a unique 'Node')` |
| override prefix | `'add' overrides a 'shared fn'; it must be 'shared fn'` |
| undecided task handle | `task 't' is never joined, cancelled or detached` |
| spent task handle | `task 't' was joined at 12:5; it cannot be used again` |
| qualified or borrowed task handle | `'task::Task' is unique: a task handle has one owner`; `a task handle is passed by value: 't' cannot be a 'view' or 'inout' parameter` |
| unawaited call | `'fetch' is an async function: 'await' its call or pass it to 'task::spawn'` |
| `await` operand | `'await' takes a call of an async function or a 'task::Task'` |
| `await` in ordinary code | `'await' is only allowed in an 'async fn'; ordinary code waits with 't.join()'` |
| `await` misplaced | `'await' cannot appear in a lambda`; `'await' cannot appear in a 'parallel' loop body`; `'await' cannot appear in a 'lock' block: a lock is never held across a suspension` |
| async as a value | `async function 'f' is not a value: call it with 'await' or pass a call to 'task::spawn'` |
| override colour | `'run' overrides an async method; it must be 'async fn'`; `'run' overrides an ordinary method; it cannot be 'async fn'` |
| async `__init__` | `'__init__' cannot be async` |
| async borrowed result | `an 'async fn' returns a value: its result cannot be 'view' or 'inout'` |
| `join` in async code | `'join' blocks its thread; inside an 'async fn' write 'await t'` |
| `join` through calls | `'helper' may wait in 'join()' (through 'g'); async code cannot block its thread: await the task instead` |
| spawn argument | `'task::spawn' takes a call of an async function, written in place` |
| spawned result | `a spawned function returns 'res::Result<T, err::Error>' or 'err::Error'; 'f' returns 'int'` |
| async result that cannot cross (prototype, 2026-11-03) | `the result of 'async fn' 'give' (a 'shared Job') cannot cross to the task's awaiter: is 'shared Job': a 'shared' reference is aliased on its own thread` (also `field 'cache' is 'shared Cache': ...`; `is an 'Obj', which may be any class`) |
| spawning a borrower | `cannot spawn 'f': parameter 'xs' is 'view'; a task owns its inputs (a 'parallel' loop may borrow)` |
| spawned receiver | `cannot spawn 'w.run': the receiver of a spawned method is 'shared', 'shared' or 'atomic shared'` |
| `spawnOn` receiver | `'task::spawnOn' runs 'w.run' on another thread: its receiver must be 'atomic shared', and 'w' is a 'shared Worker'` |
| `inout` across a wait | `'self.n' is reached through 'self'; it cannot be borrowed 'inout' across an 'await': another task may change it there` (`through 'shared' reference 's'`, `through 'view' parameter 'h'`; `across a call that may 'join'`) |
| `view` across a wait | `'s.meta' is reached through 'shared' reference 's' and is not Copyable; it cannot be lent to an awaited call: another task may change it there` |
| warning: detached in ordinary `main` | `'t' is detached in an ordinary 'main': it runs only while 'main' waits in 'join()', and is cancelled when 'main' returns` |
| warning: blocking I/O in async code | `'readln' blocks its thread; in an 'async fn' it is not a cancellation point and can delay exit` |
| capacity 0 | `a channel holds at least one value: its capacity cannot be 0` |
| `lock` operand | `'lock' takes a 'locked' variable, field or parameter` |
| nested `lock` | `a 'lock' block cannot contain another 'lock' block: put state used together in one 'locked (A, B)'` |
| `join`, `parallel` in a lock block | `'join' cannot be called in a 'lock' block`; `a 'parallel' loop cannot run inside a 'lock' block` |
| waiting call in a lock block | `'update' may wait (it takes a lock, through 'g'); a 'lock' block cannot call it` (also `it blocks on input`) |
| unresolvable call in a lock block | `'f' may run code that takes a lock; a 'lock' block calls only functions it can resolve` |
| `parallel` misplaced | `'parallel' applies only to 'for' statements` |
| outer reference in `parallel` | `'nodes' (a 'List<Node>') cannot be used in a 'parallel' loop body: an iteration uses only values, arrays of values, and 'atomic', 'atomic shared' and 'locked' handles` (for a `const` form, an immutable one in the mut model, `; make it 'atomic shared'`) |
| outer handle in `parallel` | `'cfg' is a handle owned outside this 'parallel' loop: the body cannot assign, move or lend it 'inout'` |
| warning: atomic changed every iteration (prototype, 2026-11-03) | `'hits' changes on every iteration of the 'parallel for': each atomic operation moves its cache line between cores, a thousand times a reduction's cost on 16 threads (benchmarks/README.md); count in a reduction (a plain variable of the enclosing function, '+='), or change it less often` |
| write elsewhere | `'ys[i + 1]' is written at an index other than the loop's 'i': each iteration writes only its own elements` |
| read of a written array | `'ys' is written at 'ys[i]' and cannot be read at 'ys[i - 1]' in the same 'parallel' loop` |
| whole array `inout` | `'ys' cannot be passed whole to 'inout' parameter 'p' in a 'parallel' loop body` |
| outer scalar | `'count' is assigned in a 'parallel' loop body; an outer variable changes only by a reduction ('count += e')` |
| reduction read | `'s' is a reduction of this 'parallel' loop and cannot be read in its body` |
| `break`, `return` | `'break' cannot leave a 'parallel' loop: every iteration runs` (also `'return'`) |
| unsafe call | `'log' cannot be called in a 'parallel' loop body: it prints (through 'g')` |
| unresolved virtual call | `'shape.area' may run an override from another module; a 'parallel' loop body calls only methods it can resolve` |
| immutable reference (the mut model, owner 2026-11-03) | `'x' is immutable (a 'T'; write 'mut T' to change it); cannot assign to its field 'f'` (also `; 'm' is not a 'view fn'`, `; it cannot be passed to 'inout' parameter 'p'`, `; cannot assign to its elements`); `field 'f' is immutable (a 'shared T'; write 'shared mut T' to change it); cannot assign to its field 'g'`; `'x' is frozen (an 'atomic shared T'); cannot assign to its field 'f'` (also `; 'm' is not a 'view fn'`) |
| gaining `mut` (the mut model) | `cannot store a 'T' in variable 'v', a 'mut T': a reference gains 'mut' only by 'move'`; `cannot store a 'shared mut T' in variable 'v', a 'shared T': a 'shared' reference, a frozen object and a cell convert only as they are` |
| `mut` on a value (the mut model) | `'mut' applies to objects; 'int' is a value: its storage is rewritten by assignment, which 'let' fixes` |
| `mut` on a frozen object or a cell (the mut model) | `'atomic shared mut T' would be written from several threads with no lock; write 'atomic shared T' to freeze it, or 'locked mut T' to change it under a lock`; `an 'atomic' cell takes no 'mut': it changes by its operations; an object crosses threads as 'atomic shared T' (frozen) or 'locked mut T'` |
| `mut` misplaced (the mut model) | `'mut shared' is spelled 'shared mut': the form first, then the writes`; `a type takes one qualifier: 'shared mut shared' is two` |
| warning: `locked T` (the mut model) | `'locked T' locks an object nothing writes; 'atomic shared T' is read from every thread with no lock`; and `lock c as v: inout` on it, an error: `'c' is 'locked T', whose contents are never written; declare it 'locked mut T' for a 'lock ... as v: inout' block` |
| `atomic` of an object (the mut model) | `'atomic' holds a number, a 'bool', a 'char' or a fixed array of them, not 'N': an object crosses threads as 'atomic shared N' (frozen) or 'locked mut N'` |

Run-time panics, printed after `paykan: `: `a value moved out of 'q.jobs'
is still borrowed`, `a value made 'atomic shared' is still borrowed`, `task
handle dropped without join, cancel or detach`, `'join' called inside a
task: use 'await'`, `a task cannot wait for itself`, `channel capacity 0`,
`nested lock`, `'join' while holding a lock`.  At exit, on stderr: `paykan:
2 task handles were never joined, cancelled or detached`.

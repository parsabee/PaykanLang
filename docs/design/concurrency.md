# Concurrency and ownership (v0.4)

Status: proposal, on the owner's decisions of 2026-10-28 (32 numbered
decisions and the defaults after them).  It supersedes issue #26.  Line
references are to the v0.3.0 tree.  A detail the decisions
leave open is this design's choice: it is marked "(proposed; owner may
change)" and listed under [Open questions](#open-questions).  Snippets are
independent: each declares or names the classes it uses.

v0.4 gives Paykan one model for concurrent programs.  Every reference is
unique (the default), `shared` (aliased on one thread, plain counts) or
`thread_shared` (aliased across threads, atomic counts, plain fields frozen),
and ownership moves with an explicit `move`.  Host concurrency is `async fn`
and `await`, compiled to state machines on one event loop per thread; tasks
communicate through bounded channels, rarely through `Locked<T>` or `Atomic`.
Data parallelism is a verified `@par for` with reproducible reductions.
Programs that compile have no data races, and single-threaded code keeps
plain counts.  Two settled rules are reversed: `move` returns, and a copy of
an unmarked container is no longer the same container.  `paykan migrate`
carries programs across.

## Summary of the decisions

| Topic | Decision |
|---|---|
| Qualifiers | unique (unwritten), `shared` or `thread_shared`; no conversion between the last two, none back to unique |
| Transfer | `move x`, `return move x` included; `b = a` of a unique value is an error |
| Borrows | `view` and `inout`, never counted; exclusivity exact on owned paths, held elsewhere |
| Crossing threads | the send check, the contents rule for `thread_shared`, a run-time check on values moved out of borrowed storage |
| Generics | `T` is move-only unless `T: Copyable` |
| Tasks | `async fn`, `await`, `task::spawn`, `task::spawnOn`; every handle awaited, joined, cancelled or detached |
| Scheduling | one event loop per thread, tasks pinned; work stealing in v0.5 |
| Cancellation | cooperative, at waits and `task::cancelled()`, propagated to started tasks |
| Communication | bounded `sync::Channel<T>`, `sync::Locked<T>` in `lock` blocks, `sync::Atomic` |
| Data parallelism | verified `@par for`, reductions reproducible on every machine |
| Runtime | thread-safe C11, kqueue and epoll loops, timers, files on helper threads, TSan |
| Migration | `paykan migrate <paths>` writes `shared` and `move`; one breaking release |

## Goals and non-goals

Goals:

* One model for data parallelism, host concurrency and, later, accelerators
  (decision 1), layered: explicit tasks and loops now, an opt-in dataflow
  layer later (decision 2).
* No data races in programs that compile, with diagnostics naming the path.
* Confinement by default and plain counts in single-threaded code
  (decision 3).  Value structs are the encouraged way into a task: they are
  copied, cross threads freely and need no qualifier.
* Every backend: the JIT, native builds, strict C11 from `--emit-c`, plugins.

Non-goals for v0.4: scoped task blocks (decision 6); CPU functions as tasks
and work stealing (decisions 14, 15); sockets, processes and Windows
(decision 27); the dataflow layer, tensors and accelerators (decisions 2,
13); unbounded channels (decision 30), `select` and async lambdas.

## Ownership

### The three qualifiers

| Qualifier | Written | Holders | Counts | Crosses threads | Made from |
|---|---|---|---|---|---|
| unique | nothing | one owner | plain, often none | by `move` | a new value |
| shared | `shared T` | aliases on one thread | plain | never | a unique, moved or new |
| thread_shared | `thread_shared T` | aliases on any thread | atomic | by copy | a unique, moved or new, after the send check |

There is no conversion between `shared` and `thread_shared`, and none back to
unique (decision 17).  Every reference type takes a qualifier (decision 21):
classes, `Obj`, `Str`, `Box`, `Buffer`, the containers, `Channel`,
`task::Loop` and function values.  Today no type says how a reference is
held (`ast::isRefType`, include/AST.h:2454).

### Syntax

docs/grammar.md changes first, then the frontends (docs/grammar.md:12-13):

```
typeAnnotation ::= qualifier? primaryType ( "[" "]" | "[" ( INT | "_" ) "]" | "?" )*
qualifier      ::= "shared" | "thread_shared"
unary          ::= ( "!" | "-" | "~" | "move" | "await" ) unary | postfix
funcDecl       ::= "async"? receiver? "fn" IDENT typeParams? "(" paramList? ")"
                   ( "->" ( "view" | "inout" )? typeAnnotation )?
                   ( block | "=>" expression ";" )
receiver       ::= "view" | "shared" "view"? | "thread_shared" "view"
nativeFuncDecl ::= "native" "async"? "fn" ...           -- as today after "fn"
implMember     ::= funcDecl | "static" "async"? "fn" ...
implDecl       ::= "impl" ( typeAnnotation "for" )? modulePath
                   ( "<" implParam ( "," implParam )* ">" )? "{" implMember* "}"
implParam      ::= IDENT ( ":" typeAnnotation ( "+" typeAnnotation )* )?
statement      ::= ... | qualDecl | lockStmt | attribute+ forStmt
qualDecl       ::= IDENT ":" qualifier "=" expression ";"
letDecl        ::= "let" IDENT ( ":" ( typeAnnotation | qualifier ) )? "=" expression ";"
lockStmt       ::= "lock" expression "as" IDENT ":" ( "view" | "inout" ) block
```

* The qualifier binds to the primary type: `shared Node?` is an optional of a
  shared Node, `shared Node[3]` is three.  A mode comes first: `n: inout
  shared Node`.
* `x: shared = e;` infers the type, as a borrow declaration does
  (docs/grammar.md:385) (proposed; owner may change).  A receiver prefix
  other than `view` is for methods only.
* `move`, `shared`, `thread_shared`, `async`, `await` and `lock` become
  keywords (include/paykan/Keywords.h:21-25).  `unique` is never written.
* Recovery (docs/grammar.md:672-678) adds `async fn`, `shared fn`, `shared
  view fn` and `thread_shared view fn` to the member and top-level points;
  `lock`, a statement, recovers at `;` and `}` like the rest.
* `mov` stays reserved one release: `'mov' is spelled 'move' since v0.4`
  replaces kMovRemoved (include/paykan/Frontend.h:65-68,
  src/Frontends/RecursiveDescent/Parser.cpp:2512-2535) (proposed; owner may
  change).
* `shared int` is `'shared' qualifies a reference; 'int' is a value`, and
  `shared thread_shared Node` is `a type takes one qualifier: 'shared
  thread_shared Node'`.

### Copies and `move`

A type is **Copyable** when it is a number, `bool`, `char`, an enum with
Copyable payloads, a tuple, struct, fixed array or optional of Copyable
parts, or a `shared` or `thread_shared` reference.  Everything else is
move-only: unique classes, `Str`, containers, function values, task handles,
`Locked`, `Atomic`, and every aggregate holding one.  `Locked` and `Atomic`
are values but not Copyable: a copy would split a lock from its value, or
one atomic word into two.

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
let k = sync::Atomic<int>(0);
let j = k;        // error: cannot copy 'k': 'sync::Atomic<int>' is not Copyable
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
binding.  An **owned path** is an owned root followed only by unique fields
and fixed-array elements.  Every other root is **borrowed**: `self`, `view`
and `inout` parameters and locals, `view` and `inout` match bindings,
borrowed results, and every `shared` or `thread_shared` reference.  Whatever
its declared qualifier, a borrowed root may be reached through a shared
alias: `view T` accepts every qualifier, and an `inout Node` may name the
unique field of a shared object.

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
whatever their declared qualifier.

### Parameters, borrows and exclusivity

| Parameter | The caller writes | The callee gets |
|---|---|---|
| `n: Node` | `move a`, or a temporary | ownership (decision 22) |
| `n: shared Node` | `s` (a copy), `move s`, or `move a` (converts) | a new shared reference |
| `n: thread_shared Node` | `t`, `move t`, or `move a` (converts) | a new thread_shared reference |
| `n: view Node` | `a`, of any qualifier, or a temporary | a borrow, never counted |
| `n: inout Node` | `a`, of exactly `Node` | the caller's storage |

```pkn
keep(move a);   // fn keep(n: Node): a is unusable afterwards
grow(b);        // fn grow(n: inout Node): a borrow, no 'move'
show(move c);   // fn show(n: view Node): c ends; the Node is dropped after the call
grow(s);        // s: shared Node -- error: 's' is a 'shared Node'; 'inout' parameter 'n' takes exactly a 'Node'
```

* A plain move-only parameter is always handed over, so lending skips its
  prologue (docs/pir.md:211-268).
* `view T` accepts every qualifier, and `inout` the exact one, since the
  callee may store a value with that qualifier's release (proposed; owner may
  change).
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
paths stay syntactic and held, and `@par` checks them at run time.

A plain `match` binding of a unique place is `cannot bind 'n' by copy: 'x' is
a unique 'Node'; bind 'n: view Node' or 'n: inout Node', or match 'move x'`;
a temporary subject is owned by its binding.  A downcast binding keeps its
subject's qualifier: on a `shared Obj`, `p: Node` binds a `shared Node`, and
a written qualifier must match: `'p' binds part of a 'shared Obj': its type
is 'shared Node', not 'thread_shared Node'` (proposed; owner may change).

### Conversion by declared type

The declared type converts (decision 23), in checkAssignable, the funnel for
every declaration, argument, return and store (src/Sema/Sema.cpp:343-366).

| Into, from | unique: `move x` or new | unique place | `shared` | `thread_shared` |
|---|---|---|---|---|
| unique | moves | copy error | error | error |
| `shared` | converts | copy error | copies | error |
| `thread_shared` | converts after the send check and the contents rule | copy error | error | copies |

```pkn
class Config { path: thread_shared Str; hits: sync::Atomic<int>; }
impl Config {
  fn __init__(path: Str) {
    self.path = move path;              // a unique Str converts
    self.hits = sync::Atomic<int>(0);
  }
}
let s: shared Node = move a;                       // converts
let cfg: thread_shared Config = Config("w.bin");   // a new value converts
let n: Node = s;                 // error: no conversion from 'shared Node' to 'Node'
let v: thread_shared Node = s;   // error: no conversion from 'shared Node' to 'thread_shared Node': a shared object stays on its thread
let w: Config = cfg;             // error: no conversion from 'thread_shared Config' to 'Config'
```

Upcasts keep the qualifier.  Unique to shared costs nothing at run time;
unique to thread_shared is one call, `Paykan_make_thread_shared`.  A unique
`Obj` cannot convert: `'Obj' cannot be 'thread_shared': it may be any
class`.

### What each qualifier may hold

| Holder | unique field | shared field | thread_shared field | `Locked`, `Atomic` | value |
|---|---|---|---|---|---|
| unique object | yes | yes (it cannot be sent) | yes | yes | yes |
| shared object | yes | yes | yes | yes | yes |
| thread_shared object | no | no | yes, channels included | yes | frozen, if its reference parts are thread_shared |

A unique object may hold anything (decision 20).  A thread_shared object
holds only thread_shared references, weak references to thread_shared
objects, values whose reference parts are those, and `Locked` and `Atomic`
parts (the owner's default, made precise).  The contents rule runs at every
conversion over the static class's own and inherited fields, recursing
through structs, tuples, arrays, optionals and payloads as the send check
does, and per generic instance (`Holder<(Str, int)>`):

```pkn
class Settings { path: Str; }
let st: thread_shared Settings = Settings("w.bin");
// error: 'Settings' cannot be 'thread_shared': field 'path' is a unique 'Str'; a thread_shared object holds thread_shared references, values of them, and Locked or Atomic fields
struct Meta { name: Str; }
class Model { meta: Meta; }
let m: thread_shared Model = Model(Meta("w"));
// error: 'Model' cannot be 'thread_shared': field 'meta.name' is a unique 'Str'; ...
```

The rule is needed: methods are compiled once for every receiver and keep
holds under `self`, and a hold on a unique part of an object other threads
reach would race.  With every reference part thread_shared, each such count
is atomic by type.  A channel field is `thread_shared sync::Channel<T>`
([Channels](#channels)).

Through a thread_shared reference, plain fields are frozen (decision 19), and
only `view fn`s can be called.  A frozen field is like a `view`: no `inout`
argument or `inout` arm.

```pkn
// cfg: thread_shared Config, as above
cfg.path = "x";     // error: cannot assign 'cfg.path': 'cfg' is 'thread_shared', so its plain fields are frozen; a field that changes is a Locked or an Atomic
cfg.rename("x");    // error: 'rename' is not a 'view fn'; through 'thread_shared' reference 'cfg' only 'view fn's can be called
edit(cfg.path);     // fn edit(p: inout thread_shared Str) -- error: cannot pass 'cfg.path' to 'inout' parameter 'p': 'cfg' is 'thread_shared', so its plain fields are frozen
cfg.hits.add(1);    // a sync field's operations are 'view fn's
```

### The send check

The send check (decision 20) runs at `task::spawnOn`'s arguments, receiver
and result, at every conversion to thread_shared (a channel's `T`
included), at `Locked<T>`, and at a thread_shared lambda's captures.

| Type | Crossable |
|---|---|
| numbers, `bool`, `char`, payload-free enums, unique `Str` | yes |
| unique class | when all its fields are, own and inherited |
| `Buffer<T>`, `sync::Channel<T>`, `sync::Locked<T>`, `task::Task<T>` | when `T` is |
| `sync::Atomic<T>` | yes |
| structs, tuples, fixed arrays, optionals, payloads | when their parts are |
| `shared T`, and weak references to it | no: a weak load is a plain retain |
| `thread_shared T`, and weak references to it | yes |
| unqualified function value | no: its captures are not in its type |
| `thread_shared` function value | yes: its captures are checked where it is made |
| `Obj` | no: it may be any class |
| `task::Loop` | only as `thread_shared` |

```pkn
class Job { name: Str; cache: shared Cache; }
let t = task::spawnOn(lp, run(move job));   // error: cannot send 'Job': field 'cache' is shared
```

Deeper paths are dotted (`field 'a.b.cache'`); elements are named by type
(`field 'cache' of 'Job'`) (proposed; owner may change).  Templates are
checked per instance, so the check sees concrete types.

A subclass could hide a shared field (importers subclass,
docs/language/06-traits.md:266-268).  So a class's flags, crossable and
may-be-thread_shared, cover its subclasses in its own module, and a subclass
in another module keeps them (proposed; owner may change).  `Obj` has
neither.  The flags go in the `.pkm` class record (docs/design/pkm.md:363).

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
fn hand(j: view Job, q: shared Queue, lp: thread_shared task::Loop) -> int {
  let k = q.jobs.pop();                        // the Job that 'j' names
  let t = task::spawnOn(lp, consume(move k));  // async fn consume(j: Job) -> err::Error
  t.detach();
  return j.name.len();
}
// hand(q.jobs.at(0), q, lp): the caller holds the Job, so its count is 2 at the pop
```

So a move out of storage the frame does not own is checked at run time
(proposed; owner may change).  `mem::replace` and `mem::take` are
compiler-known.  A function that moves out of its `self` or an `inout`
parameter, by them or by calling such a function, carries a MovesOut
summary bit and leaves the check to its caller: `pop`, `removeAt`, `take`,
`popFront`, `popBack`, and `trySend` for its slot.  A call whose slot is on
an owned path needs none.  Otherwise, for a crossable type, the call walks
the moved value's unique part (unique fields, `Buffer` elements, optionals
and payloads; it stops at shared, thread_shared and weak links and skips
immortals) and panics unless every strong count is 1: `a value moved out of
'q.jobs' is still borrowed`.  An unresolved virtual call returning a
crossable unique value counts as MovesOut.  The cost is one load per object
moved.  A value that crosses is then new, on an owned path, or checked as it
left, so the crossing points need no check of their own.  The walk also
catches a write-back `inout` arm (docs/language/08-match.md:470-476) whose
payload is moved out through an alias; for an uncrossable type two owners on
one thread stay memory-safe, since every release is counted.

### Generics and `Copyable`

Qualifiers are part of type arguments (decision 24): `List<Node>` and
`List<shared Node>` are separate instances.  In a template `shared T`
collapses when `T` is shared and `thread_shared T` when `T` is
thread_shared, as `T?` does (docs/language/13-generics.md:503-505); either
is an error for a value or the other qualifier: `'shared T' with 'T' =
'int': 'shared' qualifies a reference; 'int' is a value`; `'thread_shared T'
with 'T' = 'shared Node': a type takes one qualifier` (proposed; owner may
change).

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
unbounded parameter, reporting only copy errors (proposed; owner may change).

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

A weak reference refers only to a shared or thread_shared object (decision
25).  checkWeakType (src/Sema/Sema.cpp:5288) accepts `shared T?` and
`thread_shared T?`; `weak u = s` takes `s`'s qualifier, and a read gives
`Q T?`.  A weak reference to a shared object never crosses threads; one to a
thread_shared object may sit, frozen, in a thread_shared object.

```pkn
weak w = a;   // a: Node -- error: a weak reference refers to a 'shared' or 'thread_shared' object; 'a' is a unique 'Node' (unique objects form trees, with no cycle to break)
```

### `Str` and the containers

`Str` keeps in-place `concat` (src/Runtime/String.c:317-331).  A unique `Str`
moves, `shared Str` aliases as today, and a thread_shared one is frozen.

```pkn
let s = "abc";
let t = s;             // error: cannot copy 's' (a unique 'Str'): write 'move s' to transfer it, or declare it 'shared Str'
let u = s.toString();  // an explicit copy
let a: shared Str = "x";
let b = a;
b.concat("y");         // a reads "xy" too
```

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
* `Stack` and `Queue` move to a Buffer: LinkedList's weak links
  (stdlib/collections.pkn:268-281) make its nodes shared and uncrossable.
* Stores take ownership (`push(v: T)`); the eleven copying getters move to
  `impl X<T: Copyable>` blocks; `at(i) -> view T` borrows; `pop`, `removeAt`
  and `take(i)` move.

Structs whose `shared S` shares their contents are unsound: a method could
move a field out and send it.

### Values that hold unique references

A struct, tuple, array or optional holding a unique reference is moved whole,
never copied (decision 21).  `[Node(); 3]` is `'[v; n]' copies its value:
'Node' is not Copyable`.  The lowering's hidden copies for evaluation order
(docs/language/16-memory.md:663-682) never copy a move-only value: on owned
paths they become the exclusivity errors above, elsewhere holds released
after the call.  `err::Error.msg` (stdlib/error.pkn:30-32) becomes
`thread_shared Str?`, so `Error` and `res::Result<T, err::Error>` stay
Copyable when `T` is and cross threads (proposed; owner may change).

### Lambdas

Copyable values are captured by copy, as today.  `move j` in a lambda body
captures `j` by transfer when the lambda is made (decision 32):

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
values that capture objects" cannot be told by type: every unqualified
function value is unique, named functions and non-capturing lambdas
included.  Callbacks are `view` parameters; `shared (A) -> R` is today's
copyable function value.  A thread_shared one is made only by a lambda
written where that type is declared, and its captures follow the contents
rule: by copy, thread_shared references and values with no unique or shared
part; by `move`, those and `Locked` and `Atomic`; never a unique reference,
which every calling thread would borrow:

```pkn
let f: thread_shared () -> int = fn() -> int { return measure(c); };
// c: shared Cache -- error: a 'thread_shared' lambda cannot capture 'c': it is a 'shared Cache'
```

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
callable only through a shared reference; `thread_shared view fn` may copy
`self` as thread_shared.  Overrides keep the prefix, as they keep modes
(src/Sema/SemaParamModes.cpp:301): `'add' overrides a 'shared fn'; it must be
'shared fn'` (proposed; owner may change).

```pkn
class Node { weak parent: shared Node?; first: shared Node?; next: shared Node?; }
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

The default's "may touch only sync fields" through thread_shared is read as
"may change only sync fields".  `equals` becomes `view fn equals(other: view
Obj) -> bool` (docs/language/05-classes.md:395), so the runtime's drop of the
operand (src/Runtime/RuntimeInternal.h:128) never meets a thread_shared box,
and a native's plain reference parameter owns its argument (proposed; owner
may change).

A native whose result may alias an object other code holds declares it
`shared` or `thread_shared`; otherwise the runtime object it returns is
immutable or locks itself.  `io::stdin()` is `File(_stdin())`
(stdlib/io.pkn:121-122), a new unique `File` around the one immortal
`PaykanFile_Stdin` (src/Runtime/File.c:248-254, 345-346) on every call and
thread, so the runtime locks it (proposed; owner may change).

### Counting

Plain or atomic is chosen statically, by type: a header bit would branch on
every plain count (against decision 3), and no reference returns from
thread_shared.  Unique references skip counting where the owner is visible
(the owner's default), on owned paths only:

1. no copies, so no retain at a hand-off, and no prologue retain;
2. keep-alives (src/Lowering/LoweringInout.cpp:137,
   src/Lowering/LoweringHolds.cpp:997) are skipped under an owned path and
   kept under every borrowed root;
3. a `view` local of an owned path goes uncounted;
4. releases stay a decrement and test, flagged `release unique`.

Lock and unlock, `Paykan_parallel_for`'s entry and exit, and an await's
split points are barriers: unknown-effect calls, with a lock binding's holds
and temporaries released before the unlock, so rc-elimination,
moves-at-last-use and lending never move a count on a `Locked`'s contents
outside its mutex.  New `--stats` counters by qualifier and a benchmark
measure the savings first.

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
appear in a lambda, a `@par` body or a `lock` block, `__init__` cannot be
async, and the result is a value: `an 'async fn' returns a value: its result
cannot be 'view' or 'inout'` (proposed; owner may change).  `main` may be
`async fn main() -> int` (decision 11; src/Sema/Sema.cpp:4088).

### Starting tasks

```pkn
import ::io;
import ::task;
import ::sync;
import ::error as err;

async fn produce(ch: shared sync::Channel<int>, n: int) -> err::Error {
  for i in 0..n {
    let e = await ch.send(i * i);
    if !e.isOk() { return e; }
  }
  ch.close();
  return err::none();
}

async fn main() -> int {
  let ch: shared sync::Channel<int> = sync::Channel<int>(8);
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
  thread_shared task::Loop` (the owner's default); the argument is an async
  call written in place (proposed; owner may change).
* `task::spawn` never runs the new task before it returns; the task first
  runs when its thread next runs the loop (an `await` that parks, a `join`).
* Inputs are owned (decision 6): unique values moved, Copyable values copied,
  `shared` references for `spawn` only.  Decision 6's "transferred at their
  last use" is written `move` (decision 18).  `spawnOn` send-checks its
  arguments, receiver and `R`.  A callee with `view` or `inout` parameters
  cannot be spawned; a spawned method's receiver is shared (`spawn`) or
  thread_shared (`spawnOn`), kept by the task.
* A spawned function returns `res::Result<T, err::Error>` or `err::Error`
  (decision 8), so a cancelled wait is a value (proposed; owner may change).
* `task::newLoop() -> res::Result<thread_shared task::Loop, err::Error>`
  starts a thread whose loop runs until exit; making a thread can fail
  (proposed; owner may change).

### Handles

A task may outlive the function that started it (decision 5): its handle can
be returned, passed or stored, and a detached task runs on alone.
`task::Task<T>` is unique and passed by value only (proposed; owner may
change).  Its operations are `await t`, `t.join()`, `t.cancel()`,
`t.cancelAfter(ms)`, `t.detach()` and `t.isDone()`; `await`, `join` and
`detach` spend it.  A handle must be decided (decision 7), and a typestate
check in SemaMoves follows a handle in a local through every exit:

```pkn
fn start() {
  let t = task::spawn(tick());
}   // error: task 't' is never joined, cancelled or detached
```

`cancel()` decides a handle without spending it, and dropping a cancelled
handle detaches it (proposed; owner may change).  A handle stored in a field
or a container (a `List<task::Task<err::Error>>` of workers) leaves the
compile-time check: it panics if dropped undecided (`task handle dropped
without join, cancel or detach`), and at exit `Paykan_tasks_finish` reports
every undecided handle still alive in a leaked cycle (`paykan: 2 task
handles were never joined, cancelled or detached`).  Both soften decision
7's compile error (proposed; owner may change).

### Ordinary and async code

| | Ordinary code | Async code |
|---|---|---|
| Call an async function | only inside `task::spawn` or `spawnOn` | `await f(x)`, or spawn it |
| Wait for a task | `t.join()`, blocking the thread | `await t`, parking the task |
| `t.join()` | yes | error, also through calls |
| `lock`, `@par` | yes | yes, blocking the thread for their length |

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
borrowed parameter's object, a shared or thread_shared reference, or a
borrowed result, it is an error naming the root, since another task may
write there meanwhile and the update would be lost:

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
there` (proposed; owner may change).  Holds keep all of this memory-safe;
the rule keeps it correct.

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
(stdlib/error.pkn:24-26, src/Runtime/Runtime.h:373-395) (proposed; owner may
change).

```pkn
async fn serve(ch: shared sync::Channel<Job>) -> err::Error {
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
* A running `@par` loop is not interrupted; `task::cancelled()` in its body
  reads the starting task's flag (proposed; owner may change).
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
loops until empty, reports undecided handles and joins the threads.  Tasks
spawned during exit start cancelled; a task that never reaches a
cancellation point delays exit.

## Communicating

### Channels

`sync::Channel<T>` is bounded (decision 30) and a class: `shared` between
tasks of one thread, `thread_shared` across threads.  In a thread_shared
object a channel field is `thread_shared sync::Channel<T>`, an ordinary
thread_shared reference whose `T` was send-checked when the channel
converted; an unqualified one fails the contents rule like any unique field.
The ring and the waiters are runtime state behind the channel's mutex,
outside the contents rule.  A channel's code is compiled once per `T`, so it
locks for every qualifier: a `shared` channel pays an uncontended lock and
unlock per operation (proposed; owner may change).

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
operations work in ordinary code and, through a thread_shared channel, in
`@par` bodies (proposed; owner may change).

### `Locked<T>` and `lock` blocks

`sync::Locked<T>` (decision 4) is a non-Copyable value held in place, reached
only by borrow; `T` must pass the send check.  Its one operation in v0.4 is
its constructor (proposed; owner may change).

```pkn
class Counts { hits: int[16]; }
class Stats { counts: sync::Locked<Counts>; served: sync::Atomic<int>; }
impl Stats {
  fn __init__() {
    self.counts = sync::Locked<Counts>(Counts([0; 16]));
    self.served = sync::Atomic<int>(0);
  }
  view fn record(bucket: int) {
    lock self.counts as c: inout {
      c.hits[bucket] += 1;
    }
    self.served.add(1);
  }
}
```

The operand (`lockStmt`) is a variable, field path or parameter, borrowed
`view` for the block; `c` is a local borrow (src/Sema/SemaBorrows.cpp:3-21)
ending at the closing brace.  Both modes take one non-recursive mutex
(proposed; owner may change), released on every exit; the unlock is a count
barrier ([Counting](#counting)).

The rules are strict (decision 29): `await`, a nested `lock`, `join` and
`@par` are errors in a lock block, also through calls, which decision 29
left to be specified (proposed; owner may change):

* A resolvable call to a function with the Waits summary is an error.  Waits
  means it contains a `lock`, a `@par`, a `join` or a blocking native
  (`io::File` reads and writes, stdin reads), or calls a function that does.
  `await` needs no analysis: an ordinary callee cannot await.
* An unresolvable call (a function value, or a virtual call not resolved
  through an exact or closed class) is an error, as in `@par`: `'f' may run
  code that takes a lock; a 'lock' block calls only functions it can
  resolve`.
* Native code calling back into Paykan code (`toString` from printing,
  `equals`) meets a per-thread held-lock depth, a run-time backstop: a nested
  acquire or a `join` panics, and `parallel_for` runs inline.

No thread waits, while holding a `Locked` mutex, for anything that may wait
for one, so `Locked`, `join` and `@par` cannot deadlock.  Other waits still
can: two tasks awaiting each other's handles or channels, or a lock block
spinning on an `Atomic` that another thread sets only after taking the same
lock.  A task awaiting its own handle panics: `a task cannot wait for
itself` (proposed; owner may change).

### `Atomic`

`sync::Atomic<int>`, `Atomic<unsigned>` and `Atomic<bool>`, sequentially
consistent only (the owner's default): a non-Copyable 8-byte word in place.
Its methods are `view fn`s, so frozen objects' atomic fields stay usable
(decision 19): `Atomic<int>(v)`, `load()`, `store(v)`, `exchange(v) -> T`,
`compareExchange(expected, desired) -> T` (the value found), and for the
integers `add(d) -> T` and `sub(d) -> T` (the previous value, wrapping like
`+`) (proposed; owner may change).

## Data parallelism: `@par for`

### Syntax and meaning

```pkn
fn saxpy(a: float, xs: view float[], ys: inout float[]) {
  @par for i in 0..xs.len() {
    ys[i] = a * xs[i] + ys[i];
  }
}

fn dot(xs: view float[], ys: view float[]) -> float {
  s = 0.0;
  @par for i in 0..xs.len() {
    s += xs[i] * ys[i];
  }
  return s;
}
```

`@par` is an attribute on a `for` (decision 12).  The loop finishes before
the next statement, so it may borrow arrays (decision 6), and its iterations
run on a worker pool.  `..=` and `step` are allowed, since the trip count is
fixed at entry (src/Lowering/Lowering.cpp:1192-1294) (proposed; owner may
change).  A new SemaPar pass verifies that the iterations are independent;
anything it cannot verify is an error.

### What a body may use

A name is per-iteration (the loop variable included) or outer.  An outer name
may be (the owner's default, extended):

* a value of plain parts (numbers, `bool`, `char`, enums, and optionals,
  tuples, structs and fixed arrays of them), read only, unless a reduction;
* a `view` or `inout` array or slice of plain elements;
* a `Locked` or `Atomic` local, and arrays or slices of `Atomic` used at any
  index (proposed; owner may change);
* a thread_shared object: frozen fields read, sync fields used, and a
  `thread_shared sync::Channel<T>` for the try operations;
* a reduction variable.

Unique and shared references (channels included), weak variables and
function values are errors, since reading one takes a plain count from
several threads:

```pkn
@par for i in 0..n { total += nodes.at(i).w; }
// error: 'nodes' (a 'List<Node>') cannot be used in a '@par' loop body: an iteration uses only values, arrays of values, sync variables and 'thread_shared' objects
```

### Writes and reductions

* An outer array is written, or passed `inout`, only through a path where
  each enclosing `@par` variable is a whole subscript, so `y[i][j]` and
  `y[j][i]` both pass (proposed; owner may change).
* An array the body writes is read only at the written paths, and a whole
  outer array is never passed `inout`.
* An outer scalar changes only by a recognised reduction, mentioned nowhere
  else in the body.

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
`@par` adds no machine dependence beyond libm's, which sequential code has
too (proposed; owner may change).  Integer reductions are exact: `+` and `*`
wrap (docs/language/02-basics.md:691).

### Calls, control and nesting

* `break`, `return` and `await` are errors; `continue` ends the iteration.
* Calls go only to par-safe functions: no printing or I/O, no native outside
  an allow-list (`::math`, the Str, Buffer and sync runtime), no spawn or
  join, no function value, and no virtual call unresolved through an exact
  or closed class (src/Lowering/LoweringInternal.h:578-599).
* Locks, atomics and channel try operations are allowed, in unspecified
  order; reductions stay reproducible.
* Nested `@par` cannot deadlock: with caller-runs scheduling, a thread waits
  only for chunks another thread has claimed.
* Unique elements at the loop's index (`ys[i] = move Node(i)`) are excluded
  in v0.4 (proposed; owner may change).

### Lowering of `@par`

No new PIR op and no MLIR change.  The body becomes
`<f>.par<N>(ctx: ptr, c: i64, lo: i64, hi: i64)`, outlined like a closure
(src/Lowering/LoweringClosure.cpp:188) with borrowed captures in a `mem`
context and a 256-slot partials array per reduction.  The parent calls
`Paykan_parallel_for(chunk, ctx, n, flags)`, a lent use of the whole context
whose entry and exit are count barriers, and folds.  Since borrowed paths are
checked only syntactically, the entry compares the byte ranges of written
borrowed parameters and written local borrows (`ys: inout = h.arr`) with the
others; on overlap the chunks run in order on the caller (proposed; owner
may change).

## Attributes

`@par` needs #171's attributes, extended to statements (`attribute+ forStmt`
above); this design takes #171's syntax:

```
attribute ::= "@" IDENT ( "(" ( attrParam ( "," attrParam )* ","? )? ")" )?
attrParam ::= IDENT "=" constant
```

The lexer gets an `@` token (today `invalid character '@'`).  ForStmt
(include/AST.h:662-691) and declarations keep an ordered attribute list, and
the interchange writes `(attr par)` (src/ASTInterchange/Writer.cpp:437-450).
Unknown names, wrong positions, duplicates and parameters are errors.
`@par` changes only its function's `CODE` and never reaches the interface.
The summaries the compiler infers are flag bits, and FUNC's `Attrs`
(docs/design/pkm.md:445) holds only exported declaration attributes, of
which v0.4 has none (proposed; owner may change).

## Runtime

The runtime stays strict C11 (src/Runtime/CMakeLists.txt:20-26), and
`Runtime.h`, which the emitted C includes, never reaches `<stdatomic.h>` or
`<pthread.h>`.

**Thread safety (no language change).**

* The deferred-destroy state (src/Runtime/Shared.c:100-161) becomes one
  `_Thread_local` struct; a NULL `pending` means the inline array, since a
  thread-local's address is not an address constant (C11 6.6p9).
* Immortals: `PAYKAN_REF_IMMORTAL = INT32_MAX` and `PAYKAN_REF_MAX` one below
  (src/Runtime/Runtime.h:178), for None and Stdin (src/Runtime/Object.c:70,
  src/Runtime/File.c:248).  The fast paths stay one compare, retain when
  `(uint32_t)s < PAYKAN_REF_MAX` and release when `(uint32_t)s - 2u <
  PAYKAN_REF_IMMORTAL - 2u` (src/Runtime/Runtime.h:479-517,
  src/Backends/MLIR/ToLLVM/ToLLVM.cpp:693-700), and the slow paths skip
  immortals, whose counts are never written (proposed; owner may change).
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

**Atomic counts.**  `Paykan_retain_atomic`, `Paykan_release_atomic` (a
release subtract; at 1 an acquire fence, then the destroy) and weak variants;
the weak load is a compare-and-swap increment while `strong > 0`, closing the
race of src/Runtime/Shared.c:178-186.

* `Paykan_make_thread_shared` runs on a unique object and panics unless
  strong is 1 and weak 0 (`an object made 'thread_shared' is still
  borrowed`, a backstop to the move-out check).  It sets weak to 1, a unit
  the strong references share; the destroy drops it, and whoever takes
  `weak` to 0 frees, exactly once (replacing src/Runtime/Shared.c:135-136,
  188-191).
* Every header write on a thread_shared object is an atomic store, relaxed
  for the `kPending`, `kDying` and 0 markers (src/Runtime/Shared.c:123,
  132-134).  The pending stack tags thread_shared entries in the low pointer
  bit, so `destroy_dead_object` picks the weak-unit free rule.
* `Paykan_make_thread_shared` and the atomic release test for
  `PAYKAN_REF_IMMORTAL` before writing.  The atomic retain checks
  `fetch_add`'s previous value against `PAYKAN_REF_MAX` and panics, as
  `count_overflow` does (src/Runtime/Shared.c:34).
* Helpers that count through a `PaykanType` choose plain or atomic from the
  reference word's atomic bit: `PaykanType_copy` and `PaykanType_drop`
  (src/Runtime/Type.c:25, 30), the buffer drops built on them, and the tuple
  and struct `toString` holds (src/Runtime/Tuple.c:163-165).  Tuple `equals`
  (Tuple.c:277-280) passes its argument borrowed, as `equals(other: view
  Obj)` does everywhere.

PIR's count operations gain an `atomic` flag set from the type
(src/Lowering/LoweringInternal.h:711-727), and rc-elimination pairs only
like kinds.  `PaykanType` reference words become
`offset << 2 | atomic << 1 | nullable` (src/Runtime/Runtime.h:576-599), and
the ABI version goes from 300 to 400 (src/Runtime/Runtime.h:46).  Every
publication (`spawnOn`, a channel send, a store into a thread_shared object
or a `Locked`) synchronises by release and acquire, so an object's plain
accesses happen before its atomic ones.

**Worker pool.**  The first `@par` loop with more than one chunk starts it:
`PAYKAN_THREADS - 1` workers (default: the online CPUs), with main's stack
size clamped to 8-64 MiB, since macOS gives threads 512 KiB (proposed; owner
may change).  The caller claims chunks through an atomic counter, and an
acquire-release counter orders writes before the fold.

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

**Task objects.**  A poll returns 0 (pending) or 1 (ready) through vtable
slot 3, and slot 4 takes the result (src/Runtime/Runtime.h:152-169); a woken
task is re-polled from its root (proposed; owner may change).  A task record
(about 104 B) holds the atomic cancel flag and the parent link under a
striped lock.  A frame is 32 bytes plus its result, parameters and live
locals (typically 64-160 B), a handle 32 B, a channel about 150 B plus its
ring.  A spawn makes 3 allocations, an async call 1.  A `Locked<T>` is one
word pointing at a runtime mutex and value.

**Shutdown.**  `Paykan_runtime_shutdown()` runs after `main` and before the
heap dump at the three entry points
(src/Backends/LLVM/CodeGen/NativeBuild.cpp:61-110, 185-205;
src/Backends/MLIR/ToEmitC/ToEmitC.cpp:1070-1104) and joins the pool, which
restarts lazily for in-process JIT runs.

**TSan.**  A `tsan-linux` job like the ASan one
(.github/workflows/ci.yml:158-215), with `PAYKAN_TSAN`
(cmake/SanitizerSetup.cmake:63-68), runs the suite, C stress tests
(concurrent chain drops, as #26 asks; count contention; weak loads against
releases; stdin from two loops) and generated code through the C path.  It
does not block merges until green (proposed; owner may change).

## Compiler architecture

| Piece | Where | Anchors |
|---|---|---|
| Grammar, frontend | the productions of Syntax, first in the grammar | docs/grammar.md:185-232, 358-400, 658-680 |
| Qualified types | a canonical `QualifiedType`; unique is the bare type, and about 90 `ClassType` casts (`isa`, `dyn_cast`, `cast`, by grep over src/ and include/) and 57 `isRefType` calls see through it | include/ASTContext.h:179-196; include/AST.h:2443-2467; src/Sema/Sema.cpp:233-256 |
| Copies, conversions | checkAssignable, arguments, returns, literals, captures | src/Sema/Sema.cpp:343-366; src/Sema/SemaLambda.cpp:78 |
| Moves, handles | new SemaMoves.cpp | src/Sema/SemaClass.cpp:261-410 |
| Exclusivity | `move` as a write; operand conflicts on owned paths | src/Sema/SemaParamModes.cpp:25-70, 350; SemaBorrows.cpp:302 |
| Holds | borrowed roots keep BorrowedReads and SnapshotReads; `join` Unknown | src/Lowering/LoweringHolds.cpp:62-152, 1750-1752 |
| thread_shared, send check | frozen fields; contents and send checks in new SemaSend.cpp; class flags | src/Sema/SemaViews.cpp:43-168 |
| Generics | Copyable, conditional impls, archetypes | src/Sema/SemaTraits.cpp:68-100, 306-320; include/Sema.h:2004-2013 |
| Summaries | par-safe, Waits, Joins, MovesOut: a call-graph fixpoint in Sema | src/Lowering/LoweringInternal.h:537-620 |
| `@par` verifier | new SemaPar.cpp | src/Sema/Sema.cpp:4423 |
| `move`, count flags | a load and a null store, as moves-at-last-use emits; the move-out walk | src/Lowering/Lowering.cpp:1934-1973; LoweringInternal.h:711-727 |
| Async lowering | frame, create, take, resume, outlined like closures; an await is an unknown-effect call for holds | src/Lowering/LoweringClosure.cpp:88-316; LoweringHolds.cpp:256-336; LoweringInternal.h:569 |
| Split | AsyncSplit at the end of `ModuleLowering::run`, before the code passes | src/Lowering/Lowering.cpp:535-543; src/Driver/main.cpp:84-92 |
| PIR | count flags; no counts on `view` parameters or `self`; `await` in text only | include/paykan/pir/PIR.h:316-356, 483-520; src/PIR/Verifier.cpp |
| PIR for plugins | the `atomic` flag in the text; the text version follows the release, so backend plugins see 400 and must honour it | include/paykan/pir/Version.h:16; docs/plugins/pir-for-backends.md:15, 82-87 |
| Backends | LLVM inlines atomic counts; C calls the runtime | ToLLVM.cpp:617-707; ToEmitC.cpp:312-323; PIRToStandard.cpp:1055-1061 |
| AST interchange | qualifiers, `move`, `await`, `async`, receiver prefixes, `lock` and attributes in the reader, writer and format document; the differential frontend tests | src/ASTInterchange/Reader.cpp, Writer.cpp:437-450; docs/plugins/ast-format.md; tests/Frontend/DifferentialTests.cpp |
| JIT | new runtime symbols | src/Backends/LLVM/JIT/JIT.cpp:38, 56-140 |
| `.pkm` | qualifiers in types; flag bits (par-safe, Waits, Joins, MovesOut, async, frame-pinned); class flags; impl bounds | include/paykan/pkm/Interface.h:80, 94-99; docs/design/pkm.md:441-449 |

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
   keywords (`shared` in samples/codegen/32_c_identifiers.pkn:47-56).
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
included, is unchanged.  The repository migrates the same way, bottom-up,
with the diff reviewed: 449 `.pkn` files (154 `self.f = x;` sites in 97
files), 329 docs blocks and about 1,690 programs embedded in the C++ tests.
The tool runs on the stdlib first; the container redesign (classes,
`Copyable` impl blocks) is a reviewed hand edit on top.  Samples keep their
output and end with no live blocks under `--track-heap`.  In a probe, 11 of
12 codegen samples alias somewhere, so programs need `shared` as well as
`move`.

**Two settled decisions are reversed:**

* **`move` returns.**  v0.2.0 removed `mov` and inferred every transfer
  (#145; docs/language/16-memory.md:371-414, 472-480).  Now a transfer of a
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
function values are no longer copied, and callbacks become `view`
parameters; lambdas capture `self` only in a `shared fn`
(docs/design/function-types.md:89-94); `Str(s)` always copies
(docs/language/03-functions.md:283-284); and the thread-safety section
(docs/language/16-memory.md:716-722) is rewritten.

## Staging

**v0.4** (decisions 14 and 27): the runtime, pool, TSan, attributes and
`@par`; async/await with timers, files, channels and joins; `Locked` and
`Atomic`; the qualifiers (`thread_shared` replacing decision 14's shared
objects), the send check and `paykan migrate`.

A chunk is about 6-7 hours of multi-agent work plus a CI round; S is a
quarter chunk, M a half, L one, XL two to three.

| Area | Pieces | Chunks |
|---|---|---|
| Ownership | syntax M; qualified types L; copies and `move` L; use after move M; owned paths and the move-out check M; exclusivity M; conversion M; thread_shared M; weak S; generics L; lowering L; count savings M; PIR M; backends M; interchange and frontends M; `.pkm` M; stdlib L; docs L; tests L | 12-14 |
| Migration | the tool XL; the repository XL | 4-6 |
| Runtime and `@par` | thread safety M; atomic counts L; pool M; stress tests M; TSan S; TSan for LLVM and JIT M; attributes M; interface bits S; summaries L; verifier L; lowering L; docs M | 7-9 |
| Async | syntax M; typing L; spawn M; handles M; lowering L; holds M; split L; runtime L; timers M; files M; `::task` S; tests and docs L | 8-9 |
| Sync | `Locked` L; `Atomic` M; `Channel` L | 2-3 |
| **Total** | | **33-41, about 36** |

That is about twice decision 14's 13-17, which predates decisions 17-26.
Ownership adds about 18 (the qualifiers 12-14, migration 4-6), and the work
of decision 14 itself comes to 18-21 against its estimate of 13-17.

Order (proposed; owner may change): (1) runtime thread safety, and the
qualifier front end with the legacy mode as the default, so existing tests
keep passing; (2) thread_shared, the send check, generics; (3) migration,
flipping the default in one chunk; (4) attributes and `@par`; (5) async,
then the sync types.

Dependencies: **#171** comes before `@par`.  **#353** (generic methods)
changes how `impl` methods are instantiated, as conditional `impl` blocks do
(include/Sema.h:1736-2013): land it first or together.  **#354**
(higher-order functions on the collections) is written against the container
classes and `Copyable` blocks, after the stdlib rewrite, or it is migrated
twice.

**v0.5:** work stealing with the hold-across-await check (decision 15);
ordinary functions as tasks on a compute pool (decisions 11, 14); sockets,
DNS, processes and pipes with #156 (decision 27); relaxations such as unique
elements at a `@par` index.  **Later:** the dataflow layer (decision 2),
tensors and packed element storage (decision 13), accelerators with their
own reduction answer (decision 28), Windows.

## Decisions log

| # | Decision | Section |
|---|---|---|
| 1 | One model | Goals |
| 2 | Layered | Goals; Staging |
| 3 | Confinement, plain counts, value structs | Goals; The three qualifiers; Counting |
| 4 | Channels, locks, atomics | Communicating (as revised by 9 and 30) |
| 5 | Free-standing tasks | Handles |
| 6 | Owned inputs | Starting tasks; Data parallelism |
| 7 | Decided handles | Handles |
| 8 | Failures as values | Failures and panics |
| 9 | Cancellation | Cancellation; `Channel<T>` |
| 10 | async/await | `async fn` and `await`; Compiler architecture |
| 11 | Two ways to wait | Ordinary and async code |
| 12 | `@par for` | Data parallelism |
| 13 | No tensors yet | Staging |
| 14 | v0.4 scope | Staging |
| 15 | Pinned scheduling | Scheduling |
| 16 | `Shared<T>` | superseded by 17 |
| 17 | Qualifiers | The three qualifiers |
| 18 | `move` | Copies and `move`; Use after move; Migration |
| 19 | Frozen fields | What each qualifier may hold |
| 20 | Send check | The send check |
| 21 | Types covered | `Str` and the containers; Lambdas; Migration |
| 22 | Parameters | Parameters, borrows and exclusivity |
| 23 | Conversion | Conversion by declared type |
| 24 | Generics | Generics and `Copyable` |
| 25 | Weak references | Weak references |
| 26 | Migration | Migration |
| 27 | Async I/O | Runtime; Staging |
| 28 | Determinism | Reproducible chunking |
| 29 | Lock rules | `Locked<T>` and `lock` blocks |
| 30 | Bounded channels | `Channel<T>` |
| 31 | Exit | Program exit |
| 32 | Lambda captures | Lambdas |

## Open questions

Each with the answer taken here.

**The owner's defaults, kept.**

1. **What may a `@par` body use?**  Values, borrowed arrays and slices,
   thread_shared objects; also `Locked` and `Atomic` locals, which count
   nothing.
2. **What may a thread_shared object hold?**  thread_shared references,
   values of them, `Locked`, `Atomic`; so no thread_shared `List` in v0.4.
3. **Is `self` borrowed?**  Yes; through thread_shared only `view fn`s,
   changing only sync fields.  `shared fn` (23) departs from this default.
4. **Which atomics?**  `<int>`, `<unsigned>`, `<bool>`, sequentially
   consistent.
5. **Where do tasks run?**  `spawn` here; `spawnOn` with crossable inputs.
6. **Do last-use moves stay?**  As an optimisation.
7. **Do unique references skip counting?**  On owned paths, measured first.

**Ownership.**

8. **May a declaration name only the qualifier?**  Yes, `x: shared = e;`,
   like a borrow declaration's mode.
9. **Is `mov` still reserved?**  One release, its message naming `move`.
10. **Is `move` before a temporary accepted?**  Yes, as decision 23 writes
    it; the alternative is an error.
11. **May `move` end a Copyable value?**  Yes, so generic code can write
    `move v`.
12. **Does an assignment revive a moved variable?**  Yes, a non-`let` one.
13. **Is there a partial move?**  Only a payload field of an owned variant
    binding, to empty a `Result`.
14. **Which qualifiers do borrows take?**  `view` any, `inout` the exact one,
    whose release the callee may store.
15. **May a plain binding copy a unique place?**  No; a temporary subject is
    owned by its binding.
16. **What qualifier does a downcast binding have?**  Its subject's; writing
    it is optional, so `m: Str` in stdlib/error.pkn still reads.
17. **How are send paths named?**  Dotted fields, elements by type.
18. **How are subclasses in other modules checked?**  By flags fixed in the
    class's module; the alternative is a whole-program check at link.
19. **Is the move-out check worth its loads?**  Yes: without it a held
    unique object crosses threads.  A walk at each crossing point instead
    has no single point for stores into a `Locked`.
20. **How do `shared T` and `thread_shared T` substitute?**  They collapse on
    the same qualifier and are errors otherwise.
21. **How are missing `Copyable` bounds found?**  The archetype check; the
    fallback is CI instantiating every stdlib template with unique and
    shared classes.
22. **Where do copying methods live?**  In conditional `impl` blocks, as
    methods have no bounds.
23. **How does a method keep a counted `self`?**  `shared fn`, `shared view
    fn`, `thread_shared view fn`; the alternative is a free function taking
    `shared Node`.
24. **How does `equals` take its operand?**  `view Obj`; a native's plain
    reference parameter owns its argument.
25. **Are the containers classes?**  Yes, with `Buffer`; Stack and Queue on
    a Buffer; LinkedList uncrossable.
26. **What does `Str(s)` of a `Str` do?**  Copies; the alternative keeps the
    identity with the argument's qualifier.
27. **What is `err::Error.msg`?**  `thread_shared Str?`, so errors cross.
28. **How does a native return an alias?**  A `shared` or `thread_shared`
    result, or a runtime object that locks itself, as Stdin.
29. **Are function values unique?**  Yes, as types cannot show captures;
    callbacks `view`; thread_shared only from a lambda in place.
30. **Can a lambda pass its `move` capture on?**  No, each call borrows it.
    The alternative, a once-callable lambda with a run-time check, could.
31. **Can a lambda capture `self`?**  Only in a `shared fn`, reversing
    docs/design/function-types.md:89-94.
32. **Is `f(move a)` to a `view` parameter allowed?**  Yes; the value drops
    after the call.

**Tasks.**

33. **Is `spawn` a function or a keyword?**  A compiler-known function taking
    an async call in place.
34. **What may a spawned function return?**  `Result` or `Error`, so a
    cancelled wait is a value.
35. **What does `task::newLoop` return?**  A `res::Result`, since making a
    thread can fail; the thread runs until exit.
36. **What may a handle be?**  Unique, by value, with `t.isDone()`.
37. **Does `cancel()` spend a handle?**  No; it decides it, and dropping it
    then detaches.
38. **May a handle live in a field or container?**  Yes, checked at drop and
    reported at exit.  This softens decision 7's compile error; forbidding
    `task::Task` outside locals, parameters and results keeps it at compile
    time but rules out a list of workers.
39. **Is a joining call a scheduling point?**  Yes, since `join` runs the
    loop.
40. **How is `join` through an indirect call caught?**  By a run-time panic.
41. **Should a task detached in an ordinary `main` warn?**  Yes: it runs only
    while `main` joins.
42. **Which borrows may cross a wait?**  `inout` of owned storage; a `view`
    argument elsewhere is snapshotted when Copyable.  The alternative is an
    error.
43. **Are detached results dropped silently?**  Yes.
44. **Where does `Cancelled` go?**  After `OutOfRange`.
45. **What do the new modules hold?**  `::task` handles and loops, `::time`
    `sleep(ms: int)`, `::sync` channels, locks, atomics, `::fs` files; a
    `time::Duration` can come later.
46. **Can cleanup wait after a cancel?**  Not in v0.4: the flag is sticky,
    with no shield; `cancelAfter` gives `TimedOut`, with no timeout
    combinator.
47. **Does a cancelled wait yield?**  Once, so its loop is not starved.
48. **What does a cancelled joiner do?**  Cancels the task it awaits.
49. **Where do orphaned children go?**  To the grandparent.
50. **Is a running `@par` interrupted?**  No.
51. **Should blocking I/O in async code warn?**  Yes: it is no cancellation
    point.
52. **Where does exit run?**  `Paykan_tasks_finish` in every `@main`; the
    alternative is the entry points only.
53. **Are busy file helpers awaited at exit?**  No; operations own their
    descriptors.
54. **Is waiting for one's own handle caught?**  By a run-time panic.
55. **What may an async function be?**  Not a value, lambda or `__init__`,
    no borrowed result; a `native async fn` returns an operation object.
56. **How are frames polled?**  From the root, with striped task locks.
57. **Where does the split run?**  Before the code passes; frames hold what
    is live at an await.

**Communicating and `@par`.**

58. **Is `Channel` a class?**  Yes, its `T` checked at conversion, always
    locked inside.
59. **Which channel errors?**  `BrokenPipe`, `EndOfFile`; a failed send
    drops its value; `tryRecv`, `trySend(slot)`.
60. **What is `Locked`?**  A value in place with only a constructor; one
    mutex for both modes.
61. **What may a lock block call?**  Resolvable functions without Waits;
    native callbacks meet a run-time backstop.
62. **Is deadlock impossible?**  Through `Locked`, `join` and `@par`, not
    through handles, channels or spins; this scopes decision 29's wording,
    not its rules.
63. **Which `Atomic` methods?**  `load`, `store`, `exchange`,
    `compareExchange`, `add`, `sub`.
64. **How are chunks cut?**  `K(n) = min(n, 256)`, free to change before
    1.0.
65. **What if written borrows overlap?**  Chunks run in order; the
    alternative is a panic.
66. **Which reductions?**  Five forms and the four comparisons; bitwise ones
    could join.
67. **Which nested writes pass?**  Every enclosing `@par` variable a whole
    subscript.
68. **May a body write unique elements at its index?**  Not in v0.4.
69. **Are `..=` and `step` allowed?**  Yes.
70. **Is packed element storage in v0.4?**  No: decision 13 says "possibly";
    it comes with tensors.

**Runtime.**

71. **How are counts atomic in C?**  Runtime functions behind a target
    assumption; the alternative, `_Atomic int32_t` in the header, is to
    measure.  The immortal sentinel is `INT32_MAX`.
72. **How many threads, how much stack?**  `PAYKAN_THREADS`; 8-64 MiB.
73. **What does the libm wording promise?**  No machine dependence beyond
    libm's.
74. **How do async files work?**  Four helpers; io_uring later.
75. **Are summaries `Attrs`?**  No, flag bits.
76. **Does TSan block merges?**  Not until green.

**Migration and staging.**

77. **Does `paykan migrate` write?**  Yes, as decision 26 says; `--check` and
    `--diff` are proposed.
78. **Is the legacy mode the default until the flip?**  Yes.
79. **In what order?**  Runtime and front end, thread_shared, migration,
    `@par`, async, sync types.
80. **Is v0.4 about 36 chunks?**  Yes.  TSan for LLVM and the JIT and the
    count savings (about 1 chunk) can move to v0.5 without reversing a
    decision; moving async files would reverse decision 27.

## Diagnostics

Errors unless marked.  `12:9` stands for a source position.

| Situation | Message |
|---|---|
| copy of a unique place | `cannot copy 'a' (a unique 'Node'): write 'move a' to transfer it, or declare it 'shared Node'` |
| copy of a non-Copyable value | `cannot copy 'k': 'sync::Atomic<int>' is not Copyable` |
| copy of a holder | `cannot copy 'p': 'Person' holds unique field 'name' ('Str')` |
| copy of a borrow | `cannot copy 'n': it is a 'view' parameter, a borrow` (`an 'inout' parameter`, `a 'view' local`) |
| `[v; n]` | `'[v; n]' copies its value: 'Node' is not Copyable` |
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
| `mov` | `'mov' is spelled 'move' since v0.4` |
| qualified value | `'shared' qualifies a reference; 'int' is a value` |
| two qualifiers | `a type takes one qualifier: 'shared thread_shared Node'` |
| conversions | `no conversion from 'shared Node' to 'Node'`; `no conversion from 'shared Node' to 'thread_shared Node': a shared object stays on its thread`; `no conversion from 'thread_shared Config' to 'Config'` |
| `inout` qualifier | `'s' is a 'shared Node'; 'inout' parameter 'n' takes exactly a 'Node'` |
| read, later `inout` | `'s' is read by the left operand of '+'; a later operand cannot pass it to 'inout' parameter 'x' of 'shout'` |
| read, later `move` | `'j' is read by the left operand of '+'; a later operand cannot move it` |
| binding a unique place | `cannot bind 'n' by copy: 'x' is a unique 'Node'; bind 'n: view Node' or 'n: inout Node', or match 'move x'` |
| binding qualifier | `'p' binds part of a 'shared Obj': its type is 'shared Node', not 'thread_shared Node'` |
| contents rule | `'Settings' cannot be 'thread_shared': field 'path' is a unique 'Str'; a thread_shared object holds thread_shared references, values of them, and Locked or Atomic fields` (paths dotted: `field 'meta.name'`) |
| `thread_shared Obj` | `'Obj' cannot be 'thread_shared': it may be any class` |
| frozen field | `cannot assign 'cfg.path': 'cfg' is 'thread_shared', so its plain fields are frozen; a field that changes is a Locked or an Atomic` |
| frozen field to `inout` | `cannot pass 'cfg.path' to 'inout' parameter 'p': 'cfg' is 'thread_shared', so its plain fields are frozen` |
| method through thread_shared | `'rename' is not a 'view fn'; through 'thread_shared' reference 'cfg' only 'view fn's can be called` |
| send check | `cannot send 'Job': field 'cache' is shared` (`field 'a.b.cache'`; `field 'cache' of 'Job'`) |
| sending a function value | `cannot send '() -> int': a function value's captures are not part of its type; declare it 'thread_shared' where the lambda is written` |
| sending `Obj` | `cannot send 'Obj': it may be any class` |
| subclass flags | `class 'Button' cannot hold 'shared' field 'onClick': it extends 'Widget', which can be sent to another thread`; `class 'SubConfig' cannot hold unique field 'name': it extends 'Config', which can be 'thread_shared'` |
| thread_shared function value | `a 'thread_shared' function value is made by a lambda written where its type is declared` |
| thread_shared lambda capture | `a 'thread_shared' lambda cannot capture 'c': it is a 'shared Cache'` |
| weak to unique | `a weak reference refers to a 'shared' or 'thread_shared' object; 'a' is a unique 'Node' (unique objects form trees, with no cycle to break)` |
| copy of `T` | `'T' is not 'Copyable': declare 'T: Copyable' to copy it` |
| conditional method | `'get' needs 'T: Copyable', and 'Node' in 'List<Node>' is not Copyable` |
| `impl Copyable` | `'Copyable' is built in: a type is Copyable by what it holds, and cannot be implemented` |
| qualified `T` | `'shared T' with 'T' = 'int': 'shared' qualifies a reference; 'int' is a value`; `'thread_shared T' with 'T' = 'shared Node': a type takes one qualifier` |
| storing `self` | `cannot copy 'self': a method borrows its receiver; declare the method 'shared fn' to keep a reference to it` |
| capturing `self` | `cannot capture 'self': a method borrows its receiver; declare the method 'shared fn' to keep a reference to it` |
| `shared fn` call | `'add' is a 'shared fn': call it through a 'shared' reference ('root' is a unique 'Node')` |
| override prefix | `'add' overrides a 'shared fn'; it must be 'shared fn'` |
| undecided handle | `task 't' is never joined, cancelled or detached` |
| spent handle | `task 't' was joined at 12:5; it cannot be used again` |
| qualified or borrowed handle | `'task::Task' is unique: a task handle has one owner`; `a task handle is passed by value: 't' cannot be a 'view' or 'inout' parameter` |
| unawaited call | `'fetch' is an async function: 'await' its call or pass it to 'task::spawn'` |
| `await` operand | `'await' takes a call of an async function or a 'task::Task'` |
| `await` in ordinary code | `'await' is only allowed in an 'async fn'; ordinary code waits with 't.join()'` |
| `await` misplaced | `'await' cannot appear in a lambda`; `'await' cannot appear in a '@par' loop body`; `'await' cannot appear in a 'lock' block: a lock is never held across a suspension` |
| async as a value | `async function 'f' is not a value: call it with 'await' or pass a call to 'task::spawn'` |
| override colour | `'run' overrides an async method; it must be 'async fn'`; `'run' overrides an ordinary method; it cannot be 'async fn'` |
| async `__init__` | `'__init__' cannot be async` |
| async borrowed result | `an 'async fn' returns a value: its result cannot be 'view' or 'inout'` |
| `join` in async code | `'join' blocks its thread; inside an 'async fn' write 'await t'` |
| `join` through calls | `'helper' may wait in 'join()' (through 'g'); async code cannot block its thread: await the task instead` |
| spawn argument | `'task::spawn' takes a call of an async function, written in place` |
| spawned result | `a spawned function returns 'res::Result<T, err::Error>' or 'err::Error'; 'f' returns 'int'` |
| spawning a borrower | `cannot spawn 'f': parameter 'xs' is 'view'; a task owns its inputs (a '@par' loop may borrow)` |
| spawned receiver | `cannot spawn 'w.run': the receiver of a spawned method is 'shared' or 'thread_shared'` |
| `spawnOn` receiver | `'task::spawnOn' runs 'w.run' on another thread: its receiver must be 'thread_shared', and 'w' is a 'shared Worker'` |
| `inout` across a wait | `'self.n' is reached through 'self'; it cannot be borrowed 'inout' across an 'await': another task may change it there` (`through 'shared' reference 's'`, `through 'view' parameter 'h'`; `across a call that may 'join'`) |
| `view` across a wait | `'s.meta' is reached through 'shared' reference 's' and is not Copyable; it cannot be lent to an awaited call: another task may change it there` |
| warning: detached in ordinary `main` | `'t' is detached in an ordinary 'main': it runs only while 'main' waits in 'join()', and is cancelled when 'main' returns` |
| warning: blocking I/O in async code | `'readln' blocks its thread; in an 'async fn' it is not a cancellation point and can delay exit` |
| capacity 0 | `a channel holds at least one value: its capacity cannot be 0` |
| `lock` operand | `'lock' takes a variable, a field or a parameter` |
| nested `lock` | `a 'lock' block cannot contain another 'lock' block: put state used together in one 'Locked<(A, B)>'` |
| `join`, `@par` in a lock block | `'join' cannot be called in a 'lock' block`; `a '@par' loop cannot run inside a 'lock' block` |
| waiting call in a lock block | `'update' may wait (it takes a lock, through 'g'); a 'lock' block cannot call it` (also `it blocks on input`) |
| unresolvable call in a lock block | `'f' may run code that takes a lock; a 'lock' block calls only functions it can resolve` |
| attributes | `unknown attribute '@parr'; did you mean '@par'?`; `'@par' applies only to 'for' statements`; `'@par' is written twice`; `'@par' takes no parameters` |
| outer reference in `@par` | `'nodes' (a 'List<Node>') cannot be used in a '@par' loop body: an iteration uses only values, arrays of values, sync variables and 'thread_shared' objects` |
| write elsewhere | `'ys[i + 1]' is written at an index other than the loop's 'i': each iteration writes only its own elements` |
| read of a written array | `'ys' is written at 'ys[i]' and cannot be read at 'ys[i - 1]' in the same '@par' loop` |
| whole array `inout` | `'ys' cannot be passed whole to 'inout' parameter 'p' in a '@par' loop body` |
| outer scalar | `'count' is assigned in a '@par' loop body; an outer variable changes only by a reduction ('count += e')` |
| reduction read | `'s' is a reduction of this '@par' loop and cannot be read in its body` |
| `break`, `return` | `'break' cannot leave a '@par' loop: every iteration runs` (also `'return'`) |
| unsafe call | `'log' cannot be called in a '@par' loop body: it prints (through 'g')` |
| unresolved virtual call | `'shape.area' may run an override from another module; a '@par' loop body calls only methods it can resolve` |

Run-time panics, printed after `paykan: `: `a value moved out of 'q.jobs'
is still borrowed`, `an object made 'thread_shared' is still borrowed`,
`task handle dropped without join, cancel or detach`, `'join' called inside
a task: use 'await'`, `a task cannot wait for itself`, `channel capacity 0`,
`nested lock`, `'join' while holding a lock`.  At exit, on stderr: `paykan:
2 task handles were never joined, cancelled or detached`.

# Concurrency and ownership

A program can run work on more than one thread. Calling an `async fn` starts a **task**,
which runs on a pool of worker threads while the caller goes on, and `parallel for` runs the
iterations of a loop on that pool. What keeps this safe is that every reference says how its
object is held and whether it is written: by one owner, immutable unless `mut` (the bare
type, the default), by aliases on one thread (`shared`, `shared mut`), or by handles that any
thread may use (`atomic`, `locked mut` and `atomic shared`). A reference reaches another
thread only by `move` or as a handle, and the compiler checks every crossing, so two threads
never count or change one object at once. This chapter shows the qualifiers, `move`, tasks,
`parallel for` and the cells that threads share, then gives the precise rules. The model is
the one in [the design document](../design/concurrency.md); this version implements its
qualifiers, `move`, the send check, `atomic`, `locked` and `parallel for`, and runs tasks on
MLIR's async runtime and parallel loops on LLVM's OpenMP runtime ([Runtime](#runtime)).

An example that makes an `atomic` or `locked` cell, or freezes an object, shows what it
prints under **Prints** rather than **Output**: in this version such a cell or object is never
freed ([Limits for now](#limits-for-now)), so `--track-heap` reports it and the test suite does
not run those examples. Their output was checked by hand.

## Who holds an object: the qualifiers

Every reference to an object carries a **qualifier** that says how the object is held, and
each word of it buys one thing at a cost. The bare type, written with no qualifier, is the
cheapest: `T` is a **unique** reference, its object has one owner, the variable, field or
element that holds it, and nothing writes the object through it. `mut T` adds the writes:
through the owner the fields are assigned and any method is called. `shared T` is a
reference that may be copied freely, so several variables alias one object on one thread,
and none of them writes it; `shared mut T` is the aliased form with the writes. The
qualifier is part of the type of a variable, a parameter, a field or a result:

```pkn
import ::io;

class Node {
  id: int;
  name: Str;
}

fn describe(n: view Node) -> Str {
  return n.name + " #" + Str(n.id);
}

fn main() -> int {
  let a = Node(1, "a");                  // the bare type: unique, and nothing writes it
  let c: shared Node = move a;           // shared: aliases on this thread, none writing
  let d = c;                             // d is shared too
  let m: mut Node = Node(2, "m");        // mut: written through its owner
  m.name = "s";
  let s: shared mut Node = move m;       // shared mut: aliases that write
  let t = s;                             // t is shared mut too
  t.id = 22;
  io::println(describe(c) + ", " + describe(d) + ", " + describe(s));
  return 0;
}
```

Output:

```
a #1, a #1, s #22
```

A unique reference converts into every other form, since nothing else holds its object:
`move a`, or a new object (`Node(2, "m")`, unique until it is stored), into a `shared`,
`shared mut`, `atomic shared` or `locked` slot. `mut T` flows into a `T` freely: giving up
the writes costs nothing. A `T` gains `mut` only by `move`, or straight from a temporary
such as a constructor call, so that no reference writes an object that another one holds as
immutable. A `shared` reference stays what it is, since other aliases hold the object under
the form it was given: `shared T` and `shared mut T` never convert into each other, and
nothing converts back to unique:

```pkn
class Node {
  id: int;
}

fn main() -> int {
  let s: shared Node = Node(1);
  let u: Node = s;
  let a = Node(2);
  let m: mut Node = a;
  let w: shared mut Node = Node(3);
  let r: shared Node = w;
  let k: mut int = 1;
  return 0;
}
```

Error:

```
cannot store a 'shared Node' in variable 'u', a 'Node': a 'shared' reference, a frozen object and a cell convert only as they are
cannot store a 'Node' in variable 'm', a 'mut Node': a reference gains 'mut' only by 'move'
cannot store a 'shared mut Node' in variable 'r', a 'shared Node': a 'shared' reference, a frozen object and a cell convert only as they are
'mut' applies to objects; 'int' is a value: its storage is rewritten by assignment, which 'let' fixes
```

A qualifier can stand alone where the type is inferred: `let s: shared = Node(2, "s");`,
`n: mut = Node(3, "n");`. `let` and the qualifiers are different things: `let` fixes the
variable, a qualifier describes the object. A `let s: shared mut Node` can still have
`s.id = 5`, and an `n: mut Node` that is not `let` can be assigned another object, which
leaves both objects as they were; a `let n = Node(1)` is neither reassigned nor written
through. A variable declared by its first assignment or with a type is mutable where it says
so: `n: mut = Node(1)` and `n: mut Node = Node(1)`.

### Borrows take no qualifier

A `view` or `inout` parameter borrows the caller's object for the call
([Parameter modes](03-functions.md#parameter-modes-view-and-inout)), and it is written
without a qualifier. A `view T` takes every form of the object, since it only reads it; an
`inout T` takes one that is written through its owner, a `mut` or a `shared mut` one, and
writes it; a plain `T` takes ownership, so its argument is a new object or a `move`. Neither
borrow retains or releases the object. `mut` never goes on a borrow: an `inout` parameter, an
`inout` local and an `inout` match binding write what they name by their mode, and a `view`
reads.

```pkn
import ::io;

class Node {
  id: int;
  name: Str;
}

fn show(n: view Node) -> Str { return n.name + " #" + Str(n.id); }   // every form
fn mark(n: inout Node) { n.name = n.name + "!"; }                   // a mut or shared mut one
fn keep(n: Node) -> int { return n.id; }                            // takes ownership

fn main() -> int {
  let c = Node(1, "c");
  s: shared mut Node = Node(2, "s");
  mark(s);
  io::println(show(c) + ", " + show(s) + ", " + Str(keep(Node(3, "t"))));
  u: mut = Node(4, "u");
  mark(u);
  io::println(show(u) + " " + Str(keep(move u)));
  return 0;
}
```

Output:

```
c #1, s! #2, 3
u! #4 4
```

`view mut Node`, `inout shared Node` and the like are errors: `parameter 'n' is 'inout
shared': a borrow carries no qualifier; a 'view' takes every form of the object (read-only
through it) and an 'inout' a 'mut' or 'shared mut' one, with no retain or release; write 'inout
T'`. An immutable reference is not passed to an `inout` parameter: `mark(c)` with `c` a
`Node` is `'c' is immutable (a 'Node'; write 'mut Node' to change it); it cannot be passed to
'inout' parameter 'n'` ([below](#the-bare-type-is-immutable-mut-for-the-writes)), and a
`let` variable, `mut` or not, is still `'c' is declared with 'let' and cannot be passed to
'inout' parameter 'n'`.

## The bare type is immutable: `mut` for the writes

A reference written with no qualifier names an object that nothing writes through it. None
of its fields can be assigned, only its `view fn`s can be called on it, it cannot be passed
to an `inout` parameter, and what it holds through a unique field reads the same way: a
unique field follows its root, so nothing reached through an immutable reference changes
either. `mut T` buys the writes, and costs the word: through a `mut` reference the fields are
assigned, any method is called, and the object's unique fields are written too. `self` is
mutable in every method but a `view fn` (`__init__` builds it), so a method that writes
needs no spelling of its own; what needs `mut` is the reference the method is called on.

```pkn
import ::io;

class Limits {
  lo: int;
  hi: int;
}

class Config {
  name: Str;
  limits: Limits;
  view fn span() -> int { return self.limits.hi - self.limits.lo; }
  fn rename(n: Str) { self.name = n; }
}

fn main() -> int {
  let base = Config("demo", Limits(0, 100));      // the bare type: nothing writes it
  let l = base.limits;                            // a unique field follows its root: immutable too
  let cfg: mut Config = Config("x", Limits(0, 1));
  cfg.rename(base.name);                          // written through a mut reference
  cfg.limits.hi = l.hi;
  io::println(cfg.name + " " + Str(cfg.span()) + " " + Str(l.hi));
  return 0;
}
```

Output:

```
demo 100 100
```

```pkn
class Limits {
  lo: int;
  hi: int;
}

class Config {
  name: Str;
  limits: Limits;
  fn rename(n: Str) { self.name = n; }
}

fn grow(l: inout Limits) { l.hi += 1; }

fn main() -> int {
  let cfg = Config("demo", Limits(0, 100));
  cfg.name = "x";
  cfg.rename("y");
  grow(cfg.limits);
  let l = cfg.limits;
  l.hi = 5;
  let s = "a";
  s.concat("b");
  return 0;
}
```

Error:

```
'cfg' is immutable (a 'Config'; write 'mut Config' to change it); cannot assign to its field 'name'
'cfg' is immutable (a 'Config'; write 'mut Config' to change it); 'rename' is not a 'view fn'
'cfg' is immutable (a 'Config'; write 'mut Config' to change it); it cannot be passed to 'inout' parameter 'l'
'l' is immutable (a 'Limits'; write 'mut Limits' to change it); cannot assign to its field 'hi'
's' is immutable (a 'Str'; write 'mut Str' to change it); 'concat' is not a 'view fn'
```

`Str` is an object like any other: `concat` writes the string it is called on, so it needs a
`mut Str`, while `s + "b"` makes a new string and reads `s` only. A `shared mut` field is
the object's own form and keeps its writes through any root, and a `shared`
field is never written through its object: `field 'cache' is immutable (a 'shared Cache';
write 'shared mut Cache' to change it); cannot assign to its field 'hits'`. A unique field is
never written `mut`, since it follows its root. A `match` binding takes no qualifier and is
a copy of the subject in the subject's form: it writes under a `mut` subject and reads under
a bare one, and an `inout` binding on a `mut` place ([Match statements](08-match.md))
writes what it names with no copy.

`mut` is the only spelling of the writes. `const` is gone: a reference is immutable unless
it says `mut`, and `const T`, `shared const T` and `atomic shared const T` are syntax
errors that say so:

```pkn
class Node {
  id: int;
}

fn main() -> int {
  let n: const Node = Node(1);
  return 0;
}
```

Error:

```
'const' is gone: a reference is immutable unless written 'mut T' ('shared mut T', 'locked mut T')
```

## Values take no qualifier: `mem::Box`

A struct, a tuple or an array is a value: it lives in its variable and is copied by
assignment ([Structs](11-structs.md), [Tuples](10-tuples.md), [Arrays](09-arrays.md)), so
there is no object for a qualifier to describe, and `shared Point` is an error. `mut` never
goes on a value either, a number, a `bool` or a `char` included: `let` alone decides whether
a value's storage is rewritten. What a value holds keeps its own qualifiers: a struct with a
`shared Node` field is copied, and the copies alias the node; an object it holds through a
unique field or element is reached through the value, whose `let` fixes its own storage (no
field or element of it is assigned), not the object's fields.

```pkn
struct Point {
  x: int;
  y: int;
}

fn main() -> int {
  let p: shared Point = Point(1, 2);
  let q: mut (int, int) = (1, 2);
  let r: shared int[2] = [1, 2];
  let k: mut int = 1;
  return 0;
}
```

Error:

```
'shared' qualifies a reference; 'Point' is a value
'mut' applies to objects; '(int, int)' is a value: its storage is rewritten by assignment, which 'let' fixes
'shared' qualifies a reference; 'int[2]' is a value
'mut' applies to objects; 'int' is a value: its storage is rewritten by assignment, which 'let' fixes
```

To share or freeze the value itself, put it in a `mem::Box<T>`, an object holding one value
([`Box`](../stdlib/memory.md)): the box takes the qualifier. `b.get()` is the value's
storage, read or assigned through it; it is not a `view fn`, so it needs a box that is
written, a `mut` or `shared mut` one. `b.value()` is a `view fn` that only reads the value,
and is the way into an immutable box:

```pkn
import ::io;
import ::memory as mem;

struct Point {
  x: int;
  y: int;
}

fn main() -> int {
  let p = Point(1, 2);
  let q = p;                                       // a struct is a value: a copy
  let b: shared mut mem::Box<Point> = mem::Box(p); // the box is an object: shared, and written
  let alias = b;
  alias.get().x = 10;                              // get() is the value's storage
  let ro = mem::Box(q);                            // a bare box: read through value()
  io::println(Str(b.get().x) + " " + Str(q.x) + " " + Str(ro.value().y));
  return 0;
}
```

Output:

```
10 1 2
```

```pkn
import ::memory as mem;

struct Point {
  x: int;
  y: int;
}

fn main() -> int {
  let ro = mem::Box(Point(1, 2));
  ro.get().x = 3;
  return 0;
}
```

Error:

```
'ro' is immutable (a 'Box<Point>'; write 'mut Box<Point>' to change it); 'get' is not a 'view fn'
```

## `move`: an explicit transfer

`move x` is `x`'s value, which `x` no longer holds: the reference goes to the destination,
and `x` cannot be read again. The compiler still infers a transfer at a variable's last use
([Ownership transfers](16-memory.md#ownership-transfers)); `move` is for where the program
must say it: sending a reference to a task, converting a unique object into another form,
giving a reference `mut` (`let m: mut Job = move a`), or ending a variable early.

```pkn
import ::io;

class Job {
  name: Str;
}

class Holder {
  job: Job;
  tag: Str;
}

fn keep(j: Job) -> int { return j.name.len(); }

fn main() -> int {
  let a = Job("first");
  let b = move a;                     // a ends here; b owns the Job
  io::println(b.name);

  j = Job("x");                       // revived by assignment after each move
  total = 0;
  for i in 0..3 {
    total += keep(move j);
    j = Job("xy");
  }
  io::println(Str(total) + " " + j.name);

  let h = Holder(Job("inner"), "t");
  let inner = move h.job;             // a partial move: the field leaves, h ends
  io::println(inner.name + " " + Str(keep(move Job("tmp"))));
  return 0;
}
```

Output:

```
first
5 xy
inner 3
```

The compiler follows every path. Reading a moved variable is an error that names the move;
after an `if` or a `match` that moved it on one path only, the variable *may* have been moved,
which is an error too; and a loop that moves a variable of the enclosing function must assign
it again before the next iteration, as the loop above does:

```pkn
import ::io;

class Job {
  name: Str;
}

fn keep(j: Job) -> int { return j.name.len(); }

fn main() -> int {
  let a = Job("a");
  let n = keep(move a);
  io::println(a.name);
  let b = Job("b");
  if n > 3 { keep(move b); }
  io::println(b.name);
  let c = Job("c");
  for i in 0..2 { keep(move c); }
  return 0;
}
```

Error:

```
'a' was moved at 11:16; it cannot be used after the move
'b' may have been moved (by the 'if' at 14:3, at 14:19); it cannot be used after a move
'c' is moved in the loop at 17:3 and not assigned again before the next iteration
```

An assignment revives a moved variable. `move h.job` moves a unique reference field out of
a variable the function owns and ends the whole variable; a `shared` or `weak` field, a
value field and a field of a borrow cannot be moved out (`cannot move 'h.other': it is
'shared Job', not a unique part of 'h'`). `move` before a temporary (`move Job("tmp")`)
does nothing: a temporary is owned already. A move is a write, so a variable that a `view`
local reads cannot be moved until the local's last use (`'n' is viewed by 'view' local 'r'
until 'r' is last used; it cannot be moved`), and one call cannot both borrow a variable and
move it (`'f' is passed to 'view' parameter 'j'; it cannot also be moved into parameter
'k'`).

## Tasks: `async fn` and `await`

A function declared `async fn` runs as a task. Calling it starts the task at once and
returns a handle to it; `await` on the handle, or on the call itself, waits for the result.
The task runs on a worker thread while the caller goes on, so several can be started before
any is awaited:

```pkn
import ::io;

class Job {
  name: Str;
  n: int;
}

async fn fib(n: int) -> int {
  if n < 2 { return n; }
  let a = fib(n - 1);        // a task, started at once
  let b = fib(n - 2);
  return (await a) + (await b);
}

async fn run(j: Job) -> int {
  return j.n * 2;
}

fn main() -> int {
  let t = fib(20);
  io::println("fib(20) = " + Str(await t));
  let job = Job("work", 21);
  let u = run(move job);     // the Job crosses to the task; job ends here
  io::println("done: " + Str(await u));
  return 0;
}
```

Output:

```
fib(20) = 6765
done: 42
```

`main` itself may be a task, `async fn main() -> int`:

```pkn
import ::io;

async fn count(upto: int) -> int {
  s = 0;
  for i in 0..upto { s += 1; }
  return s;
}

async fn main() -> int {
  total = 0;
  for i in 0..100 {
    let c = count(10);
    total += await c;
  }
  io::println("total = " + Str(total));
  return 0;
}
```

Output:

```
total = 1000
```

In this version `await` is allowed in an ordinary function too, where it blocks the thread
until the task is done (the design gives ordinary code a `join` instead). A task's result is
a value: a number, a `Str`, or a unique object, which crosses back to the awaiter:

```pkn
import ::io;

class Node {
  id: int;
}

async fn greet(name: Str) -> Str { return name + "!"; }
async fn make(id: int) -> Node { return Node(id); }

fn main() -> int {
  let g = greet("hi");
  let m = make(5);
  let n = await m;                    // a unique Node crosses back
  io::println(await g + " " + Str(n.id));
  return 0;
}
```

Output:

```
hi! 5
```

A task handle is spent by its `await`: the variable cannot be used again, as after a `move`.
And every task the program starts is awaited, or moved into a variable that is, on every
path before its scope ends, so no task is dropped while it still runs (the design's task
handle has `join`, `cancel` and `detach` as well; this version has `await` only):

```pkn
async fn fib(n: int) -> int {
  if n < 2 { return n; }
  let a = fib(n - 1);
  let b = fib(n - 2);
  return (await a) + (await b);
}

fn twice() -> int {
  let t = fib(5);
  let x = await t;
  return x + await t;
}

fn maybe(c: bool) -> int {
  let t = fib(5);
  if c { let x = await t; }
  return 0;
}

fn main() -> int {
  let t = fib(5);
  return 0;
}
```

Error:

```
't' was awaited at 10:11; a task handle is spent by its 'await'
task 't' may not be awaited before this 'return': it is spent on some paths only (by the 'if' at 16:3, at 16:18); every path awaits it, or moves it
task 't' is not awaited before this 'return': a task handle is spent by 'await' (or 'move') on every path before its scope ends; the prototype has no 'join', 'cancel' or 'detach'
```

## What crosses to a task: the send check

A task runs on another thread, so what it takes must be safe to use there. The **send
check** runs at an async call's arguments and at the declaration of an `async fn`'s result.
A number, a `bool`, a `char`, a `Str`, a handle ([below](#locked-mut-cells-and-lock-blocks))
and a unique object (a `T` or a `mut T`) whose fields all cross (own and inherited, through
structs, tuples, arrays, optionals and enum payloads) cross; a unique object crosses by
`move` or as a temporary, so the caller's variable ends. A `shared` or `weak` reference
does not cross, since its object is aliased on its own thread, and neither does a function
value or an `Obj`. The error names the part that cannot cross:

```pkn
class Cache {
  hits: int;
}

class Job {
  name: Str;
  cache: shared Cache;
}

class Node {
  id: int;
}

async fn consume(n: Node) -> int { return n.id; }
async fn run(j: Job) -> int { return j.name.len(); }

fn main() -> int {
  let a = Node(1);
  let t1 = consume(a);
  let s: shared Node = Node(3);
  let t2 = consume(move s);
  let j = Job("j", Cache(0));
  let t3 = run(move j);
  return 0;
}
```

Error:

```
cannot copy 'a' (a unique 'Node') into a task: write 'move a' to transfer it
's' is 'shared Node': it never crosses to a task (a 'shared' reference is aliased on its own thread)
cannot send 'j' (a 'Job'): field 'cache' is 'shared Cache': a 'shared' reference is aliased on its own thread
```

A deeper part is named by its path (`field 'job.cache' is 'shared Cache'`). An `async fn`'s
parameters are numbers, references and handles in this version: a struct, a tuple, an array
or a function value as a parameter is an error (`parameter 'p' of 'async fn' 'plot' has type
'Point', a value held in memory: the prototype passes numbers, references and handles to a
task (hold the value in a class)`), and a `shared` result is one too (`the result of 'async
fn' 'give' (a 'shared Node') cannot cross to the task's awaiter`).

## `parallel for`

`parallel for` runs the iterations of a `for` loop on the worker pool, and the loop ends
before the next statement runs. The compiler checks that the iterations are independent: a
variable of the enclosing function changes only by a **reduction**, `s += e`, `s -= e` or
`s *= e`, and an `inout` array is written at the loop's index only, so no two iterations
touch one element:

```pkn
import ::io;

fn dot(xs: view float[], ys: view float[]) -> float {
  s = 0.0;
  parallel for i in 0..xs.len() {
    s += xs[i] * ys[i];                // a reduction
  }
  return s;
}

fn saxpy(a: float, xs: view float[], ys: inout float[]) {
  parallel for i in 0..xs.len() {
    ys[i] = a * xs[i] + ys[i];         // written at the loop's index only
  }
}

fn main() -> int {
  xs = [0.0; 10000];
  ys = [0.0; 10000];
  for i in 0..10000 {
    xs[i] = float(i);
    ys[i] = 1.0;
  }
  io::println("dot = " + Str(int(dot(xs, ys))));
  saxpy(2.0, xs, ys);
  io::println("ys[7] = " + Str(ys[7]));
  return 0;
}
```

Output:

```
dot = 49995000
ys[7] = 15
```

A body may read values of the enclosing function (numbers, `bool`, `char`, enums, and
arrays of them, `view` or `inout`), its cells and frozen objects (`atomic`, `locked mut` and
`atomic shared` variables), and the reduction variables; a reference, a `Str` or a function
value of the enclosing function cannot be used, since reading one would count its object
from several threads. A reduction is folded in a fixed order, so the result is the same on every run and
with any number of threads (a float sum may differ from the sequential loop's, never between
runs). The body may call functions and run loops of its own, and `..=` and `step` are
allowed; `break`, `continue`, `return`, `await` and starting a task are not:

```pkn
class Node {
  id: int;
}

fn main() -> int {
  let nodes = [Node(4), Node(5)];
  total = 0;
  parallel for i in 0..2 {
    total += nodes[i].id;
  }
  let xs = [0.0; 4];
  let ys = [0.0; 4];
  parallel for i in 0..4 {
    ys[3 - i] = xs[i];
  }
  count = 0;
  parallel for i in 0..4 {
    count = count + 1;
  }
  parallel for i in 0..4 {
    if i == 2 { break; }
  }
  return 0;
}
```

Error:

```
'nodes' (a 'Node[2]') cannot be used in a 'parallel for' body: an iteration uses only values, arrays of values and 'atomic' variables of the enclosing function
'ys' is written in a 'parallel for' body at an index other than the loop's variable 'i': two iterations could write one element
'break' cannot be used in a 'parallel for' body (the prototype)
```

(`count = count + 1` is `count += 1`, a reduction; the parser rewrites it.) A result that
is not a reduction can be an `atomic` cell ([below](#atomic-cells)), changed from any
thread by its atomic operations:

```pkn
import ::io;

fn main() -> int {
  let hits: atomic int = 0;
  parallel for i in 0..10000 {
    if i % 10 == 0 { hits += 1; }     // an atomic add, from any thread
  }
  io::println("hits = " + Str(hits));
  return 0;
}
```

Prints:

```
hits = 1000
```

An atomic add costs far more than a reduction, because every add moves the cell's cache line
between cores, so a cell changed on *every* iteration is reported:

```text
warning: 'hits' changes on every iteration of the 'parallel for': each atomic operation moves its cache line between cores, a thousand times a reduction's cost on 16 threads (benchmarks/README.md); count in a reduction (a plain variable of the enclosing function, '+='), or change it less often
```

Starting the threads has a cost of its own, so a light body (no call and no loop of its own)
over fewer than 4096 iterations runs in order on the calling thread; a body with a call or a
loop may be heavy however short the range, and always runs on the pool. The costs are
measured in [benchmarks/README.md](../../benchmarks/README.md).

A body also uses a `mem::Buffer<T>` of numbers that the enclosing function owns, as it uses
an `inout` array: it reads it at any index (`at`, `get`, `len`) and writes it at the loop's
variable only, with `set(i, v)`, where `i` is the variable or a conversion of it, so no two
iterations touch one element; `grow`, `reserve` and the other methods that reshape the buffer
are errors in the body, since they could move its storage under the other iterations:

```pkn
import ::io;
import ::memory as mem;

fn main() -> int {
  let n = 100000;
  xs = mem::Buffer<float>();
  ys = mem::Buffer<float>();
  let g = xs.grow(unsigned(n));
  let h = ys.grow(unsigned(n));
  for i in 0..n { let e = xs.set(unsigned(i), float(i)); }
  parallel for i in 0..n {
    let u = unsigned(i);
    let e = ys.set(unsigned(i), 2.0 * xs.at(u) + ys.at(u));   // written at the loop's variable
  }
  io::println("ys[99999] = " + Str(ys.at(unsigned(n - 1))));
  return 0;
}
```

Prints:

```
ys[99999] = 199998
```

```pkn
import ::memory as mem;

fn main() -> int {
  ys = mem::Buffer<float>();
  let g = ys.grow(16);
  parallel for i in 0..16 {
    let a = ys.set(unsigned(i + 1), 1.0);
    let b = ys.reserve(32);
  }
  return 0;
}
```

Error:

```
'ys' is written in a 'parallel for' body at an index other than the loop's variable 'i': two iterations could write one element
'ys' (a 'Buffer<float>' of the enclosing function) is read ('at', 'get', 'len') and written at the loop's variable ('set') in a 'parallel for' body; 'reserve' could move its storage under the other iterations
```

A buffer's `at`, `get` and `len` on a number type are read in place by the compiler, a bounds
check and a load, with no call into the memory module and no count on the buffer's object,
whether the buffer is the function's own or frozen in a box; `set` is still a call of the
memory module's method (it checks the index and returns an `err::Error`).

## `locked mut` cells and `lock` blocks

State that several threads change goes in a **`locked mut` cell**: an object held behind a
mutex. A unique object becomes a cell where it flows into a `locked mut` slot (`let counts:
locked mut Counts = Counts(...)`), after the send check of what it holds. The cell's
**handle** copies anywhere, into tasks included, and its contents are reached only in a
`lock` block, which takes the mutex, names the contents `inout` or `view`, and releases the
mutex at its end:

```pkn
import ::io;

class Counts {
  hits: int[4];
  total: int;
}

// The cell crosses as a handle; each task changes the counts under its lock.
async fn record(counts: locked mut Counts, id: int, n: int) -> int {
  for i in 0..n {
    lock counts as c: inout {
      c.hits[(id + i) % 4] += 1;
      c.total += 1;
    }
  }
  return id;
}

fn main() -> int {
  let counts: locked mut Counts = Counts([0; 4], 0);   // a unique value becomes a cell
  let a = record(counts, 1, 1000);                      // a handle copied into each task
  let b = record(counts, 2, 1000);
  let ids = await a + await b;
  sum = 0;
  lock counts as v: view {
    for i in 0..4 { sum += v.hits[i]; }
    io::println("total " + Str(v.total) + ", sum " + Str(sum) + ", ids " + Str(ids));
  }
  return 0;
}
```

Prints:

```
total 2000, sum 2000, ids 3
```

Here too `mut` buys the writes. `locked T` without it is a cell whose contents nothing
writes: the compiler accepts it with a warning, since a lock on an object nothing writes
buys nothing that `atomic shared T` ([below](#atomic-shared-frozen-objects)) does not give
for free, and its contents are reached in `view` blocks only.

```pkn
import ::io;

class Counts {
  total: int;
}

fn main() -> int {
  let counts: locked Counts = Counts(5);
  lock counts as v: view { io::println(Str(v.total)); }
  return 0;
}
```

```text
$ paykan --check-only readers.pkn
readers.pkn:8:3: warning: 'locked Counts' locks an object nothing writes; 'atomic shared Counts' is read from every thread with no lock
```

A `lock` block holds the mutex for its whole length, so nothing in it may wait for anything
else: no `await`, no nested `lock`, no `parallel for`, and no `return`, `break` or `continue`
leaves it. The cell's contents cannot be reached outside a block, a `view` binding is
read-only, and an `inout` block needs a `locked mut` cell:

```pkn
class Counts {
  total: int;
}

async fn ping(n: int) -> int { return n; }

fn main() -> int {
  let counts: locked mut Counts = Counts(0);
  let t = counts.total;
  lock counts as c: view {
    c.total = 5;
    let p = ping(1);
    let r = await p;
    lock counts as d: inout { }
  }
  let k: locked int = 1;
  let ro: locked Counts = Counts(1);
  lock ro as e: inout { e.total = 2; }
  return 0;
}
```

Error:

```
'counts' is 'locked': reach its contents in 'lock counts as c: view { }' (or 'inout')
'c' is a 'view' local; cannot assign to its field 'total'
'await' cannot be used in a 'lock' block: a wait holding a lock
a 'lock' block cannot hold another 'lock'
'locked' holds an object in the prototype, not a 'int' (wrap the value in a class)
'ro' is 'locked Counts', whose contents are never written; declare it 'locked mut Counts' for a 'lock ... as e: inout' block
```

(`ro`'s declaration gets the warning as well.) A `lock` block takes a `locked` variable,
field or parameter. A handle may also be used in a `parallel for` body, where each iteration
takes the lock for its own block.

## `atomic` cells

`atomic T` is a cell holding one number, `bool` or `char` that any thread may change with
one atomic operation at a time. `T` is any number type (`int`, `float`, `u8`, `f32`, ...),
`bool` or `char`, never a class: an object crosses threads frozen, as `atomic shared T`
([below](#atomic-shared-frozen-objects)), or `locked mut`. A read of the place is an
atomic load and `=` an atomic store; `+=` and `-=` are one atomic add or subtract (an
integer wraps like `+`, a float adds by a compare-and-swap loop); `x.exchange(v)` stores
`v` and returns the value found; `x.compareExchange(expected, desired)` stores `desired`
only if the cell holds `expected`, and returns whether it did. A variable, a parameter, a
result or a class field can be `atomic`. A copy of an atomic place (`let m = n`, an
argument, a result, a field assigned in `__init__`) is another **handle** to the same cell;
a plain value flowing into an `atomic` slot makes a new cell:

```pkn
import ::io;

class Stats {
  name: Str;
  served: atomic int;
  done: atomic bool;
}

fn bump(c: atomic int, by: int) { c += by; }   // a handle parameter: the caller's cell

fn fresh(v: int) -> atomic int { return v; }   // a value: a new cell

fn main() -> int {
  let a: atomic i8 = 120;
  a += 10;                                     // wraps like i8 arithmetic
  let f: atomic f32 = 1.5;
  f += 0.25;
  let c: atomic char = 'a';
  let old = c.exchange('b');
  io::println("i8 " + Str(a) + ", f32 " + Str(f) + ", char " + Str(c) + " (was " + Str(old) + ")");

  let n: atomic int = 5;
  let swapped = n.compareExchange(5, 9);       // True: 5 -> 9
  let missed = n.compareExchange(5, 1);        // False: it holds 9
  let m = n;                                   // another handle to n's cell
  m += 1;
  bump(n, 10);
  let k = fresh(3);                            // its own cell
  k += 1;
  io::println("n " + Str(n) + " " + Str(swapped) + " " + Str(missed) + ", m " + Str(m) + ", k " + Str(k));

  let st = Stats("s", 0, False);               // fields: the object holds cells
  st.served += 2;
  let h = st.served;                           // a handle to the field's cell
  h += 1;
  st.done = True;                              // = is an atomic store
  io::println(st.name + " served " + Str(st.served) + ", done " + Str(st.done));
  return 0;
}
```

Prints:

```
i8 -126, f32 1.75, char b (was a)
n 20 True False, m 20, k 4
s served 3, done True
```

`let` fixes the handle, not the cell, so `+=` and `=` work on a `let` atomic. An operation
that would read the cell and write it separately is an error: a read of the place on the
right of an assignment to it, and every compound operator but `+=` and `-=`. A cell takes no
`mut`, since its operations are its writes (`atomic mut int` is a syntax error,
[below](#atomic-shared-frozen-objects)). `atomic` of an object is an error that names the
two forms an object crosses in, the prototype has no atomic arrays, and a struct cannot hold
a cell:

```pkn
struct P {
  c: atomic int;
}

fn main() -> int {
  let hits: atomic int = 0;
  hits = hits * 2 + 1;
  hits *= 2;
  let s: atomic Str = "a";
  let k: atomic shared int = 1;
  return 0;
}
```

Error:

```
field 'c' in struct 'P' has type 'atomic int': a struct cannot hold an 'atomic' field in the prototype (hold the cell in a class)
'hits' is 'atomic': 'hits = ...' reads and writes it separately; write 'hits += e', or use 'exchange' or 'compareExchange'
'hits' is 'atomic': this operator reads and writes it separately; its atomic operations are '+=', '-=', 'exchange' and 'compareExchange'
'atomic' holds a number, a 'bool' or a 'char', not 'Str': an object crosses threads as 'atomic shared Str' (frozen) or 'locked mut Str'
'atomic shared' qualifies a reference; 'int' is a value
```

## `atomic shared`: frozen objects

Data that every thread reads and none changes is shared as `atomic shared T`: the
object is **frozen** with everything it holds, and its reference is a handle that copies
anywhere. A unique object, a `T` or a `mut T`, flowing into an `atomic shared` slot is
send-checked and then frozen by a walk over the objects it holds, through its fields, arrays
and buffers; reading a frozen object counts nothing, so the threads share no written memory.
Access is what a bare reference gives: no field is assigned and only `view fn`s are called,
except that an `atomic` field of a frozen object is a cell, and still changes.

```pkn
import ::io;

class Limits {
  lo: int;
  hi: int;
}

class Config {
  name: Str;
  limits: Limits;
  weights: float[4];
}

// A frozen configuration, read by every task without a count.
async fn score(cfg: atomic shared Config, x: int) -> int {
  if x < cfg.limits.lo || x > cfg.limits.hi { return 0; }
  return x * cfg.name.len() + int(cfg.weights[x % 4]);
}

fn main() -> int {
  let cfg: atomic shared Config = Config("demo", Limits(0, 100), [1.0, 2.0, 3.0, 4.0]);
  let a = score(cfg, 7);          // the handle is copied into each task
  let b = score(cfg, 8);
  let c = score(cfg, 500);
  io::println(Str(await a) + " " + Str(await b) + " " + Str(await c));
  io::println(cfg.name + " [" + Str(cfg.limits.lo) + ".." + Str(cfg.limits.hi) + "]");
  return 0;
}
```

Prints:

```
32 33 0
demo [0..100]
```

A frozen object is read by a `parallel for` body too, and that is the way to data larger
than a fixed-size array (at most 16384 elements): a `mem::Buffer<T>` is a struct, a value,
so it goes in a `mem::Box`, and freezing the box freezes the buffer and its elements. The
body reads it through the box's `value()`:

```pkn
import ::io;
import ::memory as mem;

fn main() -> int {
  b = mem::Buffer<float>();
  b.grow(100000);
  for i in 0..100000 { b.set(unsigned(i), float(i % 7)); }
  let xs: atomic shared mem::Box<mem::Buffer<float>> = mem::Box(move b);   // boxed, then frozen
  s = 0.0;
  parallel for i in 0..100000 {
    s += xs.value().at(unsigned(i));     // every thread reads the frozen buffer
  }
  io::println("sum = " + Str(int(s)) + " of " + Str(xs.value().len()));
  return 0;
}
```

Prints:

```
sum = 299995 of 100000
```

Nothing is written through an `atomic shared` reference, nothing converts it back, and an
object with a part that cannot cross cannot be frozen:

```pkn
class Cache {
  hits: int;
}

class Limits {
  lo: int;
  hi: int;
}

class Config {
  name: Str;
  limits: Limits;
  fn rename(n: Str) { self.name = n; }
}

class Holder {
  cache: shared Cache;
}

fn main() -> int {
  let cfg: atomic shared Config = Config("demo", Limits(0, 100));
  cfg.limits.hi = 5;
  cfg.rename("x");
  let u: Config = cfg;
  let h: atomic shared Holder = Holder(Cache(0));
  return 0;
}
```

Error:

```
'cfg' is frozen (an 'atomic shared Config'); cannot assign to 'cfg.limits.hi'
'cfg' is frozen (an 'atomic shared Config'); 'rename' is not a 'view fn'
cannot store an 'atomic shared Config' in variable 'u', a 'Config': a 'shared' reference, a frozen object and a cell convert only as they are
cannot make 'Holder(...)' (a 'Holder') 'atomic shared': field 'cache' is 'shared Cache': a 'shared' reference is aliased on its own thread
```

Each of the two words says one thing: `atomic`, the reference crosses threads; `shared`, the
object has several owners, one on each thread at least. Nothing writes a frozen object, so
`mut` has no place on it: `atomic shared mut T` would be written from several threads with
no lock, and the parser rejects it naming the two forms an object crosses in. The parser
also holds the other spellings to their one form: the writes come after the form (`shared
mut`, `locked mut`, never `mut shared`), a cell takes no `mut`, and a type takes one
qualifier.

```pkn
class Node {
  id: int;
}

fn main() -> int {
  let a: atomic shared mut Node = Node(1);
  let b: mut shared Node = Node(2);
  let c: atomic mut int = 3;
  let d: shared mut shared Node = Node(4);
  return 0;
}
```

Error:

```
'atomic shared mut T' would be written from several threads with no lock; write 'atomic shared T' to freeze it, or 'locked mut T' to change it under a lock
'mut shared' is spelled 'shared mut': the form first, then the writes
an 'atomic' cell takes no 'mut': it changes by its operations; an object crosses threads as 'atomic shared T' (frozen) or 'locked mut T'
a type takes one qualifier: 'shared mut shared' is two
```

Those are syntax errors, so nothing past the parser checks such a program. `atomic` alone
holds a number, a `bool` or a `char` ([above](#atomic-cells)): `atomic Node` is an error
that names the two forms an object crosses in. And a `shared` reference, aliased on one
thread, does not convert into `atomic shared`, although neither is written
([Conversions](#conversions)): a frozen object is made from a unique reference, whose object
nothing else holds.

```pkn
class Node {
  id: int;
}

fn main() -> int {
  let n: atomic Node = Node(1);
  let s: shared Node = Node(2);
  let f: atomic shared Node = s;
  return 0;
}
```

Error:

```
'atomic' holds a number, a 'bool' or a 'char', not 'Node': an object crosses threads as 'atomic shared Node' (frozen) or 'locked mut Node'
cannot store a 'shared Node' in variable 'f', an 'atomic shared Node': a 'shared' reference, a frozen object and a cell convert only as they are
```

## `Copyable`

`Copyable` is a built-in trait of the types a program may copy freely: numbers, `bool`,
`char`, `atomic` handles, and enums, tuples, structs, fixed arrays and optionals whose parts
are Copyable (a struct's `shared` field counts). A unique reference, a `Str`, a function
value and a task are move-only. It is structural, so `impl Copyable for X` is an error, and
it bounds a type parameter that copies its value:

```pkn
import ::io;

struct Point {
  x: int;
  y: int;
}

class Node {
  id: int;
}

struct Ref {
  n: shared mut Node;
}

fn twice<T: Copyable>(v: T) -> (T, T) { return (v, v); }

fn main() -> int {
  let p = twice(Point(1, 2));
  let n = twice(3);
  let s: shared mut Node = Node(7);
  let pair = twice(Ref(s));           // a struct of a shared reference is Copyable
  s.id = 8;                           // the copies alias the node
  io::println(Str(p.0.x + p.1.y) + " " + Str(n.0 + n.1) + " " + Str(pair.1.n.id));
  return 0;
}
```

Output:

```
3 6 8
```

```pkn
class Node {
  id: int;
}

impl Copyable for Node {}

fn twice<T: Copyable>(v: T) -> (T, T) { return (v, v); }

fn main() -> int {
  let d = twice(Node(2));
  let e = twice("text");
  return 0;
}
```

Error:

```
'Copyable' is built in: a type is Copyable by what it holds, and cannot be implemented
'Node' does not implement trait 'Copyable', which 'twice' requires of 'T': a reference, a 'Str', a function value or a task is move-only
'Str' does not implement trait 'Copyable', which 'twice' requires of 'T': a reference, a 'Str', a function value or a task is move-only
```

A qualifier is not part of a type argument in this version (`twice(s)` with `s: shared Node`
is checked as a `Node`, and rejected), so a `shared` reference reaches a template inside a
struct, as `Ref` above.

## The copy rule: `--strict-ownership`

Without the option, a unique reference read from a variable still copies into another
variable, a parameter or a result, as it always has, and the compiler infers transfers at
a last use. `paykan --strict-ownership` turns on the design's copy rule: a move-only value
read from a place (a unique reference, or a struct, tuple or array holding one) flows into
an owning slot only by `move`, and `[v; n]` of one is an error, since it copies `v`. A
`shared` reference, a handle and a value made of those are Copyable and flow as before.

```pkn
import ::io;

class Node {
  id: int;
}

fn make() -> Node {
  let n = Node(1);
  return n;                     // strict: write 'return move n'
}

fn take(n: Node) -> int { return n.id; }

fn main() -> int {
  let a = Node(2);
  let b = a;                    // strict: a unique reference does not copy
  let s: shared Node = Node(3);
  let t = s;                    // fine: a shared reference is Copyable
  io::println(Str(take(a)) + " " + Str(take(move b)) + " " + Str(t.id + make().id));
  return 0;
}
```

```text
$ paykan --check-only --strict-ownership copies.pkn
copies.pkn:9:3: error: cannot copy 'n' (a unique 'Node'); write 'move n' to transfer it, or declare it 'shared Node'
copies.pkn:16:3: error: cannot copy 'a' (a unique 'Node'); write 'move a' to transfer it, or declare it 'shared Node'
copies.pkn:19:24: error: cannot copy 'a' (a unique 'Node'); write 'move a' to transfer it, or declare it 'shared Node'
```

Without the option the program compiles and prints `2 2 4`.

## Runtime

In this version tasks run on MLIR's async runtime, a thread pool that takes each task's body
as a call returns, and parallel loops on LLVM's OpenMP runtime (`libomp`), which runs a loop
as at most 256 chunks handed to its threads one at a time (`OMP_NUM_THREADS` sets how many).
Both ship with the LLVM the compiler is built against: `paykan run` loads them into the JIT
when a program uses them, and `paykan build` links them. Reference counts stay plain: the
checks above keep every plainly counted reference on one thread, and a handle crosses
without a count because its cell or frozen object is never freed. The design's
[Runtime](../design/concurrency.md#runtime) section describes the runtime of Paykan's own
that the finished feature needs; what `parallel for` costs, and why, is in
[benchmarks/README.md](../../benchmarks/README.md).

---

## Quick Summary

- A reference's **qualifier** says how its object is held, and each word costs something:
  none (`T`: unique, one owner, nothing writes it; the cheapest), `mut T` (one owner, who
  writes it), `shared T` (aliases on one thread, none writing), `shared mut T` (aliases that
  write), `atomic T` (a cell holding a number, `bool` or `char`), `locked mut T` (a cell
  holding an object written under its mutex; `locked T` alone is a warning), `atomic shared
  T` (a frozen object). A struct, a tuple or an array takes none, and so does a number;
  `mem::Box<T>` does.
- A unique reference converts into every form; `mut T` flows into `T` freely, and `T` gains
  `mut` only by `move` or as a temporary; a `shared` reference, a frozen object and a cell
  convert only as they are (`shared T` and `shared mut T` never into each other). Nothing
  converts back.
- Through an immutable reference (`T`, `shared T`, a frozen object) no field is assigned, no
  method but a `view fn` is called, and nothing is passed to `inout`; a unique field follows
  its root, a `shared mut` field keeps its writes through any root. `self` is mutable
  in every method but a `view fn`. `const` is gone, a syntax error.
- A `view T` parameter takes every form of the object, an `inout T` a `mut` or `shared mut`
  one; a borrow carries no qualifier, `mut` included.
- `move x` ends `x`; reading it afterwards is an error, after a branch that moved it
  "may have been moved", in a loop it must be assigned again; an assignment revives it;
  `move x.f` moves a unique field out and ends `x`; `move` of a temporary does nothing.
- An `async fn` call starts a task and returns its handle; `await` waits for the result.
  `main` may be `async fn main() -> int`.
- The send check: a number, a `Str`, a handle and a unique object (`T` or `mut T`) whose
  parts all cross go to a task (by `move` or as a temporary); a `shared` or `weak` reference,
  a function value and an `Obj` do not.
- `parallel for` runs a loop's iterations on the pool: the body reads values, arrays of
  values and handles of the enclosing function, changes its variables only by `+=`, `-=` and
  `*=` reductions and writes an `inout` array at the loop's index only. A light body under
  4096 iterations runs inline.
- `locked mut T`: contents reached in `lock c as v: inout { }` or `: view { }`, with no
  `await`, nested `lock`, `parallel for`, `return`, `break` or `continue` inside; `locked T`
  takes `view` blocks only.
- `atomic T`: a read is a load, `=` a store, `+=` and `-=` atomic; `exchange(v)` and
  `compareExchange(expected, desired) -> bool`. A copy of an atomic place is another handle;
  a value makes a new cell. A cell takes no `mut`.
- `atomic shared T`: frozen with everything it holds after the send check; immutable
  access; its `atomic` fields still change; `atomic shared mem::Box<mem::Buffer<T>>`
  is a frozen buffer a `parallel for` reads through `value()`.
- The parser holds every qualifier to its form: `atomic shared mut T`, `mut shared T`,
  `atomic mut T`, two qualifiers on one type and any `const` are syntax errors, and `atomic
  N` with `N` a class is an error that names the two forms an object crosses in, `atomic
  shared N` and `locked mut N`.
- `Copyable` is a built-in, structural trait: numbers, `bool`, `char`, handles and values of
  such parts. `T: Copyable` bounds a template; `impl Copyable for X` is an error.
- `--strict-ownership`: a move-only value read from a place flows into an owning slot by
  `move` only.

---

## The qualifiers

| Written | Holds | Reached by | Written through it | Crosses to a task | Made from |
|---|---|---|---|---|---|
| nothing (`T`, unique) | an object | one owner | never | by `move`, or as a temporary | a new object, or a `mut T` |
| `mut T` | an object | one owner | by the owner | by `move`, or as a temporary | a new object, or `move` of a `T` |
| `shared T` | an object | aliases on one thread | never | never | a unique reference, moved or new |
| `shared mut T` | an object | aliases on one thread | by every alias | never | a `mut T`, or `move` of a `T`, or a new object |
| `atomic T` | a cell of a number, `bool` or `char` | handles on any thread | by its atomic operations | as a handle | a value (a new cell); another handle (the same cell) |
| `locked mut T` | a cell of an object behind a mutex | handles on any thread | in `lock ... as v: inout` blocks | as a handle | a `mut T`, or `move` of a `T`, or a new object, after the send check |
| `locked T` | a cell of an object nothing writes (a warning) | handles on any thread | never (`view` blocks only) | as a handle | a unique reference, moved or new, after the send check |
| `atomic shared T` | a frozen object | handles on any thread | never | as a handle | a unique reference, moved or new, after the send check |

A qualifier is written before the type of a variable (`let n: mut Node = ...`, or alone,
`let n: mut = ...`), a parameter (`cfg: atomic shared Config`), a field (`hits: atomic
int`, `cache: shared Cache`, `next: shared mut Node`) and a result (`-> mut Node`). Each
word buys one thing: `mut`, the writes through the owner; `shared`, several owners;
`atomic shared`, an object frozen for every thread (`atomic`, the reference crosses threads;
`shared`, the object has several owners, one on each thread at least); `locked mut`, an
object written under a lock. A type takes one qualifier (`a type takes one qualifier:
'shared mut shared' is two`), the writes are written after the form (`'mut shared' is
spelled 'shared mut': the form first, then the writes`), and what nothing writes takes no
`mut`: `atomic shared mut T` is `'atomic shared mut T' would be written from several threads
with no lock; write 'atomic shared T' to freeze it, or 'locked mut T' to change it under a
lock`, and `atomic mut T` or `mut atomic T` is `an 'atomic' cell takes no 'mut': it changes
by its operations; an object crosses threads as 'atomic shared T' (frozen) or 'locked mut
T'`. `const` is a keyword still, and every type that spells it is `'const' is gone: a
reference is immutable unless written 'mut T' ('shared mut T', 'locked mut T')`. Those are
syntax errors. `shared`, `shared mut` and `atomic shared` qualify a reference: on a value
(`shared int`, `shared mut Point`, `shared int[2]`, `atomic shared Pair`) each is an error,
and so is `mut` on one (`'mut' applies to objects; 'int' is a value: its storage is
rewritten by assignment, which 'let' fixes`). `atomic` takes a number, `bool` or `char`
only: on a class it is an error that names the two forms an object crosses in (`'atomic'
holds a number, a 'bool' or a 'char', not 'N': an object crosses threads as 'atomic shared
N' (frozen) or 'locked mut N'`). `locked` holds an object in this version (`locked int` is
an error: wrap the value in a class), and without `mut` it is a warning (`warning: 'locked
T' locks an object nothing writes; 'atomic shared T' is read from every thread with no
lock`).

## Conversions

The declared type of the destination converts, in every declaration, assignment, argument,
return and store:

| Into \ from | a new object, or `move` of a `T` or `mut T` | a `T` read from a place | a `mut T` read from a place | `shared` | `shared mut` | `atomic shared` | `atomic`, `locked`, `locked mut` |
|---|---|---|---|---|---|---|---|
| `T` | moves | copies | copies | error | error | error | error |
| `mut T` | moves | error | copies | error | error | error | error |
| `shared` | converts | converts | converts | copies | error | error | error |
| `shared mut` | converts | error | converts | error | copies | error | error |
| `atomic shared` | send check, then frozen | the same | the same | error | error | copies | error |
| `locked` | send check, then a new cell | the same | the same | error | error | error | copies, the same form |
| `locked mut` | send check, then a new cell | error | the same | error | error | error | copies, the same form |

A unique reference converts into every form, and gains `mut` only by `move` or as a
temporary: `cannot store a 'Node' in variable 'm', a 'mut Node': a reference gains 'mut'
only by 'move'`. Everything else converts only as it is: `cannot store a 'shared mut Node'
in variable 'r', a 'shared Node': a 'shared' reference, a frozen object and a cell convert
only as they are`, and the same for a `shared Node` into a `Node` or an `atomic shared
Node`, an `atomic shared Config` into a `Config`, a `locked Node` into a `locked mut Node`.
(Without `--strict-ownership` a copy of a `T` or `mut T` read from a place is a copy, and
the compiler transfers at a last use, [Moves](#moves).) A `view T` parameter takes every
form of `T`; an `inout T` takes a `mut` or `shared mut` one.

## Moves

- `move x` takes a local variable (a `let` one too) or a by-value parameter, and `move x.f`
  a unique reference field of one, through unique fields only. `move` of a `shared` or
  `weak` field, of a value, or of a field of a borrow is an error (`cannot move 'h.other':
  it is 'shared Job', not a unique part of 'h'`).
- `move` is also how a reference gains `mut`: `let m: mut Node = move n` with `n` a `Node`
  (a `mut T` flows into a `T` without it).
- A read after a move is an error naming the move (`'a' was moved at 11:16; it cannot be
  used after the move`); after an `if` or a `match` that moves on one path only, `'a' may
  have been moved (by the 'if' at 14:3, at 14:19); it cannot be used after a move`. A
  `return`, `break` or `continue` ends its path.
- A loop checks the end of every iteration, the body's end and each `continue`: a variable of
  the enclosing function that the body moves is assigned again before it, or `'c' is moved in
  the loop at 17:3 and not assigned again before the next iteration`.
- An assignment revives a moved variable; `move x.f` ends `x` until it is assigned whole.
- A move is a write: not of a variable a `view` local reads until the local's last use, and
  not beside a borrow of the same variable in one call.
- `move` before a temporary does nothing. Without `--strict-ownership`, a copy where a
  `move` could be written is still a copy, and transfers at a last use are still inferred
  ([Ownership transfers](16-memory.md#ownership-transfers)).

## Tasks

- `async fn f(...) -> T` declares a function that runs as a task; its parameters are numbers,
  `bool`, `char`, references and handles (a struct, a tuple, an array or a function value as
  a parameter is an error in this version), and its result a number, a `bool`, a `char`, a
  reference or nothing (`an 'async fn' returns a number, bool, char, a reference or nothing
  in the prototype, not '(int, int)'`) that passes the send check (`the result of 'async fn'
  'give' (a 'shared Job') cannot cross to the task's awaiter`).
- Calling an async function starts the task at once, and the call's value is its handle.
  `await h` waits for the task and gives its result; `await f(x)` awaits the call in place.
  `await h` spends the handle: `h` cannot be used again (`'t' was awaited at 10:11; a task
  handle is spent by its 'await'`), and every task handle is spent, by `await` or by `move`,
  on every path before its scope ends (`task 't' is not awaited before this 'return': ...`,
  `task 't' may not be awaited when its scope ends: it is spent on some paths only ...`), and
  before its variable takes another (`task 't' is replaced before it is awaited: await it (or
  move it) first`).
- `async fn main() -> int` is allowed. In this version `await` is allowed in an ordinary
  function too, and blocks the thread; an async function cannot be a method, and a module
  that declares one cannot be imported yet (`return type of function 'slow' is a task
  ('Task<int>'): a module that declares an 'async fn' cannot be imported in the prototype`).
- The send check at an async call's arguments: a number, a `bool`, a `char`, a `Str`, a
  handle (`atomic`, `locked`, `atomic shared`) and a unique object, a `T` or a `mut T`,
  whose fields, own and inherited and through structs, tuples, arrays, optionals and enum
  payloads, all cross. A unique object crosses by `move` or as a temporary; copying one into
  a task is an error (`cannot copy 'a' (a unique 'Node') into a task: write 'move a' to
  transfer it`). A `shared` or `weak` part, a function value and an `Obj` do not cross, and
  the error names the field (`cannot send 'j' (a 'Job'): field 'cache' is 'shared Cache'`; a
  deeper one by its path, `field 'job.cache'`).

## `parallel for`

- `parallel for i in a..b { }` runs the iterations on the pool and finishes before the next
  statement. `..=` and `step` are allowed; the range is evaluated once, as for `for`.
- A body uses, of the enclosing function: values (numbers, `bool`, `char`, enums, and arrays
  of them, a `view` or `inout` slice too), read only unless a reduction; `atomic`, `locked`,
  `locked mut` and `atomic shared` variables and parameters; and the reduction variables. A
  reference, a `Str`, a function value or an array of references is an error (`'nodes' (a
  'Node[2]') cannot be used in a 'parallel for' body: an iteration uses only values, arrays
  of values and 'atomic' variables of the enclosing function`).
- A variable of the enclosing function changes only by a reduction, `s += e`, `s -= e` or
  `s *= e`, or, if `atomic`, by its operations (`cannot assign to 'mx' in a 'parallel for'
  body: a variable of the enclosing function changes only by a reduction ('mx += e', 'mx -=
  e', 'mx *= e') or, if 'atomic', by its operations`).
- An `inout` array of the enclosing function is written at the loop's index only (`'ys' is
  written in a 'parallel for' body at an index other than the loop's variable 'i': two
  iterations could write one element`).
- `break`, `continue`, `return`, `await` and starting a task are errors in the body (`'break'
  cannot be used in a 'parallel for' body (the prototype)`, `'return' cannot leave a
  'parallel for' body`, `'await' cannot be used in a 'parallel for' body`, `a task cannot be
  started in a 'parallel for' body`). The body may call functions and run loops.
- The iterations are split into at most 256 chunks, each with a partial of its own for every
  reduction, folded in chunk order afterwards: the result does not depend on the number of
  threads or on which thread ran a chunk. A parallel float sum may differ from the
  sequential loop's.
- A light body (no call, no loop) under 4096 iterations runs in order on the calling thread.
- An `atomic` cell changed on every iteration is a warning (`'hits' changes on every
  iteration of the 'parallel for': ...`).
- A `mem::Buffer<T>` of numbers that the enclosing function owns is read at any index (`at`,
  `get`, `len`) and written at the loop's variable only (`set(i, v)`, `i` the variable or a
  conversion of it; `'ys' is written in a 'parallel for' body at an index other than the
  loop's variable 'i': two iterations could write one element`), and never reshaped (`'ys'
  (a 'Buffer<float>' of the enclosing function) is read ('at', 'get', 'len') and written at
  the loop's variable ('set') in a 'parallel for' body; 'reserve' could move its storage
  under the other iterations`). Its `at`, `get` and `len` are read in place, a bounds check
  and a load, with no count on the buffer's object.

## Cells

- **`locked mut T`.** A unique object flowing into a `locked mut` slot becomes a cell, after
  the send check of what it holds; the handle copies into tasks and parallel bodies. `lock x
  as c: inout { }` and `lock x as c: view { }` take the mutex for the block, where `x` is a
  `locked` variable, field or parameter (`'n' is not 'locked': a 'lock' block takes a
  'locked' variable, field or parameter`). Outside a block the contents cannot be reached
  (`'counts' is 'locked': reach its contents in 'lock counts as c: view { }' (or 'inout')`),
  and inside one `await` (`'await' cannot be used in a 'lock' block: a wait holding a
  lock`), another `lock` (`a 'lock' block cannot hold another 'lock'`), a `parallel for`,
  `return`, `break` and `continue` are errors. `locked T` without `mut` is a cell nothing
  writes: its declaration is a warning (`'locked Counts' locks an object nothing writes;
  'atomic shared Counts' is read from every thread with no lock`), it takes `view` blocks
  only, and an `inout` block on it is an error (`'c' is 'locked Counts', whose contents are
  never written; declare it 'locked mut Counts' for a 'lock ... as v: inout' block`).
- **`atomic T`.** `T` is a number type, `bool` or `char` (`'atomic' holds a number, a 'bool'
  or a 'char', not 'int?' (the prototype has no atomic arrays)`; for a class, `'atomic' holds
  a number, a 'bool' or a 'char', not 'N': an object crosses threads as 'atomic shared
  N' (frozen) or 'locked mut N'`). A cell takes no `mut` (`atomic mut int` is a syntax
  error, [The qualifiers](#the-qualifiers)). A read is an atomic load, `=` an atomic store,
  `+=` and `-=` one atomic operation (integers wrap; `f32` and `f16` round on every store;
  narrow integers wrap on reads); `x.exchange(v)` returns the value found;
  `x.compareExchange(expected, desired)
  -> bool` stores `desired` if the cell holds `expected`. A read of the place on the right of
  an assignment to it, and every other compound operator, is an error, since it would read
  and write separately (`x = x + 1` is `x += 1`, the parser's rewriting). A copy of an atomic
  place (a declaration, an argument, a result, `self.f = p` in `__init__`) is another handle
  to the cell; any other value flowing into an `atomic` slot makes a new cell. A struct
  cannot hold an `atomic` field in this version; a class can.
- **`atomic shared T`.** One qualifier of two words: `atomic`, the reference crosses
  threads; `shared`, the object has several owners, one on each thread at least. Nothing
  writes it, so `mut` has no place on it: `atomic shared mut T` is a syntax error (`'atomic
  shared mut T' would be written from several threads with no lock; write 'atomic shared T'
  to freeze it, or 'locked mut T' to change it under a lock`). A unique object, a `T` or a
  `mut T`, flowing into an `atomic shared` slot is send-checked (`cannot make 'Holder(...)'
  (a 'Holder') 'atomic shared': field 'cache' is 'shared Cache'`) and frozen: a walk visits
  every object it holds through its fields, arrays and buffers; a `shared` reference does
  not convert into the slot. The handle copies anywhere, and reads through it count nothing.
  Access is immutable (`'cfg' is frozen (an 'atomic shared Config'); cannot assign to its
  field 'hi'`, `'cfg' is frozen (an 'atomic shared Config'); 'rename' is not a 'view fn'`);
  an `atomic` field of a frozen object still changes. `atomic shared` of a value is an error
  (`'atomic shared' qualifies a reference; 'Pair' is a value: box it ('mem::Box<Pair>') to
  share what it holds`): `let xs: atomic shared mem::Box<mem::Buffer<float>> = mem::Box(move
  b)` freezes the box, the buffer and its elements.

## Limits for now

- A cell and a frozen object are never freed: their counts are left alone so that a handle
  crosses threads without an atomic count, and `--track-heap` reports them as live.
- Reference counts are not atomic. The checks keep every plainly counted reference on one
  thread; a program that calls into runtime pieces that are not thread-safe from a task (heap
  tracking, standard input) is not protected.
- No channels, no cancellation, no event loop of the program's own, and no `join`: `await` in
  ordinary code blocks the thread. A program with an `async fn` is compiled with the `-O1`
  pipeline under `-O0`: the coroutines of MLIR's async runtime do not survive an unoptimized
  split in this version.
- A task handle has one operation, `await`, which spends it; the compiler requires every
  handle in a variable to be awaited (or moved) before its scope ends, so that no task is
  dropped while it runs, and does not follow one stored in a field, an element or a tuple.
- An `async fn` is a free function of the program's own module: no async methods, and a
  module that declares an `async fn` cannot be imported. Its parameters are numbers,
  references and handles, not a struct, a tuple, an array or a function value, and its result
  a number, a `bool`, a `char`, a reference or nothing.
- A qualifier is not part of a type argument: `List<shared Node>` is a `List<Node>`, and a
  `shared Node` does not satisfy `T: Copyable`.
- `atomic` holds one number, `bool` or `char`: no atomic arrays, no `atomic` field in a
  struct, and `compareExchange` does not write the value found back into `expected`.
- `locked` holds an object, not a value: wrap a value in a class.
- `break` and `continue` cannot be used in a `parallel for` body.
- A `match` binding takes no qualifier (it is a copy of the subject in the subject's form;
  an `inout` binding on a `mut` place writes with no copy), and a move out of borrowed
  storage is not checked at run time.
- An object held in a struct, a tuple or an array is reached through the value, which takes
  no `mut`: the variable's `let` fixes the value's own storage and the objects it holds
  (`'p' is declared with 'let', and so are the objects it holds; drop 'let' to change
  them`), and without `let` their fields are written through it. A `shared mut` field keeps
  its writes through a struct too.
- The wording of the diagnostics is not final.

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Conversion refused | `let u: Node = s` with `s: shared Node`; `let m: mut Node = a` with `a` a `Node` read from a place (`a reference gains 'mut' only by 'move'`); a `shared mut` into a `shared` slot or back; a `shared` into an `atomic shared` slot; an `atomic shared` into anything but `atomic shared`; a `locked` into a `locked mut` slot |
| Qualifier on a value | `shared int`, `shared mut Point`, `shared int[2]`, `atomic shared Pair`; `mut int`, `mut (int, int)` (`'mut' applies to objects; ... which 'let' fixes`) |
| Qualifier on a borrow | `n: view mut Node`, `n: inout shared Node` |
| Spelling (syntax errors) | `const Node`, `shared const Node`, `atomic shared const Node` (`'const' is gone`); `atomic shared mut N`; `mut shared N`, `mut locked N`; `atomic mut int`, `mut atomic int`; `shared mut shared N` and any second qualifier; `atomic N` with `N` a class (Sema's) |
| Write through an immutable reference | `cfg.name = "x"` with `cfg` a `Config` (`'cfg' is immutable (a 'Config'; write 'mut Config' to change it); cannot assign to its field 'name'`); `c.xs[0] = 1` (`cannot assign to its elements`); through a `shared` field (`field 'cache' is immutable (a 'shared Cache'; write 'shared mut Cache' to change it)`), a frozen object (`'cfg' is frozen (an 'atomic shared Config')`); `mark(cfg)` to an `inout` parameter (`it cannot be passed to 'inout' parameter 'n'`); a `match` binding |
| Non-`view` method on an immutable reference | `cfg.rename("y")`, `ro.get()`, `s.concat("b")` with `s` a `Str` (`'s' is immutable (a 'Str'; write 'mut Str' to change it); 'concat' is not a 'view fn'`) |
| `locked` without `mut` | `let c: locked Counts = ...` (a warning); `lock c as v: inout { }` on it (`'c' is 'locked Counts', whose contents are never written; declare it 'locked mut Counts' for a 'lock ... as v: inout' block`) |
| Copy into a task | `consume(a)` with `a` a unique `Node` |
| Task handle | `let t = fib(5);` with no `await` before the scope ends or a `return`; `await t` twice; `await t` in one branch only; `t = fib(4)` before `await t` |
| Part that cannot cross | `move j` with a `shared`, `weak`, function-value or `Obj` part; a `shared` result of an `async fn` |
| Value as a task parameter | `async fn plot(p: Point)`, `async fn call(f: () -> int)` |
| Use after move | `move a` then `a.name`; a move in one branch; a move in a loop with no assignment before the next iteration |
| Move of what is not a unique part | `move h.other` with `other: shared Job`; a move of a viewed variable; a move beside a borrow in one call |
| Body of a `parallel for` | a reference, a `Str` or a function value of the enclosing function; a write that is not a reduction; an `inout` array written at another index; `break`, `continue`, `return`, `await`, a task started |
| Buffer in a `parallel for` body | `ys.set(unsigned(i + 1), v)` with `ys` a `mem::Buffer<float>` of the enclosing function; `ys.reserve(32)` or `ys.grow(1)` in the body |
| Atomic read and write | `hits = hits * 2 + 1`, `hits *= 2`, `hits.load()`, `atomic Str`, an `atomic` field in a struct, `hits += 1.5` on an `atomic int` |
| Locked contents | `counts.total` outside a `lock`; `await` or a `lock` in a `lock` block; `lock n` of what is not `locked`; `locked int` |
| `Copyable` | `impl Copyable for X`; `twice(Node(2))` with `twice<T: Copyable>` |
| Copy rule (`--strict-ownership`) | `let b = a` with `a` a unique `Node`; `return n`; `take(a)`; `[Node(1); 3]` |

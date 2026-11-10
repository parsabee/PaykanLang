# The concurrency prototype

A working sketch of the v0.4 concurrency and ownership model
(`docs/design/concurrency.md`), built to show the idea end to end rather
than to be complete.  The numbered programs run with `paykan run`; the
ones in `errors/` show what Sema rejects (each line marked `error`).

| Program | Shows |
|---|---|
| `01_parallel_for.pkn` | `parallel for` with a reduction, an `inout` array written at the loop's index, and an `atomic int` counter |
| `02_async_await.pkn` | `async fn`, `await`, tasks started by calling an async function, `move` of a reference into a task, `async fn main` |
| `err_03_first_checks.pkn` | the first checks: copying a unique reference into a task, use after `move`, sending a `shared` reference, writing anything but a reduction in a parallel body |
| `04_qualifiers.pkn` | the qualifiers on places: the bare type (unique, immutable), `mut`, `shared`, `shared mut`; the conversions between them; a `view` parameter taking every form |
| `05_locked.pkn` | a `locked mut` cell shared by tasks, written in `lock c as v: inout { }` and read in `: view` blocks |
| `06_atomic_shared_const.pkn` | an `atomic shared` object frozen with everything it holds, read by every task without a count |
| `07_atomic.pkn` | `atomic` on every number type, `bool` and `char`; `atomic` fields of a class; `exchange` and `compareExchange`; handles |
| `08_moves.pkn` | precise moves: a move on one path, revival by assignment, moves in loops, `break`, a partial move `move h.job` |
| `09_send.pkn` | the send check on what a task takes and returns; `Copyable` as a generic bound; a `view` taking every form |
| `10_buffers.pkn` | heap buffers in a `parallel for`: one of the enclosing function read at any index and written at the loop's variable, and one frozen in a box read by every thread |
| `errors/err_qualifiers.pkn` | the conversions a qualifier refuses (`shared` into unique, `mut` gained without `move`, `shared mut` into `shared`), writes through an immutable reference and an immutable field, `mut` on a value |
| `errors/err_strict_copies.pkn` | the copy rule: a unique value read from a place moves, `[v; n]` copies |
| `errors/err_locked.pkn` | reaching a `locked` cell outside a `lock` block, `await` and nested `lock` inside one |
| `errors/err_atomic_shared_const.pkn` | writing through a frozen `atomic shared` object, converting it back, qualifying a value |
| `errors/err_atomic.pkn` | operations that would read and write a cell separately, `atomic` of a `Str`, a cell in a struct |
| `errors/err_atomic_spellings.pkn` | the spellings the parser rejects: `const` in any form, `atomic shared mut`, `atomic mut`, `mut shared`, a second qualifier |
| `errors/err_moves.pkn` | "may have been moved" after an `if` or `match`, a loop that does not assign again, moving a viewed variable, a move beside a borrow in one call, a partial move of a `shared` field |
| `errors/err_buffers.pkn` | a buffer written at another index than the loop's variable, or reshaped, in a parallel body |
| `errors/err_send.pkn` | a `shared` part, an `Obj`, a function value or a `weak` field that cannot cross; a result that cannot; qualifiers on borrows rejected; `Copyable` |

## What is in the prototype

* **Syntax**: keywords `move`, `async`, `await`, `parallel`, `atomic`,
  `shared`, `mut`, `locked` and `lock`.  `async fn f(...) -> T`, `await
  e`, `move x`, `move x.f`, `parallel for i in a..b { }`, the qualifiers
  on any place's type (`let m: mut Node`, `p: shared mut Node` as a
  parameter, `hits: atomic int` or `cfg: atomic shared Config` as a
  field, `-> locked mut Counts` as a result; `n: mut = e` and `x: shared
  = e` with the type inferred), and `lock cell as c: inout|view { }`.
* **Qualifiers** (Sema): the bare type (unique, immutable), `mut`,
  `shared`, `shared mut`, `atomic`, `atomic shared` and `locked mut` on
  variables, parameters, fields and results, and on the parts of a type
  (#368: `List<shared Node>`, `(int, shared Node)`, `shared Node[3]`,
  `(shared Node) -> int`); `locked` alone is accepted
  with a warning, as it locks what nothing writes.  Every word added buys
  one thing, and the bare type costs nothing: a reference is immutable
  unless written `mut`, so through it there is no field assignment, no
  method that is not a `view fn` and no passing to `inout`, and what it
  holds reads immutable (a unique field follows its root and is never
  written `mut`; a `shared mut` field keeps its writes through any root).
  `self` is mutable in every method but a `view fn`; a `match` binding of
  a place is a borrow of it (a `view`, or an `inout` arm), and `match move
  x` owns the parts; an object held in a struct,
  tuple or array follows the variable's `let`.  `mut T` flows into `T`;
  `T` gains `mut` only by `move` or straight from a temporary; `T` or
  `mut T` moves into `atomic shared T`, frozen then; `shared` and `shared
  mut`, a frozen object and a cell convert only as they are.  `const` is
  gone, and `mut` goes on no value (`let` alone decides whether a number,
  struct, tuple or array is rewritten), no borrow, no cell and no frozen
  object.
  A borrow is written with no qualifier: a `view` parameter takes every
  form of the object, an `inout` a `mut` or `shared mut` one, neither
  counting it, and no borrow reaches a `locked`
  cell.  The copy rule holds: a move-only value read from a place (a
  unique reference, a `Str`, a container, or a struct, tuple or array
  holding one) flows into an owning slot only by `move` (or as a
  `.clone()`); a `shared` reference, a handle and a value of such parts
  are Copyable.
* **`locked mut T`**: a cell holding an object behind a mutex, made where a
  unique value flows into a `locked mut` slot; its handle copies into tasks
  and parallel bodies; its contents are reached only in a `lock` block,
  which may not `await`, `return`, `break`, `continue`, nest another
  `lock` or run a `parallel for`.  `locked T` alone is read in `: view`
  blocks only, with the warning `'locked T' locks an object nothing
  writes; 'atomic shared T' is read from every thread with no lock`; a
  `: inout` block on it is an error that says to declare it `locked mut T`.
* **`atomic shared T`**: a unique value flowing into such a slot is
  frozen with everything it holds (a vtable walk, `__walk`, visits every
  class's reference fields and every buffer's elements), after the send
  check of its contents; the handle then copies anywhere, and reads
  inside count nothing.  An `atomic` field of a frozen object still
  changes.  Each word says one thing: `atomic`, the reference crosses
  threads; `shared`, the object has several owners, one on each thread at
  least; and the bare type, that nothing writes it.  `atomic shared mut
  N` is the parse error `'atomic shared mut T' would be written from
  several threads with no lock; write 'atomic shared T' to freeze it, or
  'locked mut T' to change it under a lock`, `atomic mut N` is `an
  'atomic' cell takes no 'mut': it changes by its operations; an object
  crosses threads as 'atomic shared T' (frozen) or 'locked mut T'`, and
  `atomic N` of a class is Sema's `'atomic' holds a number,
  a 'bool' or a 'char', not 'N': an object crosses threads as 'atomic
  shared N' (frozen) or 'locked mut N'`.
  A struct, a tuple or an array is a value wherever it is and
  takes no qualifier of its own (`shared Node[3]` is three shared
  references, its elements' form); to freeze one, box it: `let xs:
  atomic shared mem::Box<mem::Buffer<float>> = mem::Box(move b)` freezes
  the box, the buffer it holds (`mem::Buffer<T>` crosses when `T` does,
  the design's table) and the elements, and a `parallel for` body reads
  it through the box's `value()`, the prototype's way to data larger than
  a fixed-size array (`benchmarks/sum_buf.pkn`).  A `parallel for` body
  also uses a `mem::Buffer<T>` of numbers that the enclosing function
  owns: read at any index (`at`, `get`, `len`), written at the loop's
  variable only (`set(i, v)`), never reshaped (`grow`, `reserve` are
  errors there).  A buffer's `at`, `get` and `len` on a number type
  are read in place by the lowering, a bounds check and a load, with no
  call into the memory module and no count on the buffer.
* **`atomic T`**: `T` any number type, `bool` or `char`; variables and
  class fields.  A read is an atomic load, `=` a store, `+=` and `-=` one
  atomic add (floats by a compare-and-swap loop, `f32` and `f16` rounded
  on every store, narrow integers wrapping on reads); `x.exchange(v)` and
  `x.compareExchange(expected, desired) -> bool`.  A `let` atomic is
  never assigned to and never changes through its name (`'k' is declared
  with 'let' and cannot be reassigned`, `... cannot be changed by
  'exchange'`): a cell that changes is declared without `let`, a `let`
  one is a warning (an atomic matters only when threads change it), and a
  cell field of a `let` object is the object's own, which changes.  A copy
  of an atomic place (`j = k`, an argument, a result, `self.f = p` in
  `__init__`) is another handle to the cell, `let j = k` a read-only copy
  of its value; any other value flowing into an `atomic` slot makes a new
  cell.  `x = x * 2` and any other read of the place on
  the right of an assignment to it is an error (it would read and write
  separately); `x = x + 1` is `x += 1`, the parser's desugaring.
* **Moves**: a forward "maybe moved" set over each body.  `move x` ends
  `x`; a later read is an error naming the move, or "may have been moved
  (by the 'if' at ...)" after a branch that moved it on one path; a
  `return`, `break` or `continue` ends its path.  A loop checks every
  iteration's end (the body's end and each `continue`): what the body
  moves of the enclosing function's variables is assigned again, or an
  error.  An assignment revives a moved variable.  A move is a write: not
  of a variable a `view` local reads, not beside a borrow of the same
  storage in one call.  `move x.f` moves a unique reference field out of
  an owned variable (through unique fields only; `shared` and `weak`
  fields, values and fields of a borrow are errors) and ends the
  variable; `move` before a temporary does nothing.
* **Task handles**: `await t` spends `t` (a later use is an error naming
  the await), and every task handle in a variable is spent, by `await` or
  by `move`, on every path before its scope ends and before the variable
  takes another task: `task 't' is not awaited before this 'return'`,
  `task 't' may not be awaited when its scope ends: it is spent on some
  paths only`, `task 't' is replaced before it is awaited`.  The design's
  typestate reduced to the one operation the prototype has (no `join`,
  `cancel` or `detach`), so that no task is dropped while it runs; a
  handle stored in a field, an element or a tuple is not followed.
* **The send check**: at an async call's arguments, at the declaration of
  an `async fn`'s result, and at every conversion to `atomic shared` or
  `locked`.  A number, a `Str`, a cell, a handle and a unique class, bare
  or `mut`, whose fields (own and inherited, through structs, tuples, arrays,
  optionals and enum payloads) are all crossable cross; a `shared` or
  `weak` part, a function value and an `Obj` do not, and the error names
  the field (`field 'job.cache' is 'shared Cache'`).  A unique reference,
  bare or `mut`, crosses by `move` or as a temporary; a value holding a
  unique part moves too, one of `shared` parts and handles copies.
* **`Copyable`**: a built-in, structural trait: numbers, `bool`, `char`,
  `shared` references (a `T` whose argument is `shared Node` too),
  `atomic` handles, and enums, tuples, structs, fixed arrays and optionals
  of Copyable parts (a struct's `shared` field counts; a unique reference,
  `Str`, a function value or a task does not).  `T: Copyable` bounds a
  template; `impl Copyable for X` is an error.
* **PIR**: `async fn`, `async_call` (a task, a `ptr`), `await`, and a
  `parallel` annotation on `while` (`pir::Parallel`: the counter, the trip
  count, the locals of one iteration, the partials).  Read as a plain
  `while` it is the same loop run in order, which every PIR pass and the C
  path do, so nothing else in the compiler had to learn about threads.
  Cells, freezing and locks are runtime calls.
* **MLIR**: `pir.async_call`, `pir.await` and `pir.parallel` ops.  The
  shared lowering runs a light body (no call, no loop) inline below 4096
  iterations, where a fork cannot pay (`benchmarks/README.md`); makes an
  `async fn` an `async.func` whose body is an
  `async.execute` region (which the runtime hands to its worker pool, so a
  call returns at once with the task and the body runs on a worker), an
  async call an `async.call` and an await an `async.await`, a suspension
  point inside a task; a `pir.parallel` becomes an `scf.parallel` over at
  most 256 chunks, each chunk running its range in an inner loop into its
  own partial, the partials folded in chunk order afterwards (the design's
  reproducible reduction).  The LLVM path then runs
  `convert-scf-to-openmp` (the chunks become an `omp.parallel` /
  `omp.wsloop`, scheduled dynamically one chunk at a time: on a hybrid
  CPU a static split lets an efficiency core set the loop's time,
  `benchmarks/README.md`), `async-func-to-async-runtime`, `async-to-async-runtime`
  and `convert-async-to-llvm`, and translates OpenMP to LLVM IR.  The C
  path (`--emit-c`) lowers the same ops sequentially: a task is the
  function's result carried in a pointer, a parallel loop a plain loop.
* **Runtimes**: tasks run on MLIR's async runtime (`libmlir_async_runtime`:
  a thread pool and reference-counted token, value and group objects with
  awaiter callbacks; no event loop, timers, cancellation or channels),
  parallel loops on LLVM's OpenMP runtime (`libomp`, in the release's
  `lib/<target triple>/` on Linux); both ship with the LLVM the compiler is
  built against, and `paykan run` loads them into the JIT when a program's
  code calls them (`paykan build` links them, by path, after the program's
  objects).  An installed `paykan` takes them from the build tree it was
  built in, so a program using tasks or parallel loops runs from an
  installation only while that tree is there.  After `main` returns, the
  JIT waits for every worker of the async runtime to leave the task it ran
  before it frees the program's code.  The prototype
  keeps that runtime; the finished feature needs Paykan's own, as the
  design's Runtime section describes.  The Paykan runtime's
  deferred-destroy state became thread-local; an `atomic` cell
  (`src/Runtime/Atomic.c`), a `locked` cell (`Locked.c`) and a frozen
  object (`Paykan_freeze`, `Object.c`) are immortal, their counts
  `PAYKAN_REF_MAX`, which retain and release leave alone, so a handle
  crosses threads without an atomic count and a frozen graph is read
  without one.

## Trying it

```
build/bin/paykan run samples/concurrency/01_parallel_for.pkn
build/bin/paykan run samples/concurrency/07_atomic.pkn
build/bin/paykan --check-only samples/concurrency/errors/err_moves.pkn
build/bin/paykan --check-only samples/concurrency/errors/err_strict_copies.pkn
build/bin/paykan --emit-pir samples/concurrency/05_locked.pkn
build/bin/paykan --emit-mlir=scf samples/concurrency/02_async_await.pkn
build/bin/paykan --emit-mlir=llvm samples/concurrency/01_parallel_for.pkn
build/bin/paykan build samples/concurrency/06_atomic_shared_const.pkn -o cfg
```

`--emit-c` of every program compiles as strict C11 against the runtime
and the standard library's module objects (as `scripts/c_strict.py` builds
a program) and prints the same output.

## What is not (on purpose)

Channels, `task::spawnOn`, event loops pinned per thread, cancellation,
task-handle typestate, the migration tool, holds across an `await`, atomic
counts on cells and frozen objects (they are immortal: cells and frozen
graphs leak), `atomic` fixed arrays, `compareExchange`'s `inout` write-back
of `expected`, `locked` type arguments and `atomic shared` or `locked`
tuple and array elements (`List<shared Node>` and `(int, shared Node)`
are there), the archetype check of templates (a copy of an unbounded
`T` is caught per instance only), `match`
bindings' qualifiers, the run-time check of a move out of borrowed
storage, a value held in memory (a struct, tuple or array) as a task
parameter, async methods, importing a module that declares an `async fn`
(`... is a task ('Task<int>'): a module that declares an 'async fn' cannot
be imported in the prototype`), a task whose result is a tuple, a struct
or an optional value (`an 'async fn' returns a number, bool, char, a
reference or nothing in the prototype`), task handles in fields, elements
or tuples (not followed by the await check), `break`/`continue` in a
parallel body, the par-safe call check, and the diagnostics' final
wording.  A program with an `async fn` is compiled with the `-O1` pipeline
under `-O0` (the coroutines of MLIR's async runtime do not survive an
unoptimized split; the design's own runtime replaces the lowering).
Non-atomic
reference counts are only safe because the checks keep references on one
thread; a program that calls into non-thread-safe runtime pieces from a
task (stdin) is not protected.  `--track-heap` counts every thread's
allocations: its counters are atomic.

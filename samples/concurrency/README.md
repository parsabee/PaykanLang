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
| `04_qualifiers.pkn` | the qualifiers on places: unique, `const`, `shared`, `shared const`; the conversions between them; a `view` parameter taking every form |
| `05_locked.pkn` | a `locked` cell shared by tasks and read in `lock c as v: inout { }` / `: view` blocks |
| `06_atomic_const.pkn` | an `atomic const` object frozen with everything it holds, read by every task without a count |
| `07_atomic.pkn` | `atomic` on every number type, `bool` and `char`; `atomic` fields of a class; `exchange` and `compareExchange`; handles |
| `08_moves.pkn` | precise moves: a move on one path, revival by assignment, moves in loops, `break`, a partial move `move h.job` |
| `09_send.pkn` | the send check on what a task takes and returns; `Copyable` as a generic bound; a `view` taking every form |
| `errors/err_qualifiers.pkn` | the conversions a qualifier refuses, `const` fields, qualifiers on values |
| `errors/err_strict_copies.pkn` | the copy rule under `--strict-ownership`: a unique value read from a place moves, `[v; n]` copies |
| `errors/err_locked.pkn` | reaching a `locked` cell outside a `lock` block, `await` and nested `lock` inside one |
| `errors/err_atomic_const.pkn` | writing through `atomic const`, converting it back, qualifying a value |
| `errors/err_atomic.pkn` | operations that would read and write a cell separately, `atomic` of a `Str`, a cell in a struct |
| `errors/err_moves.pkn` | "may have been moved" after an `if` or `match`, a loop that does not assign again, moving a viewed variable, a move beside a borrow in one call, a partial move of a `shared` field |
| `errors/err_send.pkn` | a `shared` part, an `Obj`, a function value or a `weak` field that cannot cross; a result that cannot; `inout` exactness, `view const` rejected; `Copyable` |

## What is in the prototype

* **Syntax**: keywords `move`, `async`, `await`, `parallel`, `atomic`,
  `shared`, `const`, `locked` and `lock`.  `async fn f(...) -> T`, `await
  e`, `move x`, `move x.f`, `parallel for i in a..b { }`, the qualifiers
  on any place's type (`let c: const Node`, `p: shared const Node` as a
  parameter, `hits: atomic int` or `cfg: atomic const Config` as a field,
  `-> locked Counts` as a result; `x: shared = e` with the type inferred),
  and `lock cell as c: inout|view { }`.
* **Qualifiers** (Sema): unique (unwritten), `const`, `shared`, `shared
  const`, `atomic`, `atomic const` and `locked` on variables, parameters,
  fields and results.  A unique value converts into any form, a `const`
  one into the other `const` forms, a `shared` reference and a handle only
  as they are.  A `const` place is frozen: no field assignment, no
  non-`view` method, and what it holds reads as `const`.  A `view`
  parameter takes every form (`view const` is an error: a `view` is
  read-only already), `inout` exactly its own qualifier, and no borrow reaches a `locked`
  cell.  Under `--strict-ownership` the copy rule holds: a move-only value
  read from a place (a unique reference, or a struct, tuple or array
  holding one) flows into an owning slot only by `move`; a `shared`
  reference, a handle and a value of such parts are Copyable.
* **`locked T`**: a cell holding an object behind a mutex, made where a
  unique value flows into a `locked` slot; its handle copies into tasks
  and parallel bodies; its contents are reached only in a `lock` block,
  which may not `await`, `return`, `break`, `continue`, nest another
  `lock` or run a `parallel for`.
* **`atomic const T`**: a unique value flowing into such a slot is frozen
  with everything it holds (a vtable walk, `__walk`, visits every
  class's reference fields and every buffer's elements), after the send
  check of its contents; the handle then copies anywhere, and reads
  inside count nothing.  An `atomic` field of a frozen object still
  changes.
* **`atomic T`**: `T` any number type, `bool` or `char`; variables and
  class fields.  A read is an atomic load, `=` a store, `+=` and `-=` one
  atomic add (floats by a compare-and-swap loop, `f32` and `f16` rounded
  on every store, narrow integers wrapping on reads); `x.exchange(v)` and
  `x.compareExchange(expected, desired) -> bool`.  A copy of an atomic
  place (`let j = k`, an argument, a result, `self.f = p` in `__init__`)
  is another handle to the cell; any other value flowing into an `atomic`
  slot makes a new cell.  `x = x * 2` and any other read of the place on
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
  an owned variable (through unique fields only; `shared`, `const` and
  `weak` fields, values and fields of a borrow are errors) and ends the
  variable; `move` before a temporary does nothing.
* **The send check**: at an async call's arguments, at the declaration of
  an `async fn`'s result, and at every conversion to `atomic const` or
  `locked`.  A number, a `Str`, a cell, a handle and a unique or `const`
  class whose fields (own and inherited, through structs, tuples, arrays,
  optionals and enum payloads) are all crossable cross; a `shared` or
  `weak` part, a function value and an `Obj` do not, and the error names
  the field (`field 'job.cache' is 'shared Cache'`).  A unique or `const`
  reference crosses by `move` or as a temporary; a value holding a unique
  part moves too, one of `shared` parts and handles copies.
* **`Copyable`**: a built-in, structural trait: numbers, `bool`, `char`,
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
  shared lowering makes an `async fn` an `async.func` whose body is an
  `async.execute` region (which the runtime hands to its worker pool, so a
  call returns at once with the task and the body runs on a worker), an
  async call an `async.call` and an await an `async.await`, a suspension
  point inside a task; a `pir.parallel` becomes an `scf.parallel` over at
  most 256 chunks, each chunk running its range in an inner loop into its
  own partial, the partials folded in chunk order afterwards (the design's
  reproducible reduction).  The LLVM path then runs
  `convert-scf-to-openmp` (the chunks become an `omp.parallel` /
  `omp.wsloop`), `async-func-to-async-runtime`, `async-to-async-runtime`
  and `convert-async-to-llvm`, and translates OpenMP to LLVM IR.  The C
  path (`--emit-c`) lowers the same ops sequentially: a task is the
  function's result carried in a pointer, a parallel loop a plain loop.
* **Runtimes**: tasks run on MLIR's async runtime (`libmlir_async_runtime`:
  a thread pool and reference-counted token, value and group objects with
  awaiter callbacks; no event loop, timers, cancellation or channels),
  parallel loops on LLVM's OpenMP runtime (`libomp`); both ship with the
  LLVM the compiler is built against, and `paykan run` loads them into the
  JIT when a program uses them (`paykan build` links them).  The prototype
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
build/bin/paykan --check-only --strict-ownership samples/concurrency/errors/err_strict_copies.pkn
build/bin/paykan --emit-pir samples/concurrency/05_locked.pkn
build/bin/paykan --emit-mlir=scf samples/concurrency/02_async_await.pkn
build/bin/paykan --emit-mlir=llvm samples/concurrency/01_parallel_for.pkn
build/bin/paykan build samples/concurrency/06_atomic_const.pkn -o cfg
```

`--emit-c` of every program compiles as strict C11 against the runtime
and the standard library's module objects (as `scripts/c_strict.py` builds
a program) and prints the same output.

## What is not (on purpose)

Channels, `task::spawnOn`, event loops pinned per thread, cancellation,
task-handle typestate, the migration tool, holds across an `await`, atomic
counts on cells and frozen objects (they are immortal: cells and frozen
graphs leak), `atomic` fixed arrays, `compareExchange`'s `inout` write-back
of `expected`, qualifiers as part of type arguments (`List<shared Node>`
is `List<Node>`), the archetype check of templates (a copy of an unbounded
`T` is caught per instance under `--strict-ownership` only), `match`
bindings' qualifiers, the run-time check of a move out of borrowed
storage, a value held in memory (a struct, tuple or array) as a task
parameter, async methods, async functions across modules, a task whose
result is an optional value, `break`/`continue` in a parallel body, the
par-safe call check, and the diagnostics' final wording.  Non-atomic
reference counts are only safe because the checks keep references on one
thread; a program that calls into non-thread-safe runtime pieces from a
task (heap tracking, stdin) is not protected.

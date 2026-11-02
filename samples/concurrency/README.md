# The concurrency prototype

A working sketch of the v0.4 concurrency and ownership model
(`docs/design/concurrency.md`), built to show the idea end to end rather
than to be complete.  Three programs here run with `paykan run`:

| Program | Shows |
|---|---|
| `01_parallel_for.pkn` | `parallel for` with a reduction, an `inout` array written at the loop's index, and an `atomic int` counter |
| `02_async_await.pkn` | `async fn`, `await`, tasks started by calling an async function, `move` of a reference into a task, `async fn main` |
| `03_errors.pkn` | what Sema rejects: copying a unique reference into a task, use after `move`, sending a `shared` reference, writing anything but a reduction in a parallel body |

## What is in the prototype

* **Syntax**: keywords `move`, `async`, `await`, `parallel`, `atomic` and
  `shared`.  `async fn f(...) -> T`, `await e`, `move x`, `parallel for i
  in a..b { }`, `let k: atomic int = 0`, `x: shared T = e` (and `let x:
  shared = e`), `p: shared T` as a parameter.
* **Sema**: a call of an `async fn` has type `Task<T>`; `await` takes one
  and yields `T`.  `move x` ends `x` (a later read is an error; an
  assignment revives it; a move inside a loop of a variable declared
  outside it is an error).  The send check: a reference argument of an
  async call must be `move`d or a temporary, and never `shared`.  A
  `parallel for` body may read only values, arrays of values and `atomic`
  variables of the enclosing function, and changes an outer variable only
  by a reduction (`s += e`, `s -= e`, `s *= e`); `break`, `continue`,
  `return` and `await` are errors in it.  An `atomic int` reads as its
  value, `k += e` is one atomic add, and passing it to an `atomic int`
  parameter passes the handle.
* **PIR**: `async fn`, `async_call` (a task, a `ptr`), `await`, and a
  `parallel` annotation on `while` (`pir::Parallel`: the counter, the trip
  count, the locals of one iteration, the partials).  Read as a plain
  `while` it is the same loop run in order, which every PIR pass and the C
  path do, so nothing else in the compiler had to learn about threads.
* **MLIR**: `pir.async_call`, `pir.await` and `pir.parallel` ops.  The
  shared lowering makes an `async fn` an `async.func`, an async call an
  `async.call` and an await an `async.await`; a `pir.parallel` becomes an
  `scf.parallel` over at most 256 chunks, each chunk running its range in
  an inner loop into its own partial, the partials folded in chunk order
  afterwards (the design's reproducible reduction).  The LLVM path then
  runs `convert-scf-to-openmp` (the chunks become an `omp.parallel` /
  `omp.wsloop`), `async-func-to-async-runtime`, `async-to-async-runtime`
  and `convert-async-to-llvm`, and translates OpenMP to LLVM IR.  The C
  path (`--emit-c`) lowers the same ops sequentially: a task is the
  function's result carried in a pointer, a parallel loop a plain loop.
* **Runtimes**: tasks run on MLIR's async runtime (`libmlir_async_runtime`),
  parallel loops on LLVM's OpenMP runtime (`libomp`); both ship with the
  LLVM the compiler is built against, and `paykan run` loads them into the
  JIT when a program uses them (`paykan build` links them).  The Paykan
  runtime's deferred-destroy state became thread-local, and an `atomic
  int` is a heap cell (`src/Runtime/Atomic.c`) whose count is immortal, so
  a handle crosses threads without an atomic count.

## Trying it

```
build/bin/paykan run samples/concurrency/01_parallel_for.pkn
build/bin/paykan run samples/concurrency/02_async_await.pkn
build/bin/paykan --check-only samples/concurrency/03_errors.pkn
build/bin/paykan --emit-pir samples/concurrency/01_parallel_for.pkn
build/bin/paykan --emit-mlir=scf samples/concurrency/02_async_await.pkn
build/bin/paykan --emit-mlir=llvm samples/concurrency/01_parallel_for.pkn
build/bin/paykan build samples/concurrency/02_async_await.pkn -o fib
```

`--emit-c` of either program compiles as strict C11 against the runtime
and the standard library's module objects (as `scripts/c_strict.py` builds
a program) and prints the same output.

## What is not (on purpose)

The design's `const`, `locked`/`lock`, channels, `task::spawnOn`, event
loops pinned per thread, cancellation, task-handle typestate, the
`Copyable` rule for generics, the migration tool, holds across an `await`,
`atomic const` freezing, atomic counts on cells (cells leak), `atomic`
floats and arrays, async methods, async functions across modules, a task
whose result is an optional value, `break`/`continue` in a parallel body,
the par-safe call check, and the diagnostics' final wording.  Non-atomic
reference counts are only safe because the checks keep references on one
thread; a program that calls into non-thread-safe runtime pieces from a
task (heap tracking, stdin) is not protected.

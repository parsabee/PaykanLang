# Ownership qualifiers: `view`, `inout` and `let` (prototype)

Status: **prototype** on branch `proto/ownership`, behind `--ownership`.
Nothing here is a commitment; it exists to try the model end to end.

## The model

| | **references** | **values** |
|---|---|---|
| types | classes, `Str`, arrays, tuples, `Obj`, `File` / `Error` | `int`, `float`, `bool`, `char`, enums, every optional `T?` |
| passing / assigning | shares the object (ARC) | copies the value |
| changing | through any reference | only the variable's own copy |
| qualifiers | none, anywhere | on parameters only: `view x: int`, `inout x: int` |

- **References** behave exactly as Paykan does without the flag: no
  read-only references, nothing to mark.
- **Optionals are values**: a value and a flag (for a class, `Counter?`,
  just a pointer).  Copying a `Counter?` copies the pointer: the object it
  names is still shared.  `inout c: Counter?` is the caller's slot: the
  callee can set it to None, to a new value or to another object.
- **`view x: int`** is a read-only parameter: it cannot be assigned or
  passed to an `inout` parameter.  Reading it gives a copy, which may be
  stored or returned freely.
- **`inout x: int`** is the caller's storage, changed in place.  The call is
  not marked.  The argument is a changeable variable, field or array element
  of exactly the parameter's type: not a `let` local, a `view` parameter, a
  literal or another expression.  An `inout` parameter can be passed on to
  another `inout` parameter.
- `view` or `inout` on a reference type is an error (`'inout' applies only
  to value types (int, float, bool, char, enums and optionals); 'Counter'
  is a reference`); on a local, field or result it is a syntax error.  An
  override keeps each parameter's `view` / `inout`.
- **`let x = e;`** (or `let x: T = e;`) declares a local that cannot be
  reassigned.  It says nothing about changing the object it refers to.
- Under `--ownership` the keywords `view`, `inout` and `let` are reserved.

### Example

```pkn
class Counter {
  n: int;
  fn __init__() { self.n = 0; }
  fn tick() { self.n = self.n + 1; }
}

fn tickTwice(c: Counter) { c.tick(); c.tick(); }   // shared: the caller's

fn bump(inout n: int) { n = n + 1; }               // the caller's int

fn report(view n: int) -> Str {                    // read-only
  m = n;                                           // a copy: may change
  m = m * 10;
  return Str<int>(n) + " -> " + Str<int>(m);
}

fn main() -> int {
  a = Counter();
  b = a;                 // the same counter
  tickTwice(b);          // a.n == 2
  bump(a.n);             // a field: a.n == 3
  let k = a.n;           // k cannot be reassigned or passed to inout
  x = k;
  bump(x);               // x == 4
  println(report(x));    // 4 -> 40
  return 0;
}
```

## Prototype scope

- Opt-in: `paykan --ownership`.  Without it nothing changes: the keywords
  are not reserved and a qualifier or `let` that reaches Sema (from the AST
  interchange) is an error.  The frontend option (`frontend::Options`)
  enables the keywords, Sema runs the checks, and the lowering passes
  `inout` parameters by address.  Every existing test and sample is
  unchanged.
- Recursive-descent frontend only; the AST interchange carries
  `(qual view|inout)` on a parameter and `(let)` on a local.
- Not in the prototype: generics with qualifiers, qualifiers in `.pkm`
  interfaces, an exclusivity check.

## Plan (stacked PRs against `proto/ownership`, at most about 500 changed lines each)

1. Syntax: the keywords under the option, `view` / `inout` parameters,
   `let`, AST, `--ownership`.  Parser tests.
2. Printer, AST interchange, and the Sema gate (the syntax needs
   `--ownership`).
3. Sema: `view`, `inout` and `let` rules, value types (optionals
   included) only, overrides.  Sema tests.
4. PIR address ops `local.addr`, `ptr.load`, `ptr.store` (of a scalar or a
   box) on both backends.
5. Lowering: an `inout` int, float, bool, char, enum or optional parameter
   is a PIR `ptr`; a caller passes `local.addr` of a variable, an `inout`
   parameter passes its pointer on, and a field or element goes through a
   temporary written back after the call.  An optional's slot owns its box:
   a store releases the old one.  CodeGen tests on both backends.
6. Samples (`samples/ownership/`), error cases and the demo script
   (`scripts/ownership_demo.py`, the `OwnershipSamples` ctest).

## Try it

The samples in `samples/ownership/` run with `--ownership`; each says what
it shows and prints its results.  The ones in `samples/ownership/errors/`
are rejected, each with the diagnostic in its `// expect-error:` line.

```sh
cmake --build build -j16
build/bin/paykan --ownership samples/ownership/01_references.pkn
build/bin/paykan --ownership --backend=llvm --track-heap samples/ownership/02_view_inout.pkn
build/bin/paykan --ownership samples/ownership/04_optionals.pkn
build/bin/paykan --ownership --check-only samples/ownership/errors/view_to_inout.pkn
# every sample on both backends, then every error case, as a transcript
python3 scripts/ownership_demo.py --paykan build/bin/paykan --demo
```

Without `--demo` the script is the `OwnershipSamples` ctest: the expected
output, zero live heap blocks and identical output on the C and LLVM
backends, and each error case's exact diagnostic.  The regular samples
harnesses (`samples_parity.py`, `c_strict.py`) do not read
`samples/ownership`; the AST interchange round trip parses it with the
ownership keywords on.

## Status

Works, on the C and LLVM backends: references shared without qualifiers;
`view` and `inout` parameters of functions, methods and constructors
(including `__super__`) for int, float, bool, char, enums and optionals
(an `inout` optional rebinds the caller's slot, with its reference counts);
`inout` arguments from variables, `inout` parameters, fields and array
elements; `let`; the override rule.

Known gaps:

- No exclusivity check: two `inout` parameters may name the same variable
  (`swap(k, k)` is accepted).
- A field or element `inout` argument is copy-in / copy-out through a
  temporary, so the callee sees the old value if it reaches the same field
  another way (through `self`) before it returns, and the write-back then
  overwrites what it stored there.
- Qualifiers are not in `.pkm` interfaces (or a module's exported
  interface): calls into another module's functions are not checked, and a
  method of another module's class with an `inout` parameter is called as if
  it took the value, which is wrong code.  Functions of another module are
  called with their own PIR signature, so they get the address.
- Generics with qualifiers are out of scope: a qualified parameter of a
  generic is checked per instantiation.
- An optional is still a box at runtime: `int?` is not unboxed into a value
  and a flag.  A box is immutable, so value semantics hold; unboxing is a
  separate, compiler-wide change.
- The PIR version is not bumped for the new address ops (`local.addr`,
  `ptr.load`, `ptr.store`).

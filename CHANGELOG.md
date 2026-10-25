# Changelog

All notable changes to PaykanLang are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Pure virtual methods (docs/language/05-classes.md, "Pure virtual methods", #18): a method
  of a class body whose signature ends with `;`, `view fn area() -> float;`, is a vtable
  slot without a definition, which a subclass implements by overriding it. There is no
  keyword: a class with a pure virtual method, its own or inherited, that it does not
  implement is abstract, and one that implements them all is not.
  - Constructing an abstract class is an error wherever it is written, naming every missing
    implementation: `cannot instantiate abstract class 'Square': 'Shape.area' has no
    implementation`. An abstract class is otherwise an ordinary type (of variables,
    parameters, fields, arrays, optionals, type arguments), with fields, an `__init__` its
    subclasses call with `__super__(...)`, and `impl` methods.
  - A call of a pure virtual method goes through the vtable, from an `impl` method too.
    Once an abstract class's `__init__` has assigned its fields, the object is one of that
    class until the subclass's `__init__` has assigned its own (see "Changed": an object is
    one of the class whose `__init__` runs), so a call of a pure virtual method then reaches
    the trap below: `call of the pure virtual method 'Shape.kind', which has no
    implementation`.
  - Only a class body's method may leave out its body: `fn f();` elsewhere (but in a
    trait) is a syntax error (`function 'f' has no body: ...`, or in an `impl` block `'f'
    in an 'impl' block has no body: ...`, `'f' has no body: a 'static fn' needs one` and
    `'__init__' has no body: an initializer needs one`). A method its superclass
    implements cannot be made pure virtual again, and a `match` arm naming an abstract
    class is rejected, as an object is of one only while it is being constructed (`a
    'match' arm cannot name abstract class 'Shape': an object is of it only while it is
    being constructed`, docs/language/08-match.md); the arm unwrapping a `Shape?` stays
    fine.
  - Generic classes follow the same rules per instantiation (a superclass cannot be
    generic, so a generic class with a pure virtual method has no objects; a generic
    subclass can implement a non-generic class's), and modules export the pure
    bit with each method, from a source and from a `.pkm` (the method flags byte's bit 4,
    the `PURE` bit docs/design/pkm.md reserved; an importer works out abstractness itself).
    The AST interchange format writes a pure virtual method's body as `_`
    (docs/plugins/ast-format.md).
  - An abstract class has no constructor in the PIR; a pure virtual method's function, the
    target of its slot, calls the new runtime function `Paykan_panic_pure_virtual` and is
    `unreachable` (docs/pir.md §5, §8).
  - `samples/codegen/63_pure_virtual.pkn` runs a `Shape` hierarchy.
- Moves at last use (#186): a copy of a variable where it is last used is a transfer, made
  by a new backend-independent PIR pass, `moves-at-last-use` (docs/pir.md §12), on every
  program and on the code of every module written to a `.pkm`, the standard library's
  included.  Assigning a variable to another, storing it in a field or an array element,
  and returning it hand its references on without a retain, whatever it holds (a tuple, a
  struct, an enum, a function value, a fixed-size array of up to four elements or an
  optional of one included), and so does passing a variable that holds an object, a string
  or an optional of one to a function; the variable then releases nothing at the end of its
  scope.  A tuple, a struct, an enum, a function value or a fixed-size array passed to a
  function stays a copy: the function keeps one of its own.  Each path counts on its own
  (one branch may hand a variable on while another releases it), a use inside a loop the
  next iteration reads again stays a copy, and in `f(a, a)` only the second `a` moves.
  `self`, `inout` parameters, `view` parameters and variables, fields, array elements and
  the members of a value are never moved, and a lambda's captures are always copies.  A
  hand-off also stays a copy when the function goes on to use, across a call or a release,
  an object it holds no reference to (a field's value read in place, anything reached
  through `self`), since the variable may be what keeps that object alive; in `__init__`,
  whose object the constructor holds, `self` is no such object.  Over the samples and the
  standard library's tests the pass removes about a fifth of the retains and a tenth of the
  releases.  PIR marks the slot of a `view` parameter or variable, `local %v: box view`
  (docs/pir.md §3), in the text form backend plugins read and in a `.pkm`'s code, where a
  code before the local's slot type marks it, so that code written before reads the same
  (docs/design/pkm.md §5.4, §5.7).
- `paykan --stats` prints what the PIR passes did, on standard error after the last of them
  ran (`moves-at-last-use transfers`, `retains removed`, `releases removed`), for the
  program and for the modules this run builds from source (`pir::Statistics`,
  `include/paykan/pir/Pass.h`; `Pass::run` takes it).
- Reference-count elimination (#98, part 1): a new backend-independent PIR pass,
  `rc-elimination` (docs/pir.md §12; docs/language/16-memory.md, "Reference counts the
  compiler removes"), run after the moves at last use on every program and on the code of
  every module written to a `.pkm`, removes a `retain` together with the releases that end
  the reference it made where something else keeps the object alive in between: another
  reference the function holds on the same value (a match subject's around its binding's, a
  `weak.load`'s, a box parameter), a local that is not reassigned, released, handed on or
  copied meanwhile, or nothing in between that can let go of a reference (no call that may
  run code, no release, no store over a reference).  A reference read from a field or an
  array element, `stable` or not, keeps its first retain whenever a call that may run code
  comes before its release, since the callee may clear the field and free the object (the
  field-load hazard): every hold the lowering makes stays, which the lowering tests check
  for every program they lower and the corpus tests for every sample, and the retains
  nested in a hold go.  A pair is removed only when it is balanced on every way out of its
  range (`ret`, `break`, `continue`, a panic), so every object is destroyed where it was
  before: every sample and standard-library test prints what it printed without the pass,
  with the same heap statistics, at `-O0`, `-O1` and `-O2`.  Over the samples and the
  standard library's tests it removes, after the moves, 22% of the retains and 9% of the
  releases of the lowered programs (3368 to 2615, 9360 to 8535), and 19% and 7% of what
  `--emit-pir` prints of the runnable ones, whose imports come from the module cache (1387
  to 1129, 4399 to 4110).  It speeds up little on its own: in the `match` of binary trees
  the subject's retain is a hold across the call and the argument's the one an owned
  parameter takes (borrowed parameters, #98 part 2, would remove it), and what is left
  costs a few instructions with the inline retains and releases (*Changed*).  `--stats`
  counts `rc-elimination retains removed` and `rc-elimination releases removed`.
  `samples/codegen/67_reference_count_elimination.pkn` runs each shape and each hazard (a
  callee that clears the field a match read, `x.next = x.next.next`, an array element
  replaced or emptied by a call), clean under ASan, LSan and UBSan through `--emit-c`.
- Adjacent string literals are one literal, joined at compile time, as in C and C++
  (docs/grammar.md §1, docs/language/04-strings-and-conversions.md, #197): `"this message
  is far too long " "to fit on one line"` may be split across lines, with comments between
  the pieces, and makes no string at run time.  Each piece's escapes are decoded on their
  own; the joined literal is one primary expression (`"ab" "c".len()` is 3), spans every
  piece in diagnostics, and joins in a `match` pattern and a native function's symbol too.
  A character literal does not join (`"a" 'b'` stays an error).  `scripts/pkfmt.py` keeps a
  statement whose literal is split across lines as it was written.

- `::error` (docs/stdlib/error.md), a new standard library module: `ErrorCode`, `Error`
  and its `isOk` and `message`, `none`, `name` and `fromErrno`, which were `::result`'s.
  `::io`, `::memory` and `::collections` report their errors with it.
- A generic function's type arguments that its arguments leave open are taken from the
  type the call's value flows into (a declared variable, a parameter, a `return`, a field,
  an array element), matched against its result type: `s: Str? = none();` with `fn
  none<T>() -> T?`, and `return res::ok(n);` in a function returning a
  `res::Result<int, err::Error>` (docs/language/13-generics.md, "Inference").

- Tagged unions (docs/language/07-enums.md, "Variants With Fields"): an enum's
  variant may have fields, `enum Shape { Empty, Rect(w: int, h: int) }`. The
  enum is then a value held in place, like a struct: its tag, then each
  variant's fields.
  - `Shape::Rect(2, 3)` makes one, its arguments checked as a constructor's; a
    variant without fields stays `Shape::Empty`.
  - A `match` arm `r: Rect { ... }` binds the variant's fields, a copy, or with
    `view` / `inout` the subject's own. Their struct is the type
    `Shape::Rect`, so a function can take them.
  - `toString()` and `io::println` show `Rect(w: 2, h: 3)`; `==` and `!=`
    compare the variant and its fields.
  - A variant cannot hold its own enum (hold it in a class instead), a field
    is not a fixed-size array yet, and a tagged union does not cross into C.
  - Imported tagged unions work from a source and from a `.pkm`: an `ENUM`
    record lists its variants with fields, whose fields are `STRUCT` records
    (docs/design/pkm.md). The AST interchange format writes such a variant as
    `(variant NAME VAR...)` (docs/plugins/ast-format.md). The runtime adds
    `PaykanEnum_toString` and `PaykanEnum_equals` and the layout kind `ENUM`
    (docs/pir.md).
- Generic enums (docs/language/07-enums.md, "Generic Enums"): an enum takes
  type parameters, `enum Maybe<T> { Just(value: T), Nothing }`, and each
  instantiation, `Maybe<int>`, is an enum of its own.
  - A variant's type arguments are written (`Maybe<int>::Nothing`), taken from
    the type the value flows into (a declared variable, a parameter, a result,
    a field, an array element, the other side of `==`, the other arm of a
    conditional expression), or inferred from its fields (`Maybe::Just(3)`).
  - Generic functions infer through them (`fn orElse<T>(m: Maybe<T>, d: T)`);
    a bound is checked per instantiation; an instantiation that holds itself is
    reported where it is made.
  - A module's generic enums are exported with its templates, from a source
    and from a `.pkm` (its `TMPL` section). The AST interchange format writes
    their type parameters, `(enum NAME (type-params ...) ...)`, and a
    variant's type arguments, `(enum-value ... (type-args ...))`.
- Every enum is `Show`: `d.toString()` and `io::println(d)` give the variant's
  name, also for an enum in a tuple, a struct or an optional (which printed
  its ordinal before).
- `scripts/pkfmt.py` keeps a variant's fields on its line.
- Expression bodies (docs/language/03-functions.md, "Expression Bodies";
  docs/language/14-function-types.md): a function or a lambda whose body only
  returns an expression is written with `=>`, `fn double(x: int) -> int => x
  * 2;` for `fn double(x: int) -> int { return x * 2; }`, and
  `fn(x: int) -> int => x * 2` for a lambda. The parser makes the body one
  `return`.
  - `=>` is a new token. `=` stays a native function's (its C symbol), and
    after any other function's signature it is an error that names `=>`.
  - Without the result type, `fn double(x: int) => x * 2;` or
    `fn(x: int) => x * 2`, the result type is the expression's. Sema infers a
    function's before checking any other body, so it may be used before its
    declaration; a generic function's is inferred for each instantiation.
  - An expression with no value, and a function whose expression depends on
    its own result type (`fn fact(n: int) => ... fact(n - 1)`), are errors that
    ask for the result type. A method writes its result type (`fn area() ->
    int => ...`); a `static fn` infers.
  - The AST interchange format marks such a function or lambda with
    `(inferred)` (docs/plugins/ast-format.md), so a template in a `.pkm` keeps
    it.
- Untyped parameters (docs/language/03-functions.md, "Untyped Parameters"): a
  free function's parameter may leave out its type, `fn double(x) => x * 2;`,
  which the parser makes `fn double<T>(x: T) => x * 2;`. Each untyped parameter
  adds a type parameter of its own, after the written ones, so the function is
  generic and each call instantiates it for its argument types.
  - With an expression body, the result type is inferred for each
    instantiation; a block body cannot name the added types.
  - A lambda's, a method's, a `static fn`'s, a trait method's, a native
    function's and `main`'s parameters still need types.
  - The added type parameters are named `typeof(x)`, which no program can
    write, in the AST and its interchange format.
- Two parameters with the same name in a function or a method are an error
  (`duplicate parameter 'x' in function 'f'`), as they were in a lambda. They
  used to be accepted, and a program using one failed in code generation.
- Compound assignment (docs/language/02-basics.md, "Compound assignment"):
  `x += e`, `-=`, `*=`, `/=` and `%=`, on a variable, a field or an element.
  The parser builds `x = x op e`, so the rules of `=` and of the operator
  apply, and the target is evaluated twice (`xs[f()] += 1` calls `f` twice).
  `+=`, `-=`, `*=`, `/=` and `%=` are new tokens (docs/grammar.md).
- `for` loops over ranges of integers (docs/language/02-basics.md, "for"):
  `for i in a..b { }` counts from `a` to `b - 1`, `a..=b` includes `b`, and
  `step s` sets the distance between the values (a negative one counts down).
  - The range is of `int`, a sized integer or `unsigned`, one type for both
    ends: a literal takes the other end's type, and `for b: u8 in 0..=255`
    names it. The step is an `int`.
  - What the loop does is fixed when it starts: the start, end and step are
    evaluated once, in that order, and the variable is read-only in the body.
  - `for _ in 0..n` runs the body `n` times without naming the value.
  - The number of iterations is known before the first, so a loop always ends,
    even at the end of its type: `for b: u8 in 0..=255` runs 256 times.
  - A constant step of `0` is a compile error; a computed one panics when the
    loop starts (`the step of a 'for' loop is 0`).
  - `for` is a keyword (a program can no longer name anything `for`), and `..`
    and `..=` are tokens; `in` and `step` mean something only in a `for`
    header, so a variable may still be called `in` or `step`. `0..n` lexes as
    `0` `..` `n`, never as the float `0.`.
  - The AST interchange format has a `(for ...)` statement
    (docs/plugins/ast-format.md). The loop lowers to the PIR `while` loop, so
    the PIR and the backends are unchanged apart from one new runtime function,
    `Paykan_panic_for_step`.

- The `mlir` backend (docs/design/mlir-backend.md), which replaces the C and llvm backends' two
  translations of PIR with one lowering through MLIR.  It imports the program into `pir`, an
  MLIR dialect with an op for each PIR item, statement and instruction, and lowers that once
  for both of its targets: to func, scf and arith, plus the pir ops for memory, globals and
  optionals; structured control flow with `break`, `continue` and early `return` becomes
  `scf.if` / `scf.while` with an exit flag, objects get their C layouts, and every symbol a
  link name that is a C identifier.  From there the LLVM path: upstream conversions take
  func, scf, cf and arith to the LLVM dialect and a pattern each the remaining pir ops, and
  MLIR translates it to LLVM IR, which `paykan run` runs with the JIT and `paykan build`
  builds into a native object, its imported modules cached as `<module>.bc`.  And the C
  path: upstream conversions take func, scf and arith to EmitC and a pattern each the
  remaining pir ops, and MLIR's C emitter prints the program's C (`--emit-c`).
  `--emit-mlir=<stage>` prints the program at a stage of the pipeline: `pir`, `scf` (after
  the shared lowering), `llvm` or `emitc`.  The `pir-mlir` test suite imports every item and
  instruction of PIR and every sample program, checks that the MLIR verifies and parses back
  to the same text, and that the shared lowering leaves only those ops.
- A `.pkm` holds its module's generic classes, structs and functions in a `TMPL` section
  (docs/design/pkm.md §4.3), so an importer instantiates them from a prebuilt or cached module
  as from its source (#160).  Every module compiles its own copy of the instantiations of
  other modules' templates it uses, so its `.pkm` is the same in every program, and a program
  keeps one copy of each.  Modules with templates, or with such copies, are now cached; an
  entry records the templates' hash of every module it imports, so editing a generic body
  recompiles the modules that may hold a copy of it.  `--emit-pkm --module-name=<name>`
  names the module the file records.
- PIR passes (docs/pir.md §12): a pass manager, and a dead-code pass that erases what no root
  of a program reaches, following calls, symbol operands, the classes instructions name,
  vtables and the declarations between modules.  `paykan` runs it on every program with
  `main` as the root before the backend sees it: an executable, and what `run` compiles,
  hold only the code the program uses, and `--emit-pir`, `--emit-llvm`, `--emit-c` and
  `--emit-mlir` print that.  A module's cached LLVM IR is keyed on the part of it the
  program uses.
- A `.pkm` keeps only the code its module and its importers can reach, and compiles most of
  it (docs/design/pkm.md §5.13): the module's own code stays, with its copies of the
  instantiations its interface exposes, and its other copies only as far as its own code
  uses them.  The copies stay PIR (marked `shared`, docs/pir.md §3), which every program
  shares and prunes; the rest is compiled by the backend (LLVM bitcode, for the mlir
  backend) into a new `OBJ` section, which the module's PIR declares, each function with the
  names its code uses.  A program links only the part of that code it uses.  A cache entry
  whose code another backend or another LLVM compiled is rebuilt.  The standard library
  ships compiled.
- `paykan pkm extract --object <file.pkm>` writes a module's compiled code as a native
  object, which a program's `--emit-c` C is linked with; `paykan pkm dump --section=obj`
  shows who compiled it.  `paykan --version` names the standard library's directory.
- Backends may compile modules (the `CompileModules` capability: `compileModule`,
  `objectKind`, `emitModuleObject`, and `Input::Compiled`, docs/writing-a-backend.md).

### Changed

- **Breaking:** the runtime ABI and PIR are 0.3.0's (the version is `0.3.0-dev`,
  `PAYKAN_RUNTIME_ABI_VERSION` 300), for borrowed box parameters (#98): a function that
  takes a reference, a `box` parameter, now also takes an ownership mask after its
  parameters, a `uint64_t` whose bit k says whether the caller hands its k-th box argument
  over (the callee releases it, keeps it or hands it on, as before) or lends it, keeping
  the object alive until the call returns (docs/pir.md §3; Runtime.h, "The ownership
  mask").  The 65th box parameter and those after it have no bit and are always handed
  over.  So does every vtable slot and function value that takes a box; the runtime's
  own functions and a native function's C function take none.  The runtime's `equals`
  are `int64_t PaykanObject_equals(PaykanObject *self, PaykanShared *other, uint64_t
  owned)` (and `PaykanString_equals`, `PaykanFile_equals`), which release `other` only
  when bit 0 is set, and their PIR externs spell the mask out (`(obj, box, i64) -> i64`).
  PIR writes a call's lent arguments and the ones a function hands on as its caller gave
  them (`call @f(%a, lent %b, owned(%o) %c)`, in a `vcall` and an `icall` too), reads a
  parameter's bit (`%o = owned %p`) and marks a copy the lowering makes only to hand an
  argument over (`retain lend %v`, a plain retain to every pass but the one that will
  lend it); in binary PIR they are the opcode 54 and the marks 55 and 56 (docs/pir.md §6,
  §11; docs/design/pkm.md §5.5), and a vtable's target is checked against its slot as a
  machine function.  An out-of-tree backend must give every lending function, slot and
  function value its mask, pass each call's from its arguments' ownership and read `owned`
  from it (docs/writing-a-backend.md); C code calling a runtime `equals` passes a mask;
  `.pkm` files, cache entries and plugins of 0.2.0 are not accepted.  Each body the
  lowering makes of a function that takes a box starts with a prologue that retains each
  parameter its caller lent (docs/pir.md §3), so it owns it either way.  The lowering
  marks the copies a callee could be lent instead (a variable's, or a field's no call in
  use may let go of, docs/pir.md §7), and a new pass, `lending`, lends on both sides of a
  call (docs/pir.md §12).  It lends each marked copy whose object the caller keeps alive
  through the call: the copy's retain goes and the argument is `lent`, unless a statement
  between the read and the call, or one in an `&&`, `||`, `?:` or `match` among them, may
  let the object go, as in `f(x, x)`, `f(x.name, x)` or `f(x.name, c && g(x))` at `x`'s
  last use.  And it lends each box parameter its body never writes: the prologue's
  retain goes, and the body releases the parameter only when it was handed over (`if
  %p.own { release %v }`), hands it on with its own bit (`owned(%p.own) %v`: a chain
  `push -> pushFront -> Node(v) -> __init__` retains `v` once, at the field store, and
  only when the outermost caller lent it), and retains it where it keeps it (in a field,
  a variable or memory, as its result, for a callee without its bit) only when it was
  lent.  A parameter the body assigns or gives to an `inout` parameter, or uses in a way
  the pass does not follow, keeps its prologue (`--stats`: `lending marks`, `lending
  arguments lent`, `lending copies kept for the window`, `lending parameters lent`,
  `lending parameters handed on`, `lending parameters owned on entry`, `lending retains
  at keeps`).  The reference-count elimination (#98 part 1) then lends a variable at its
  last use where another reference the function holds keeps the object alive through
  the call (docs/pir.md §12, "Moves lent"): a `match` binding passed on in its arm, which
  the matched value holds, or a copy of a variable read again (`u = t; count(u)`).  The
  copy's retain goes, and with it the release the callee made of what it was handed,
  which was never the last (`--stats`: `rc-elimination moves lent`).  And a native
  function's IFACE record says it is one (a flags byte after its result mode,
  docs/design/pkm.md §3.5; `pkm dump` prints `native func`), so an importer knows that a
  call of it changes nothing but its `inout` arguments, and lends it a field read as the
  module's own calls do.  In the 122 programs of the corpus that compile (samples, leak
  checks, standard library tests, imports) the lending pass lends 296 of the 297 marks
  (keeping one for the window, `both(t, t)`) and 727 parameters (165 of them handed on),
  with 128 retains at keeps, and leaves one parameter owned from its entry (sample 66's
  `use(p)`, which passes `p` to an `inout` parameter); the reference-count elimination
  removes 251 retains and 276 releases, one of each more than the 250 and 275 it removed
  before (in `tests/Stdlib/box_test.pkn`, `alias = total`'s, now that `Counter(total)`
  is lent `total` instead of handed a copy of it), and lends 2 moves (samples 49 and 54),
  whose retains go too (`--stats`: `rc-elimination retains removed` 253).  No program of
  the corpus gives a field read to another module's native function (the print family's
  were lent already, `println<Str>` being given nothing that reaches an object), so the
  flag changes nothing there.  And an `__init__`'s first assignment of a field releases
  no old value (docs/pir.md §7): the object is new, and nothing else reaches its fields
  while `self` is only the place they are assigned in or the values assigned are
  variables, so every assignment of a default `__init__` is one, and so are the leading
  ones of most written ones (141 releases in the corpus, 25 of them `weak.release`s).
  The keeps had made such an `__init__` too large for the backend to inline into its
  constructor, which then paid a call and a test of each old value where it had paid
  neither (binary trees' `Node.__init__` with gcc, a stack's or queue's
  `_Node<Str>.__init__` with LLVM).  So the retains in `--emit-pir` fall from PR #340's
  1385 to 1232 (−11%: 1089 copies, 142 keeps, one prologue) and, after the
  reference-count elimination, from 1135 to 979 (−14%); the releases from 4407 to 4291
  and from 4132 to 4015 (−3%), 672 of them asking a parameter's bit.  A program makes fewer
  retains and as many fewer releases when it runs (in the probes' own code, `show(a);
  pass(a); keep(h, a)` makes 1 retain and 8 releases instead of 7 and 14, a chain of
  hand-offs 0 and 40 instead of 10 and 50, the writes of `w1_written` 12 and 110 instead
  of 44 and 143; binary trees' count of a tree costs each child the matched value's
  retain and release, 2 operations where it cost 4: 4094 retains and 8190 releases
  instead of 6140 and 10236 at depth 10, 4.19 and 8.39 million instead of 6.29 and 10.49
  million at depth 20), and every object dies where it did: the corpus prints and frees
  memory as before on every backend at `-O0`, `-O1` and `-O2`, and the moves at last use
  make the same moves.  The generated code of a function that neither takes a box nor
  inlines one that does is unchanged: fib's, and the dispatch benchmark's `area`
  methods, constructors and destructors (its `main` passes `List<Shape>.push` the mask,
  and LLVM now inlines that push into it).  At `-O2`, against the part 1 and 4 build,
  callgrind counts the same instructions for fib, 0.2% fewer for dispatch, 1.8% fewer
  for binary trees, 0.5 to 3.0% fewer for the hash map and set loops, 1.3 to 6.9% fewer
  for the stacks and queues of ints and strings, and as many for the priority queues.
  The C path (`--emit-c`, gcc `-O2`) counts as many for fib, dispatch and a virtual
  call loop, and 1.1% more for binary trees, whose constructor tests the two bits of the
  mask `make` passes it.  (While an `__init__`'s first assignments still released the
  old values, binary trees' C counted 6.7% more, and a stack and a queue of strings 4.9%
  and 1.1% more.)  CPU times (median of 5 runs, pinned to one CPU) moved by −15% to +9%
  on the container loops and by −11% to +8% on binary trees, dispatch and fib, within
  what repeated runs differ by on this machine (fib's LLVM build, whose code is the
  same, measured −8%; binary trees', which cachegrind finds making fewer instructions and
  data accesses and the same cache misses, +5%).
  Per-parameter conventions in the interface (`Sig.conv`, `nConventions`) are retired,
  always 0 (docs/design/pkm.md §3.5, §5.9).
- **Breaking:** an `-> inout T` result can only point into storage its caller keeps alive
  while the result is used (docs/language/03-functions.md, "Where an `inout` result can
  point"): into `self` (a field of a class's `self`, or a field of a struct or an element
  of a tuple held in it; a struct's `self` counts as an `inout` parameter), or into what an
  `inout` parameter names and the object a reference held there refers to (`return n.k;`
  with `n: inout Node`, `return s.b.k;` with `s: inout S`, `return a[5].k;` with
  `a: inout B[6]`).  One in a separate object, reached through a field of `self` or past
  the object a parameter refers to, wrote to freed memory when another reference replaced
  that field while the result was in use, and is now an error:
  `fn e() -> inout Str => self.c.k;` is `'e' returns an 'inout' reference into 'self.c',
  a separate object: an 'inout' result can only point into 'self' or into an 'inout'
  parameter's storage; return a copy, or call an accessor on 'self.c' directly`.  So are
  `self.c.p.x`, `self.arr[0].k` with object elements, `n.c.k`, an object of the array an
  `inout` slice reaches (`a[i].k` with `a: inout B[]`, which also wrote to freed memory),
  and another call's `inout` result that can point into one (`return self.c.slot();`,
  `return pick(self.c);`).  A call's result counts as pointing wherever its signature
  lets it, whatever its body returns: past its receiver and `inout` arguments, into an
  object held there that can hold a place of the result's type (a field, or a method's
  `inout` result, of that type or holding it).  So `return self.s.num();` with
  `num() -> inout int` is an error when the struct `self.s` holds an object with an `int`
  field, and `return self.s.n;` is not; `return self.s.ref();` with `ref() -> inout C` and
  `return s.ref().k;` with `s: inout S` compile when a `C` holds no `C`, as before.  A
  reference followed from a call's result is named by the call (`'f' returns an 'inout'
  reference into a separate object, the one referred to by the 'inout' result of 'deep',
  which can point into 'n': ...`).  Return a copy, or call the object's own
  accessor where the result is used (`h.c.slot()`), whose receiver the caller then keeps
  alive.  The rule is checked where a function is defined, an override and an
  `impl ... for` method too, and in each instantiation of a generic one.  `-> view T`
  results are unchanged: their caller holds what they name.
- A read of storage a function never hands on (a method's `self`, an `inout`, `view` or
  slice parameter's) takes a reference of its own across a call only when the call may let
  go of a reference of the read's class: one the code it runs stores into, or one that
  letting go of those may let go of in turn, as an object of an exact class (`Str`, an
  instance of a generic class) holds its fields' (docs/pir.md §7, "Reads a call may
  replace").  The main module of a program (one that is run or built, not one `--emit-pkm`
  writes) knows the classes derived from its own, as no module can import it to derive more:
  a virtual call on one of its classes does what the implementations it may run do
  (`shapes[i].area()` of a `view Shape[]`, the `equals` of a map's `Point` keys), and an
  object of one that dies lets go of what their fields hold (a `Dog` holding a string).  A
  class of another module, and a method it compiles, stay unknown.  A field no code stores
  into once its object is made (only `self.f = v` in an `__init__`, where `self` is always
  the new object (*Fixed*), and no assignment, `inout` argument, local or `match` arm,
  `-> inout` result or lambda anywhere else) keeps what its `__init__` gave it, so a read of
  one of `self`, at any depth, needs no reference of its own across any call, one of a
  method another module compiles too (`self.origin.sum()`): in a program's main module for
  every field of its own classes, elsewhere for a private one.  The standard library's
  containers no longer retain and release the object holding their state around their own
  methods over numbers, strings, a program's own classes and the like (`Stack<int>.push`,
  `HashMap<Str, int>.put`, `HashSet<int>.clear`, `MinPriorityQueue<Str>.pop`,
  `HashMap<Str, Dog>.put`): 77 of the 86 retain/release pairs the fix for reads through
  objects added (*Fixed*) are gone, and over the samples and the standard library's tests
  the static retains go from 1421 to 1344 (releases 4355 to 4278), with no other change to
  the code.  Built with `-O2`, loops of millions of operations on them run up to about 13%
  faster (`Stack<int>`, `Queue<int>`; maps and priority queues within a few percent), and a
  loop of virtual calls on `self`'s fields and a `view Shape[]` about twice as fast.  Such a
  read is a `stable` load in PIR (`%b = field.load %o, C.f stable`,
  `ptr.load box, %p stable`; docs/pir.md §6), which the moves at last use count as held by
  its slot: the arguments the hold let them hand on are still handed on (`Stack<Str>.push`
  gives its string to the list with no retain), so every object dies when it did with the
  hold.  A `.pkm`'s code marks a stable load with a code before its opcode, so code written
  before reads as it did (docs/design/pkm.md §5.5).  A read rooted in a variable, which the
  function may hand on, keeps its reference across any call that may let go of one, so the
  moves at last use there stay as they were.
- A retain and a release are inline in the generated code (#98, part 4), no longer a call
  of the runtime: a null box is skipped, a retain below the limit adds one to the count in
  the object's header and a release that leaves the count above zero subtracts one, and
  only a retain at the limit (which panics) and a release that destroys the object call
  `Paykan_retain` or `Paykan_release`, so each does what the call did, at every count.  The
  LLVM path emits that code for every call at every `-O`, in the standard library's
  compiled code too (`src/Backends/MLIR/ToLLVM/ToLLVM.cpp`); the C of `--emit-c` calls
  Runtime.h's new `static inline` `Paykan_retain_inline` and `Paykan_release_inline`
  (strict C11), and `PaykanShared_get_inline` joins them for native code (`unbox` already
  was the identity on both paths, never a call).  The runtime calls them everywhere but in
  the file that defines the functions: a container's copies and drops, the argument an
  `equals` consumes, printing.  The functions stay the ABI, for the JIT's symbol table,
  code compiled before and native code.  With the reference-count elimination (*Added*),
  built with `-O2` against a Release-built runtime, in CPU time (the median of 5 runs
  pinned to one CPU, in three rounds) and in instructions executed: binary trees (4 trees
  of depth 22, 33.5M nodes, counted through the `match` of #98) takes 8 to 14% less time on
  the LLVM path and 9 to 12% on the C path, and 12.7% and 11.9% fewer instructions, all of
  it from the inline paths (the same C with them turned back into calls executes the
  instructions it did before); 60 rounds of `List.get` and a virtual call over 1M objects
  of three classes 9 to 21% less time on the LLVM path and 7% and 12% fewer instructions
  (LLVM, C); loops of millions of operations on the standard library's containers 10 to 41%
  less time (`Stack<int>` 34 to 40%, `Queue<Str>` 32 to 34%, `HashMap<int, int>` 26 to 38%,
  `HashSet<Str>` 19 to 29%, `MaxPriorityQueue<Str>` 14 to 16%, `MinPriorityQueue<int>` 10
  to 15%; `Stack<int>` 37% fewer instructions); and `fib(40)`, which counts no references,
  is the same machine code and the same instructions.
- An object handed on at a variable's last use lives as long as its destination holds it,
  no longer until the variable's scope ends (docs/language/16-memory.md, "Ownership
  transfers").  Nothing is ever released earlier than its last holder lets it go, and
  programs free the same objects, but an object may be destroyed sooner than before: a
  `weak` reference made elsewhere reads `None` as soon as, say, the function it was passed
  to returns, and an `io::File` passed to a function at its variable's last use is closed,
  what was written to it on disk, when that function returns.
- While a class's `__init__` runs, the object is one of that class, as in C++, once every
  field of the class's body has a value (docs/language/05-classes.md, "Every field before
  `self`"): a superclass's `__init__` leaves the object one of the superclass, and the
  subclass's `__init__` makes it one of its own class where it assigns the last of its own
  fields (right after `__super__(...)` when it has none to assign).  So a virtual method a
  superclass's `__init__` calls is the superclass's, and a pure virtual one its trap; and so
  is one called, or a `match` made, through a reference to the object the superclass's
  `__init__` gave away, until the subclass's fields have values.  Before, the constructor
  made the object one of its final class before any `__init__` ran, as in Java: a field
  has no default value, so an override a superclass's `__init__` reached could read a field
  of the subclass before the subclass's `__init__` assigned it, and crash the program.
  PIR has the instruction for it, `vtable.store %self, C` (opcode 52, docs/pir.md §5,
  docs/design/pkm.md §5.5), and since an object's vtable pointer now changes while it is
  constructed, the LLVM IR no longer marks a load of it `!invariant.load`.
- An array or tuple literal holds its elements, so a `view` that shares what it holds (of an
  object, a string or an array, a tuple or optional holding one, `self` in a `view fn`) can
  no longer be one of them: `[v]`, `(v, 1)`, `[v; 3]` and `return [self.name, "x"];` in a
  `view fn` are `'v' is a 'view' parameter; it cannot be stored, only read or passed to a
  'view' parameter`, as storing `v` is (docs/language/03-functions.md).  A literal copied
  the reference, through which the viewed object could be changed, stored or returned.  A
  `view` of a value type is still copied into a literal.
- A `match` arm's `n: inout T` binding is the matched value, narrowed to `T`, which goes back
  to the subject after its last use in the arm (or on a `return`, `break` or `continue`
  out of the arm before it), no longer a pointer into the subject's storage
  (docs/language/08-match.md, "Borrowing the subject").  Until then the subject is still
  used only through `n`, so a program sees the same changes; what another reference to the
  subject's object writes there meanwhile no longer changes what `n` holds, and `n`'s value
  replaces it at that last use.
- `::result`'s `Result` is a generic tagged union, `enum Result<T, E> { Ok(value: T),
  Err(error: E) }` (docs/stdlib/result.md), no longer the struct `Result<T>` of an `error`
  and an optional `value`, and its errors are `::error`'s.  The standard library returns a
  `res::Result<T, err::Error>`: `io::open` (`res::Result<io::File, err::Error>`), the
  `File` reads (`res::Result<Str, err::Error>`) and `memory::Buffer.copy`; what makes no
  value returns an `err::Error`.  A program that read a result changes from
  `match r.value { f: io::File { ... } None { ... r.error ... } }` to
  `match r { f: Ok { ... f.value ... } e: Err { ... e.error ... } }`, and spells the error
  types and functions `err::Error`, `err::ErrorCode`, `err::none()`, `err::name(code)`,
  `err::fromErrno(n)` (`import ::error as err;`).  `Result.isOk()` is gone (match the
  result); `res::ok` is `ok<T, E>(value)`, with `E` from the type the result flows into,
  and the new `res::fail<T>(error)` makes the `Result` of an operation that failed with an
  `err::Error` (`::result` imports `::error`).
  A `Result<Str?, err::Error>` now tells a success that found `None` from a failure.
- PaykanLang is licensed under the Apache License, Version 2.0 (`LICENSE`), no
  longer the MIT License: every source file's `SPDX-License-Identifier` is
  `Apache-2.0`, and the copyright notice is in `NOTICE`, which the Debian
  package installs with the license.
- The standard library's containers hand out an element two ways (docs/stdlib/collections.md).
  `view fn`s lend it as a `view`, which stays the container's: `List`'s `at` and `last`,
  `LinkedList`'s `front` and `back`, the `peek`s, and `HashMap`'s `at` and `atOr`.  The new
  `get`s hand over a copy the caller keeps (`-> T`): `List.get(i)`, `HashMap.get(k)` and
  `getOr(k, otherwise)`, `LinkedList.getFront()` and `getBack()`, and `get()` on `Stack`,
  `Queue` and the priority queues.  A copy of an object shares it with the container, so the
  `get`s are not `view fn`s, and a `view` of a container reads with `at` (`'xs' is a 'view'
  parameter; 'get' is not a 'view fn'`).  `List.get` and `HashMap.get` / `getOr` were the
  lending reads; code that reads through a `view` of the container, or binds the result to a
  `view` local, now calls `at` / `atOr`.  `memory::Buffer<T>` follows suit: `at(i)` lends a
  slot, and `get(i)` is the copy that was `copyUnchecked(i)`.
- `--emit-c` prints the C of the program's PIR.  A module loaded from a module file with
  compiled code (the standard library's, an import's cache entry) is linked as an object
  (`paykan pkm extract --object`), and the C keeps everything that code uses
  (docs/c-backend.md).  A loaded backend plugin gets such a module's code declared, not
  defined (docs/plugins/pir-for-backends.md).

- The standard library is installed as `.pkm` files only: each module is compiled, as
  `::<name>`, against the `.pkm` files of the modules it imports, and a program loads all of
  them from those files instead of compiling the generic ones from their sources every time.

- A `match` on an enum, or on integer, `char` or `bool` literals, is a `switch` statement in
  PIR (docs/pir.md §6), a case per value and the wildcard its default, instead of an if-else
  chain; one on floats, classes, optionals or strings stays a chain.  Both targets switch on
  it: the LLVM IR at every optimisation level (`switch i64`), and `--emit-c`'s C (`switch`).
- LLVM is 22.1.8 (was 17.0.6, for the llvm backend), whose prebuilt release includes MLIR.
  Only what PaykanLang uses is extracted (headers, the LLVM and MLIR libraries and the tools
  the build and the scripts run), about 2 GB of the archive's 12.  The prebuilt LLVM links
  with zlib and zstd from the system (`zlib1g-dev libzstd-dev` on Debian and Ubuntu, `brew
  install zstd` on macOS); a configure without them says so.  The vendored clang-format and
  clang-tidy are LLVM 22's too.
- `mlir` is the only backend built in this repository, always built and the default:
  `paykan --list-backends` prints `mlir (default)` and the configure `Backends: mlir
  (default: mlir)`.  `paykan run` runs a program in process with LLVM's ORC JIT, with no C
  compiler involved; `paykan build` emits a native object and links it against
  `libpaykan_runtime.a` with the system C compiler.  Other backends are plugins
  (docs/writing-a-backend.md).
- `--emit-c` prints the mlir backend's C, made with MLIR's EmitC: the whole program as one
  strictly conforming ISO C11 translation unit (docs/c-backend.md, now "The C output").  The
  CStrictC11 test (scripts/c_strict.py) compiles it for every runnable sample with
  `-std=c11 -pedantic-errors -Wall -Wextra -Werror` on GCC and Clang, at `-O0` and `-O2`,
  and runs it like `paykan run`.  Vtables are arrays of Runtime.h's `PaykanMethod`: an
  object's vtable pointer is read as a `PaykanMethod *` and a slot as a `PaykanMethod`,
  converted to the method's type at the call through `uintptr_t` (#62).  `--emit-llvm` is
  the mlir backend's LLVM IR (its `--emit-source`); `--emit-llvm`, `--emit-c` and
  `--emit-mlir=<stage>` need the mlir backend and exclude each other.
- A default configure needs LLVM 22.1.8 with MLIR: it uses `-DLLVM_DIR=<llvm>/lib/cmake/llvm`
  when given, and otherwise downloads the prebuilt release once into
  `build/third-party/llvm`.  So the core build is no longer standard C++ only, and a plain
  configure is no longer one that downloads nothing (#16, #77, #123): it downloads LLVM and
  nothing else.  The tests stay off by default, and GoogleTest is fetched only when they are
  enabled.  The DefaultConfigure test configures with no option but the enclosing build's
  `LLVM_DIR` and checks that the defaults are recursive-descent + mlir, with nothing
  downloaded.
- The bitcode module cache (`.paykan_cache/<module>.bc`) is the only backend cache.
- The release tarballs and the Debian package ship the defaults, the recursive-descent
  frontend and the mlir backend, with LLVM linked in statically.  Homebrew installs the
  release's prebuilt tarballs (macOS ARM64, Linux x86-64 and ARM64) instead of building from
  source ([parsabee/homebrew-paykanlang#2](https://github.com/parsabee/homebrew-paykanlang/pull/2)).
- CI: the "Barebones | Linux x86-64" job, which built without LLVM, is replaced by "Install |
  Linux x86-64": a plain configure, installed, with plugins loaded into the installed
  `paykan`.  Every job builds the defaults.
- CI: a pull request that changes the standard library's sources (`stdlib/*.pkn`) builds
  and runs every suite whose programs import the copy the build stages: `sema`, `lowering`,
  `pir-mlir` and the end-to-end group (`codegen`, `driver`, `plugin`, `samples`, `stdlib`,
  `c-strict`, `docs`, `out-of-tree`), where it ran only `stdlib`; one that changes
  `stdlib/CMakeLists.txt`, which stages it, runs every suite (tests/suites.json,
  `scripts/affected_tests.py`).  The `TestSelection` test (`configure` suite,
  `scripts/test_selection.py`) checks both picks.
- Tests: the standard library's tests (`tests/Stdlib`, `scripts/stdlib_tests.py`) run on
  every path at -O0 and -O2, where they ran only with `paykan run` at -O2: `paykan run`
  (`StdlibTests`), `paykan build` (`StdlibTestsBuild`, `--build`) and the C `--emit-c`
  prints, compiled with the runtime's sources (`StdlibTestsC`, `--cc <compiler>`).  The
  samples corpus is built at -O0 too (`SamplesParityBuildO0`).

### Removed

- The `c` backend: the C emitter and its build (src/Backends/C: CEmitter, CBackend, CBuild,
  CNames; include/paykan/backends/c), the `paykan_c_build` library and the C object cache
  (`.paykan_cache/<module>.<hash>.{c,o,key}`).  `--emit-c` is the mlir backend's.
- The `llvm` backend: src/Backends/LLVM/LLVMBackend.cpp and PIRToLLVM.cpp.
  src/Backends/LLVM/JIT and src/Backends/LLVM/CodeGen stay, as the mlir backend's JIT and
  native code generation.
- `--backend=c` and `--backend=llvm` (unknown backends now).  In `PAYKAN_BACKENDS` the names
  `c` and `llvm` are still accepted, with a warning, and build `mlir`.

### Fixed

- A value read from a variable could be freed while still in use when a call later in the
  same statement replaced the variable, through an `inout` parameter or a struct's method
  that may change `self`: `s + repl(s)` with `fn repl(x: inout Str)` joined freed memory,
  and `l.greet(repl(l))` or `s.n.greet(s.reset())` ran a method on a freed object.  Such a
  read now takes a reference of its own, released once it is used (docs/pir.md §7, "Reads
  a call may replace"), so it is the value the variable held before the call
  (docs/language/03-functions.md, "Exclusivity").  The moves at last use rely on it: a
  function handed a variable's last reference sees what a temporary would give it.
- A value read through an object could be freed while still in use when a call made before
  its use replaced the field it was in, or the field holding the object it was in: a
  method's receiver (`h.b.m(h)`, `self.child.detach(self)`, or a method replacing its own
  receiver through what it reaches or a closure it calls), an operand of `+` or `==`
  (`h.b.s + g(h)`), the object a field assignment writes to (`h.b.k = g(h)`, `h.b.s +=
  g(h)`), an array in a field while a call computes its index or the element stored in it
  (`h.b.a[g(h)]`, `h.b.a[1] = g(h)`), a value held in a field and passed by address
  (`f(h.a.xs, h)`, `h.a.s.take(h)`, an `inout` argument in a struct's field), a call's
  `inout` result, an `inout` local or a `match` arm's `n: inout T` of such storage, and
  what an `inout` parameter, a slice or a struct method's `self` names.  Such a read now
  takes a reference of its own until it is used, and storage used where it is keeps the
  object it is in alive, wherever a call that may replace it is made in between.  A call
  given nothing that reaches an object replaces nothing, and neither does one of a function
  the module compiles whose body stores no reference outside its own variables and calls
  nothing that may (a method returning a field, `len()` or `isEmpty()` of a stdlib
  container, a map's lookup of a string key): the reads around it stay in place
  (docs/pir.md §7, "Reads a call may replace"; docs/language/03-functions.md,
  "Exclusivity").  The moves at last use made this more likely: after `take(a)` handed
  `a`'s last reference on, `use(h)`, reading `h.node.name + replace(h)`, joined freed
  memory (`took !!`); it prints `took first1!`.
- An `inout` result in the object an `inout` argument holds was not kept alive while it was
  used: with `fn pick(b: inout B) -> inout Str { return b.k; }`, `x: inout = pick(h.b);` or
  `put(pick(h.b), h)` wrote to freed memory once another reference to `h` replaced `h.b`.
  The caller now holds what each `inout` argument holds when the call returns, the object
  of a class-typed one and those in a struct, a tuple or an array passed `inout` (and in a
  struct method's receiver), for as long as the result is used (docs/pir.md §7, "Reads a
  call may replace").  An `inout` result in an object the function reaches through
  another object's field (`return self.c.k;`), which its caller cannot hold, is now a
  compile error (see *Changed*).
- An `inout` result could not be another method's result in `self`: `return self.key();`
  in a class's method was `'self' cannot be returned 'inout': the method would no longer
  know its object`, though only part of `self` is returned.  The receiver of a call whose
  `inout` result is returned is not itself returned, so this is allowed.
- `s[i]` computed the index before reading the string, against the left-to-right order of
  operands (docs/language/03-functions.md, "Exclusivity"): a call in the index that
  replaced the string (`t[bump(t)]` with `fn bump(s: inout Str)`, or `h.b.s[g(h)]` with
  `g` assigning `h.b`) changed the character read.  The string is now read first, and keeps
  a reference of its own while the index is computed when that may replace it.
- `Str(s)` of a field handed on (`return Str(self.v);`, `f(Str(self.v))`) passed the field's
  own reference as if it were a new one, and the release that ended it freed the string the
  object still held (`heap corruption or invalid free`); it is retained, as `self.v` is.
- An `__init__` could read a field before assigning it, or use `self` before every field was
  assigned, and the program crashed (a `Str` or an object field read as a null reference) or
  read `0`: `io::println(self.name); self.name = "x";`, a method called on `self`, `self`
  passed to a function or captured by a lambda, in a class's or a struct's `__init__`.
  Until every field of the class's own body is assigned on every path (an optional one starts
  as `None`), and in a subclass after `__super__(...)`, `self` is now only a place to assign
  fields and to read the ones already assigned (`self.f(1)` with `f` a field of function
  type reads `f`; a write in a field's own storage, `self.a[0] = 5` with `a` a fixed-size
  array or `self.p.x = 1` with `p` a struct, reads nothing and assigns nothing): `field
  'name' is read before it is assigned`, `'self' is used before every field is assigned:
  'name' is not assigned yet`, and in `__super__(...)`'s arguments `'self' is used before
  every field is assigned: '__super__' has not run yet` (docs/language/05-classes.md,
  "Every field before `self`").
  What a static check cannot see, a superclass's `__init__` calling a method a subclass
  overrides, or giving `self` away to code the subclass's `__init__` then calls, is safe by
  the change to the object's class while an `__init__` runs (see "Changed").
- `void`, the result type of a function that returns nothing, passed type checking as the
  type of a value and then failed in the lowering (an assertion) or in PIR verification: a
  variable (`x = g();`, `x: void = g();`, `v: view = g();`), a parameter of a function or a
  method, a class's field, a type argument (`id(g())`, `Box<void>`) and `return g();` in a
  function that returns nothing.  Each is an error now: `variable 'x' cannot have type 'void'
  (the expression produces no value)`, `parameter 'v' cannot have type 'void'`, `field 'v'
  in class 'C' cannot have type 'void'`, `type argument 1 of 'id' cannot be 'void': it has
  no value (it is a function's result type only)` and `return value has type 'void' (the
  expression produces no value): make it a statement, then 'return;'`
  (docs/language/02-basics.md, "Builtin Types").  So are a `match` on one (`match subject
  has type 'void' (the expression produces no value)`), which was accepted, and an array
  of them (`array element 0 has type 'void' (the expression produces no value)`, and for
  a declared `void[2]` or `void[]`, `variable 'y' has type 'void[2]': an array element
  cannot have type 'void'`), which was "not supported yet", and a trait method's `void`
  parameter, which no class could implement (`parameter 'v' of method 'm' of trait 'Tr'
  cannot have type 'void'`).
- A `match` arm's `n: inout T` on a tagged union's variant, an optional's value or a
  subclass could be left pointing at what another reference to the subject's object put
  there (`g.s = Sh::E;` with `g = h` in an arm of `match h.s`, a parameter that is `self`, or
  a variable `h` passed with `h.s` to the function that matches it), and reading `n` then
  crashed or read the object as the wrong class.  The binding is a value of its own now
  (see "Changed").
- An `inout` local or a `match` arm's `n: inout T` naming a field of a struct held in an
  object (`x: inout = o.h.p.s;`, `match o.h.p.s { n: inout L { ... } }`, with `p` a struct)
  did not keep that object (`o.h`) alive, as it does for a field of the object itself: when
  another reference replaced it (`g.h = H(...)` with `g = o`), a write to `x`, or the
  arm's binding going back, wrote freed memory.  The object is kept while the local or the
  binding can be used.
- A `match` with an `n: inout T` arm evaluated its subject twice, for the matched value and
  again for the storage the binding goes back to, so a call on the way to it
  (`b.slot().s`, with `slot` returning `inout`, or `mk().s`) ran twice, and when the
  second call named other storage, the binding went back there and not where its value
  came from.  The subject's storage is found once, when the `match` starts, and the value
  is read there (docs/language/08-match.md, "Borrowing the subject").
- A local borrow's liveness (docs/language/02-basics.md, "Local borrows"): a lambda or a call
  through a function value (`fs[0](5)`) after a borrow's last use kept every borrow of the
  block live, so a valid use of the borrowed variable was rejected; a call through a
  function-valued borrow (`v(3)` with `v: view = f;`) did not count as a use of it, so `f`
  could be reassigned while `v` was live; and a lambda could capture the borrowed variable
  while the borrow was live, reading what an `inout` local borrows or changing what a
  `view` local views.  A lambda and a call through a function value use what they name
  only, a call through a borrow uses it, and a capture of a borrowed variable is an error:
  `'k' is borrowed by 'inout' local 'x' until 'x' is last used; a function cannot capture
  it`, `'h' is viewed by 'view' local 'v' until 'v' is last used; a function cannot capture
  it (it could change it through its copy)`.
- A struct or a tagged union holding itself in two places (`struct S { a: S; b: S; }`, a
  field `(S, S)`, `enum Expr { Num(v: int), Add(l: Expr, r: Expr) }`) was reported twice and
  then stopped the compiler (`the same diagnostic was reported twice`); it is reported once
  per declaration, and an enum once per variant that holds it, also through an
  instantiation of a generic struct (`struct S { a: Box<S>; b: Box<S>; }`).
- `__init__` with a result type (`fn __init__() -> Str { ...; return ...; }`) was accepted
  and every construction leaked what it returned; it is an error, `'__init__' cannot have a
  result type: it sets up 'self' and returns nothing`.
- A lambda's captured struct, tuple or tagged union holding a reference was treated as a
  `view`: it could not be returned, stored in a local or passed to a by-value parameter (`'p'
  is captured by copy by the function; it cannot be returned`), while a captured `Str` or
  object could, so `fn constant<T>(x: T) -> () -> T { return fn() -> T { return x; }; }`
  failed for such a `T`.  A captured name is read-only, not a `view`: read, it is a value
  like any other (docs/language/14-function-types.md, "Captures").
- A generic body's `match` arm naming a type parameter on a `T?` (`v: T` in `orElse<T>(x: T?,
  d: T)`) could not be instantiated with an optional `T` (`match arm type 'int?' cannot be
  optional`); it matches what `T` wraps, the present case, and binds it as a `T`, so the
  template can compare it with another `T` or, through `v: inout T`, assign it one
  (docs/language/13-generics.md).
- A function whose only way out of a `while True` loop is `return` was rejected as not
  always returning; such a loop with no `break` out of it never falls through
  (docs/language/03-functions.md, "Return Values").
- A local could take the name of a function of the module and hide it: `func = fn(x: int,
  y: float) => x + y;` next to `fn func<T>(x: T) => x + 2;` made `func(1, 2.5)` call the
  lambda.  A variable, a parameter (a method's or a lambda's too), a `for` loop variable, a
  `match` binding or a destructuring target named like one of the module's functions (a
  generic or a native one included, declared before or after it) is now an error, `'func'
  is a function of this module and cannot be used as a variable`, reported once
  (docs/language/02-basics.md, "Scoping Rules").  A field, a native function's parameter
  and another module's function (always qualified) may still share the name.
- A generic function that its own module instantiated (`fn id<T>` and a call `id(3)` in
  the module) could not be instantiated with the same type arguments by an importer
  (`'$m::id<int>' is a builtin function and cannot be redeclared`); the importer now calls
  the module's instantiation, as it uses an imported instantiation of a generic struct.
- A static function of an instantiation named in a generic function's body (`Box<int>::of(3)`
  for a generic `Box` of the module) failed when another module instantiated the function
  (`call to undeclared generic function or class 'Box::of'`); it is resolved as the
  template's module declares it, as a variant of a generic enum is (`Maybe::Just(x)`).
- The AST interchange reader (docs/plugins/ast-format.md) took a method with `(native
  ...)` for its body (a class body's crashed the compiler) and a class's or a trait's
  method or a free function with `(qual static)` (read as if it had none). As in the
  recursive-descent grammar, a native body is now a top-level function's only and `(qual
  static)` an `impl` block's method's only.
- A method's body could give the name `self` to another variable, hiding the object the
  method was called on: a `match` arm `self: A { … }`, `for self in 0..3`, a declaration
  in a nested block (`self: A = a;`, `self: inout = a;`) or a destructuring target.  In an
  `__init__`, `self.f = 1` in such an arm assigned the matched object's field and counted
  as assigning the new object's, which was then used with `f` unset (a crash), and `self,
  k = (A(), 2);` stopped the compiler (`internal compiler error: destructuring into unowned
  variable 'self'`).  Each is now an error, `'self' cannot be used as a variable name in
  method 'm' of class 'A'; it is the implicit receiver`, as a parameter named `self` is
  (docs/language/05-classes.md, "Declaration").  A lambda written in a method may still
  name its parameter `self`, and a free function's variables may take the name.
- The `==` and `toString()` of a tuple, a struct or a tagged union called an element's
  `equals` or `toString` without holding the element, which the value only borrows from the
  storage it is in: an override that replaced that storage while it ran (through a closure
  assigning the field holding the tuple, `h.t = (Item("z"), 2)`) freed its own object, and
  read freed memory after (`self.tag` printed garbage).  The runtime now holds the element
  while its `equals` or `toString` runs, as a call's receiver is held, so such an element
  dies right after the call returns instead of during it: a later death, only in a program
  that used freed memory.  Each reference element of a tuple, struct or tagged union
  compared or printed costs one retain and one release more.
- The heap statistics of `paykan run --track-heap`, and of a built program run with
  `PAYKAN_TRACK_HEAP` set, came before the program's own output when standard output was a
  pipe or a file that standard error shared (`paykan run --track-heap hello.pkn 2>&1 | cat`
  printed `paykan heap stats:` first).  The runtime now flushes standard output before it
  prints them (`Paykan_heap_dump`), on all three paths.
- A `memory::Buffer` panic printed an index of 2^63 or more, an `unsigned` the program
  passed, as a negative number: `b.at(unsigned(9223372036854775807) + unsigned(1))` panicked
  with `buffer index -9223372036854775808 out of bounds (len=1)`.  The runtime's Buffer
  panics now print indices and counts unsigned (`buffer index 9223372036854775808 out of
  bounds (len=1)`).  A count of 2^63 or more given to the runtime's `PaykanBuffer_new`,
  `PaykanBuffer_reserve` or `PaykanBuffer_grow` panics with `out of memory growing a buffer
  to <n> values` (or `by <n> values`), as `PaykanBuffer_tryGrow` and `PaykanBuffer_tryReserve`
  report `ENOMEM` for it, instead of `the count cannot be negative`, and `PaykanBuffer_shrink`
  with one with `buffer shrink by <n>: it has only <len> values`.
- `err::fromErrno` gave a value outside C's `int` the code of the whole value but the text
  of the `int` it was truncated to: `fromErrno(4294967298)` (2^32 + `ENOENT`) was `Other`
  with `No such file or directory`, and `fromErrno(4294967296)` was `Other` with
  `Success`.  Such a value, which `strerror` cannot take, now has the text `Unknown error
  <errno>` (`Paykan_error_text`; docs/stdlib/error.md).
- `io::open` cut a path or a mode short at a NUL byte, as C's `fopen` reads them:
  `io::open("nul\0trunc.txt", "w")` opened and wrote a file named `nul`, and
  `io::open("three.txt", "w\0garbage")` opened `three.txt` to write.  A path or a mode
  holding a NUL byte is now not opened: `Err` with `InvalidArgument` (`EINVAL`), and no file
  is made (`PaykanFile_open`; docs/stdlib/io.md).
- A program that ran out of memory crashed with a segmentation fault, and lost what it had
  printed to a pipe or a file: an allocation outside `memory::Buffer` (an object, a string,
  a `HashMap`'s entry, a `Queue`'s node) was used without a check.  `Paykan_malloc` and
  `Paykan_realloc`, which the generated code and the runtime allocate with, now never return
  NULL: when the memory is not there the program panics with `paykan: out of memory
  allocating <n> bytes`, after flushing its output, and exits with status 134.  A
  `memory::Buffer` and an `io::File` read still report `OutOfMemory`, through internal
  allocations that return NULL with `ENOMEM` and leave the old block as it was
  (`Paykan_tryMalloc`, `Paykan_tryRealloc`).  An allocation of 0 bytes is a block of its
  own on every platform, so NULL always means out of memory (C lets `malloc(0)` return
  NULL and `realloc(p, 0)` free `p`).  An allocation that succeeds pays only the test of
  its result: without tracking, `Paykan_malloc` and `Paykan_free` call libc's `malloc` and
  `free` with no wrapper between, and what a NULL needs is out of line.  Instructions
  (callgrind, `build -O2` and the C path) of 20 binary trees of 32,767 objects and of
  300,000 string concatenations, against the runtime before the check: +0.00% and +0.00%
  with a GCC-built runtime, +0.60% and +0.71% with a Clang-built one and on the C path
  (Clang).
- Under `--track-heap` (or `PAYKAN_TRACK_HEAP`), a `memory::Buffer` asked for nearly all of
  memory overflowed the heap: `reserve(2305843009213693951)` of a `Buffer<int>`, 2^64 - 8
  bytes, crashed with a segmentation fault and `grow` with `heap corruption or invalid free`,
  where without tracking both are `OutOfMemory`.  The tracking allocator added its 16-byte
  header to the size, which wrapped around to a block of 8 bytes.  A size with no room
  for the header is now out of memory before anything is allocated, so these are
  `OutOfMemory` too, the buffer unchanged (`Heap.c`).
- A file opened for update (`"r+"`, `"w+"`, `"a+"`) lost a write that a read followed:
  `f.write("hello\n"); f.read(); f.write("world\n");` on a `"w+"` file left only
  `world\n` in it, though the first write had reported `Ok`.  C lets a read follow a write
  on such a stream only after an `fflush`, and a write follow a read only after a file
  positioning call (C11 7.21.5.3), and glibc drops what a write left in its buffer under a
  read longer than the buffer.  Each `io::File` now remembers whether it last read or
  wrote, flushes its stream before a read that follows a write, and positions it
  (`fseek(f, 0, SEEK_CUR)`) before a write that follows a read: a read after a write starts
  where the write ended, a write after a read where the read stopped (at the end in
  `"a+"`), and a read fails with the error of writing out the data before it (`NoSpace`
  on a full disk) instead of reading past it (`PaykanFile.lastOp`; docs/stdlib/io.md).
- A development build of `paykan` (one with a build ID) used a prebuilt `.pkm` module that
  another development build of the same version wrote, though two such builds may lay out
  what they compile differently: given the standard library of another 0.3.0-dev build
  (`PAYKAN_STDLIB`), or a library one of them built, a program misread that code, and one
  using a `memory::Buffer` instantiation compiled before the `PaykanType` change below
  crashed.  Such a file is now an error naming both builds (`module 'lib' (lib.pkm) was
  compiled by paykan 0.3.0-dev (...); build ID b7d975d89acc6d2b; this paykan is development
  build 91da536eca5b47fb and takes only files of its own build. Rebuild it from source.`).
  A cache entry of another build is rebuilt, as it was, and a release takes a prebuilt file
  of any build of a version it accepts (docs/language/15-modules.md, "Prebuilt modules";
  docs/design/pkm.md §2.2, §8.2).
- A `memory::Buffer` slot never set was taken for a value when zero bytes are no value of
  its type but a reference among them may be null: a tagged union whose first variant holds
  a `Str`, a class or a function value, and a function value itself.  With `enum F { B(s:
  Str), A }`, `copy(0)` of a new slot gave `Ok` and a `B` whose `Str` was a null reference
  (a segmentation fault once read, `B(s: None)` printed), `swap` with it succeeded, and a
  new `() -> int` slot's `get` gave a function value whose call jumped to address 0.  Such
  a slot is now unset, as a `Str`'s is: `copy` and `swap` give `NotFound`, and `at` and
  `get` panic with `buffer value <i> was never set` (docs/stdlib/memory.md).  So is one of
  a struct or a tuple holding a function value, of a tagged union whose first variant holds
  such a tagged union, and of a generic one's instantiation (`Maybe<Str>`); a type whose
  zero bytes are a value has no unset slot (`int`, an optional, `enum G { A(n: int, s:
  Str?), B(s: Str) }`, whose new slot is `A(n: 0, s: None)`).  A `PaykanType` has a fifth
  word, `nunset`, and after its references names the words of a value that are all 0 only
  in storage never set: a reference that cannot be `None`, a function value's code word,
  or a tagged union's tag and its first variant's words (`PaykanType_unset`; Runtime.h,
  "Generic native functions"; docs/pir.md §4).  It is part of 0.3.0's runtime ABI, so
  C code of a generic native function is compiled against 0.3.0's `Runtime.h`.  A type
  that names no such word (`int`, `Str?`) costs no more on `at`, `get`, `copy` and
  `swap`: the buffer looks at `nunset` before it calls `PaykanType_isUnset`.

## [0.2.0] - 2026-10-20

### Added

- Linux ARM64 (aarch64) release builds: a `paykanlang-<version>-linux-arm64.tar.gz` tarball
  and an `arm64` Debian package, built and smoke-tested like the x86_64 ones, with the LLVM
  backend (the pinned LLVM 17.0.6 aarch64 build is downloaded at configure time).
- `::result` (stdlib/result.pkn, imported `as res`): errors as values.  `enum ErrorCode`,
  the language's standard set of errors (`Ok`, `NotFound`, `PermissionDenied`,
  `AlreadyExists`, `InvalidArgument`, `OutOfMemory`, `Io`, `EndOfFile`, `OutOfRange`, ...),
  `struct Error { code: ErrorCode; msg: Str?; }` and
  `struct Result<T> { error: Error; value: T?; }`, the value present exactly when the code
  is `Ok`, with `ok(v)`, `none()`, `name(code)`, `fromErrno(errno)` and `isOk()` /
  `message()`.  Every standard library function that can fail returns one: `io::open`,
  `io::File`'s methods, `memory::Buffer`'s.  The runtime maps a C `errno` to its code
  (`Paykan_error_code`) and text (`Paykan_error_text`).
- `::io` (stdlib/io.pkn): `print`, `println`, `printerr` and `printerrln` take any
  `T: Show` (so `io::println(5)` needs no `Str(...)`); `class File`, whose `impl` methods
  `write` returns an `Error` and `readln`, `readbytes` and `read` a `Result<Str>`
  (`EndOfFile` at the end); `open(path, mode) -> Result<File>`; and `stdin() -> File`.
- Optionals of enums, tuples and structs (docs/language/12-optionals.md): `Suit?` is held
  like `int?`, a `(int, Str)?` or a `Point?` in place as a presence flag and the value,
  `None` all zeros.  They are made, compared (`== None`, `==`), unwrapped by `match`
  (`p: Point { ... } None { ... }`), printed, and stored in fields, arrays, tuples,
  parameters and results like any value.  A template's `T?` of an optional `T` is `T`
  itself, so `Result<Str?>` works (a written `int??` is still an error).  The value of an
  optional tuple or struct is at most 64 bytes: `Big?` for a 72-byte `Big` is rejected (`use
  'Box<Big>?' (memory::Box) instead`).  A nested tuple literal widens into an optional slot
  (`a: (int, (Str, Str)?) = (1, ("x", "y"));`).
- An optional of a `Show` type is `Show`: `x.toString()` on an optional is "None" or its
  value's text.
- `::math`, a standard library module of the C library's math functions on `float`
  (docs/stdlib/math.md): `sqrt`, `cbrt`, `pow`, `hypot`,
  the exponentials and logarithms, the trigonometric and hyperbolic functions,
  `floor`, `ceil`, `trunc`, `round`, `fmod`, `remainder`, `abs`, `min`, `max`,
  `copysign`, `erf`, `erfc`, `gamma` and `lgamma`, each a native function bound
  directly to its `<math.h>` symbol, plus `pi()`, `e()`, `inf()`, `nan()`, `isNaN`,
  `isInf` and `isFinite` in Paykan, with a stdlib test and a docs example.
- Sized numeric types (docs/language/02-basics.md, "Sized numeric types";
  docs/design/sized-types.md): `i8`, `i16`, `i32`, `u8`, `u16`, `u32`, `unsigned`, `f16`
  and `f32`, with `i64`, `u64` and `f64` as other spellings of `int`, `unsigned` and
  `float`.  Every numeric value takes an 8-byte slot, like an `int`, so a sized type
  changes no layout, runtime structure or `.pkm` record.  The operands of an operation have
  one type (`int` with `float` stays the one promotion), integer results wrap to the type's
  width, `f16` and `f32` results are rounded after each operation, and an `unsigned`
  divides, takes the remainder and orders unsigned.  A numeric literal takes the sized type
  its context expects when it fits it (`x: u8 = 255`, `x + 1`, a `match` arm) and is an
  error when it does not.  The conversion constructors convert between every pair of
  numeric types (integer to integer keeps the low bits, float to integer truncates and
  panics outside the target's range, integer to float rounds), `Str(x)` prints every
  numeric type (`unsigned` without a sign) and `u8(s)` & co. parse a `Str` into a `u8?` &
  co.  Every numeric type is `Show` and `ValueType` and the integer ones `Hashable`.  PIR
  gains `wrap`, `mask`, `udiv`, `urem`, `ucmp`, `utof`, `ftou` and `fround32` (docs/pir.md
  §6) and the runtime `PaykanString_from_uint`, `PaykanString_parse_uint`,
  `Paykan_round_f16`, `Paykan_panic_float_to_type` and the layout kind
  `PAYKAN_KIND_UINT`.  Not yet: bitwise operators, literal suffixes, hexadecimal literals,
  `char` to the sized types, implicit widening.
- Weak references (docs/language/16-memory.md, "Weak references"; design in
  docs/design/weak-references.md): `weak y: Point? = x;` declares a local, and `weak
  parent: Node?;` a class field, that refers to an object without keeping it alive.  The
  type is `T?` of a class `T` (a user class, `Str`, `io::File` or `Obj`), inferred from the
  initializer when left off (`weak u = x;`), and a local without an initializer starts as
  `None`.  A weak reference reads as a `T?`: the object while something holds it, `None`
  once it has been destroyed; it is assigned a `T` or a `T?`, matched, compared with `==
  None`, passed, returned and copied into a strong variable or field (which then keeps the
  object), and a lambda captures a `weak` variable weakly.  It is the back link that breaks
  a reference cycle (a child's `parent`, a closure stored in the object it refers to).  A
  weak reference cannot be passed to an `inout` parameter, named by an `inout` local or
  bound by an `inout` match arm; `weak` is a keyword.  A struct field cannot be `weak` (a
  struct is a value); not yet: a `weak` tuple or array element, parameter or result.  In
  PIR a weak reference has the type `weak` and the ops `weak.new`, `weak.load` and
  `weak.release`; the runtime has `Paykan_weak_new`, `Paykan_weak_load` and
  `Paykan_weak_release`; a `.pkm` interface marks a weak field with a flags byte (`pkm
  dump` prints `weak field parent: Node?`).
- Structs (docs/language/11-structs.md): `struct Point { x: int; y: int; }` declares a
  value type with named fields, held in place and copied by assignment like a tuple, with
  no superclass, no vtable and no identity.  `Point(1, 2)` constructs one from its fields
  in order, or through an `__init__` declared in an `impl Point { ... }` block, which also
  holds its methods and `static fn`s; `p.x = 5` assigns a field in place (also through a
  field, an array element or an `inout`), and in a method `self` is the receiver's
  storage.  `==` compares field by field, `toString()` renders `Point(x: 1, y: 2)`, every
  struct implements `Show` and `ValueType`, and `impl Hashable for` and user traits work
  as for classes, so a struct is a key of `HashMap` and `HashSet`.  A struct crosses
  modules and prebuilt `.pkm` files (a `STRUCT` interface record), and the runtime renders
  and compares one through its layout (`PaykanStruct_toString`, `PaykanStruct_equals`,
  the layout kind `PAYKAN_KIND_STRUCT`).  Not yet: `match` on a struct, a fixed-size array
  field and an `impl` for another module's struct.
- Generic structs (docs/language/11-structs.md, "Generic structs"): `struct Pair<A, B>
  { a: A; b: B; }` is a template like a generic class, instantiated by its type
  arguments (`Pair<int, Str>`) or inferred from a constructor call (`Pair(1, "a")`, from
  the fields or from an `__init__` of an `impl Pair<A, B>` block), with `impl` and `impl
  Trait for` blocks and bounds, in every position a struct goes, across modules too.
  A `static fn` of its `impl` block is the instantiation's: `Pair<int, Str>::of(1, "a")`.
- Private names, at two levels (docs/language/05-classes.md "Private fields and methods",
  docs/language/15-modules.md "Private to the module", design in docs/design/visibility.md).
  A field or a method whose name starts with two underscores, `__x`, is private to its
  class or struct: it is reachable only from that type's own methods, `impl` blocks, trait
  impls and `static fn`s (`'__count' is private to 'Counter': it is reachable only from its
  own methods`), not from a subclass, the rest of the module or an importer; the default
  `__init__` still takes every field.  A name that starts with one underscore is private to
  its module: an importer cannot name a `_` free function, class, struct, enum or trait
  (`'m::_f' is private to module 'm'`), while the module's exported generic templates keep
  using it, and a `_` field or method is reachable from any code of the module that
  declares its type, a subclass and a free function included, and from no importer (`'_w'
  of 'Pub' is private to module 'lib::shapes'`).  A top-level declaration named with `__`
  is rejected (`a module's own function is spelled '_f'`).  A public function, field or
  method cannot name a private type (`'make' is public but its result names the private
  type '_Node'; make the function private or the type public`).  `fn ___init__(...)`
  (three underscores) in a class's or a struct's `impl` block is a module-private
  `__init__`: the type is constructed (`T(...)`) and extended (`__super__(...)`) by the
  module's own code only, its templates included wherever they are instantiated, and an
  importer makes one through the module's functions (`'m::T' cannot be constructed here:
  its initializer '___init__' is private to module 'm'`).  `io::File`'s is one: `open` and
  `stdin` make a `File`, a program cannot.  `__init__` and `__super__` keep their meaning.
  Private names stay in a `.pkm` interface and are refused by name.
- Function types and lambdas (docs/language/14-function-types.md, design in
  docs/design/function-types.md): `(int, float) -> bool` is the type of a function
  value, with the parameters' modes part of it (`(view Str, inout int) -> bool`) and the
  result a copy; `() -> R` takes no parameter, `-> void` returns nothing, `->` is
  right-associative, and `((int) -> int)[3]` / `((int) -> int)?` give a function type a
  suffix.  A value is a lambda `fn(x: int) -> int { return x + 1; }`, or a named function
  used without calling it (`square`, `m::square`, `Point::origin`).  A lambda captures the
  variables of the function it is written in by copy (a reference retained, a value held in
  place copied), read-only inside it; a `view`, an `inout`, a `view fn`'s `self` or a
  struct method's `self` cannot be captured.  A function value is called like a function
  (`f(1)`, `obj.cb(1)`, `fs[0](1)`), with the arguments checked and passed as for the
  function's own parameters.  Function types go in every type position, cross modules
  and prebuilt `.pkm` files, and are type arguments (`List<(int) -> int>`).  A function
  value is a value held in place (docs/design/function-types.md, "Run-time
  representation"): 16 bytes, the address of its code and the box of its closure, the
  object holding its captures, which is null when it captures nothing, so a named function
  or a lambda that captures nothing used as a value allocates nothing.  Copying one shares
  the closure; `==` is true for the same code and the same closure, so `square == square`
  is `True` and `List.contains(square)` finds it (within a module: a value of the function
  made in another module is a different value for now), while each evaluation of a
  capturing lambda makes a new closure.  A function type is a `ValueType`, not an `Obj`,
  and `((int) -> int)?` takes the same 16 bytes, `None` having no code.  A call loads the
  code and the closure, keeps the closure for the call and calls the code through its
  address, through the PIR instructions `fn.addr` and `icall` (docs/pir.md); at `-O2` LLVM
  makes the call direct where the code is known.  Not yet: borrowed results in a function
  type, methods and generic functions as values, inferred lambda parameter types, capture
  by reference.

- The standard library ships with the compiler (docs/stdlib/index.md), in
  `stdlib/`: `::collections` has `List<T>`, `LinkedList<T>` (doubly linked, each node
  holding the previous one as a `weak` reference so no reference cycles form),
  `Stack<T>` and `Queue<T>` (on a `LinkedList<T>`), `HashMap<K: Hashable, V>` and
  `HashSet<K: Hashable>` (separate chaining), and `MinPriorityQueue<T>` and
  `MaxPriorityQueue<T>` (binary heaps of elements pushed with an `int` priority, equal
  priorities popping in push order); `::memory` has `Buffer<T>`, a struct that is a handle
  to the runtime's storage, so its copies share the slots.  Every index and size of the
  library is an `unsigned` (`len()` returns one, `get(i)` & co. take one, and `indexOf`
  returns an `unsigned?`, `None` for a missing element), and the containers hold elements
  and keys of the sized numeric types too (`List<u8>`, `HashMap<u32, Str>`, `Box<f32>`).
  A broken precondition of a container panics (`pop from an empty Stack`).  A `Buffer`
  starts empty, `Buffer<T>()`, and its operations report what goes wrong instead of
  panicking: `grow`, `reserve`, `push`, `set`, `swap` and `shrink` return a `res::Error`
  (`OutOfMemory`, `OutOfRange`, `InvalidArgument`, `NotFound`) and `copy(i)` a
  `res::Result<T>`; `get(i)` and `copyUnchecked(i)` panic on a bad index, for code that
  has checked it.  The build compiles each module to a `.pkm` with the `paykan` it built
  and stages it with its source in `lib/paykan/stdlib/<version>/` next to `bin/paykan`;
  `cmake --install` installs them in `<prefix>/lib/paykan/stdlib/<version>/` and the
  release packages include them.  `-DPAYKAN_BUILD_STDLIB=OFF` leaves the library out, and
  it is off by default when cross-compiling.  The `stdlib` test suite
  (`scripts/stdlib_tests.py`) runs `tests/Stdlib` on every backend.
- A system import (`import ::collections;`) finds the installed standard library: it
  looks in `$PAYKAN_STDLIB` when it is set, then in `<prefix>/lib/paykan/stdlib/<version>/`
  next to the `paykan` executable when that exists, then in `<source-root>/stdlib`.
- The built-in trait `Hashable` (docs/language/06-traits.md, "Hashable"):
  `view fn hash() -> int`, implemented by `int`, `bool`, `char`, `Str` and the sized
  integer types, and by a class or a struct with an `impl Hashable for` block.  The hash
  of equal values is equal on every run and backend: the runtime's `PaykanInt_hash` (a
  `bool` or `char` hashes as its `int` value) and `PaykanString_hash` (FNV-1a over the
  bytes), both ending in the SplitMix64 finalizer.  Declaring a trait named `Hashable` is
  an error.
- The built-in trait `ValueType` (docs/language/06-traits.md, "ValueType"), with no
  methods: implemented by the types held in place, `int`, `float`, `bool`, `char`, the
  sized numeric types, every enum, tuple, struct and function type, and a fixed-size array
  (which cannot be a type argument yet).  A class, `Str` and an optional are not, and `impl
  ValueType for` a class is an error (`class 'P' cannot implement 'ValueType': a class is
  shared by reference, not held in place`).
- `memory::Box<T: ValueType>`: one value in a shared object, whose `get()` is the
  value's storage (`-> inout T`).
- Runtime functions for the standard library's panics, reached through `native fn`s:
  `Paykan_panic_message(Str)` prints `paykan: <message>` and aborts, and
  `Paykan_panic_value`, the same as a generic `native fn` with a result, for where a value
  is due.
- `[v; n]` is an array of `n` copies of `v` (docs/language/09-arrays.md, "Repeated
  values"): `slots: Entry?[8] = [None; 8];`, and `[[0; 3]; 2]` is an `int[2][3]` of zeros.
  The count is an integer literal of at least 1 (`[0; 0]` is an error, `an array has at
  least one element: the count must be 1 or more`), and the literal types as one of `n`
  elements, so `[0; 4]` flowing into a `float[4]` is four `0.0`s. `v` is evaluated once:
  `[Node(); 3]` holds one object three times. In PIR, `[0; n]` (or `0.0`, `False`,
  `'\0'`) of a scalar element type is one `mem.zero` of the array. In the AST interchange
  format it is `(array-repeat LOC EXPR INT)`.
- Traits (docs/language/06-traits.md): `trait Named { view fn name() -> Str; }` lists the
  methods a type must have, and `impl Named for A { ... }` declares that class `A` has
  them. The block's methods join `A` like an `impl` block's, and a method `A` already has
  meets a requirement too. Traits can be generic (`trait Into<T>`), and `Self` names the
  implementing type. A class implements its superclasses' traits, and every class
  implements the built-in `Show` (`view fn toString() -> Str`) through `Obj`, as do
  `int`, `float`, `bool` and `char` (`n.toString()` is `Str(n)`) and tuples. Bounds
  (`fn f<T: Show + Into<int>>`, `class Box<T: Named>`) are checked where the template is
  instantiated (`'Str' does not implement trait 'Named', which 'greet' requires of 'T'`).
  A trait is not a type.
- `impl` blocks (docs/language/05-classes.md, "`impl` Blocks"): `impl Point { ... }` adds
  `fn`, `view fn` and `static fn` methods to a class of the same module, and holds its
  `__init__`. They are called directly, not through the vtable, and subclasses inherit
  them. A `static fn` is called as `Point::origin()`, or `mod::Point::origin()` from an
  importer. No `impl` method may reuse a name the class or a superclass has, and none can
  be overridden.
- Generic `impl` blocks (docs/language/13-generics.md, "`impl` Blocks"): `impl Box<T> { ... }`
  and `impl Named for Box<T> { ... }` give each instantiation of `Box` the block's methods
  and trait, checked when it is instantiated, also in a module that imports `Box`. A
  `static fn` in one is the instantiation's, called as `Box<int>::of(3)`
  (`m::Box<int>::of(3)` through an import).
- Generic classes and functions of an imported module (docs/language/13-generics.md,
  "Modules"): `gen::Box<int>(1)`, `gen::Box("s")`, `b: gen::Box<Str>` and `gen::first(xs)`
  instantiate `gen`'s templates in the importer, in every position a local template may be
  used, its own templates included.  A body names what its module declares and imports,
  whatever the importer calls those names, and is checked per
  instantiation in the template's file (`in instantiation of 'lib::gen::show<int>'
  requested here`).  `Box<int>` is one class however many modules instantiate it, the
  template's module included, and is compiled once per program; inference through
  `Box<T>` parameters sees an imported `Box<int>` too.  A module with templates, with
  instantiations of another module's, or using such an instantiation, is built from source
  every time (a `.pkm` does not hold templates yet).  Instantiating both this module's
  `Box<Str>` and an imported template's is an error (`conflicts with another template's
  instantiation of the same name`).
- Runtime functions for the standard library's `memory::Buffer<T>`: `PaykanBuffer_new`,
  `_len`, `_get`, `_copy`, `_isSet`, `_set`, `_reserve`, `_grow` and `_shrink`
  (`Runtime.h`, "Buffer"), generic native functions over a `PaykanBuffer` of values of any
  size, laid out by their `PaykanType`.  New values are all zero bytes (0, False or None);
  the buffer retains the references it holds and releases them when they are overwritten,
  shrunk away or destroyed; growing at least doubles the allocation and shrinking to a
  quarter of it halves it.  An index out of bounds, a negative count, shrinking by more
  than the length and reading a never-set reference that may not be None panic (`buffer
  index 3 out of bounds (len=3)`); `PaykanBuffer_tryGrow` and `PaykanBuffer_tryReserve`
  report a failed allocation as `ENOMEM` instead of aborting (`memory::Buffer`'s
  `OutOfMemory`).
- Generic native functions (docs/language/03-functions.md, "Generic native
  functions"): `native fn pick<T>(first: bool, a: view T, b: view T) -> T = "c_pick";`.
  Each instance calls the same C function with a `PaykanType` for each type argument first
  (`Runtime.h`: the type's size, alignment, kind and reference offsets), and passes what
  the function types `T` by address (a `-> T` result through a `void *out`), so a value
  of any type and size crosses into C; `PaykanType_copy` and `PaykanType_drop` copy and
  release one.  In the AST interchange format a generic native function has its
  `(type-params ...)` like any other.  See `samples/imports/15_native_generic`.
- Borrowed results (docs/language/03-functions.md, "Borrowed results"): a
  function or method declared `-> view T` returns a borrow of part of `self` or of a
  `view` or `inout` parameter, never a new value, a copy parameter or a local (`'d' is a
  copy: a 'view' result must be part of 'self' or of a 'view' or 'inout' parameter`).  The
  caller sees the result as a `view`: it is read, passed on to `view` parameters or bound
  to a `view` local, and a value type can be copied out (`the result of 'peek' is a
  'view'; 'tick' is not a 'view fn'`).  A `view` local of a borrowed result borrows what
  the call borrowed, its receiver and its arguments to `view` / `inout` parameters, until
  its last use.  `-> inout T` returns the storage of part of `self` or of an `inout`
  parameter, of exactly its type: the caller binds it to an `inout` local, passes it on to
  an `inout` parameter or reads it, and it borrows the receiver and the `inout` arguments
  as an `inout` local would; it is lowered as the address the function returns.  An
  override keeps how the method it overrides returns; `main` returns a copy.  The result's
  mode is part of a module's interface (a result-mode byte after a `.pkm` record's
  parameter modes), and in the AST interchange format it is a `(qual view)` after the
  return type.
- `match` arms that borrow the subject (docs/language/08-match.md, "Borrowing
  the subject"): `d: view Dog { … }` binds `d` as a `view` local of the subject, whatever
  the subject is, so nothing changes through it and it passes on only to `view`
  parameters; the variable the subject starts from cannot change until `d`'s last use in
  the arm.  `d: inout Dog { … }` binds `d` as the subject's storage, like an `inout` local:
  assigning to it writes the matched variable or field (a variable, a field, an optional,
  an `inout` parameter; not an expression, `self`, a `let` local, a `view`, or a primitive
  or an enum inside an optional yet), and the subject's variable cannot be used until
  `d`'s last use.  With the mode before the name (`view d: Dog`) the syntax error shows the
  fix.  In the AST interchange format the arm carries `(qual view)` or `(qual inout)` after
  its type.
- `view fn` (docs/language/05-classes.md, "Methods that don't change self"): a method
  declared `view fn len() -> int` does not change `self`, and is the only kind of method
  that can be called on a `view` (`'c' is a 'view' parameter; 'tick' is not a 'view fn'`).
  `toString`, `equals`, `len` and `length` are `view fn`s.  An override is a `view fn`
  exactly when the method it overrides is one; `__init__` and a free function never are.
  The marker is part of a module's interface (method flag `0x2` in a `.pkm`), and a
  function in the AST interchange format carries it as a trailing `(qual view)`.  In a
  `view fn`, `self` and everything reached through it is read-only, as through a `view`
  parameter (`'self' is read-only in 'view fn get'; cannot assign to its field 'n'`), and
  part of `self` that is an object, string or optional (or an array or tuple holding one)
  is returned as a `view`, `-> view T`: `view fn name() -> Str { return self.name; }` is an
  error, `'self' is read-only in 'view fn name', so part of it can only be returned as a
  'view': write '-> view Str'`, while a field of a value type (or an array or tuple of
  them) is returned as a copy.  A method that never changes `self` but is not a `view fn`
  gets a warning (`'area' never changes 'self': make it a 'view fn' so 'view' parameters
  can call it`), worked out across a method's overrides and its calls on `self`; a method
  that returns part of `self` does not.  The samples and the manual mark theirs.
  `samples/codegen/48_view_fn.pkn` shows them.
- Local borrows (docs/language/02-basics.md, "Local borrows"): a local declared
  with a mode where the type goes, `v: view = k + 1;` or `w: view float = k;`, the type
  inferred when left off.  A `view` local reads its initializer, which may be any
  expression: a copy of a value type, the same object, string or array otherwise.  It
  cannot be assigned, destructured into or passed to an `inout` parameter, nothing reached
  through it can be assigned, and only a `view fn` (`toString`, `equals`, `len`,
  `length`, or a method declared one) can be called on it.  An `inout` local (`x:
  inout = k;`, `t: inout = self.total;`) is another name for a variable or a field, of any
  type: assigning to it writes there, and it passes its address on to an `inout`
  parameter.  It names a place as an `inout` argument does (not an expression, an array
  element yet, a string's character, `self`, a `let` local or a `view`), of exactly its
  type, and keeps a field's object alive while it can be used.  A local borrow of a
  variable is exclusive while it is live, from its declaration to its last use in its
  block (a loop that uses it keeps it live): the variable an `inout` local names cannot be
  used (`'k' is borrowed by 'inout' local 'x' until 'x' is last used`), and one a `view`
  local reads cannot change (`'c' is viewed by 'view' local 'v' until 'v' is last used;
  'tick' is not a 'view fn'`).  A borrow of a borrow keeps the first variable borrowed.
  A local borrow needs an initializer, is never `let` and is not a destructuring target;
  written before the name (`view x = e;`) the error shows the fix.  In the AST
  interchange format a local borrow carries `(qual view)` or
  `(qual inout)`.
- A `view` of a reference stays a `view` (docs/language/03-functions.md, "A
  `view` of a reference stays a `view`"): a `view` of an object, a `Str`, an optional of
  one, or a struct, tuple or array holding one can only be passed on to a `view` parameter
  (`io::println` and the other print functions take a `view T`; the builtin methods read
  their arguments; `Str(s)` returns `s` itself and takes none).  A value type read through
  a `view` (`n: view int`, `k: view = 3`, `xs[i]` with `xs: view int[]`, a `-> view int`
  result, `self.x` with `x: int` in a `view fn`) is passed to a by-value parameter as a
  copy, as assigning it copies it.  A `view` that shares what it holds is never stored
  (assigned, destructured) or returned, and a `match` arm's name for one is a `view` too;
  an array or tuple literal is a new value, so any `view` can be one of its elements
  (`[v]`, `(s, 1)`, `return (s, 1);`).  Passing a `view` to an `inout` parameter reads
  `'v' is a 'view' parameter; it cannot be passed to 'inout' parameter 'n'`.
- `view` and `inout` parameters (docs/language/03-functions.md, "Parameter
  modes: view and inout"), on functions, methods and constructors, for parameters of any
  type (type parameters included).  The mode is written where the type goes: `fn bump(n:
  inout int)`, `fn scale(x: inout float, by: view float)`, `fn show(c: view Counter)`,
  called as `bump(k)`.  A `view` parameter is read-only, and so is what it holds: no field
  or element write and only `view fn` calls through it.  An `inout` parameter is
  its caller's storage, passed by address so
  every write reaches the caller at once: a local variable, a parameter (an `inout` one
  passes its address on) or a field of an object (`obj.f`, `self.f`, `a.b.f`; by real
  address, never copied in and out, with the object kept alive for the call), of exactly
  the parameter's type (a subclass variable is not a base-class `inout` argument).  An
  `inout` parameter of a reference type is the caller's variable, not a second
  reference: passing it neither retains nor releases, and assigning to it replaces what
  the caller's variable holds.  A literal or other expression, `self`, a `view` parameter
  or anything reached through one, a `let` local, an array element (not yet) or a
  string's character is an error.  An override keeps every parameter's mode; modes are
  part of a module's interface (a `.pkm` record's parameter modes), so calls and overrides
  in another module are checked against them.  `view` and `inout` are reserved words, a
  syntax error anywhere but before a parameter's type; written before the name (`inout n:
  int`), the error shows the fix.  In the AST interchange format a parameter carries
  `(qual view)` or `(qual inout)`.  `samples/codegen/47_inout_view.pkn` shows them.
- What a call passes to an `inout` parameter shares no storage with anything else the
  call borrows (docs/language/03-functions.md, "Exclusivity"): another
  `inout` argument, a `view` argument, or the receiver of a method that may change it or
  of a `view fn`, and a variable overlaps a field chain on it.  `both(k, k)` with
  `fn both(a: view int, b: inout int)` is `'k' is passed to 'view' parameter 'a'; it
  cannot also be passed to 'inout' parameter 'b'`, and `p.take(p.x)` is `'p.x' is part of
  'p', the receiver of 'take' (not a 'view fn'); it cannot also be passed to 'inout'
  parameter 'm'`.

- `let` local declarations: `let n = 3;` and `let s: Str = "a";` declare a variable
  that cannot be reassigned (`'n' is declared with 'let' and cannot be reassigned`),
  neither by assignment nor by destructuring.  `let` needs an initializer and applies to
  local declarations only; it fixes the variable, not the object it refers to.  `let` is
  a reserved word now.  In the AST interchange format a `let` local carries `(let)`.

- Imported modules are compiled to `.pkm` module files (docs/design/pkm.md, prototype of
  phase A): the module's interface, its PIR and a manifest, portable across backends.
  A program's imports are cached as `.paykan_cache/<module>.pkm` and reused while the
  source, the toolchain and the interfaces of the module's own imports are unchanged
  (a body edit rebuilds one module, a signature edit its importers); where no source is
  found, a prebuilt `.pkm` next to it, under `--module-path=<dir>` or `$PAYKAN_MODULE_PATH`
  is loaded instead, and the program builds and runs identically on the c and llvm
  backends from it.  New: `--emit-pkm [-o f.pkm]`, `--module-path=<dir>`,
  `--rebuild-modules`, `--no-module-cache`, `--verbose`, and the `paykan pkm dump|check`
  command.  The backends' own caches stay beside the module files for now.
  `samples/imports/13_pkm` walks through it.  Libraries `paykan_pkm` (the file format) and
  `paykan_modules` (the resolver); the runtime ABI version moved to
  `PAYKAN_RUNTIME_ABI_VERSION` in `Runtime.h`.
- Each build of the compiler has a build ID, a hash of its sources (`cmake/BuildId.cmake`,
  #188), which a `.paykan_cache` module entry records (`core.build`) and which must match
  for the entry to be used, so a development build never reuses an entry another build of
  the same version wrote; `--verbose` reports `is stale (compiled by paykan <version>
  (<id>); this is paykan <version> (<id>))`.  A release build (`-DPAYKAN_RELEASE=ON`, set
  by the release workflow and the Homebrew formula) has no build ID, and a prebuilt `.pkm`
  is still judged by its version alone.
- `native fn name(params) -> T = "c_symbol";` declares a function whose body is a C
  function written against the runtime ABI (#198; docs/language/03-functions.md,
  "Native Functions"): `int`, `float`, `bool`, `char`, an enum (as its ordinal, an
  `int64_t`), `Str` and `Obj` cross by the runtime builtins' convention (`Str?` and `Obj?`
  as a result).  Only `int`, `float`, `bool`, `char` and an enum cross by value: any other
  parameter is a `view`, which C borrows, or an `inout`, which C changes in place
  (`parameter 's' must be 'view' or 'inout' ('s: view Str'); only int, float, bool, char
  and an enum cross into C by value`).  An `inout` argument is the caller's variable:
  `int64_t *`, ..., `PaykanShared **` for a `Str` or `Obj`, and for a generic instance's
  `inout T` the value's own address.  The result can be a `view` of a `view` or `inout`
  parameter.  A call in the declaring module calls the C function directly with the
  caller's own references, so it does no reference counting.  `native` is a reserved word.
  In PIR the C function is a module-less `extern fn @$c.<symbol>`; both backends declare it
  from its signature.  `paykan` does not build native code: `--object=a.o,b.o` (a
  comma-separated list, repeatable) names the objects or archives that define it, and
  every backend links them (the llvm JIT loads them).  Sample: `samples/imports/14_native`.
- `scripts/pkfmt.py` checks (`python3 scripts/pkfmt.py`) or applies (`--apply`) the layout
  of Paykan source (`.pkn`); every tracked file is formatted with it, and the Lint job
  checks it.

### Changed

- `::collections`' `List`, `Stack`, `Queue`, `HashMap`, `HashSet`, `MinPriorityQueue` and
  `MaxPriorityQueue` are structs, each holding the one object that keeps its state (the hash
  table of a `HashMap` or a `HashSet` is a private class of its own), so a copy, a parameter
  or a field still names the same container. As structs they are `ValueType`s and not
  `Obj`s: put one in a `memory::Box` where an `Obj` is needed. A container in a `let`
  variable can no longer be changed, and printing one shows the struct's fields.
  `LinkedList` stays a class.
- The documentation has one language reference: the manual's chapters and the reference's
  are merged, a chapter per feature that teaches it and then gives its precise rules
  (`docs/language/`, in reading order from getting started to the `paykan` command). The
  standard library has a section of its own, a page per module with every function and type
  (`docs/stdlib/`). Extending PaykanLang with plugins moved to the implementation overview
  (`docs/internals.md`).
- The package channels have repositories of their own (#178, #181).  Homebrew installs from
  the tap `parsabee/homebrew-paykanlang` (`brew install parsabee/paykanlang/paykanlang`, no
  tap URL), and the signed apt repository is `https://parsabee.github.io/paykanlang-apt`,
  published by that repository instead of the documentation site.  The Release workflow
  opens the tap's formula bump pull request and sends the apt repository its publish
  dispatch, with the `RELEASE_CHANNELS_TOKEN` secret.  A tap of the compiler repository
  moves with
  `brew tap --custom-remote parsabee/paykanlang https://github.com/parsabee/homebrew-paykanlang`.
- Every format is versioned by the release: PIR (binary, and the text a backend plugin
  receives), the runtime ABI, the `.pkm` container, manifest and interface, the plugin API
  and the AST interchange format all carry the release version, packed as
  `major * 10000 + minor * 100 + patch` (200 for 0.2.0) where the formats had their own
  numbers, and dotted in text (`(paykan-ast 0.2.0 ...)`, `pkm dump`, diagnostics).  A
  reader accepts exactly its own release's: a prebuilt `.pkm` or a plugin of another
  release is rejected, and a cache entry of another release is rebuilt.  0.2.0 accepts
  plugins built with 0.2.0 only.
- Printing and files moved to the standard library: the builtins `print`, `println`,
  `printerr`, `printerrln`, `open`, the classes `File` and `Error` and the identifier
  `Stdin` are gone.  A program writes `import ::io;` and `io::println(...)`,
  `io::open(...)`, `io::stdin()`; the samples, tests and docs are rewritten so.  Only `Obj`
  and `Str` remain builtin classes, and `print`, `open`, `File` and `Error` are free names
  (a program that imports `::io` still cannot declare a `File`, `Error`, `Result` or
  `ErrorCode` of its own: type names are global across an import graph).
- An if-statement chains with the new keyword `elif` instead of `else if`
  (docs/grammar.md): `if x > 0 { … } elif x == 0 { … } else { … }`.  `else if` after an
  if-statement's block is a syntax error that names `elif`; the conditional expression
  keeps `else if` (`if x > 0 then 1 else if x == 0 then 0 else -1`) and rejects `elif`.
  `elif` is reserved.  The AST is unchanged: an `elif` is an `IfStmt` nested as the else
  branch.
- `if` and `while` take their condition without parentheses (docs/grammar.md):
  `if x > 0 {`, `elif x == 0 {`, `while i < n {`; the block's `{` ends the condition.
  A parenthesised condition is still a parenthesised expression, so `if (x > 0) {`
  keeps parsing.  A statement starting with `if` is a ternary expression statement when
  `then` follows its condition, as before.  The samples, the standard library, the
  tests and the docs are rewritten without the parentheses.
- Arrays are `T[n]` (docs/language/09-arrays.md), values of a fixed length, instead of the
  dynamically sized `T[]` objects: `push`, `pop`, `toString` and `equals` on an array are
  gone, and storage that grows is `collections::List<T>` or `memory::Buffer<T>`.
  `xs: int[3] = [1, 2, 3];` holds three
  ints inline. An array is a value, so `ys = xs` copies it, and its one method is `len()`.
  A literal for a `T[n]` has exactly `n` elements. An index out of bounds is an error when
  it is a constant, and a panic (`paykan: array index 3 out of bounds (len=3)`) otherwise.
  A `let` array's elements cannot be assigned. `int[2][3]` is two `int[3]`s, and an array
  holds at most 16384 elements, counted across its rows. A parameter can be one in every
  mode (a plain one is the function's own copy), and so can a result, also across
  modules. An element can be a scalar or an optional of one (`int?[3]`), an enum, a
  reference (`Str[2]`, `Point[4]`: a copy of the array shares the elements' objects) or
  another array. For now an array is only ever a local variable, a parameter, a result or
  a field of a class (held inside its object): not a tuple element, a struct field, an
  optional, a type argument, a `match` arm or an `Obj` (`a fixed-size array ('int[2]') can
  only be a local variable, a parameter, a result or a field for now`), and it has no `==`
  and is not printed. In a variable declaration with an initializer a length can be `_`,
  the initializer's: `x: int[_][_] = [[1, 2, 3], [4, 5, 6]];` is an `int[2][3]`, and
  `z: float[_] = f();` takes the length of the array `f` returns.
- An array literal is a `T[n]` of its own length (docs/language/09-arrays.md,
  "Literals"): `[1, 2, 3]` is an `int[3]`, `[1, 2.5]` a `float[2]` and `[[1, 2], [3, 4]]`
  an `int[2][2]`, and objects of different classes give their nearest common ancestor.
  Where the literal flows into a typed slot, its elements take the slot's element type
  (`xs: float[2] = [1, 2]`). An array has at least one element: `[]` is an error (`an
  empty array literal has no type: an array has at least one element`).
- Slices (docs/language/09-arrays.md, "Slices"): `T[]` is the type of a `view` or `inout`
  parameter, a slice. It takes an array of any length whose elements are exactly `T`
  (a literal becomes one) or another slice, and reads or writes the elements in place: a
  slice is indexed, measured with `len()` and passed on. A generic `xs: view T[]` takes
  `T` from the elements. Anywhere else `T[]` is an error (`variable 'x' has type 'int[]':
  a slice can only be a 'view' or 'inout' parameter, and an array has a length
  ('int[3]')`). In PIR a slice parameter is two, the `ptr` address of the elements and
  their `i64` number.
- An object is one allocation (docs/language/16-memory.md, "The reference counts"):
  its 16-byte header (src/Runtime/Runtime.h, `PaykanObject`) is the vtable pointer, then a
  32-bit strong count and a 32-bit weak count, `{ vtable, strong count, weak count,
  fields… }`, and the separate `PaykanShared` box is gone.  A strong reference
  (`PaykanShared *`) and a weak reference (`PaykanWeak *`) are the object's own pointer:
  `PaykanShared_new` is a retain and returns the object, `PaykanShared_get` is the
  identity, and both backends lower `unbox` to the identity.  A count that would pass
  `INT32_MAX` panics (`too many strong references to one object`) instead of wrapping.
  A `destroy` (vtable slot 0) releases what the object holds and tears down its resources
  but does not free the object: the runtime frees it after the destroy, or keeps it,
  empty, until the last weak reference to it is released (`Paykan_weak_release`).
  Generated destructors do not end in `free`, `PaykanObject_destroy` is a no-op,
  `PaykanString_destroy` frees the bytes only, and a string temporary the generated code
  drops goes through the new `Paykan_dispose(obj)` (destroy and free an object that was
  never boxed).  Deferred destruction keeps its pending objects on a runtime stack (256
  inline slots, then the C heap); the destroy order is unchanged.  Every object struct,
  the runtime's and the generated ones, starts with the header; the runtime's immortal
  `None` and stdin `File` start with a strong count of 1.  The runtime ABI version is the
  release's (below): delete every `.paykan_cache` directory written by an older build.
- The program's entry point is `fn main() -> int` or `fn main(args: view Str[]) -> int`
  (docs/language/03-functions.md): the arguments are a slice, which `main`
  reads (`args[0]` is the script path, or the executable's) but does not keep.  Any other
  signature is an error (`the program's entry point must be 'fn main() -> int' or 'fn
  main(args: view Str[]) -> int', not 'fn main(int) -> int'`).  In PIR `@main` is
  `() -> i64` or `(ptr, i64) -> i64` (docs/pir.md §4): the backend's entry code makes the
  slice before it calls `@main` and drops it after, in an executable with the runtime's
  `Paykan_args_new` and `Paykan_args_free`.  A backend plugin must do the same.
- A class without an `__init__` gets the default one (docs/language/05-classes.md, "The
  default `__init__`"): it takes the superclass constructor's arguments, then one per
  field of the class, in order, so `class Point { x: int; y: int; }` is built as
  `Point(1, 2)`, and `Box(3)` infers `Box<int>` from `class Box<T> { v: T; }`.
  `Point()` no longer leaves the fields zero (a `Str` or object field was null):
  `function 'Point' expects 2 argument(s), got 0 (it has no '__init__', so it takes
  x: int, y: int)`. A subclass needs no `__init__` of its own when its superclass has
  one. An `__init__` that a subclass writes calls `__super__(...)` first unless the
  superclass's constructor is the default one of a class with no fields, as are its
  superclasses'.
- `__init__` goes in an `impl` block of its class (docs/language/05-classes.md, "`impl`
  Blocks"): `class A { fn __init__() { ... } }` is an error that says to write
  `impl A { fn __init__() { ... } }`. The class body holds the fields and the virtual
  methods.
- Assigning `self` in a class method (`self = C(1)`) is now an error (`cannot assign to
  'self'`): `self` names the object the method was called on, and the assignment changed
  nothing the caller could see.
- An override of `toString` or `equals` is declared `view fn` (`view fn toString() -> Str`),
  like the `Obj` methods it overrides: a plain `fn` override is an error.
- PIR: `mem(N, A)` slots (docs/pir.md §1 and §6), a local's or a field's N bytes
  aligned to A, for aggregates: fixed-size arrays and tuples.  A `mem` slot is never a
  value: it is reached through its address, with `ptr.add` to its parts, and copied and
  cleared with `mem.copy` and `mem.zero`.  A backend plugin must handle the three
  instructions.
- PIR: four address instructions, `local.addr`, `field.addr`, `ptr.load` and
  `ptr.store` (docs/pir.md §6), which `inout` parameters and locals lower to.  Scalar
  (`i64`, `f64`, `bool`, `char`), optional and `box` slots have addresses, and so has a
  `mem` local.  A backend plugin must handle them.
- PIR: inline optionals (#96, docs/pir.md §1 and §6).  Four value types, `opt_i64`,
  `opt_f64`, `opt_bool` and `opt_char` (a bool presence flag, then the payload at its
  alignment: 16 bytes for `opt_i64` and `opt_f64`, 2 for `opt_bool` and `opt_char`), and four
  instructions, `some`, `none`, `is_some` and `unwrap`; optional slots have addresses and
  may be fields.  A backend plugin must handle them.
- An optional is not an object (#96): a `T?` no longer converts to `Obj`, so `o: Obj = maybe`
  and passing, returning or storing a `T?` where an `Obj` is expected are errors (`cannot use
  optional 'Node?' as 'Obj': an optional is not an object; unwrap it with match`).  The print
  family takes an optional as it is and prints `None` or the value, as before.  A `view` is
  never converted: a `view T` cannot be passed to a `view T?` parameter or bind a `view T?`
  local (`'v' is a 'view' parameter; a 'view' is not converted, and parameter 1 of 'take' is a
  'view int?'`); a value that is not a `view` still widens.
- An optional primitive (`int?`, `float?`, `bool?`, `char?`) is a value, the primitive and a
  presence flag held inline (#96), instead of a heap box: a local, parameter, result, field,
  array element or tuple element of one allocates nothing and is never reference counted, and
  `int<Str>` & co. return one without allocating (the runtime's `PaykanString_parse_int` &
  co.).  The semantics are unchanged.  An `int?[3]` holds its elements inline, 16 bytes
  each, and a tuple holds an `int?` element inline at its offset.  In #96's benchmark
  (10M pushes onto a `Stack<int>` and 10M pops through `pop() -> T?`) popping through the
  `int?` allocates nothing and takes as long as popping a plain `int`.

- Tuples are values held in place (docs/language/10-tuples.md, "Tuples as Values"), like
  `T[n]`: a tuple's elements are laid out like a C struct's fields in the variable, field
  or array element that holds it, a reference element as its box and an `int?` element or
  a nested tuple inline.  Assigning, passing or returning a tuple copies it and retains
  the references it holds, which each copy releases in turn; a tuple of `int`s allocates
  nothing.  A tuple is not an `Obj`: `o: Obj = (1, 2)` is an error (`initializer of type
  '(int, int)' does not match declared type 'Obj' for variable 'o'`), and `Tuple` is no
  longer a builtin class name (`x: Tuple` is an unknown type, and a program may declare a
  class `Tuple`).  `==` / `!=`, printing and `toString()` are unchanged, and a tuple meets
  a `Show` bound; `t.equals(u)` is gone (`method call on non-class type '(int, int)'`):
  compare with `==`.  A `view` of a tuple of values is copied when it is assigned; one
  holding a reference follows the `view` rules (`'p' is a 'view' parameter; it cannot be
  stored, ...`).  Runtime: the `PaykanTuple` object (its vtable, `PaykanTuple_new`,
  `_get`, `_set`, `_set_obj`, `_destroy`) is replaced by `PaykanTuple_toString(value,
  layout)` and `PaykanTuple_equals(a, b, layout)` over a tuple layout the compiler emits
  per tuple type (`Runtime.h`, "Tuples"), and `PaykanTypeKind` gains `PAYKAN_KIND_TUPLE`:
  a generic native function's `T` that is a tuple crosses by address, its references at
  their offsets.  In PIR a tuple is `mem` storage, passed and returned by address like a
  fixed-size array, and its layout a `.tupleN` `data` global (docs/pir.md §1, §4, §7);
  the lowering emits no `bytes` globals, and a `.pkm` file's `bytes` may hold any values.

### Removed

- The `mov` keyword (#145).  Using it is now an error: `'mov' was removed in v0.2.0;
  ownership transfers are inferred` (also from the AST interchange reader for a
  `(mov ...)` node).  Drop the keyword: `t = mov s` becomes `t = s`, with the same output
  and the same objects freed; `s` simply stays readable.  Until the last-use pass (#186)
  lands, each former `mov` costs one retain/release pair.  The use-after-move,
  loop back-edge and "cannot 'mov' ..." diagnostics went with it.  `mov` stays reserved.
- The boxed primitive classes `Int`, `Float`, `Bool` and `Char` (#96), now that an optional
  primitive is a value: with them went their conversions `Int<Str>`, `Float<Str>`,
  `Bool<Str>` and `Str<Int>` & co. (use `int<Str>` & co., which return `int?` & co.), and
  the runtime's `PaykanInt` / `PaykanFloat` / `PaykanBool` / `PaykanChar` (`Basic.c`).  The
  names are no longer reserved, so a program may declare a class `Int`.

### Fixed

- A `weak` local read from a `view` (`weak h = self.head;` in a `view fn`) was an ordinary
  reference once matched, so a `view fn` could return part of `self` as a copy and the caller
  could change it through a `view`.  Its reads are `view`s now: a `match` arm's name for one
  is bound to a `view`, which cannot be changed, stored, returned or captured
  (`'n' is bound to a 'view'; it cannot be returned`).  A `-> view T` function may return
  a `match` arm's name for a `view` of `self` or of a parameter.  `LinkedList`'s `front()`
  and `back()` and `Stack` and `Queue`'s `peek()` return a `view T`.
- A `view fn` of the standard library handed out an object element as a copy, so a `view`
  of a container could change its elements: `List`'s `get` and `last`, `HashMap`'s `get`
  and `getOr` and the priority queues' `peek` return a `view` now (`view fn get(i:
  unsigned) -> view T`; `getOr` takes `otherwise: view V`), and `memory::Buffer`'s `copy`
  and `copyUnchecked` are no longer `view fn`s (a `view` of a buffer reads with `get`).
  A value element is still copied out (`n = xs.get(0);`); an object element is read or
  bound to a `view` local (`f: view = fs.get(i);`).
- `"a" + n` and `n + "a"` with a number `n` passed type checking and then failed in PIR
  verification ("arithmetic on obj"); they are a type error now, `operator '+' is not
  defined for types 'Str' and 'int' (convert the 'int' with 'Str(...)')`.
- A process that compiles two programs importing the same source module (the test
  binaries, an embedding host) no longer fails to lower the second: the module's analysis
  is cached with its exports (`Sema::ModuleContextCache`), so every compilation that
  imports it can lower it.
- A conditional expression whose branches are calls of functions returning nothing, as a
  statement (`if c then f() else g();`) in a function that returns a value, failed PIR
  verification (`control can fall off the end of a non-void function`): the rest of the
  function was emitted inside the branch.  One branch now runs for its effect.
- A top-level declaration named after a builtin type (`class int { ... }`, `struct u8`,
  `enum unsigned`, `fn float(...)`, under any of the types' spellings) was accepted and
  silently shadowed by the builtin; it is now rejected (`'u8' is a builtin type and cannot
  be redeclared`), as a declaration named after a builtin function or class is.
- `Str(s)` with a fresh string argument — a method call's result (`Str(y.toString())`), a
  conversion's or a concatenation's — failed PIR verification (the call was classified as
  not holding a box while it passed its argument's box through); the `Str` constructor now
  takes its argument's ownership classification, so the string is passed on and released
  once.
- Calling `__init__` on an object (`c.__init__(2)`) stopped the compiler in the lowering;
  it is now an error (`'__init__' cannot be called directly; it runs when a value is
  constructed ('C(...)')`).
- A generic class whose method instantiates its template with an ever-growing type
  argument (`Deep<Deep<T>>`) is now reported (`exceeds the maximum instantiation depth`)
  instead of instantiating without end, as a field's type already was.
- A literal argument of `__super__(...)` was typed on its own instead of by the superclass
  `__init__`'s parameter: `__super__([1, 2])` for a `float[2]` parameter was an error
  (`argument 1 of '__super__' has type 'int[2]', expected 'float[2]'`), as was `None` or
  `(None, 3)` for an optional or a tuple of optionals.  It now takes the parameter's type,
  as a call's argument does.
- The llvm backend's `run` (and `--emit-llvm`) optimised the program under LLVM's default data
  layout instead of the host's, which the JIT then ran it with: an object with a `char` or
  `bool` field before an `int` was allocated a few bytes short, and stores could land at the
  default layout's offsets.  The module now gets the host's layout before it is optimised.
- On macOS, programs are compiled and linked for the deployment target the
  runtime was built for.  CMake 4 builds the runtime for the host's macOS
  version, which can be newer than the system `cc`'s default; `ld` then
  warned on every link ("built for newer 'macOS' version") and the warning
  ended up in the program's stderr.

## [0.1.1] - 2026-10-06

A patch release of 0.1.0.  It accepts plugins built with 0.1.1 and 0.1.0:
no plugin interface changed.

### Added

- The release tarballs (Linux x86-64, macOS arm64) and the Debian package
  ship the llvm backend next to the c backend: `--backend=llvm` runs
  programs with the ORC JIT and builds them as native objects.  LLVM is
  linked into `paykan` statically, so nothing else is needed at run time.
  The c backend stays the default.  The Homebrew formula still builds the
  core (c only).

### Fixed

- The llvm backend builds with GCC 15 (#163): libstdc++ 15 warns on LLVM
  17's `<ciso646>` include in C++20, which `-Werror` made fatal.  The
  targets that include LLVM headers no longer turn warnings into errors;
  the move to LLVM 20 (#164) removes the warning itself.
- `find_package(Paykan)` no longer requires LLVM when the installed
  PaykanLang has the llvm backend: only a driver that links the llvm
  backend (`paykan_add_driver`) needs it, so plugins build against the
  Debian package without LLVM installed.
- The Homebrew formula has the real checksum of the release's source
  archive (#161).

### Changed

- The tests are no longer built by default: a plain `cmake -B build` builds
  only the compiler and downloads nothing.  They are now in suites
  (listed once, in `tests/suites.json`): `-DPAYKAN_BUILD_TESTS=<suites>` builds the
  named ones (a CMake list, e.g. `"parser;sema"`) and
  `-DPAYKAN_BUILD_ALL_TESTS=ON` all of them.  `PAYKAN_BUILD_TESTS=ON` is an
  error that points to `PAYKAN_BUILD_ALL_TESTS`.
- CI builds and runs only the test suites a pull request can affect
  (`scripts/affected_tests.py`); pushes still run every suite.

## [0.1.0] - 2026-10-06

The first tagged release. PaykanLang is now split into a core, pluggable
frontends and pluggable backends around a backend-neutral IR (PIR); the
default build needs only CMake and a C++20 compiler, plus a C11 compiler at
run time. Out-of-tree frontends and backends are loadable modules with a C
interface that the installed `paykan` loads at startup. Tuples, optional types
(optional primitives and optional-mode `match` included) and generics are
stable in v0.1 (see "Stable in v0.1" under Added).

### Breaking changes

- **The default build is the core: the recursive-descent frontend and the C
  backend (#123).** A plain `cmake -B build` builds only those two and
  downloads nothing. The LLVM backend, with its JIT, is opt-in at configure
  time: `-DPAYKAN_BACKENDS="llvm;c"`. The release tarballs and the Homebrew
  formula ship the core only.
- **The default backend is c, in every build (#27).** The default backend
  changes from llvm to c, and stays c even when the LLVM backend is built:
  with `-DPAYKAN_BACKENDS="llvm;c"` (in any order) `--list-backends` shows
  `c (default)`. Running a program through the JIT now needs
  `--backend=llvm`; without it, `paykan` runs programs with the C backend,
  which needs a C11 compiler (`cc`, or `$CC`) at run time. A configuration
  without the c backend (never produced in tree) falls back to the first
  backend listed. The default frontend stays recursive-descent.
- **The Bison frontend moved to its own repository (#60).** The hand-written
  recursive-descent frontend (standard C++ only) is the only in-tree frontend
  and the default; the Bison/Flex frontend (#17), which implements the same
  grammar (`docs/grammar.md`) and builds the same AST, is the out-of-tree plugin
  [PaykanLang_Bison_Frontend](https://github.com/parsabee/PaykanLang_Bison_Frontend),
  built against an installed PaykanLang with `find_package(Paykan)`; it
  produces a `paykan` driver with `--frontend=bison`. PaykanLang no longer
  builds or downloads Bison, Flex or m4: `bison` is no longer
  accepted in `PAYKAN_FRONTENDS` (which lists in-tree frontends only), and
  the full build is `-DPAYKAN_BACKENDS="llvm;c"`. The frontend differential
  check (`scripts/diff_frontends.py`, the "Frontend differential" CI job)
  moved with it. `--trace-parser` / `--trace-scanner` stay, for any
  frontend that has traces.
- **Conversion constructors replace the conversion builtins (#64).**
  `StrInt`, `StrFloat`, `StrBool`, `StrChar`, `IntStr` and `FloatStr` are
  removed. Every conversion is now spelled `Target<Source>(value)`, with the
  source type always written and the argument required to have exactly that
  type: `Str<int>(n)`, `Str<float>(f)`, `Str<bool>(b)`, `Str<char>(c)`,
  `int<Str>(s)`, `float<Str>(s)`. The old names are no longer reserved: a
  call to one is an ordinary undeclared-function error. The `Str<…>` formatting is unchanged,
  including the canonical `nan` / `inf` / `-inf`.
  - **Parses return optionals.** `int<Str>(s)` returns `int?` and
    `float<Str>(s)` returns `float?`, with `None` for an invalid string. They
    used to return an `Obj` holding an `Int` / `Float` box or an `Error`.
    Code that matched on `n: Int { … } err: Error { … }` now matches on
    `n: int { … } None { … }`.
  - **Stricter parsing.** Leading whitespace and embedded NUL bytes now make
    a parse fail (`strtoll` / `strtod` used to skip leading whitespace).
- **New numeric conversions (#64).** These are defined in the lowering, so
  both backends behave identically, and none of them is undefined behaviour
  in the generated C:
  - `int<float>` truncates toward zero, and panics on NaN, ±inf or a value
    outside the int64 range.
  - `float<int>` gives the nearest double.
  - `int<bool>` gives 1 / 0, and `bool<int>` is `!= 0`.
  - `int<char>` gives the char's byte code 0..255, and `char<int>` is its
    inverse, panicking outside 0..255.
  - Any other pair is an error that lists the valid sources. PIR gains a
    numeric `ftoi` instruction, which the lowering emits only after the
    range guard.
- **Conversion constructors are a closed set of specializations, and the
  type argument is optional (#88).** Each builtin target (`Str`, `int`,
  `Int`, `float`, `Float`, `bool`, `Bool`, `char`) has a fixed set of valid
  sources, and no other type, a user class included, can be one.
  - **Inferred form.** `Target(value)` picks the specialization whose source
    is exactly the argument's type: `Str(n)` with an `int` is `Str<int>(n)`,
    `int(f)` is `int<float>(f)`, `int(s)` is `int<Str>(s)` (an `int?`). No
    widening or unwrapping is applied. The explicit `Target<Source>(value)`
    still works, with the argument required to have exactly that type.
    `Str(s)` with a `Str` is still the `Str` constructor.
  - **New boxed forms.** `Str<Int>`, `Str<Float>`, `Str<Bool>` and
    `Str<Char>` format a box exactly like the primitive form. `Int<Str>`,
    `Float<Str>` and `Bool<Str>` parse like `int<Str>` & co. and return the
    optional box (`Int?`, `Float?`, `Bool?`). `Int`, `Float` and `Bool` have
    no other constructor, so `Int(5)` is an error.
  - **`bool<Str>` (new)** returns `bool?`: `True` / `False` for exactly
    `"True"` / `"False"`, the spellings `Str<bool>` prints, and `None` for
    anything else. The runtime gains `PaykanBool_from_str`.
  - **New diagnostic.** Both forms now report a missing pair as `no
    specialization of 'Str' for 'A'; its specializations are int, float,
    bool, char, Int, Float, Bool, Char`, replacing `no conversion from … to
    …`. This covers user classes, `Obj` (a bare `None`), optionals and the
    other builtins. It also replaces `a conversion to 'int' names its source
    type`, since `int(2.5)` is now valid.
  - The samples and the language reference use both forms.
- **Modules are named canonically, and `-O2` is the default (#102).** Every
  module is identified by its canonical module name (`geometry::shapes`,
  `::io` for a system module; the main module by its file stem), never by its
  file path, so no absolute path reaches the PIR, the generated C, symbol
  names or cache keys, and builds of one project in two directories are
  identical. Cache entries are named after the module
  (`.paykan_cache/geometry/shapes.bc`, `shapes.<hash>.{c,o,key}`, system modules under
  `@system/`), and the generated-code ABI version is 6, so older caches are
  rebuilt. `paykan run` and `paykan build` optimise at `-O2` by default (was
  `-O0`); pass `-O0` for unoptimised code.
- **Builtin names are reserved.** A top-level function, class or enum named
  like a builtin function (`print`, `println`, `printerr`, `printerrln`,
  `Str`, `open`) or a builtin class (`Obj`, `Str`, `Array`, `File`, `Error`,
  `Int`, `Float`, `Bool`, `Char`) is a compile-time error at the
  declaration (`'print' is a builtin function and cannot be redeclared`). A
  class silently replaced the builtin of the same name before. One namespace
  holds every top-level name, so a function, class or enum cannot reuse
  another's name either. Fields, methods, parameters and locals are not
  affected.
- **Method symbols are `<Class>.<method>` (#86).** Methods and destructors
  were lowered to `<Class>_<method>`, so a user `fn K_w()` next to
  `class K { fn w() … }` (or `fn K_destroy()`) clashed with the method and
  failed with an internal error. The `.` cannot occur in a Paykan
  identifier, so the names can no longer clash; the LLVM backend's vtables
  are `<class>..vtable`. Code built against the old names, and the
  bitcode cache, must be rebuilt: the generated-code ABI version was bumped
  (to 5, and to 6 by #102).
- **`==` / `!=` on reference types dispatch to `equals`, so `Str ==`
  compares contents.** For classes, `Str`, and arrays, `a == b` now calls
  the virtual `equals` method (and `!=` its negation). `Str` comparison is
  therefore **content** equality (`"hel" + "lo" == "hello"` is `True`; it
  compared identities before); arrays keep identity comparison via
  the default `equals`; comparing arrays of different element types is a
  compile error. Overriding `equals` in a class changes how `==` behaves for it.
- **Backend plugins are loadable modules with a C interface (#141).** An
  out-of-tree backend is no longer a static C++ library linked into a
  `paykan` driver of its own: it is a shared library (`.so`, `.dylib`) with
  the pure C11 interface of `include/paykan/plugin_api.h`, which the
  **installed** `paykan` loads at startup, with no rebuild of PaykanLang. It
  links nothing of PaykanLang, so it can be written in any language with a C
  layer (C, C++ with any compiler, Rust, ...). It receives the verified
  program as PIR text and talks to `paykan` through a host table
  (diagnostics, output, allocator, log, the runtime's paths, link and run
  helpers). For plugin authors: `paykan_add_backend_plugin()` now builds such
  a module (`MODULE`, `lib<target>.so` / `.dylib`) instead of a static
  library linked with `Paykan::backend`, and a backend written against
  `Backend.h` must be ported to `plugin_api.h` (see
  `docs/writing-a-backend.md`). `src/Backends/PrintPIR` is now a plain-C plugin. The
  static C++ path (`paykan_add_driver`) stays as an advanced option, and the
  built-in plugins keep their in-process C++ interface.
- **Frontend plugins are loadable modules too (#141).** A frontend plugin
  receives the source text through the same C interface and returns the
  program as text in the new, versioned **AST interchange format**
  (`docs/plugins/ast-format.md`, an S-expression form with source
  locations), which `paykan` reads, checks and hands to Sema; syntax errors
  go through the host's diagnostics. `paykan_add_frontend_plugin()` now
  builds such a module instead of a static library linked with
  `Paykan::frontend`, and `paykan_add_frontend_tests()` takes the plugin as
  `PLUGIN <target-or-file>` (instead of `PLUGINS <lib>...`): the suites load
  it with the installed `paykan`'s loader, and a new
  `InstalledPaykan.<frontend>` test runs the installed `paykan` with only
  the plugin loaded over the samples corpus.

### Added

- **A Debian/Ubuntu package.** Each release carries
  `paykanlang_<version>_amd64.deb`, built by CPack (`cmake/Packaging.cmake`,
  `cpack -G DEB`) from the core build and installable with
  `sudo apt install ./paykanlang_<version>_amd64.deb`; it depends on `gcc` or
  `clang` for the C backend.
- **The release tag is checked against the built version.** The version is
  spelled only in the top-level `CMakeLists.txt` (`project()` plus
  `PAYKAN_VERSION_PRERELEASE`); the accepted plugin list starts with it, and
  the release workflow fails when a tag `vX.Y.Z` does not match
  `paykan --version`.
- **Stable in v0.1: tuples, optional types and generics (#27).** They are no
  longer marked prototype/experimental: their documented syntax and semantics
  (`docs/language/10-tuples.md`, `10-optionals.md` with optional primitives
  and optional-mode `match`, `11-generics.md`) are covered by the
  compatibility promise for the 0.1 series. The documented limitations stay: importing generics
  across modules is not supported until v0.2.0 (#57), and a present optional
  primitive is boxed (#96 changes that representation, not the semantics).
  Plugins are versioned separately: the C plugin interface by its plugin API
  version, plugin builds by the release's list of accepted versions (#103).
- **Pluggable frontends (#16, #17).** The frontend (lexer and parser) is a
  plugin behind an interface (`include/paykan/Frontend.h`) and a registry:
  `-DPAYKAN_FRONTENDS=<list>` picks the frontends to build,
  `--frontend=<name>` selects one at run time and `--list-frontends` lists
  them. The grammar every frontend implements is specified in
  `docs/grammar.md`, and `--dump-tokens` prints a frontend's token stream.
- **Pluggable backends and PIR (#16, #35, #48).** Programs are lowered from
  the AST to PIR, the backend-neutral Paykan IR (`docs/pir.md`), in which
  every type, retain/release, vtable and scope cleanup is explicit; every
  backend consumes PIR. `--emit-pir` prints it, and its text form parses
  back (the `PIRRoundTrip` tests round-trip the whole samples corpus).
  `-DPAYKAN_BACKENDS=<list>` picks the backends to build, `--backend=<name>`
  selects one and `--list-backends` lists them; `--emit-source` prints a
  backend's output.
- **The C backend (#16, #36).** `--backend=c` translates PIR to strict ISO
  C11 (`docs/c-backend.md`) and compiles it with the system C compiler. It
  is part of the core: standard C++ only, no third-party library. `run`
  builds into a temporary directory and runs the program; `--emit-c` prints
  the C. Imported modules are compiled once and cached
  (`.paykan_cache/`, keyed by the generated C, the compiler and its flags).
- **The LLVM backend translates PIR (#38).** The old AST code generator is
  gone: the llvm backend lowers PIR to LLVM IR and runs it with the ORC JIT
  (`--emit-llvm` prints it).
- **`paykan build` (#36, #38).** `paykan build prog.pkn -o prog` compiles a
  native executable with either backend (the llvm backend emits an object
  file and links it with the system C compiler). `main`'s return value is
  the exit code, and `main(args: Str[])` receives the command line.
- **Out-of-tree plugins (#37).** `cmake --install` installs the interfaces
  and `find_package(Paykan)`, so a backend or frontend can be built outside
  the tree and linked into a custom `paykan` (`docs/writing-a-backend.md`).
  `src/Backends/PrintPIR` is the smallest complete backend; CI and the
  `PrintPIROutOfTree` ctest build it against an installation.
- **Documented: selective imports, `Stdin`, `File.readbytes` / `File.read`
  and system imports.** These worked before but were not listed:
  `import lib::{foo, bar as b};` imports several modules of one directory;
  `Stdin` is a builtin `File` over standard input; `readbytes(n)` and
  `read()` read `n` bytes or the rest of a file (`None` at the end);
  `import ::io;` looks a module up in `$PAYKAN_STDLIB` (no standard library
  ships yet, #113). See `docs/language/02-basics.md` and
  `06-modules.md`.
- **Generics.** Generic classes (`class Box<T> { … }`,
  `class Pair<K, V> { … }`) and generic free functions
  (`fn first<T>(xs: T[]) -> T`), implemented by monomorphisation in Sema: each
  distinct type-argument tuple instantiates the declaration once into an
  ordinary class/function named `Box<int>`, `Pair<Str, int>`, `first<int>`.
  Type arguments are accepted in every type position (`Box<int>[]`,
  `Box<Box<int>>`), on constructor calls (`Box<int>(3)`) and on calls
  (`first<int>(xs)`); a generic function's type arguments are inferred from
  the argument types (also through `T[]` and `Box<T>`), as are a generic
  class's from its `__init__` arguments (`Box(3)`). Bodies are checked per
  instantiation; an error inside one names it (`in instantiation of
  'Box<bool>' requested here`). Templates are not exported across modules
  (`mod::Box<int>` is rejected with "generic types cannot be imported yet");
  a module's own instantiations are exported as concrete classes. No
  constraints, variance, defaults or specialisation yet — see
  `docs/language/13-generics.md`.
- **Optional types `T?` (issue #5).** Any reference type (a class,
  `Str`, or an array) has an optional form `T?` holding either a `T` or
  `None`. `T` widens to `T?` implicitly; a `T?` never narrows back without a
  `match`, whose `T` arm binds every present value and whose `None` arm (or
  `_`) covers the absent case. `x == None` / `x != None` is a null check,
  two optionals compare with the usual `equals` once both are present, an
  optional field is implicitly `None` if `__init__` does not assign it, and
  `T?[]` / `T[]?` are supported. At runtime a `T?` is the same reference-
  counted box as a `T` with "no box" meaning `None` — no layout change.
  Nested optionals, flow typing, `if let`,
  `??` and `?.` are not supported in v0.1. See
  `docs/language/12-optionals.md`.
- **Optional primitives `int?`, `float?`, `bool?`, `char?` (#66).** A
  primitive widens to its optional implicitly (`x: int? = 5`; an `int` also
  widens to `float?`), and `None` is the absent value. In `match o { n: int
  { … } None { … } }` the arm binds a plain `int`. `== None`, equality of two
  optionals, fields (implicitly `None`), arrays, tuples, parameters, returns,
  ternaries and generic arguments (`Box<int?>`, and `T?` with `T = int`) all
  work. A present value is boxed in the runtime's `Int` / `Float` / `Bool` /
  `Char` object (`Char` is a new boxed class), so it prints and matches as
  that class in an `Obj` slot. `Enum?` is still rejected, now with "optional
  enum types are not supported yet". `int?` does not convert to `float?`. See
  `docs/language/12-optionals.md`.
- **Tuples (#4).** Fixed-arity, heterogeneous, immutable values:
  types `(int, Str)` (nesting, `(int, Str)[]` and `(int[], Str)` allowed),
  literals `(1, "a")`, compile-time-checked element access `t.0` / `t.1.0`,
  multiple return `fn f() -> (int, int)`, and destructuring
  `q, r = divmod(7, 2);` (`_` skips, `q: int, r: int = ...` annotates).
  Tuples are `Obj` subtypes under ARC (retain/release/`mov` like any
  reference value); `==` / `!=` compare element-wise and `toString` renders
  `(1, a)`. Tuple-typed signatures round-trip through module imports.
  Not yet: tuples in `match`, nested destructuring, mutation, named elements.
  See `docs/language/10-tuples.md`.

- **Move semantics with `mov`.** `mov <expr>` transfers ownership of a local
  variable, parameter, or temporary without a retain/release pair. Use of a
  moved variable is a compile error until it is re-assigned, tracked
  flow-sensitively: a `mov` in one `if`/`match` branch does not poison sibling
  branches, paths are merged conservatively after the construct, and moving a
  variable declared outside a loop without re-assigning it before the loop
  repeats is rejected. `mov self` and moving fields or array elements are
  compile errors. See `docs/language/16-memory.md`.
- **`-DPAYKAN_BUILD_TESTS=OFF` builds without the test suite (#77, #78).** It
  skips GoogleTest, so a barebones configure downloads nothing (the default is
  ON when PaykanLang is the top-level project). With tests on, an installed
  GoogleTest (`find_package(GTest CONFIG)`) is used before downloading one.
  The Homebrew formula and the release workflow build with tests off. CI and
  the release workflow can be run by hand (`workflow_dispatch`); a manual
  release run is a dry run by default and publishes nothing. JIT errors in the
  CodeGen test harness are now reported in the test's stderr.
- **Run-time plugin loading (#141).** `paykan` loads plugins from, in order:
  `--plugin=<file>` (repeatable), each directory of `$PAYKAN_PLUGIN_PATH`,
  `~/.paykan/plugins/<version>/`, and the installation's
  `lib/paykan/plugins/<version>/` (found relative to the executable, and
  installed empty). `--no-plugins` / `PAYKAN_NO_PLUGINS=1` turn the directory
  search off. Each plugin's descriptor (from its one entry point,
  `paykan_plugin_init`) is checked before any of its callbacks run: the
  plugin API version (`PAYKAN_PLUGIN_API_VERSION`, 1), then its build
  version against the release's list (#103). An incompatible plugin is
  listed with the reason and its file, and selecting it exits with status 2;
  a file that can't be loaded, has no entry point or a malformed descriptor
  is listed as `rejected plugin <file>: <why>` (and stops a compile with
  status 2 when named with `--plugin`); a name two plugins provide is
  ambiguous and can't be selected. `--list-backends` and `--version` show
  each loaded backend's file; `--version` also prints the plugin API version,
  the plugin directories and every plugin file. The dynamic loader sits
  behind the portability layer (POSIX `dlopen`; a documented Windows stub).
  The CMake package adds `Paykan::plugin_api`, `PAYKAN_PLUGIN_API_VERSION`,
  `PAYKAN_PLUGIN_INSTALL_DIR`, `PAYKAN_EXECUTABLE`,
  `paykan_install_plugin()` and `paykan_check_plugin_built_with()`.
  New docs: `docs/plugins/` (overview, the C API, the AST format, PIR for
  backends).
- **The AST interchange format (#141)**: its writer and reader in the core
  (`paykan/ast/Interchange.h`, `Paykan::ast_interchange`), `paykan
  --emit-ast` to print a program in it, and `src/Frontends/ASTText`, a
  plain-C frontend whose source language is the format itself. The writer
  and reader round-trip the AST of every program in the samples corpus.
- The installed static libraries are built as position-independent code, so
  a plugin may link them into its module (a C++ frontend reusing the core's
  AST and writer internally).
- **Plugin compatibility check (#103).** Every frontend and backend plugin
  records the PaykanLang version it was built with:
  `PAYKAN_REGISTER_FRONTEND` / `PAYKAN_REGISTER_BACKEND` capture
  `PAYKAN_PLUGIN_BUILD_VERSION` from the installed headers
  (`paykan/PluginCompat.h`, generated). Each release holds an explicit list of
  the plugin build versions it accepts, in one place
  (`cmake/PluginCompat.cmake`), compiled into the core and exported in the
  CMake package as `PAYKAN_PLUGIN_COMPATIBLE_VERSIONS`. Versions match as
  exact strings, pre-release label included. The registry checks a plugin at
  registration and at selection; a plugin not on the list is listed by
  `--list-frontends` / `--list-backends` as
  `<name> (incompatible: built with PaykanLang <v>; this paykan <v> accepts
  <list>)`, is never instantiated, and selecting it with `--frontend=` /
  `--backend=` fails with exit status 2. `paykan --version` lists the
  accepted versions and every plugin with its build version and
  compatibility. The built-in plugins go through the same check.
- **`paykan_add_frontend_plugin()` / `paykan_add_backend_plugin()`** in the
  CMake package: they add a plugin library and fail at configure time when
  the installed Paykan does not accept the version the plugin is built with
  (by default the installation's own, `PAYKAN_TOOLCHAIN_VERSION`, or the one
  pinned with `BUILT_WITH <version>`). `src/Backends/PrintPIR` uses
  `paykan_add_backend_plugin()`. See `docs/writing-a-backend.md` (section 7)
  and the new `docs/writing-a-frontend-plugin.md`.
- This release accepts plugins built with `0.1.0`. From now on each
  release's entry states the plugin build versions it accepts.
- **Test support for out-of-tree frontends (#60).** An installation carries
  the frontend-parameterized parser and Sema suites, the fuzz smoke and
  differential tests and the samples corpus, and `find_package(Paykan)`
  provides `paykan_add_frontend_tests()`, which builds them against a
  plugin's frontend (`docs/writing-a-frontend-plugin.md`).
  `-DPAYKAN_INSTALL_TEST_SUPPORT=OFF` leaves them out.

### Changed

- **Repository layout.** The language reference moved from its top-level
  directory into `docs/language/` (same file names). The example
  out-of-tree backend moved from `examples/backends/print-pir` to
  `src/Backends/PrintPIR`. `example_program/` is gone: its `calc` program is now
  the multi-module sample `samples/imports/12_calc`, with its expected
  output, so every harness runs it. `PLAN-0.1.md` (superseded by #27) and
  `proposals/` were removed; the normative parts of the proposals are in
  the reference (`docs/language/10-tuples.md`, `10-optionals.md`,
  `11-generics.md`). New ctests: `MarkdownLinks` (`scripts/check_links.py`)
  and `DocExamples` (`scripts/doc_examples.py`, which runs every doc example
  that states its output).
- Runtime object header grew by 8 bytes: every heap object now carries a
  backpointer to its reference-count box (the unique-box invariant above).
- Cached import bitcode is stamped with an ABI version; caches written by a
  compiler with a different (or missing) ABI stamp are recompiled instead of
  loaded.
- Parse errors changed format from yacc-style one-liners to clang-style
  caret-and-snippet diagnostics.
- The C backend's output is strict ISO C11 (#62): it compiles with
  `-std=c11 -pedantic-errors -Wall -Wextra -Werror` on GCC and Clang (the
  new `CStrictC11` ctest and *Strict C11* CI job, over the whole samples
  corpus), so the C compiler no longer runs with `-w`. Every vtable, the
  runtime's included, is now an array of `PaykanMethod` slots (`Runtime.h`,
  `PAYKAN_SLOT_*`) instead of a struct of function pointers, and `Runtime.h`
  checks the target assumptions (LP64, 8-bit bytes, two's complement,
  IEEE 754 doubles) with `_Static_assert` (`docs/c-backend.md`).
- **Dependencies.** PaykanLang depends only on standard C and C++; GoogleTest
  (tests), LLVM (the opt-in `llvm` backend) and Python 3's standard library
  (the scripts) are the only other tools it uses. The script-driven ctests
  now run with the Python CMake finds (`find_package(Python3)`: the system's,
  or `-DPython3_EXECUTABLE=<path>`) instead of whatever `python3` is on
  `PATH`, and are skipped when there is none. The example plugin backend
  lives in `src/Backends/PrintPIR`.

- **The plugin registration ABI (#103).** `paykan::plugin::Registration`
  takes a `PluginInfo { Name, Create, BuildVersion }` (plain data, the version
  a `const char *`), and registry entries carry `BuildVersion`, `Compatible`
  and `Incompatibility`. `Registry::create()` returns null for an
  incompatible plugin. Plugins that register through the macros need only a
  rebuild. The check lives in a new core library, `paykan_plugin`
  (`Paykan::plugin`), which `Paykan::frontend` and `Paykan::backend` link.
- `paykan --version` prints one more line (`accepts plugins built with
  PaykanLang <list>`), and each plugin line now ends in
  `(built with PaykanLang <v>, compatible)`; a backend's description follows
  after `: ` instead of in parentheses. With #141 it also ends with
  `plugin API <n>`, `plugin directories: ...` and one line per plugin file,
  and a loaded plugin's lines end in ` [<file>]`.
- Registry entries (`paykan::plugin::Registry<I>::Entry`) also carry the
  plugin's `Path`, `Description` and `Conflict`, and their factory is a
  `std::function` (#141).

### Fixed

- Two modules (or the main file and an import) that each define a free
  function with the same name no longer make the lowering fail with an
  internal error (#70): `x::tag`, `y::tag` and a local `tag` each call
  their own module's function. Two imports that bind one qualifier to
  different modules (`import a::util; import b::util;`) are now an error.
- A tuple or array literal flowing into a typed slot takes the slot's
  element types (#71): `(None, "a")` is an `(int?, Str)` and
  `[("a", 1), ("b", nb)]` a `(Str, int?)[]` where the destination says so.
- A string literal as a `match` subject (`match "s" { "t" { … } _ { … } }`)
  failed PIR verification (#72); it is now a `Str` like any other subject.
- Float range (#73): subnormal values are accepted everywhere (float
  literals, `float<Str>`, PIR constants), and a float literal that
  overflows to infinity, or a nonzero one that underflows to zero, is a
  compile-time error (`float is out of range: <text>`).
- The C backend rebuilds a cached object that was truncated or corrupted
  after it was cached (#74), instead of failing every later link until
  `.paykan_cache` was deleted.
- A user function named `<Class>_<method>` no longer clashes with that
  class's method (#86); see *Breaking changes*.
- `float` `!=` is IEEE 754's unordered not-equal (#58): `nan != nan` is
  `True` on both backends.
- The GCC 13 Release build compiles under `-Werror` (#76).
- A string literal used as an object (`"ab"[1]`, `"abc".len()`, `mov "lit"`
  in any position, a literal in a tuple, array or optional) is now a `Str`
  object; it was passed as its raw C string, an internal compiler error
  (#116). User functions spelled like runtime symbols (`PaykanString_new`,
  `Paykan_println`, `Paykan_panic_div_by_zero`, ...) no longer replace or
  clash with the runtime's, which was an internal compiler error or a silent
  miscompile (#117): runtime externs are now named `$rt.<symbol>` in PIR
  (`docs/pir.md` §3), a name no Paykan identifier can spell.
- **Diagnostics and driver (#119, #120, #126).** A failed import (missing,
  circular, a directory, or a module with errors) is reported once, and uses
  of its names are not reported again; import messages name modules
  canonically (`lib::m`) and files from the source root. `''` is one
  `empty character literal` error. `x: Str? = mov None` is accepted. `-O4`
  and above are rejected; a directory as the source file is a clean error.
  The Bison frontend enforces the 512-level nesting limit. `Stdin` and type
  names are rejected by every binder, including typed declarations and
  parameters.
- **Concurrent builds with different flags no longer link each other's
  objects (#134).** The C backend's cache checked a module's `.key` and
  linked its `.o` later; a concurrent `paykan` with other flags (a sanitizer,
  another `-O`) could replace the object in between, and the link failed
  (`undefined reference to __asan_init`). Entries are now named after a hash
  of their key (`.paykan_cache/geometry/shapes.<hash>.{c,o,key}`), so
  different configurations never share a file and alternating `-O0` / `-O2`
  builds no longer rebuild each other. Each module keeps its four most
  recently used entries (and any used within the hour); older ones are
  removed. The llvm backend's `.bc` cache was not affected.
- **Diagnostics follow-ups (#132).** A module that fails to load is reported
  once per compilation however many import paths reach it (twice from one
  file, or through a diamond); each later import adds a note. `mov None`
  inside an array or tuple literal takes the slot's element type
  (`xs: Str?[] = [mov None]`). Past the nesting limit, the recursive-descent
  frontend skips the nested block whole and reports nothing more. A missing
  or ill-typed `main`, or an empty file, is one Sema error with a source
  location (`program has no entry point 'fn main() -> int'`), on both
  frontends, instead of a PIR verifier failure; `--check-only` still accepts
  a module without `main`.
- An empty array literal `[]` stored into a class field, passed as a
  call/method/`push` argument, returned, stored through a subscript, or nested
  in another literal was compiled as a primitive-element array regardless of
  the slot's element type, so the objects later pushed into it were never
  released (a leak in every `self.items = []` pattern). Sema now adopts the
  destination's array type at every typed sink, not only declarations.
- A tuple holding a NaN compared equal to itself (#111): `t == t` was `True`
  and `t != t` `False` when both sides were the same tuple object, because
  the runtime's tuple `equals` returned "equal" for one object before looking
  at the elements. Tuple equality is now element-wise in every case, so a NaN
  element (directly, in a nested tuple, or in a `float?` element) makes a
  tuple unequal to itself, as IEEE 754 makes the NaN unequal to itself.
  `float?` already compared its values; arrays keep their reference-identity
  `==`.
- Passing an array or tuple to a user-defined method (`b.take(xs)`,
  `b.take([1, 2])`) freed the argument twice: the callee owns every
  reference-typed parameter, but the call site only handed over an owned box
  for class and optional parameters.
- Integers flowing into `float[]` element slots (`xs: float[] = [1, 2]`,
  `xs.push(3)`, `xs[0] = 4`, `[1, 2.5]`) were stored as integer bit patterns
  and read back as denormal floats; they are now promoted like scalar stores.
- The import cache key now includes the identity of the compiler binary, so a
  development build with changed codegen but an unchanged version string no
  longer serves stale `.paykan_cache` modules compiled by the previous build.
- Double-frees when an object was reached through more than one owning alias:
  using `self` as a value (return/argument/assignment/`mov`), ownership-taking
  uses of `match` bindings, and array-typed class fields. Fixed by a new
  unique-box runtime invariant — every heap object records its single
  reference-count box in its header, and creating an owning reference reuses
  that box instead of creating a second one.
- `push` / `pop` called on arbitrary receivers (call results, nested
  subscripts) no longer hit an internal assertion.
- `mov` soundness holes: moves in loops of loop-external variables (previously
  compiled, then crashed on the second iteration), false "moved" errors in
  sibling branches, and `mov self` (previously accepted) are now handled
  correctly at compile time.
- A `match` over both `bool` literals now counts as exhaustive for
  "always returns" and field-initialisation analysis.
- The imported-module bitcode cache no longer goes stale or lands in the
  wrong place: it now lives in `.paykan_cache/` under the project root (the
  main file's directory) instead of the current working directory, entries
  are keyed by a content hash of the module plus every module it transitively
  imports (and the compiler/ABI version) instead of file timestamps — so
  editing a dependency's class layout or signature recompiles its importers —
  and entries are written atomically; a corrupt or truncated entry is
  regenerated instead of being loaded.
- Declaring `self` as a method parameter is now a clear compile error instead
  of a confusing arity error at the call site.
- Parser diagnostics are now clang-style (caret + source snippet), matching
  semantic errors; a raw newline inside a string or character literal is
  rejected with a targeted message (use `\n`); the unused `&` token was
  removed from the lexer; out-of-range integer literals are reported reliably;
  an unopenable file no longer calls `exit()` from library code.
- One rejected class declaration (a builtin or imported name, an undefined
  superclass, a bad member type) no longer makes every other class of the
  module undeclared (#79): the other classes are still declared, the bad
  class is registered as far as it can be, and the follow-on errors on its
  uses are not reported.
- The recursive-descent frontend reports each syntax error once (#79): no
  follow-on error right after a lexical error (an unterminated string, an
  invalid character), one error (with a note at the open `{`) for blocks left
  unclosed at end of file instead of one per block, and one error for a
  multi-character literal such as `'ab'`.
- Output printed before a runtime panic is no longer lost when stdout is not
  a terminal (#92). A panic now flushes stdout (and every open `File`) before
  printing its message and aborting, on both backends, in `paykan run` and in executables from `build`;
  with `2>&1` the output comes before the panic message.
- A runtime panic ends `paykan run` the same way on both backends (#79): with
  exit status 134 (128 + `SIGABRT`). The llvm backend's JIT used to let the
  abort kill the compiler process itself; executables from `build` still die
  by `SIGABRT`.
- A qualified match arm on an enum (`base::Color::Green { … }`) now says to
  use the bare variant name (`'Green'`) instead of claiming it is not a
  variant (#79).
- A failed declaration reports only its own error (#89). A variable whose
  first assignment, initializer, type annotation, destructuring or match-arm
  pattern is in error is still declared, with an internal error type, so its
  later uses are no longer reported as `use of undeclared variable`, and
  nothing built on it (a call, an operator, a member access, a derived
  variable, ...) reports a follow-on either. A later valid assignment
  re-declares it with the value's type. Calls to a function whose signature
  failed are likewise not reported as calls to an undeclared function.
- Array `push` / `pop` no longer thrash at the capacity boundary (#95). An
  array now shrinks only once its length falls to a quarter of its capacity,
  and then to half the capacity, never to exactly its length; capacity never
  drops below 8 slots on its own. A push/pop loop at a length of 524,288 went
  from about 40 s to 15 ms. `--track-heap` also reports the number of
  reallocations.
- A use of another module's generic class or function (`shapes::Box<int>(7)`,
  `x: shapes::Box<int>`, `shapes::first<int>(xs)`, `g::Box<int>(7)`) inside
  a generic body is rejected once, not once per instantiation of that body
  (#112). Debug builds of the compiler now abort with an internal error when
  the identical diagnostic (same location, message and notes) is reported
  twice.
- The runtime lookup of `paykan run` / `build` no longer prefers a stale
  build tree over an installed binary's own prefix (#124). The order is now
  `$PAYKAN_RUNTIME_DIR`, the install prefix around the executable, the build
  tree (only for binaries inside it, marked by `.paykan-build-tree`), then the
  configured install prefix. `PaykanConfig.cmake` records the installed
  runtime (`PAYKAN_RUNTIME_LIBRARY` / `PAYKAN_RUNTIME_INCLUDE_DIR`), and
  drivers made with `paykan_add_driver` use it, so they work wherever they
  are built. The samples that write files put them in a directory the
  harness passes per run (`// args: {tmpdir}`), so parallel runs no longer
  race on fixed `/tmp` paths.
- Dropping a deep chain of objects (a long `Node?` list, a deeply nested tree,
  arrays and tuples nested inside each other) no longer overflows the C stack
  (#118). The runtime destroys objects iteratively, in constant stack space
  (`docs/language/16-memory.md`).
- **Documentation matches the compiler (#121).** The final audit's
  discrepancies are fixed. README: the build prerequisites and defaults.
  Language reference: array printing, selective imports
  (`import a::{b, c as d}`), system imports and `PAYKAN_STDLIB`, `File`'s
  `readbytes` / `read`, `Stdin`, the boxed types, `Str<int>` as a
  conversion, and diagnostics quoted as the compiler prints them.
  `docs/grammar.md`: the comparison-in-arguments case and the nesting limit
  on every frontend. `docs/pir.md`: `destroy`, `bool<Str>` and
  `PaykanBool_from_str`. `docs/c-backend.md`: LLVM is an in-tree, opt-in
  backend. The `MarkdownLinks` and `DocExamples` ctests keep links and
  stated outputs checked.
- `Driver.BuildAndRunDefaultToO2` failed in every sanitizer and coverage
  build (#122), because it expected the C cache key to end in ` -O2`, but
  instrumented builds append their own flags after the level. The test now
  parses the key's compile flags and checks for the `-O` token.
- **Release and CI (#133).** `release.yml` publishes a tag with a
  pre-release suffix (`v0.1.0-alpha`, `-rc1`) as a GitHub pre-release that
  is never marked "Latest", and a "Release plan" job shows that decision on
  every run, dry runs included. The barebones CI checks written as
  `! grep …` partway through a script never failed, because `bash -e`
  ignores a negated command; they are now explicit `if …; then exit 1; fi`
  checks. The Lint job runs actionlint, with shellcheck, and any finding
  fails it.
- **A truncated plugin file no longer crashes `paykan`.** A plugin whose file
  ends inside its loadable segments (an interrupted copy, download or install)
  was mapped by `dlopen` and killed every command with SIGBUS, `--version`
  included. On Linux the loader now checks the file's ELF program headers
  against its size first and lists the file as a rejected plugin.
- **A name two plugins provide is reported as ambiguous even when the first
  provider is incompatible.** The listings, `--version` and selection used to
  report only the first provider's incompatibility.
- **Bounded build parallelism.** README, CI, the release workflow, the Homebrew
  formula and the out-of-tree frontend test passed a bare `--parallel`, which
  with the Makefile generators is an unlimited `make -j`; they now pass the
  number of processors.

## Initial prototype - 2026-06-28

The first prototype; it was never tagged or released. PaykanLang was a
statically-typed, object-oriented language that compiled to LLVM IR and was
**JIT-executed**: deliberately small, with several features planned for v0.1.

### Added

- Static typing with full semantic analysis and clear, source-located diagnostics.
- Primitive types `int`, `float`, `bool`, `char`, and the built-in `Str` class.
- Single-inheritance classes with vtable-based virtual dispatch, automatic
  construction (`__init__`) and destruction.
- Object equality with consistent identity semantics: `==` and the default
  `Obj.equals()` agree, and classes may override `equals()`.
- Automatic reference counting (ARC) for deterministic object lifetimes; no GC.
- Enum types and `match` expressions with class-hierarchy, array, and
  int/str literal value patterns.
- Heap-allocated, element-typed arrays (`int[]`, `Str[]`, `Point[]`, …) with
  subscript read/write, `.len()`, `.push()`, `.pop()`, and 2D arrays.
- File-based module system with selective and aliased imports, with bitcode
  caching of imported modules.
- Built-in I/O: `open()` / `File` / `Error`, and `println` / `printerrln`.
- LLVM 17 backend with JIT execution via the ORC LLJIT.
- `paykan <file.pkn>` runs a program; `--emit-llvm` dumps IR;
  `--version` / `-v` reports the compiler and LLVM versions.
- `cmake --install` rules (GNUInstallDirs): the `paykan` binary, the runtime
  archive, and `Runtime.h` are staged for packaging.
- Integer divide and modulo by zero trap via a `noreturn` runtime panic.
- The `destroy()` destructor is compiler-generated and final: user classes cannot
  override it, and direct calls are rejected at compile time.

[Unreleased]: https://github.com/parsabee/PaykanLang/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/parsabee/PaykanLang/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/parsabee/PaykanLang/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/parsabee/PaykanLang/releases/tag/v0.1.0

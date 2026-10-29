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
    is not a fixed-size array yet, and a tagged union does not cross into C
    other than as a type argument of a generic native function.
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
  - An expression with no value is an error that asks for a braced body, and
    a function whose expression depends on its own result type (`fn fact(n:
    int) => ... fact(n - 1)`) is one that asks for the result type. A method
    writes its result type (`fn area() -> int => ...`); a `static fn` infers.
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
- `paykan --list-modules prog.pkn` prints the module file of each imported module whose
  compiled code the program links, one per line: the standard library's, the cache entries
  of the imports built from source and the prebuilt modules' (next to the source, in a
  `--module-path` directory or in `$PAYKAN_MODULE_PATH`).  They are the files whose objects
  `--emit-c`'s C is linked with, and docs/c-backend.md's build loops over them.
- **Parsing is a method of `Str`** (docs/language/04-strings-and-conversions.md, "Parsing a
  string").
  - The methods are `s.toInt()`, `s.toI8()`, `s.toI16()`, `s.toI32()`, `s.toUnsigned()`,
    `s.toU8()`, `s.toU16()`, `s.toU32()`, `s.toFloat()`, `s.toF16()`, `s.toF32()` and
    `s.toBool()`.
  - Each gives an optional of its type, and `None` when the whole string is not a value of
    it.
  - A text parses exactly when it is a valid literal of the type, and gives the literal's
    value. On top of that, a sign is allowed for the signed and float types, and
    `toFloat()` accepts what C's `strtod` accepts, `"inf"` and `"nan"` included.
  - The methods are builtins, not `Str` vtable slots, so the runtime ABI does not change.
- **`Str(x)` takes every value that has a text** and gives the text `x.toString()` gives:
  - an optional (`"None"` or its value's text), `Str?` included;
  - a tuple, a struct, an enum;
  - an object (its `toString()`) and `None`.

  A `Str` is the string itself, the same object, in both forms (`Str(s)`, `Str<Str>(s)`),
  so neither takes a `view` of one; `s.toString()` is a copy. A value without a text is an
  error that names the part without one: `'Str' cannot convert a value of type 'F', which
  does not implement trait 'Show': its field 'f' has type '(int) -> int', which does not
  implement it`.
- **New conversions:**
  - `i8` … `u32`, `int` and `unsigned` convert from `bool` (0 or 1) and from `char` (its
    code: `u8(c)` is the byte, `i8(c)` its signed reading).
  - `bool(x)` converts from every integer type (`x != 0`).
  - `char(x)` converts from every integer type, and never panics from a `u8`.
  - `T(x)` of a `T` is the value itself (`i64(n)` with an `int` `n`, `float(2.5)`).
  - `u.toInt()` gives an `unsigned`'s value as an `int?`, `None` above
    9223372036854775807.
- **Functions on strings in `::collections`** (docs/stdlib/collections.md, "Strings"):
  - `split(s, delim) -> List<Str>`: every field, empty ones kept (`"a,,b"` gives `["a",
    "", "b"]`, `""` gives `[""]`), so `join(split(s, d), d) == s`; an empty delimiter
    panics (`split: the delimiter is empty`).
  - `join(parts, sep) -> Str`.
  - `lines(s) -> List<Str>`: split at each `\n`, one `\r` dropped from the end of each line,
    and no empty last line for a `\n` at the end (`""` has none).
  - `words(s) -> List<Str>`: the runs between ASCII whitespace, never an empty one.
  - `indexOf(s, needle) -> unsigned?`, `contains`, `startsWith` and `endsWith`.
  - `substring(s, from, to) -> Str`, which panics unless `from <= to <= s.len()` (`string
    slice 2..7 out of bounds (len=3)`), `trim(s)`, `replace(s, from, to)` (every
    occurrence; an empty `from` panics: `replace: the text to replace is empty`) and
    `repeat(s, n)`.

  They count bytes, as `s.len()` and `s[i]` do, with every index and size an `unsigned`,
  take their strings as `view`s and return new ones. Two new runtime functions,
  `PaykanString_find` (C's `memchr` and `memcmp`) and `PaykanString_slice`, search and copy
  for them (Runtime.h; the runtime ABI only gains them, and keeps its version), so
  `split`, `lines`, `words`, `indexOf` and `contains` take time linear in the string for a
  delimiter or a needle of a few bytes, and at worst proportional to the string's length
  times the needle's. `join` and `replace` put their pieces together in pairs, which
  copies each byte of a result of k pieces about log2(k) times, and `repeat` doubles its
  string: replacing each of the million line feeds of a 10 MB string takes about a
  second. The language reference's strings chapter, which said the standard library had
  no such functions, points to them.
- Hexadecimal and binary integer literals (docs/language/02-basics.md, "Literals";
  docs/design/bitwise.md): `0xFF`, `0b1010_0101`, `_` between two digits (`0xFFFF_0000`),
  typed by their context as a decimal literal is (`x: u8 = 0xFF`; `y: i8 = 0xFF` is `the
  literal 255 does not fit in 'i8' (-128 to 127)`: a literal is a value, not a bit pattern).
  - An integer literal of any base is 0 to 18446744073709551615, so the upper half of
    `unsigned` is written directly (`m: unsigned = 0xFFFF_FFFF_FFFF_FFFF`); above
    9223372036854775807 a literal needs an unsigned or float context, and is an `unsigned`
    as a conversion's argument (`u8(0xFFFF_FFFF_FFFF_FFFF)` is 255).
  - Malformed literals are lexer errors that name the rule: an upper-case prefix, a
    misplaced `_`, a digit of another base, octal (`0o17`), a fraction (`0x1.5`), a `_` in a
    decimal (`1_000`, not yet) and a suffix (`255u8`: `a literal takes its type from the
    context, as in 'x: u8 = 255'`).
- PIR: `and`, `or`, `xor`, `shl` and `shr` on `i64` (docs/pir.md §6; binary codes 58-62,
  docs/design/pkm.md).  `shl` and `shr` shift by the count's low six bits (`shr` shifts in
  zeros), so every count is defined on every backend: the shared lowering masks the count
  with `& 63`, LLVM never sees a shift by 64 or more, and the C is `(uint64_t)a <<
  ((uint64_t)n & 63)`.  The format versions stay 0.3.0's.
- Bitwise operators on the unsigned types (docs/language/02-basics.md, "Bitwise
  operators"; docs/design/bitwise.md): `&`, `|`, `^`, `~`, `<<` and `>>` (logical), and `&=`,
  `|=`, `^=`, `<<=`, `>>=`, on `u8`, `u16`, `u32` and `unsigned`.
  - `& | ^` take one type, as `+` does (a literal takes the other operand's: `x & 0xF0`); a
    shift's count is any unsigned type and the result has the value's type; `~0x0F` takes
    an unsigned context's type.  A signed, `bool`, `char` or float operand is an error that
    names the fix (`... (the bitwise operators take unsigned operands: convert with
    'u32(...)')`, `(for a logical and, write '&&')`, `(a shift count is unsigned: ...)`).
  - A shift by the type's width or more is 0 and never panics (`u8(1) << 8`, `x >> n` for
    any `n` up to 2^64-1), on every backend; a literal count that large is a warning.
  - Swift's precedence: `<< >>` above `* / % &`, above `+ - | ^`, above the comparisons, so
    `x & mask == 0` is `(x & mask) == 0`; shifts do not chain (`a << b << c` is an error).
    `>>` is two `>` written together, so `Box<Box<int>>` closes as before.
  - The AST interchange format names them `bitand bitor bitxor shl shr` and `bitnot`
    (docs/plugins/ast-format.md); a template using them is kept in a `.pkm`'s TMPL.
- **Stability sweep: expressions, places, borrows and closures.**
  - A warning for each hidden heap copy of a slice (docs/language/02-basics.md,
    "Evaluation order").  A slice parameter passed on to another, while a call in a later
    argument may write its elements where the exclusivity rule cannot see it (a function
    storing into the array field the slice names, a function value, a virtual call whose
    override does, a function writing through a chain of objects), has its elements copied
    to the heap where it is passed, an allocation each time the call runs.  The compiler
    now says so there, naming the call that may write them and the call they are passed
    to, with the line and a caret as for any diagnostic: `counts.pkn:14:51: warning:
    'reset' in a later argument may write the elements of slice 'xs': they are copied to
    the heap before the call to 'total'`.
    - Once per place, however many instantiations of a generic function reach it and
      however many modules lower a copy of one; an instantiation of another module's
      template points into that module's file.
    - The calls are named as an error names them (`'reset'`, `'first<int>'`, `'f'` for a
      variable holding a function value), `'=='` for an `==` whose `equals` may write, and
      `a function value` for a call of another expression; in an instantiation of another
      module's template, that module's functions without the module, as its file writes
      them, and those of the modules it imports by their full names, a submodule's too
      (`'lib::sl::inner::wipe'`), as an error in the instantiation names them.
    - None for the copies of a struct, a tuple, a fixed-size array or a tagged union (on
      the stack, as a plain parameter's value), and none where nothing is copied.  No
      option turns it off.
    - The lowering reports it, through a diagnostics engine of its own: `run`, `build`,
      `--emit-pkm`, `--list-modules` and the `--emit-*` options that print code print it on
      standard error.  `--check-only` never lowers, so never warns, and an imported module
      read from its cache entry or a prebuilt module file is not lowered again, so it
      warns only when it is compiled from its source
      (docs/language/17-the-paykan-command.md).
    - `scripts/c_strict.py` leaves the warnings `paykan run` prints before the program
      runs out of the standard error it compares with the C executable's.
      `samples/codegen/78_mem_arg_eval_order.pkn` has five such copies, and the new
      `samples/codegen/88_hidden_slice_copies.pkn` four, next to the copies that are not
      warned about.
- **Stability sweep: runtime, standard library and tests.**
  - `scripts/let_check.py`, run by the new ctest `LetDecls`, checks that every local that is
    never reassigned is declared with `let` in the samples (`samples/**/*.pkn`), the
    examples of docs/language and docs/stdlib, and the standard library (`stdlib/*.pkn`). A
    local declared without it (`x = e;` where that declares `x`, or `x: T = e;`) must be
    reassigned or compound-assigned, passed to an `inout` parameter or arm, or, for a
    struct, tuple or fixed-size array, have a field or element assigned or a method that may
    change it called on it. `paykan` decides: the script makes every other declaration of a
    program `let`, takes back each one the compiler then refuses, and reports those left
    when the program keeps its diagnostics and its `--emit-pir` byte for byte. An
    assignment in an inner block to a variable of an enclosing one is not a declaration
    (`let` there would declare another variable), and a program Sema rejects keeps its
    errors. `--apply` makes the declarations it reports `let`. There is no compiler warning
    for it.
    - Not checked: borrows, `weak` locals, destructuring, `for` variables and declarations
      without an initializer, which cannot be `let`; a generic body that the program never
      instantiates, which Sema checks for each instantiation only (the standard library's
      generic code is checked through the programs of tests/Stdlib, compiled against the
      changed sources); and a docs example whose fence follows the line
      `<!-- let_check: plain -->`, as the three of docs/language/02-basics.md that show the
      plain declaration itself do.
    - The tree follows it: 1,450 declarations are `let` now (832 in samples/codegen and
      samples/leak-check, 376 in samples/sema and samples/imports, 211 in the docs'
      examples, 31 in the standard library), and the `--emit-pir` of no program changed.

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
  point"): into `self` (a field of a class's `self`, or a field of a struct held in it; a
  struct's `self` counts as an `inout` parameter), or into what an `inout` parameter
  names and the object a reference held there refers to (`return n.k;` with `n: inout
  Node`, `return s.b.k;` with `s: inout S`, `return a[5].k;` with `a: inout B[6]`).
  One in a separate object, reached through a field of `self` or past
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
  class of another module, and a method it compiles, stay unknown, but for the methods of an
  instantiation an imported module's interface gives (the `List<Str>` of
  `collections::split`, its `Buffer<Str>`), which an importer uses as that module's copy:
  Sema checks a copy of each from its template, in the `TMPL` of the template's module,
  which the program need not import, and the lowering reads what they do from it as from
  its own copies (docs/design/pkm.md §4.8), so a `MinPriorityQueue<Str>`'s `pop` holds
  nothing, as when the program makes `Buffer<Str>` itself.  A field no code stores into
  once its object is made (only `self.f = v` in an `__init__`, where `self` is always
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
  chain.  One on floats, classes, optionals or strings tests its arms in turn, each only
  while none before it held, then switches on the index of the first that held (*Fixed*).
  Both targets switch on it: the LLVM IR at every optimisation level (`switch i64`), and
  `--emit-c`'s C (`switch`).
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
- A method or a `static fn` with type parameters of its own is a syntax error, on every
  frontend: `a method cannot have type parameters of its own: only a free function is
  generic (the methods of a generic class or struct use its type parameters)`, and `a
  'static fn' cannot have type parameters of its own: ...` (`frontend::kGenericMethod`,
  `kGenericStaticFn`), at the `<`.  They were parsed and ignored: `class A { fn f<T>(x:
  int) -> int { ... } }` compiled as if `T` were not there, `static fn id<T>(y: T) -> T` in
  an `impl` block was `return type has unknown class type 'T'`, and `fn map<U>(x: U) -> U`
  in a generic class `unknown class type 'U'`.  The AST interchange reader rejects a
  class's or an `impl` block's method with `(type-params ...)` alike
  (docs/grammar.md section 4, docs/language/13-generics.md, "Declaring a Generic Function").
- Two new limits hold a program to what the passes after the frontend can take on a
  quarter of the default stack (*Fixed*, docs/grammar.md section 9), on every frontend and
  in the AST interchange reader:
  - A chain of more than 2048 links is `nesting too deep (more than 2048 levels)`
    (`frontend::kMaxChain`): each binary operator, call, field, method call, tuple index
    and subscript is a link, counted along any path down the expression, so `x = 1 + 1 +
    ...` with 3000 terms, which compiled, is rejected.  Split such a chain with locals
    (`a = x0 + ... + x999; b = a + x1000 + ...;`).
  - An `elif` is a level of nesting, as the `if` of an `else` block, so an `if` takes up
    to 510 of them in a function's body, and more are `nesting too deep (more than 512
    levels)` (`frontend::kMaxNesting`).  Write a longer chain as a `match`, which takes any
    number of arms, or as `if`s in turn that each `return`.
  - The AST interchange format nests up to 4112 lists (`kMaxDepth`, was 2048), and its
    reader counts the levels and links the frontends count, an operand the grammar writes
    in parentheses (`a + (b + c)`, `(a == b) == c`) as the level its parenthesis is
    (docs/plugins/ast-format.md section 3): a frontend plugin's AST deeper than the
    frontends parse is rejected.
- Tests: the standard library's tests (`tests/Stdlib`, `scripts/stdlib_tests.py`) run on
  every path at -O0 and -O2, where they ran only with `paykan run` at -O2: `paykan run`
  (`StdlibTests`), `paykan build` (`StdlibTestsBuild`, `--build`) and the C `--emit-c`
  prints, compiled with the runtime's sources (`StdlibTestsC`, `--cc <compiler>`).  The
  samples corpus is built at -O0 too (`SamplesParityBuildO0`).
- A tuple, struct, enum or optional holding a function value does not implement `Show`
  (*Fixed*): `io::println(S(fn(x: int) => x + k))` for `struct S { f: (int) -> int; }`,
  `io::println((1, f))`, `E::F(f).toString()` for `enum E { F(h: (int) -> int), N }` and
  `O(None)` for `struct O { g: ((int) -> int)?; }` compiled and printed the closure's code
  address and environment (`S(f: (139785902924240, Object@0x55b69d53ee30))`, `O(g: (0,
  None))`), different on every run.  An aggregate now implements `Show` only when every
  element, field, variant field and optional's value does (`Sema::hasText`, the one rule
  for which types show themselves), so these are errors naming the part that does not:
  `'S' does not implement trait 'Show', which 'io::println' requires of 'T': its field 'f'
  has type '(int) -> int', which does not implement it`, for a `T: Show` bound,
  `toString()` (`no method 'toString' on type 'S', which does not implement trait 'Show':
  ...`) and `impl Show for S {}` (`struct 'S' cannot implement 'Show' (its 'toString' is
  built in): ...`), through nested aggregates (`its field 's' has type 'S', whose field 'f'
  has type ...`).  A function value inside an object does not count: the object shows
  itself through its class.  No program of the standard library, the samples or the docs
  printed such a value.
- A `char` ordered against a number by `<`, `<=`, `>` or `>=` is an error, as `==` on the
  pair is: `c < 100`, `c < n` and `100 > c` got past Sema and stopped the compiler with
  `paykan: PIR verification failed: ... cmp operands have different types: char and i64`
  on every path, and now are `operands of '<' have mismatched types 'char' and 'int'` (`'int'
  and 'char'` for `100 > c`), with the line and caret of the compare.  Compare with a char
  (`c < 'd'`) or the byte value (`int(c) < 100`) (docs/language/02-basics.md, "Relational &
  Equality").  No program that ran or built is rejected, though `--check-only` accepted
  these.
- A trait requirement's parameters follow a function's rules: two of one name (`trait T {
  fn m(x: int, x: int); }`) are `duplicate parameter 'x' in method 'm' of trait 'T'`, and
  one named like a type (`fn m(u8: int)`, a class's name) is `'u8' is a type name and
  cannot be used as a parameter name`, at the parameter.  Both were accepted, unlike a
  function's or a method's parameters (docs/language/06-traits.md, "Semantic Checks").  A
  function of the module may still name one, the requirement having no body, as for a
  native function.
- `destroy` cannot name a method or a `static fn` anywhere: a struct's `impl` block took
  `fn destroy()` as an ordinary method, which no call could reach (`p.destroy()` was
  `'destroy' cannot be called directly; an object is destroyed automatically ...`, wrong for
  a struct), and `static fn destroy()` too, a generic struct's as well; a class's `impl`
  block got `class 'A' already has a method 'destroy' (from 'Obj')` and its body `'destroy'
  is the compiler-generated destructor and cannot be overridden`.  Each is now the one
  error `'destroy' is reserved for the compiler-generated destructor and cannot be declared
  in class 'A'` (`... in struct 'P' (a struct has none)`), for a class body's method and
  an `impl` block's method or `static fn` of a class or a struct, generic or not
  (`Sema::rejectReservedDestroy`), and a call of the rejected `static fn` is a follow-on;
  a bodiless one in an `impl` block, a `static fn` too, is the syntax error `'destroy' is
  reserved for the compiler-generated destructor and cannot be declared`, at the function
  as Sema's (docs/language/05-classes.md, 11-structs.md, "Semantic Checks").
- A compilation prints at most 20 errors: past them `paykan` prints `note: stopping after
  20 errors, the error limit (--error-limit=N sets it, and --error-limit=0 shows every
  error)` and no more diagnostics, though it still exits with status 1 (and a file that
  does not parse still ends with `parsing failed with N error(s)`, counting them all).  The
  new `--error-limit=N` sets the limit (`0` for none; leading zeros are allowed); warnings
  and notes do not count, and an imported module's errors count against the importer's
  limit.  And a source line longer than 120 characters is shown in 120 around the caret,
  `...` standing for what is cut, a line above or below it whole if it fits in 120 and
  from the same column if not, where every error printed its whole line and the lines
  around it: 10000 errors on one 148 KB line printed 2.2 GB, and print 5 KB now; with
  `--error-limit=0`, 20000 errors on one 100 KB line take 1 s to print 5 MB, where they
  took 24 s to print 2.8 GB.  Only the characters around the caret are looked at, so an
  error's cost does not grow with its line either.  A caret whose column is past its line
  (a frontend plugin's AST of another file) stands after the line's last character.  The
  caret counts a UTF-8 character as one, and each byte in none (a Latin-1 file's `©`) as
  one too, where it counted bytes and stood to the right of its place after a UTF-8
  character (docs/language/17-the-paykan-command.md).
- A field named like a type is an error whatever declares it: `class C { u8: int; }` with
  `impl C { fn __init__() { self.u8 = 1; } }`, `struct S { Str: int; }` with its own
  `__init__` and `enum E { B(u8: int) }` compiled, since only the default `__init__`'s
  parameters were checked, which a written one replaces and a variant has none of, against
  docs/language/02-basics.md ("Builtin Names Are Reserved").  The field itself is checked
  now: `'u8' is a type name and cannot be used as a field name`, at the field; a generic
  type's once, at its template, whether or not it is instantiated (`struct Box<T> { u8: T;
  }` compiled until a program instantiated it, and was then reported once per
  instantiation).  And a
  parameter named like a type or like a function of the module is reported at the
  parameter, as one (`'u8' is a type name and cannot be used as a parameter name`), where
  the error stood under the whole function, lambda or trait requirement (the whole struct
  or class for a field) and said `... as a variable`; so is a parameter's type error
  (`parameter 'v' cannot have type 'void'`), a method's parameter named `self`, and a
  duplicate parameter, at the second.  A `param` of the AST interchange format has its
  location, which is optional: one without it is at its function's or lambda's
  (docs/plugins/ast-format.md).  `--emit-ast` writes it, so a reader of the format takes it
  (the 0.2.0 format had none).
- `paykan build --track-heap` is an error (``--track-heap applies to `run`: to track the
  heap of a program `build` wrote, run it with PAYKAN_TRACK_HEAP set``), where it was
  accepted and did nothing: the executable tracks its heap when it runs with
  `PAYKAN_TRACK_HEAP` set (docs/language/17-the-paykan-command.md,
  docs/language/16-memory.md).
- A command line asks for one thing, and `-o`, `--section` and `--track-heap` are errors
  where they do nothing, where they were ignored: two of `run`, `build`, `pkm`,
  `--check-only`, `--emit-pkm` and the options that print the program (`--emit-c
  --emit-pir` printed PIR, `--check-only --emit-pir` printed nothing, `build --emit-c`
  printed C and built nothing), `-o` under `run`, `--check-only`, `--dump-*`,
  `--list-modules` or `pkm dump|check`, `--section` outside `pkm dump`, and `--track-heap`
  outside `run`.  Other options are still accepted where they change nothing (`-O3` and
  `--stats` with `--check-only`).  The error names what clashes (`--check-only and
  --emit-pir exclude each other`, ``-o does not apply to `run` ``, ``--section applies to
  `pkm dump` ``).  The options that print the program take no program arguments, as
  `build` and `--emit-pkm` do, so their options may follow the file, and an extra argument
  names the option it is extra to (`unexpected argument 'extra' (--check-only takes one
  file and no program arguments)`, where `--emit-pkm hello.pkn extra` said `` `build`
  takes one file``).  Under `run`, everything after the source file is still the program's
  (docs/language/17-the-paykan-command.md, "Commands").
- `-o <file>` names the file an `--emit-*` option (`--emit-c`, `--emit-llvm`,
  `--emit-mlir`, `--emit-source`, `--emit-pir`, `--emit-ast`) writes what it prints, where
  `paykan --emit-c f.pkn -o f.c` printed the C and wrote no `f.c`; like every file the
  driver writes, it is never the source file.  The file is written once all of it was
  printed, as a module file is, so a backend that fails leaves what `-o` names as it was.
  Without `-o` they print to standard output.
- A class whose superclass is another module's `Hashable` class cannot repeat `impl
  Hashable for` (`class 'Sub' already implements 'Hashable' (through 'CK')`), as in one
  module.  It is `Hashable` through its superclass.
- An importer's instantiation of another module's generic type checks the type's `impl
  Trait for` blocks, which were left out before, as the module checks its own
  instantiations: also the methods the importer never calls.  So `gb::Box<float>`, with
  `impl Hashable for Box<T> { view fn hash() -> int { return self.v.hash(); } }`, is now
  `method call on non-class type 'float'` in gb's file, as `Box<float>` is in gb itself.
  A block of gb's own trait is checked the same way (`impl Named for Box<T>`, whose
  `name` returns `self.v.name()`, is that error for `gb::Box<int>`), and `impl Show for
  SBox<T> {}` makes `gb::SBox<int>` the error `class 'SBox<int>' already implements 'Show'
  (through 'Obj')`.
- A bound of another module's template is never resolved against the importer's own
  traits.  An importer that declared a `trait Named` of its own satisfied
  `lib::greet<T: Named>` with it, whatever the trait's methods.  That is now the #147
  error under Fixed, with a note at the importer's trait.
- **Breaking:** a primitive type no longer parses a `Str`. `int(s)`, `float(s)`, `bool(s)`,
  `unsigned(s)`, `i8(s)` … `f32(s)` and their explicit forms (`int<Str>(s)` …) are errors
  that name the method: `no specialization of 'int' for 'Str': parse a 'Str' with its
  method 'toInt()', which returns 'int?'`. Migration: `T(s)` and `T<Str>(s)` become
  `s.toT()` (`int`/`i64`: `toInt`, `unsigned`/`u64`: `toUnsigned`, `float`/`f64`:
  `toFloat`, `bool`: `toBool`, `u8`: `toU8` …).
- **Breaking:** the conversions follow the target's family
  (docs/language/04-strings-and-conversions.md, "Between numbers"):
  - `int`, `i8`, `i16` and `i32` no longer convert from `unsigned` (`int(unsigned(-1))`
    was `-1`). Write `u.toInt()`, an `int?`; or `i32(u32(u))`, which gives what `i32(u)`
    gave; or keep the arithmetic `unsigned`.
  - `unsigned`, `u8`, `u16` and `u32` no longer convert from a float (`u8(255.5)` was
    `255`). Write `u8(int(f))`, after checking the range: it wraps where the old conversion
    panicked.
  - Every conversion that remains gives the result it gave in 0.2.0, except the fixes
    below.
  - The lists in `no specialization of …; its specializations are …` change accordingly.
- An `f32` or `f16` from a text with a fraction or an exponent (a literal, or `toF32()` and
  `toF16()`) is documented as the `float` nearest the text rounded to the type, as C's
  `(float)strtod(s)` is. For a text of 13 or more significant digits very close to a point
  halfway between two values of the type, this can differ from rounding the text once
  (`1.0000000596046448` is `1` as an `f32`). A literal and the parse of its text always
  agree.
- `paykan run` generates code at the level `paykan build` does for the same `-O`: LLVM's
  code generator does no optimisation at `-O0` (its fast instruction selector), and works
  at its least at `-O1`, its default at `-O2` and its most at `-O3`
  (docs/language/17-the-paykan-command.md).  The JIT used LLVM's default level at every
  `-O` before, so `run -O0` and `run -O1` now generate less optimised code than they did,
  `run -O3` more optimised code, and `run -O2` the same.  The samples print the same at
  `run -O1` and `run -O3` as at `run -O2`, with the same heap statistics.
- A call a module makes of its own `native fn` bound to a runtime function that never
  returns (`Paykan_panic_message`, `Paykan_panic_value`, `Paykan_unreachable_view`: the
  `_panic` and `_fail` of `::collections`) releases nothing its arguments made after it,
  where no path went (docs/pir.md §3, "Native names"): the `List<Str>.__check` a program
  links from `::collections` releases no message after the panic, as one the program makes
  itself does not, and the standard library's code has up to 9 fewer `release`s in a
  program's `--emit-pir`.
- An integer literal above 9223372036854775807 is no longer a lexical error
  (`integer is out of range`, which now means 2^64 or more): Sema checks it against its
  context's type on every frontend (`x: int = 9223372036854775808` is `the literal
  9223372036854775808 does not fit in 'int' (...)`), and `-9223372036854775808` in an
  expression is Sema's error with its `-9223372036854775807 - 1` hint.  The AST interchange
  format's `(int ...)` reads values up to 18446744073709551615.
- `&`, `|`, `^` and `~` are tokens (no longer `invalid character`).
- **Stability sweep: modules, pkm and driver.**
  - `$PAYKAN_RUNTIME_DIR`, when it is set, is the only place `build` (and a plugin
    backend's `runtime_library`) takes the runtime from, as `$PAYKAN_STDLIB` is the only
    place of the standard library: one that does not exist, or that lacks
    `lib/libpaykan_runtime.a` or `include/paykan/Runtime.h`, was passed over without a
    word for the runtime next to the executable or in the build tree, so a program could
    be linked with another runtime than the one asked for.  It is now an error naming
    what is missing (`cannot find the Paykan runtime in $PAYKAN_RUNTIME_DIR
    (/opt/rt): /opt/rt/include/paykan/Runtime.h does not exist`); an empty
    `PAYKAN_RUNTIME_DIR` is not set (docs/language/17-the-paykan-command.md).
  - A source file, and a module file `pkm` or an import reads, is at most 256 MiB, a new
    limit (docs/grammar.md section 9, docs/design/pkm.md §1.6): `--check-only /dev/zero`
    and `pkm dump /dev/zero` read until an allocation failed and aborted with an
    uncaught `std::bad_alloc`, and a larger regular file was read whole first.  A regular
    file over the limit is now refused by its size, before it is read, and a pipe or a
    device is read only up to it: `'/dev/zero' is too large for a source file (more than
    256 MiB)`, `paykan: '/dev/zero' is too large (more than 256 MiB)` from `pkm`, exit
    status 1.  A pipe is still read to its end (`cat prog.pkn | paykan /dev/stdin`), and
    the source is read through a buffer on the heap rather than 64 KB of the stack.
  - A chain of imports is at most 256 modules deep, a new limit (docs/grammar.md section
    9, docs/language/15-modules.md): a chain of about 1460 modules built from source
    overflowed the stack (a segmentation fault, no diagnostic), since each is analysed by
    a Sema nested in its importer's.  The import 257 levels down is now an error at that
    import, `imports nested too deep (more than 256 modules): 'c::m256' is imported 257
    modules deep`, exit status 1, however long the chain.  The levels are counted whether
    the modules come from source or from module files (a cache entry is checked against
    each module it imports, one level deeper), so a program is not accepted with a warm
    cache and rejected with a cold one; and reported at the same import with the same
    notes, since a cache entry whose imports nest too deep is built from its source.  The
    nested Sema is on the heap, which halves the stack each level takes (2.9 KB in a debug
    build).
- **Stability sweep: expressions, places, borrows and closures.**
  - `weak a = None;`, a `weak` local with no type whose initializer is `None` (or a
    conditional of two `None`s), is an error: `cannot infer the class of weak local 'a'
    from None; write 'weak a: T? = None'`.  `None` has no class, and the local was a weak
    reference to the None value itself, which never read `None` (`Str(a == None)` printed
    `False`).  With its type written (`weak a: Node? = None;`) it starts as nothing, as
    before (docs/language/16-memory.md, "Weak References").  An `Obj` that holds `None`
    (`x = None; weak a = x;`, or a function returning `Obj`) is still accepted and is
    still a reference to the None value, as a strong `y: Obj? = x;` is: the conversion of
    an `Obj` to an `Obj?` keeps the value (docs/language/12-optionals.md, "Not supported
    yet").
  - An `-> inout T` result cannot be an element of a tuple, tuples being immutable:
    `return self.t.1;`, `return t.0;` with `t: inout (int, int)`, either branch of
    `return if c then t.0 else t.1;`, and an element of another call's `inout` result
    (`return self.slot().0;`) are `an element of a tuple cannot be returned 'inout':
    tuples are immutable`.  The docs said such a result could point into "an element of a
    tuple", Sema accepted it, and `run`, `build` and `--emit-c` then stopped with
    `internal compiler error: an 'inout' argument is not a variable or a field`.  A place
    in another call's `inout` result is now checked as any other: a field of a struct in a
    tuple there (`return self.slot().0.n;`), and a character of a string
    (`return self.str()[1];`), are the errors they are elsewhere, where they too were
    internal errors (docs/language/03-functions.md, "Borrowed results").
  - **Breaking:** a borrow lasts until its call runs (docs/language/03-functions.md,
    "Exclusivity").  What a call passes to a `view` parameter (a slice parameter too), and
    the receiver of a struct's `view fn`, no call anywhere in a later argument may write:
    pass it, what holds it or a part of it to an `inout` parameter, or call a method on
    it that may change `self`.  With `look(p: view P, x: int)` and `bump(p: inout P)`,
    `look(p, bump(p))` is `'p' is passed to 'view' parameter 'p' of 'look'; a later
    argument of 'look' cannot pass it to 'inout' parameter 'p' of 'bump'`, and so are
    `sum(arr, bumpA(arr))` with `sum(xs: view int[], k: int)`, `sum(xs, clear(xs))` with
    `xs` an `inout int[]`, `look(h.p, bumpH(h))` (`'h' contains 'h.p', ...`),
    `p.show(bump(p))` with `show` a `view fn` (`'p' is the receiver of 'view fn show'; an
    argument of 'show' cannot pass it to ...`) and `look(p, p.reset())` (`... cannot call
    'reset' on it (not a 'view fn')`), a class's method that may change `self` included.
    Every call borrows and writes alike: a function's, a method's, a constructor's,
    `__super__`'s and a function value's; a lambda's body is not part of the call.  Each
    write is one error: an `inout` result passed on is written by the call returning it
    (`look(h.p, bump(h.slot()))` is the error for `slot`), and a write its own call
    reports, as sharing storage with another of its arguments (`two(p, p.a)`), is not
    reported again.  Exclusivity was a rule of one call, and these programs ran, the callee given a copy of
    the value read; `samples/codegen/78_mem_arg_eval_order.pkn` had three of them, now
    `samples/sema` error files.  What the rule does not see is accepted and copied as
    before: a plain parameter (`first(p, bump(p))`), a write through an object
    (`look(c.p, reset(c))`) or a function value, a write before the borrow or after it has
    ended (`add(look(p, 0), bump(p))`), another field (`look(two.x, bump(two.y))`), a
    lambda's body, and the operands of `==`; a slice's copy is warned about (see Added).
- **Stability sweep: classes, generics and names across modules.**
  - `__super__(...)` may be called only once, as the first statement of `__init__`: a
    second call, after the first or under an `if` or a loop (`__super__(1); if b {
    __super__(2); }`), is an error at that call, `'__super__' may be called only once, as
    the first statement of '__init__'`.  It was accepted, and its stores of the
    superclass's fields leaked what the first call had stored (2 blocks, 35 bytes for one
    `Str` field), on every path.  A call after another statement is the same error, at the
    call, also when the superclass's `__init__` does nothing (which was accepted); where it
    has one it was `'__super__' must be the first statement of '__init__' in class 'C'`, at
    the method (docs/language/05-classes.md, "Inheritance" and "Semantic Checks").
  - **Breaking:** a type parameter names no field, parameter or local of its declaration, as
    a type's name names none anywhere (docs/language/13-generics.md, "Type Parameters Are
    Types"; 02-basics.md, "Builtin Names Are Reserved").  A local named like one was already
    an error (`fn h<T>(y: T) -> int { T = 3; return T; }` is `'T' is a type name and cannot
    be used as a variable`), while a field and a parameter were accepted: `class D<T> { T:
    int; v: T; }` and `fn g<T>(T: int, y: T) -> int { return T; }` ran.  The field is now
    `'T' is a type name and cannot be used as a field name`, at the field, once (of a class,
    a struct or an enum's variant, whether or not it is instantiated), and the parameter
    `... as a parameter name` for each instantiation, a method's, an `impl` method's and a
    `static fn`'s too.  An `impl` block's type parameters are the names it gives them: in
    `impl R<U>`, `U` is the type parameter, in its methods' parameters and bodies, and `T`
    an ordinary name (a body there was checked with the class's `T`, so `fn x(T: int) -> int
    { T = T + 1; return T; }` was an error and `U = 1;` was not).  A template's declarations
    are checked with its own type parameters, not those of a generic body that instantiates
    it: `g<int>(3, 4)` inside `fn outer<T>`, with `fn g<U>(T: int, u: U)`, was `'T' is a
    type name ...`.  `samples/codegen/89_type_parameter_names.pkn` runs the names these
    leave free.
  - An instantiation requested inside an instantiation of its own template, directly or
    through other templates (a method's or a function's body included), that grows its
    type arguments from that one's has a name of at most 4,096 characters as diagnostics
    spell it, a new limit (docs/grammar.md section 9, docs/language/13-generics.md, "How
    Errors Are Reported").  It grows them when one of its arguments holds one of that
    instantiation's, nested more deeply, and coming back the same way would deepen it
    again (`C<(T, T)>` in `C<T>`, `P<B, (A, A)>` in `P<A, B>`); what an argument holds is
    followed through the type arguments written at each request on the way, and an
    inferred one may hold any.  A template instantiating itself with an ever-growing
    argument ran on until the depth limit (16):
    `class C<T> { v: T; n: C<(T, T, T)>?; }` with `C<int>(1, None)` was still checking
    after 5 minutes (28 minutes and more were seen), and `n: C<(T, T)>?` stopped after
    1.5 s, its notes 900 KB of names.  The instantiation whose name passes the limit is now
    the one error, `instantiating 'C<((((((int, int, int), (int, int, int), (int, int,
    int)), (...' inside an instantiation of 'C' exceeds the maximum instantiation name
    length (4096 characters; this one has 4374); a generic class may not instantiate itself
    with an ever-growing type argument` (the name cut to its first 60 characters), reached
    in 0.01 s; so it is for a struct, an enum, a function and two templates instantiating
    each other.  Templates that instantiate one another without coming back to one are
    held to the depth only, whatever their names: ten classes each holding the next with a
    tuple of three of its argument (`class L0<T> { v: T; n: L1<(T, T, T)>?; }`, and so on)
    are accepted, the last name 354,296 characters long.  So is a template instantiating
    itself with arguments that do not grow, of thousands of characters: swapped or
    repeated (`fn swap() -> Pair<B, A>` and `fn dup() -> Pair<A, A>` in `impl Pair<A, B>`,
    or `g<B, A>` in `fn g<A, B>`), grown from one it keeps (`fn grow() -> Pair<A, (A,
    A)>`, whose own is itself), or holding none of its own: `class X<T> { v: T; c:
    L0<int>?; }` with the chain's eighth class holding an `X<T>`, which comes back to `X`
    with a 13,122-character argument made from `L0<int>`, in a field or a method's body.
    The ten classes took 1.3 s to check, eleven 10.8 s and twelve 118 s, as each type a
    template is given (an argument put into it, a signature written back) was resolved
    again, element by element, and each element's description, holding the whole
    instantiation's name, built for its diagnostics.  A tuple, optional or function type
    resolved once is now itself at once, and the descriptions are built only for an
    error: 0.03 s, 0.07 s and 0.2 s.  The longest
    instantiation name in the standard library is 26 characters, in the samples, the
    stdlib tests and the documentation's examples 55.
- **Stability sweep: runtime, standard library and tests.**
  - `File.readbytes(n)`'s `n` is a maximum, not a size it allocates before reading
    (docs/stdlib/io.md, "File.readbytes"): a read with an `n` too large to allocate is no
    longer `OutOfMemory`, but the bytes that are there, or `EndOfFile` when there are none
    (`readbytes(1152921504606846976)` at the end of a file). It is `OutOfMemory` only when
    the data it reads does not fit in memory.

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

- **Stability sweep: lowering and backends.**
  - A struct, a tuple, a fixed-size array or a tagged union passed as an argument, or as the
    left operand of `==` or `!=`, was read only once the later operands had run, so one that
    wrote its storage changed the value the callee or the comparison saw: with `reset(c)`
    storing into `c.p` and `bump(p: inout P)`, `first(c.p, reset(c))`, `first(p, bump(p))`
    and `q == P(bump(q) + 9)` used the values written (9, 9, `True`), not those read (1, 1,
    `False`), on every path, and so did a method's, a constructor's, `__super__`'s and a
    function value's arguments, a `view` or slice parameter (an array given to one, and a
    slice parameter passed on to another) and the receiver of a struct's `view fn`.  Such a
    value is passed by address, and its callee copies it (or reads it where it is) when the
    call starts.  Now, when an operand after it may write its storage (an `inout` argument
    naming it or a part of it, a struct method that may change it, a call given what
    reaches the object it is in whose code may store into a field of that name, or into a
    field whose value may hold the caller's storage it names, or a call through a function
    value, whose code is not known), the caller copies it where it is evaluated, into a
    temporary whose references it releases after the use; a slice passed on, by the length
    it has when the program runs, into memory allocated for the call and freed after it
    (docs/language/02-basics.md, "Evaluation order"; docs/pir.md §7, "Reads a call may
    replace").  No other value is copied: a later call storing only into other fields
    (`first(c.p, counter.next())`, `at(xs, rng.next())` with `next` storing an `int`
    field) leaves the value where it is, as base did, and the samples and the standard
    library's tests lower to the same PIR as before, with and without the passes, so the
    static counts recorded here do not move.  The receiver of a struct method that may
    change `self` is still its storage, as an `inout` argument is.  (For a `view` or slice
    parameter and the receiver of a struct's `view fn`, an `inout` argument or a method
    that may change it, in a later argument, is now an error: see Changed.  A slice's copy
    is now warned about: see Added.)
    `samples/codegen/78_mem_arg_eval_order.pkn` runs each shape on every path, with
    `__super__`'s arguments, a function value a field holds, an argument under an `inout`
    result, one before a closure that writes it, slices passed on to a function, a
    function value, a method and a constructor, and methods writing the value through an
    `inout` local, a `match` arm's binding and a field holding it.
  - The `lending` pass took time quadratic in the arguments it lent in one block: it
    removed the retain of each copy it lent one at a time, and each removal moved the rest
    of the block.  A function of 3000 statements `r += first(p, bump(p)) + first(c.p,
    reset(c));`, each lending `reset` its `c` beside the copies of `p` and `c.p` above,
    took 37 s to reach `--emit-pir` (31 s without those copies).  A block's retains now go
    in one pass over it: 2.5 s, for the same PIR.
  - Every check that panics on a bad operand (an integer `/` or `%`, an index into a
    fixed-size array, a conversion's range, a `for` loop's step) nested the rest of its
    function one level deeper, under an `scf.if` on the function's exit flag and so in the
    C: Clang rejected the C of a function with 128 integer divisions (`bracket nesting
    level exceeded maximum of 256`), and the compile time grew with the square of the
    checks on every path (3000 divisions in one function took 75 to 86 s to reach
    `--emit-mlir=scf`, and take 1.3 s now).  A check ends in `unreachable` after a panic,
    which never returns, so it is no exit now: what follows it runs beside it, and the
    runtime's panics are declared `noreturn` on the LLVM path, as `Runtime.h` declares them
    `_Noreturn` for the C (docs/c-backend.md, "Control flow").  An early `return`, a
    `break`, a `continue` and an `elif` still nest what follows them, so Clang's default
    limit still rejects a function with more than about 250 of them (documented there).
    `samples/codegen/79_many_guards.pkn` makes 920 checks in one function.
  - A `match` with many literal arms compiled in time quadratic in its arms: the lowering
    and PIR's verifier compared each case of its switch with every case before it, to drop
    or reject one listed twice (20000 arms took 46 s to reach `--emit-pir`, and take 0.5 s
    now).  Both keep a set of the cases seen, which holds two cases as one exactly when the
    comparison did (`pir::CaseSet`, with `pir::sameConstant`'s equality: an `i64`, a `char`
    or a `bool` constant of the same value, and never a float).  And `run -O0` generated
    code at LLVM's default level from a module it had not optimised, which took 49 s for a
    5000-armed match whose `build -O0` took 3 s: the JIT now generates code at the
    level a native build does for the same `-O` (under Changed), and takes 0.3 s.
  - The LLVM path's lowering of `scf` to `cf` took time and memory quadratic in the number
    of `if`s in one function's block (a long run of `if` statements, or a `match` on a
    `Str` or a float with many arms, which tests each in turn): a function of 3000 `if`s
    beside a 2000-armed `Str` match took 24 s and 1.3 GB to reach `--emit-mlir=llvm`,
    for `run` and `build`.  Lowering an `scf.if` splits its block after it, and the
    conversion driver recorded every op each split moved, so that it could undo it, while
    each split moved everything after it.  The same patterns now run on each op once, a
    block's last first and an op before those in its regions, with nothing to undo: 2 s
    and 0.1 GB (docs/design/mlir-backend.md).  The C path, which lowers `scf` to EmitC
    and not to `cf`, took time quadratic in a module's string constants instead, one for
    each arm of a `match` on a `Str`: it drops a constant nothing reads, and looked for
    each constant's readers with a walk of the whole module.  The same program took 38 s
    to reach `--emit-c`, and a 2000-armed `Str` match alone 32 s; one walk of the module
    finds every constant's readers now, and they take 2.2 s and 1.5 s, for the same C.
  - Compiling a program took time quadratic in its number of modules on the LLVM path: each
    imported module's cached LLVM IR is keyed on the module's MLIR as text, and printing a
    module nested in the program verified and numbered the whole program each time.  A
    program of 400 small modules took 19 s of processor time to print its LLVM IR from
    the cache, and takes 1 s now: a module prints on its own, the program verified once,
    when it is imported.  The keys' text changes, so every cached `.bc` is rebuilt once.
  - `--emit-c` wrote a string constant as one C string literal, however long, but C11
    promises a compiler only 4095 characters in a literal, counted after adjacent literals
    are joined, so the C of a program with a longer string (which adjacent literals make
    easily) failed the strict build docs/c-backend.md promises: `-std=c11
    -pedantic-errors` reported `string length '4096' is greater than the length '4095'
    ISO C99 compilers are required to support`.  A longer string is now written as its
    bytes, character constants in braces, then the terminating `0`.  docs/c-backend.md
    ("Standard C") names the limits the C does not keep within: those only a program's own
    size passes, such as an object of more than 65,535 bytes, and the initial characters
    C11 promises are significant in a name (31 in an external one, 63 in another), which
    the generated names, a module's name and a symbol's in full, often run past.
    `samples/codegen/80_long_string_literal.pkn` holds a string of 4096 bytes.
  - The C of a program that assigned a struct or a fixed-size array field of an object it
    held in a variable or a lambda's capture (`d = h.c; d.p = P(7, 8, 9);`, `k = fn() {
    c.p = P(7, 8, 9); };`, `d = c; d.p = P(7, 8, 9);` with `c: inout C`) failed the strict
    build at GCC's `-O1` and above: `-Werror` reported `'__builtin_memcpy' offset [0, 23]
    is out of the bounds [0, 0] [-Warray-bounds]`.  The retain that made the variable's
    reference tested the box for `NULL`, and GCC followed that path into the field's write.
    A value whose type is a class that is not optional is never `NULL` (Sema reads one only
    once it is set, and a native function never returns one as `NULL`), so the lowering
    now marks each retain of one `retain nonnull` (docs/pir.md §6, a new mark in PIR's text
    and binary forms, code 57): a variable's, an `inout` parameter's, a field's or an
    element's value read, or copied into another variable, a field or the slots of `[v;
    n]`, a capture into a closure and out of it, a tuple's element taken apart, an `inout`
    result and the object it is in, a native function's `view` result, and a `match`
    arm's binding of a subject that is not optional and its write-back.  Both paths
    retain it, and release the same value, with no test for null: Runtime.h's new
    `Paykan_retain_nonnull_inline` and `Paykan_release_nonnull_inline` on the C path
    (docs/c-backend.md).  The mark is of the value, wherever it is used: a type arm's
    retain of an optional subject's box (`match _open(...) { h: Obj {...} None {...} }`)
    keeps its test, since the same box, null, is released on the way to `None`.  A
    release of a variable's slot keeps its test too, since the moves at last use may have
    emptied the slot.  The same failed through an object a value held in memory held (a
    struct parameter, a copy of a struct, a tuple or an array, a union's variant bound by
    a `match`, `inout` too, an optional struct's, a lambda's captured struct: `fn set(s:
    S) { s.c.p = P(5, 6, 7); }`): a copy of such a value retains each class reference it
    holds with no test now, but those of an optional's payload, a tagged union's variants
    (whose other variants are zeros) and a function value's closure.
    `samples/codegen/81_field_struct_store_retained.pkn` writes such fields through a
    variable, a capture, a loop's variable, a copy of an `inout` parameter (a lambda's
    too), of another variable and of an `inout` result, and through each value above, and
    matches a native function's `Obj?` result to its `None` arm; its C builds strictly
    with GCC and Clang at `-O0` to `-O3`.
  - A `match` on a tagged union whose arms return took time quadratic in the union's
    variants to compile: each way out of a scope (every `return`, `break` and `continue`)
    released every reference of every variant of each value the scope held in memory, one
    release per slot written out in full.  Matched by 100 returning arms, a union of 100
    variants lowered to 14,766 lines of PIR and took 4.6 s to `run`, and one of 200
    variants to 55,862 lines and 28 to 30 s.  Such a value is still released in place
    at the end of its scope, and at a way out or as a statement's temporary until its
    function's ways out and temporaries have written 512 releases in place.  After that, a
    struct, a tuple, a fixed-size array or a tagged union whose release takes more than
    four `release`s (an array of more than four elements counting its element's once, for
    the loop that visits them, and a union every variant's fields) is released there by one
    call of its type's drop function, `@.drop.<type>(ptr)`, which the lowering writes once
    per type in the module (docs/pir.md §7) and which releases a tagged union's references
    by its tag, the current variant's fields only.  A drop function links like the module's
    other functions: a template's copy the module lowered, which a program may keep from
    the module's `.pkm` (a standard library container's method, or a generic function the
    module instantiates), calls it from the program's code
    (`samples/imports/22_drop_functions`).  On the LLVM path a drop function's call is
    given a copy of the local, in a slot of its own: LLVM keeps a local whose address a
    call takes in memory throughout its function, so once a function's ways out had used
    the 512 releases (90 earlier statements each dropping a struct of six `Str`s), a loop
    of 30 million `w = mk(k, i); if w.n % 2 == 0 { continue; }` read every field of `w`
    from memory and ran in 0.57 s at `build -O2` against base's 0.30 s; it takes 0.31 s
    now, and a function of 45 early returns dropping two such structs 0.39 s against base's
    0.37 s (0.49 s).  A release of an empty slot does nothing, so a value whose references
    were handed on, or whose bytes are all zero, is safe to drop.
    The 100 variants take 2,811 lines and 0.42 s, the 200 4,467 lines and 0.70 s.  The
    moves at last use count a drop function's call as the release of each of its local's
    references, and PIR's verifier checks that a drop function is `(ptr) -> void` and is
    given the whole of a `mem` local (docs/pir.md §9).  Any other release of a value held
    in memory (an assignment's old value, an object's field when the object is destroyed)
    is in place as before.  Of the 147 programs of the samples and the standard library's
    tests (`samples/codegen`, `samples/leak-check`, `samples/imports` and `tests/Stdlib`),
    only `samples/leak-check/33_drop_by_tag.pkn` and `samples/imports/22_drop_functions`,
    written to reach them, call a drop function: every other lowers to the PIR it did
    without them, so the static counts recorded here do not move, and its loops run as fast
    (a loop of 50 million `m = mk(); match m {...}` on a union of five references takes
    0.25 s at `build -O2`, as it did, where a drop call at the end of its body took
    0.75 s).  Sample 33 drops each kind of value in place and by its drop function, a
    union's by each of its variants and a parameter's whose references were handed on,
    with no live block left on every path.
- **Stability sweep: modules, pkm and driver.**
  - A damaged bitcode cache entry (`.paykan_cache/a/b.bc`, the mlir backend's LLVM IR of
    an imported module) crashed every later compile of the program until the cache was
    deleted, where a damaged entry is promised a rebuild: one changed byte of
    `.paykan_cache/@system/io.bc` gave a segmentation fault, an LLVM assertion or `LLVM
    ERROR: Broken module found`, since no hash covered the file and LLVM's bitcode reader
    is not made for damaged input.  An entry is now a header (a magic number, the entry's
    key and the SHA-256 of its bitcode) and the bitcode: one whose header does not match
    its bitcode is not read, one whose module does not verify is not used, and either is
    translated and written again, `--verbose` saying why (`module ::io: bitcode cache
    .paykan_cache/@system/io.bc is corrupt (its bitcode does not match its hash);
    translating it again`).  An entry that is no regular file is not read but translated
    again too: a FIFO blocked the compile for good, and a link to `/dev/zero` was read
    until LLVM ran out of memory (`LLVM ERROR: out of memory`).  An older compiler's
    entries are translated again once (`... is stale (an older paykan wrote it, with no
    header)`, not `corrupt`).  A module file's compiled code (its OBJ section) that did
    not read or verify failed the compile that linked it, with `LLVM ERROR: Broken module
    found` or an error at the link: the backend now reads and verifies it when the file is
    loaded (`Backend::checkModuleObject`), so a cache entry's is rebuilt and a prebuilt
    file's is an error where it is imported (`module 'm' (m.pkm) has a bad OBJ section:
    the compiled code does not verify: ...`), and the program's LLVM IR is verified before
    it is optimised or run.  The module the backend reads then is the one the program's
    link takes, so the bitcode is read once: it was read again to link it, 4.4% of the
    instructions of `run` in a program of 60 modules.  Past that check a module file's
    compiled code is trusted once its hashes match: bytes crafted to pass them can still
    crash LLVM's reader, so a prebuilt module is as safe as where it came from
    (docs/design/pkm.md §8.4).  The modules the mlir backend writes as bitcode no longer
    carry LLVM's "Debug Info Version" flag (they have no debug info), since the reader
    verifies a module that has it and stops the process when that module does not verify.
  - A module file whose CODE did not decode or verify was accepted when it was loaded and
    reported only when the program was lowered, which then went on without the module and
    stopped at an assertion (`Scope::lookup`, SIGABRT); `--check-only` accepted it, and a
    cache entry of this kind was called usable and never rebuilt.  A file whose interface
    did not match its CODE, its own records or the modules it imports, all its hashes made
    to match, failed PIR's verifier (`extern class 'A' has a different layout than its
    definition in module 'm'`, for a class whose fields its interface gave in another
    order), the lowering's checks (`internal compiler error: a call of slot 6 of 'Seg'
    does not match the class's definition in its module`, for a file whose copy of an
    imported superclass lost a slot; `no function for 'Shape.__init__' called by
    'Sq.__super__'`, in an importer deriving a class from one whose initializer CODE did
    not have; `an optional value where a box is due`, for an initializer whose parameter
    the interface gave as `int?`), or Sema (a signature's unknown type reported under both
    of the import's names, which stopped a debug build with `the same diagnostic was
    reported twice`), or it built a program that crashed when it ran (an empty vtable
    slot, of a class of the module or of a copy of an instantiation it holds; a slot
    naming another function than the method an importer's subclass inherits, whose slot
    the importer then left empty; a constructor returning nothing) or crashed the mlir
    backend (a `void` parameter) or rc-elimination (a function whose next value id is not
    above its ids, which sizes the pass's tables: `std::out_of_range` or a heap overflow),
    or did not link (`JIT session error: Symbols not found: [ pk__SBox_3Cint_3E ]`, for a
    copy of an instantiation's constructor whose name CODE had lost, or which CODE no
    longer had as `shared`).  Now, when the file is loaded, its interface is checked for
    consistency (each superclass a class it describes, whose virtual methods begin its
    subclass's; every class and struct has an initializer), for types it does not
    describe, and against the modules whose types it records; its CODE is decoded,
    verified (a `void` parameter is an error, and so are a class whose fields do not begin
    with its superclass's, a value id at or past its function's next one and a function or
    class with no name) and checked against the interface: each class the module defines
    with its superclass, its own fields in their order, its vtable's slots, its
    initializer and `impl` methods (which an importer calls by their names) and its
    constructor, each copy of an instantiation it holds for what it has, each method of
    each of its structs and each function, a native one's too, all with the PIR types the
    interface's types give them, which every type it can name now has (the optional of a
    scalar or of an enum without payloads is its optional type, an enum without payloads
    an `i64`, a tuple, a fixed-size array or a function value is passed in memory); each
    slot of a class the module defines names the function an importer's subclass inherits,
    the first `X.m` of the class and its ancestors, and a class's functions are `shared`
    exactly when the class is; its externs are checked against the CODE of the module
    files they name; and its templates are decoded.  So such a cache entry is rebuilt and
    such a prebuilt module is an error at the import, `--check-only` included (`module 'm'
    (m.pkm) has a bad CODE section: binary PIR: bad magic at byte 0. Rebuild it from
    source.`, `module 'lib::b' (pre/lib/b.pkm) has a bad IFACE section: the vtable of
    class 'Line' does not start with the slots of its superclass 'Shape'.`, `... has an
    IFACE section that does not match its CODE: method 'Shape.__init__' is not in CODE.`,
    `... class 'Shape': slot 4 ('scaled') of its vtable names '@Rhape.scaled' in CODE,
    which is not the class's or an ancestor's method 'scaled'.`).  A cache entry that
    records a dependency no source or module file is there for is rebuilt too, its
    source's own import saying where a dependency is missing (`module 'bsae' not found`
    was reported at the import of the module the entry is of).  What CODE does not record
    is the layout of a value held in memory (a struct's fields, a tagged union's variants
    and payloads, a tuple's or an array's elements), which a signature passes by its
    address: like the compiled code, it is trusted once the file's hashes match, so a file
    whose interface gives a struct with an initializer of its own its fields in another
    order, its hashes made to match, is used, and the program reads the wrong bytes
    (docs/design/pkm.md §8.4).  The module decoded then is the one the program is lowered
    with, so nothing is decoded twice: `run` of a program importing every module of the
    standard library takes 1% fewer instructions than it did and its `--check-only` 1%
    more (1,196 million and 324 million where there were 1,208 million and 320 million, in
    a debug build); in a chain of 60 modules each importing the one before and defining a
    class, whose interfaces hold 1,830 class records in all, the interface checks add 11%
    to `--check-only` (1,045 million instructions where there were 938 million) and
    nothing to `run` (3,215 million where there were 3,228 million, the compiled code
    being read once).  And the lowering stops at an import it cannot lower, instead of
    lowering the importer without it (docs/design/pkm.md §8.4).
  - `paykan pkm check` printed `usable` for files the compiler rejects or crashes on (a
    CODE section that does not decode, a TMPL section the importer rejects, compiled code
    that does not read or verify, an interface that does not match its CODE), and printed
    it before it decoded anything, followed by an error and exit status 1 for some of
    them.  It now reads the file as a program that imports it does, through the code that
    loads a module (`modules::readModuleContents`): the verdict against this `paykan`, the
    interface, CODE (decoded, verified and checked against the interface), the compiled
    code (read and verified by the backend that would link it: the default one, or
    `--backend`'s) and the templates, and then prints one line, `usable`, or `stale: `,
    `rejected: ` or `corrupt: ` and why (`m.pkm: corrupt: has a bad CODE section: binary
    PIR: bad magic at byte 0`), exiting with 1 when the file is not usable.  A file that
    is not a module file at all is `corrupt: ...` too, where it was an error on standard
    error.  Compiled code another backend or target compiled, which the link refuses, is
    `rejected: its code was compiled by backend 'mlir' (...)`, and an unknown `--backend`
    (`MLIR`) is an error, as for `pkm extract`, where both said `usable`.
  - `--emit-source` with `--emit-c` or `--emit-mlir` was accepted and one of the two
    outputs dropped without a word (`--emit-c --emit-source` printed the C,
    `--emit-source --emit-mlir=scf` the MLIR).  `--emit-source` is the backend's own
    output, the mlir backend's LLVM IR, so with either of them it is now an error
    (`--emit-c and --emit-source exclude each other`), as with `--emit-pir`; with
    `--emit-llvm`, which names the same output, it is accepted.
  - `--emit-pkm` named a module after a path with a keyword in it, and `--module-name`
    took one: `--emit-pkm x/y/if.pkn` and `--module-name=a::match` wrote module files
    no import can name (`import x::y::if;` is a syntax error).  Each part of a module
    name is now an identifier as the grammar defines it, which no keyword is (the
    frontends' list, `frontend::kKeywords`, now in `paykan/Keywords.h`): `cannot name
    the module of 'x/y/if.pkn' after its path: 'if' is a keyword, which no import can
    name (name it with --module-name)`, and `--module-name: 'a::match' is not a module
    name: 'match' is a keyword, which no import can name`.
  - `build -o` into a directory that does not exist, or onto a directory (or `build
    hello.pkn` next to a directory `hello`), compiled the whole program and then failed
    with the linker's own words (`/usr/bin/ld: cannot open output file nodir/hello: No
    such file or directory`).  `build` now checks its executable before it compiles
    anything and says so as the other writers do: `cannot write 'nodir/hello': the
    directory 'nodir' does not exist`, `cannot write 'adir': Is a directory`.
  - `--emit-c` exited 0 with C that could not be linked when an import built from source
    could not get its cache entry (a read-only directory, `.paykan_cache` a file): the C
    declared the module's compiled functions and defined them nowhere (`undefined
    reference to pk_lib_3A_3Ax_Sv`), since that code was in no module file, and
    `--list-modules` failed for it.  Such a module's code now stays in the C, as under
    `--no-module-cache`, and `--list-modules` names the module files the C does need
    (docs/c-backend.md).
  - An error in an imported module named the module's file by its path from the source
    root, which is not that file unless `paykan` runs there: `paykan --check-only
    tasks/main.pkn` reported a mistake in `tasks/tasks/text.pkn` as `tasks/text.pkn:1:27:
    error: ...`, and so did an error in a template's body, the files an import that is not
    found tried (`not found (tried tasks/nothere.pkn, ...)`), an imported file that cannot
    be read or is over the size limit (`'tasks/big.pkn' is too large for a source file`)
    and `--verbose`'s cache entries.  Each is now named by the main file's directory as it
    was given followed by that path (`tasks/tasks/text.pkn:1:27`,
    `tasks/.paykan_cache/tasks/text.pkm`), a path that works from the directory `paykan`
    runs in, and absolute for an absolute main file; the module files record the path from
    the source root as before, so they are the same wherever `paykan` runs
    (docs/language/15-modules.md, docs/design/pkm.md §8.6).
  - `--emit-pkm --module-name=a::b src/a/b.pkn` resolved the module's imports against
    `src/a`, the file's directory, so its sibling `a::c` was looked for as
    `src/a/a/c.pkn` and not found.  When the name's parts before its last are the last
    directories of the file's path, the module's imports are now resolved against the
    directory above them (`src`), where a program that imports it finds them, run from
    anywhere (`cd src/a && paykan --emit-pkm --module-name=a::b b.pkn` too); otherwise
    against the file's directory, as before (docs/language/17-the-paykan-command.md).
  - A line of some 20,000 invalid characters (`$`, NUL bytes) overflowed the stack in the
    lexer (a segmentation fault), far below the 256 MiB a source may have, since it called
    itself once per character it skipped.  It skips them in a loop: 100,000 `$` in a row
    are 100,000 `invalid character '$'` errors, 20 of them printed, and exit status 1.
- **Stability sweep: expressions, places, borrows and closures.**
  - A lambda inside a lambda captured a `weak` variable strongly: only the outermost
    closure held the object weakly.  So the closure an outer one returned kept the object
    alive and read it as such after its last owner let go of it (`weak me = b; outer =
    fn() -> () -> Str { return fn() -> Str => if me == None then "gone" else "alive"; };`
    printed `alive` once `b` was assigned another object), and one stored in the object
    formed a cycle that leaked it (2 blocks, 56 bytes), on every path.  Every lambda
    inside now captures it weakly too, however deeply nested, and it stays read-only there
    as any capture is (docs/language/16-memory.md, "Weak References").
  - The docs did not say how an `f32` or `f16` literal meets the ends of its type.  They
    now do (docs/language/02-basics.md, "Sized numeric types" and "Literals";
    docs/design/sized-types.md): one that rounds to infinity is an error, the midpoint
    above the largest value included (`3.4028235677973366e38` for `f32`, an error since the
    conversions redesign, and `65520` for `f16`), and one below the type's range is 0 or a
    subnormal of its sign, with no error (`h: f16 = 1e-9;` is `0`), where a `float`
    literal that rounds to zero (`1e-400`) is one.
  - A tuple index on an optional tuple said the tuple was none: `t: (int, int)? = (1, 2);
    return t.0;` was `tuple index '.0' applied to non-tuple type '(int, int)?'`, and
    destructuring it, `a, b = t;`, `only tuples can be destructured`.  Both are now the
    error every other use of an optional gives, `cannot use optional '(int, int)?' as
    '(int, int)' without unwrapping (use match)` (docs/language/12-optionals.md,
    "Semantic Checks").
  - A `-` in a match pattern followed by anything but a number said a type was expected:
    `match c { -'a' { } _ { } }` was `unexpected '-'; expected a type`.  It is now one
    error at what follows the `-`, `unexpected character literal; expected a number after
    '-' in a match pattern` (docs/language/08-match.md, "Matching values").
  - A compound assignment whose target had an error reported it twice, once for the
    target and once for the copy of it that reads it (the parser builds `t op= v` as `t =
    t op v`): `xs[y] += 1` with no `y`, and `mk(zzq).n += 1` with no `zzq`, were two
    `use of undeclared variable` errors, which a debug build then aborted on (`internal
    compiler error: the same diagnostic was reported twice`), for each of `+=`, `-=`,
    `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=` and `>>=`; and `xs["a"] += 1` was two errors
    in two wordings.  Each is now one error, and the value's own errors are still
    reported (`xs[y] += zw` is two, `us["a"] <<= zw` reported no `zw` before), unless the
    target names no variable or no field, which ends the check as for `=` (`q.n += zw`
    with no `q` is one error, for `q`, as before).  An index
    of another type in an assignment is said as in a read: `array index must be 'int',
    got 'Str'`, where it was `array index must be int` (docs/language/02-basics.md,
    "Compound assignment"; docs/grammar.md).
  - A local borrow, or a `match` arm's binding that borrows the subject, stayed live until
    the last statement naming it, whatever that name meant there: a later lambda's
    parameter, `for` loop's variable, `match` arm's binding or declaration in an inner
    block of the same name kept it live, so a valid program was rejected (`r: inout = k; r
    = 5; io::println(Str(k)); f = fn(r: int) => r * 2;` was `'k' is borrowed by 'inout'
    local 'r' until 'r' is last used`).  A use is now of the name where it means the
    borrow; a use in a lambda, before an inner declaration hides the name, or in its
    initializer still is one.  An `inout` arm's binding goes back to the subject after its
    last use counted so, before a statement that names another variable of its name
    (docs/language/02-basics.md, "Local borrows").
  - A `match` arm's `n: inout T` could not match a call's `inout` result, though an `inout`
    argument and an `inout` local can name one: `match b.slot() { r: inout R { r.w +=
    100; } A { } }`, with `slot` returning `inout`, was `'inout' arm 'r' needs a variable or
    a field to match on`.  It is accepted, for a method's result (a class's, a generic
    class's, a struct's) and a function's: the call runs once, the binding goes back to
    the storage it returned on every way out of the arm, and what the call borrowed (its
    receiver, its `inout` arguments) cannot be used but through the binding while it is
    live (docs/language/08-match.md, "Borrowing the subject").
  - A field of a conditional's object could not be a borrowed result, though a conditional
    of two places can: `return (if c then a else b).n;` with `a` and `b` `inout`
    parameters of a class, as an `-> inout int`, and `return (if c then a else b).s;` with
    `view` parameters, as a `-> view Str`, were `... must be part of 'self' or of ...
    parameter, not a new value`.  Each is accepted when it is one on each branch, nested
    conditionals and an object in a tuple included; the `inout` result is the field of
    the object the chosen branch holds, which the caller keeps alive while the result is
    used.  A branch that is not one gives its own error: a copy, a local, a `view` for an
    `inout` result, and an object reached through a field of another, `(if c then o else
    self.k).n`, which `self.k.n` is too.  A conditional's struct or tuple is still a
    temporary, whose field or element no `inout` result is (docs/language/03-functions.md,
    "Borrowed results").
- **Stability sweep: classes, generics and names across modules.**
  - A superclass that names something other than a class says what it is: `class A : S`
    with `S` a struct was `superclass 'S' of class 'A' is not defined`, and so were an
    enum, a trait, a builtin type (`class D : int`), a function, a generic struct, enum,
    class or function named without type arguments, a generic class's own type parameter
    (`class K<U> : U`), and an imported struct or enum.  It is now `... is a struct, not a
    class` (`an enum`, `a builtin type`, `a function`, `a generic struct`, `a type
    parameter`, ...), for a generic class `... is a generic class: a superclass cannot be
    generic`, and for a trait `superclass 'T' of class 'C' is a trait, not a class: a
    class implements a trait with 'impl T for C { ... }'` (`impl T for Box<T>` for a
    generic class); a name that is none of these is still `not defined`
    (docs/language/05-classes.md, 06-traits.md, 11-structs.md and 13-generics.md,
    "Semantic Checks").
  - A superclass with type arguments, `class Full : Half<int>` or `class Full<T> :
    Half<T>`, was a bare `unexpected '<'; expected '{' to open the class body`, and the
    parse of the class stopped there, so its body's own syntax errors went unreported.
    It is now `a superclass cannot be generic: a class, generic or not, extends a class
    that has no type parameters`, at the `<`; the type arguments are read past and the
    body is parsed.  The AST interchange reader gives the same error for a superclass
    such as `"Half<int>"`, where it said the name was not an identifier
    (docs/language/13-generics.md, docs/grammar.md, docs/plugins/ast-format.md).
  - The warning that a method never changes `self` and should be a `view fn` was given for
    a generic class's or struct's method when one instance could be one, though another
    could not, so following it broke the program: with `impl Box<T> { fn get() -> T =>
    self.v; }` used as `Box<int>` and `Box<Str>`, `Box<int>` gave `'get' never changes
    'self': make it a 'view fn' ...`, and `view fn get` was then `'self' is read-only in
    'view fn get', so part of it can only be returned as a 'view'` for `Box<Str>`.  The
    method of a template is now one for all its instances, warned about once and only
    when every instance could make it a `view fn`, whatever its type arguments: one that
    returns part of `self` holding a type argument (`self.v` of a `T` field), or keeps it
    in a local, an array or a tuple (`let x = self.v; return x;`, `[self.v, self.v]`,
    `(self.v, 1)`, where a `view fn` is `'self' is read-only in 'view fn get'; it cannot be
    stored ...` for `Box<Str>`), or calls such a method on `self`, gets no warning, also in
    a module that makes only a `Box<int>`, where following the warning broke an
    importer's `Box<Str>`
    (docs/language/05-classes.md, "Methods that don't change self").
  - A module of the program named like a library module could not be imported next to it,
    even with both imports aliased: with a `math.pkn`, `import ::math as cmath; import math
    as mine;` was `import of module 'math': qualifier 'math' already names module '::math'
    (give one of them an alias with 'as')`, and every `mine::` use another error.  A system
    import's full path binds `math` only when no other import of the file binds that name to
    another module (OD-8): its full path is `::math`, which binds no qualifier code can write
    (`::math::sqrt` does not parse) and so never clashes, so the two
    imports go together, and `math::` names the program's module, as it does with `import
    ::math as cmath; import math;`.  The path without the `::` still names the library's
    module when nothing else claims it (`collections::List<int>()` after `import
    ::collections as c;`), and an alias may now take that name from it (`import ::io as
    math; import ::math as m;`).  A plain `import ::math;` binds `math` as its qualifier and
    still clashes with a module `math`, and the uses of the clashing import's other
    qualifiers are no longer reported again, as for an import that fails
    (docs/language/15-modules.md, "Aliased Import", "System Imports" and "Semantic
    Checks").
  - After a type-name clash with an imported type, the uses of the name were taken for the
    other module's type and reported again.  A declaration named like a type an import
    reached (`enum ErrorCode { A, B }` next to `import ::io;`, which imports `::error`'s
    `ErrorCode`) is now the one error: a use of the name in the file means that declaration
    and is not reported (it gave `enum 'ErrorCode' has no variant 'A'` for each variant
    used, and for a class, struct, function or tagged enum named `Error`, `operator '+' is
    not defined for types 'ErrorCode' and 'int'` or `call to undeclared function 'Error'`,
    `'Error::make'`, `'Error::A'`).  An import whose module declares a type another module
    already gave the program reports each such type and then imports nothing of that
    module, so its other types, its functions and its methods are follow-ons too: `e::mk()`
    returning the module's `Error` was taken to return `::error`'s, so `e::mk().code + 1`
    was `operator '+' is not defined for types 'ErrorCode' and 'int'` (docs/language/
    15-modules.md, "Type names are global" and "Semantic Checks").
  - A class whose superclass is in error (`class Puppy : Nope {}`, or `class Puppy : d::Dog
    {}` with `lib::d` an import that failed for an error of its own) is declared without it,
    and each place it then met another class added a type mismatch: `x: Animal = Puppy()`
    was `initializer of type 'Puppy' does not match declared type 'Animal' for variable
    'x'`, and an assignment, an argument (a method's too), a return value, an optional and
    an array element gave their own.  Such a class, or a subclass of it, given for a class
    that can have subclasses is now a follow-on of that error, which is the one reported
    (the exit status is still 1).  A mismatch it could not have kept its superclass for is
    still reported: given for a type that is not a class or for a final one (`s: Str =
    Puppy()`), the other way round (`p: Puppy = Animal()`), or as an optional for a class;
    so is the mismatch of a class in error whose superclass is intact (`class Puppy :
    Animal { x: Nope; }` given for a `Cat`) (docs/language/05-classes.md, "Semantic
    Checks").
  - A clash between two templates' instantiations of one name (type names are global,
    OD-9 b) was reported where the second was made, so a program's own `_Node<int>`,
    instantiated before a `collections::Stack<int>`, gave three errors in the library's
    collections.pkn, one for each use of `_Node<T>` in `LinkedList<int>`,
    and in the other order one error with no word of the other template.  It is now one
    error, at the program's own instantiation, `instantiation '_Node<int>' of '_Node'
    conflicts with the instantiation '_Node<int>' of '::collections::_Node'`, with notes at
    the other template's declaration and at its instantiation and the requests that led to
    it, in either order; a later clash of the name is not reported again.  An instantiation
    clashing with one an import gave the program (`Result<Str, err::Error>` next to
    `import ::io;`) names its template and has a note at the import it came through, and
    at the other template's declaration when the program sees it.  docs/stdlib/index.md
    and docs/language/02-basics.md said a program may declare its own `Result`: they now
    say that the program's instantiations of it are global too, and the first lists the
    names `::collections` reserves,
    `_Node`, `_Entry`, `_Table`, `_Heap` and `_Pieces` (docs/language/13-generics.md,
    "Modules" and "Semantic Checks").
  - A type argument that does not meet its parameter's bound where the bound can be
    checked only once the program's `impl` blocks are known (a class's or a struct's field,
    `class Holder { m: collections::HashMap<Foo, int>; }`) was reported, and then the
    instantiation's bodies were checked anyway: `'Foo' does not implement trait 'Hashable',
    which 'collections::HashMap' requires of 'K'` came with nine more errors in
    collections.pkn (`no method 'hash' on type 'Foo'` seven times, and `_Table`'s bound
    twice), and a `Box<int>` of `struct Box<T: Named>` with `method call on non-class type
    'int'` from its method's body.  The bound error is now the one error, as it is when the
    bound is checked at the request: the bodies of such an instantiation are not checked,
    nor are those of an instantiation only it uses (`Inner<Foo>` for its field `i:
    Inner<T>?`, which gave `no method 'h' on type 'Foo'`, in the library's file when the
    template is another module's), and the bound of an instantiation made in one is a
    follow-on.  An instantiation the program also uses itself, in a declaration or in any
    body (a generic function's or a generic class's method included), is checked as before;
    when only a body checked after it asks for it, its errors come after that body's.  The
    order of the other diagnostics is unchanged (the bounds were already checked before
    every body)
    (docs/language/13-generics.md, "How generic code is checked"; 06-traits.md, "Semantic
    Checks").
  - Checking that a struct, or an enum's variant, does not hold itself in place looked into
    an enum once for each path to it, so a graph of enums each with three variants holding
    the one before (`enum E1 { A(x: E0), B(y: E0), C(z: E0) }`, and so on) took three
    times as long for each enum more: 13 of them 0.45 s under `--check-only`, 15 4 s, 16
    13.3 s, and 20 more than 2 minutes.  Each question looks into a type once now: 20 enums
    take 0.01 s, and 400 take 0.14 s.
- **Stability sweep: runtime, standard library and tests.**
  - Building a string by appending took time quadratic in its length: `s.concat(t)`
    allocated a block for the whole string and copied it there at each append, and
    `collections::listToString` put its result together with `+`, copying the text so far
    for each element. Under `paykan run`, 1,600,000 one-byte appends took 16.4 s of
    processor time and `listToString` of 400,000 ints 15.8 s (400,000 appends 1.1 s, and
    100,000 ints 0.9 s). `concat` now grows the string's own block with C's `realloc`,
    which can extend it where it is, and `listToString` appends each element's text to a
    string of its own with `concat`: each of the two takes 0.14 s, compiling included
    (docs/language/04-strings-and-conversions.md, "String Type"; docs/stdlib/collections.md,
    "listToString"). `PaykanString` keeps its layout and the runtime ABI its version; the
    runtime only gains a test hook, `Paykan_heap_total_allocs()`, the number of blocks the
    tracking allocator has allocated (Runtime.h).
  - `File.readbytes(n)` allocated `n` bytes before reading: `readbytes(1099511627776)` of a
    5-byte file was `OutOfMemory` where the system refuses an allocation that large
    (Linux) and took 1 TB of address space where it does not (macOS), and
    `readbytes(1000000000)` 1 GB.
    `n` is a maximum: the buffer starts at 64 KiB (or `n` bytes, when that is less) and
    doubles as the data fills it: the tracking allocator's peak for either read of the
    5-byte file is 65,630 bytes, where it was 1,099,511,627,870 and 1,000,000,094
    (docs/stdlib/io.md, "File.readbytes"). The runtime gains a test hook,
    `Paykan_heap_peak_bytes()`, the most bytes the tracking allocator has had live at once
    (Runtime.h).
  - `List`'s `indexOf`, `contains` and `remove` read the list's length once, before the
    walk: an element whose `equals` emptied the list made the next comparison panic with
    the Buffer's internal message, `buffer index 1 out of bounds (len=0)` (exit 134). The
    length is read again for each element, so such an `equals` makes them find nothing
    (`None`, `False`, `False`) (docs/stdlib/collections.md, "List<T>"). A buffer's length
    is read in place, with no call of the runtime's `PaykanBuffer_len`: the LLVM path loads
    it, and declares `PaykanBuffer_get` as writing no memory the program reaches, so a walk
    whose `==` writes nothing (a `List<int>`'s) loads it once, before the loop, and one
    whose `==` calls an `equals` loads it for each element; the C path calls `Runtime.h`'s
    `PaykanBuffer_len_inline` (docs/c-backend.md, "Calls"). The call for each element had
    made a search of a 1,000-int buffer 33% slower (`build -O2`, 200,000 searches, median
    user CPU of 11 interleaved runs: 0.261 s against 0.196 s with the length read once) and
    50,000 of a 1,000-`Str` buffer 13% (0.145 s against 0.128 s): they take 0.189 s and
    0.127 s now.
  - The C path and the runtime round an `f32` value through a C `float`, but `Runtime.h`
    checked only that `double` is IEEE 754 binary64. It also checks now that `float` is
    binary32 (`FLT_MANT_DIG == 24`, `FLT_MAX_EXP == 128`, `FLT_MIN_EXP == -125`), so a
    target whose `float` is another format fails to compile instead of rounding `f32`
    values wrongly (docs/c-backend.md, "Target assumptions"). Every supported target's is.
  - The runtime's unit tests leaked: the helpers of `NumericTests` and a round trip of
    `FileTests` freed each string's bytes with `PaykanString_destroy` and never the string,
    and the two `DestroyViaVtable` cases never freed their object, 20,106 blocks (965,072
    bytes) in all under macOS's `leaks --atExit` (LeakSanitizer on Linux failed
    `RuntimeTests` with them). They free each string with `Paykan_dispose`, and an object
    whose destroy they call through its vtable with `Paykan_free` after it, as
    `Paykan_release` does: `leaks` finds none.
  - Four files of `sema_tests` defined a `struct SemaFileResult` at namespace scope, not all
    with the same members, which broke C++'s one-definition rule: the linker could take
    one file's for another's (chunk C moved `ModuleTests.cpp`'s, whose count was left
    unassigned that way). `ImportTypeTests.cpp`, `OptionalTests.cpp` and `TupleTests.cpp`
    define theirs in an anonymous namespace too.
  - The language reference did not say that a native function's integer result crosses
    as an `int64_t` whatever its Paykan type, so a C function returning a C `int` bound as
    it is (`native fn rand() -> int = "rand";`) read the undefined upper half of the
    register. docs/language/03-functions.md ("Native Functions") says so now, and shows
    the wrapper returning an `int64_t` to bind instead (KF-9). Mapping `i32` and `u32`
    natives to `int32_t` and `uint32_t` is left for a later release (OD-17).
  - A checked read of a `memory::Buffer` slot (`at`, `get`, `copy`) of a type whose zero
    bytes are no value, such as an enum whose first variant holds a `Str`, tested that the
    slot was set with a call to the runtime's `PaykanType_isUnset`, and a runtime compiled
    by GCC (CMake's C compiler on Linux) saved and reloaded the read's registers around
    that call. Setting 1,000,000 such slots and reading them 9,000,000 times (the
    `buffer_union` program of the allocation slowdown, `build -O2`, median user CPU of 11
    interleaved runs on an Apple M3 Max, with the runtime built by GCC 16 at CMake's
    Release flags) took 0.113 s, against 0.106 s at `fab0a56`, before the test was added.
    The read tests the slot's words itself now, with no call: 0.103 s with GCC, and 0.103 s
    with Apple clang's runtime (0.111 s before, 0.114 s at `fab0a56`), 8.6% fewer
    instructions (1.906 billion, from 2.084). The other two programs of the slowdown were
    not slower than at `fab0a56`, before or after: 10,485,740 objects allocated and freed
    (`make(18)` 20 times) take 0.296 s (0.301 s at `fab0a56`), and 3,000,000
    `Str(i) + "x"` 0.484 s (0.506 s).
  - A generic function's instantiation whose signature is an error (an instance of a
    generic native function binding a C symbol that another native function binds with a
    different signature) was reported at its first call, and again at each later call whose
    value initializes a declaration with `let` or with a type: after `let a =
    absOf<int>(1);`, `let b = absOf<int>(2);` and `c: int = absOf<int>(3);` each added
    `cannot determine type of initializer for variable ...` (`d = absOf<int>(4);` did not).
    The error at the first call is the only one now. The standard library's `let`s (see
    "Added") made a program clashing with an instance of `::memory`'s `_len` show it.
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
- Deeply nested lambda bodies, function-type results and array type suffixes did not count
  toward the nesting limit, and overflowed the stack: `fn() => fn() => ... 0` 5000 deep
  and `() -> () -> ... int` 10000 deep crashed the parser (exit status 139), 3000 nested
  lambdas crashed `paykan run`, and `int[][]...` 20000 deep crashed Sema.  A lambda's and
  a function's `=>` body (as the block it stands for), a lambda's result type, a function
  type's result and each `[]`, `[N]` and `?` suffix, around the levels of the type it
  wraps, are now a level each, and 513 of them are `nesting too deep (more than 512
  levels)` (docs/grammar.md section 9).  The AST interchange reader, which reads the AST
  of a frontend plugin and the `ast-text` frontend's input, held an AST only to its 2048
  lists, so it took 600 nested blocks or 600 nested lambdas, which no frontend parses.  It
  now counts the levels the frontends count, in the AST, and rejects one more with the same
  message (docs/plugins/ast-format.md section 3).
- Recovering from `nesting too deep` in nested arrays of arrays took time quadratic in their
  depth: `[[[...0; 1]; 1]; 1]` 20000 deep took 38 seconds and reported 20001 errors (and
  parentheses, subscripts and calls inside such arrays one minute), each printing the
  whole line, as the statement's recovery stopped at the `;` of each `[v; n]` left open.
  The brackets, parentheses or arguments one level too deep are now skipped whole, through
  their closer, as a block one level too deep is: the limit is the only error, in 0.1
  seconds, and the statement after them is parsed (docs/grammar.md section 9).
- Long chains of operators and postfix suffixes overflowed the stack on valid programs:
  `x = 1 + 1 + ...` with 10000 terms, `True && ...` and `x - x - ...` crashed Sema (exit
  status 139), `"a".toString().toString()...` 5000 deep crashed the lowering, `x.0.0...`
  and `x[0][0]...` 20000 deep crashed Sema, and 20000 `elif`s crashed `paykan run`.  The
  recursive-descent frontend took ASTs that the AST interchange format could not hold (3000
  terms, 3000 `elif`s, or 511 nested `let f = fn() { ... };`: `paykan --emit-ast` failed
  with `nesting too deep (more than 2048 levels)`), so no frontend plugin could hand them to
  `paykan`.  Now:
  - A chain of more than 2048 links along any path down the AST is `nesting too deep (more
    than 2048 levels)` on every frontend: each binary operator is a link over its two
    operands, each call, field, method call, tuple index and subscript over the expression
    it applies to and its arguments, and `x += e` over `x` and `e`, counted through
    parentheses, operands and arguments (docs/grammar.md section 9,
    `frontend::kMaxChain`).  An `elif` is a level of nesting, as the `if` of an `else`
    block: an `if` takes up to 510 of them in a function's body, and past them the rest
    of the chain is skipped after the block one level too deep, so the limit is its only
    error.
  - Sema, the lowering (with its analyses of the reads a call may replace) and the copies of
    a generic's body take a chain from its innermost link out, in a loop, and the lowering's
    address of a field of a struct, an element of a tuple or of an array in a chain of them
    is one loop too, through the calls in such a chain as well: a struct's methods
    (`p.m().m()...`, generic structs' too), a field's function value (`c.f().f()...`), and
    the element, the field or the function value of a call's result (`c.t().0.t().0...`,
    `p.a()[0]...`, `c.g()()...`).  A lambda in a lambda's body is lowered after that body,
    in a loop too, and the parser, the AST interchange reader (which reads its text's lists
    in a loop) and the lowering of method calls hold the frames a nesting recurses through
    small: a level of `let f = fn() { ... };` took about 16 KB of an AddressSanitizer
    build's stack (without its fake stack for use after return) in the parser and 17 KB in
    the lowering, which overflowed 8 MB at 511 levels, and takes 6 KB in the parser and none
    in the lowering.  Every chain and nesting within the limits, and the deepest mixes of
    them (2048 links in 511 levels of lambdas), compiles and runs on every path (`paykan
    run`, `paykan build`, `--emit-c`) with a quarter of the default stack, 2 MB, in a GCC
    debug build, and with half of it, 4 MB, in a clang one, whose unoptimised frames can be
    4 times larger (4 KB, not 0.9 KB, for a binary operator in the lowering), and the test
    suite checks that it does.
  - The AST interchange format nests up to 4112 lists (`kMaxDepth`), which holds every AST
    within the limits, and its reader counts the links and the levels as the frontends do,
    so an AST from a plugin, or the `ast-text` frontend's input, is held to the same limits
    (docs/plugins/ast-format.md section 3).  An operand the grammar writes in parentheses
    is the level its parenthesis is, though the AST has none: a right operand of the same
    or a lower precedence (`a + (b + c)`), a comparison compared (`(a == b) == c`), an
    operation under a prefix operator or before a suffix (`-(a + b)`, `(a + b).m()`), a
    conditional as an operand.  So the reader takes no `(binary add (int 1) (binary add
    ...))` 2048 deep, which no frontend can write and which overflowed Sema at 2 MB, an `==`
    chain the lowering took a level a link, or an `&&` nesting 2048 regions for `--emit-c`.
- A `match` on strings, floats or classes with thousands of arms overflowed the stack on a
  valid program: 3000 string arms crashed `--emit-pir`, `paykan run`, `paykan build` and
  `--emit-c` (exit status 139) with a quarter of the default stack, 2 MB, and 10000 with the
  default 8 MB, as the lowering nested each arm's test in the `else` of the one before, a
  block an arm, which the passes and backends recurse through.  The lowering tests the arms
  in turn, each only while none before it held, as before, then switches on the index of the
  first that held (docs/pir.md section 6): two blocks deep however many arms there are.
- A frontend plugin's AST, or the `ast-text` frontend's input, could declare a local without
  an initial value, which the grammar has no way to write (`x: int;` is not a statement):
  `(decl (var "s" (named-type "Str") _))` passed every check and `io::println(s)` crashed
  (exit status 139) on a null reference.  The AST interchange reader now rejects a `decl`
  without an initialiser that is not `weak` (`a local declaration has an initialiser (only
  a (weak) one may leave it out)`, docs/plugins/ast-format.md), and Sema rejects such a
  local whichever frontend built it (`variable 's' needs an initial value (only a 'weak'
  local starts without one, as None)`), so none reaches the lowering.  A `weak` local
  without one (`weak w: Node?;`, which starts as `None`) and a `for` loop's variable stay
  as they are.
- A frontend plugin's AST, or the `ast-text` frontend's input, could give a `match` arm on a
  number, a character or a string a pattern that is not a literal, which the grammar has
  no way to write: `(value-arm "" (ident "k") ...)` or `(binary add (int 1) (int 2))`
  passed `--check-only` and failed PIR verification at run time (`switch case %1 is not a
  constant`).  The AST interchange reader now rejects it (`a value arm's pattern is a
  literal (int, float, bool, char, string or none)`, docs/plugins/ast-format.md), and Sema
  rejects such an arm whichever frontend built it (`a value-match arm's pattern must be a
  literal (a number, a character, a string, True, False or None)`).  A negative number's
  literal, `(int -1)`, stays a pattern.
- A negative number could not be a `match` pattern: `match n { -1 { ... } _ { ... } }` was
  `unexpected '-'; expected a type`, though the AST interchange format's `(int -1)` was one,
  and docs/language/08-match.md did not say so.  `-` and an integer or a float, together
  or apart (`-1`, `- 2.5`), is now one literal of the negated value (docs/grammar.md
  section 6), which takes a sized subject's type when it fits as an assigned one does: `-1`
  on an `unsigned` is `the literal -1 does not fit in 'unsigned' (0 to
  18446744073709551615)`.  The smallest `int` is the pattern `-9223372036854775808`: the
  lexer takes that magnitude right after a `-`, and the parser rejects it anywhere else
  with the error it had (`integer is out of range: 9223372036854775808`), so an expression
  still writes it `-9223372036854775807 - 1`.  The C `--emit-c` prints spelled such a
  `switch` case `case -9223372036854775808:`, which a C compiler rejects as a constant too
  large for any signed type (from an AST plugin's `(int -9223372036854775808)` pattern
  too); it is `case INT64_MIN:`.  `samples/codegen/72_negative_patterns.pkn` runs them.
- The AST interchange reader took ASTs the grammar has no way to write, from a frontend
  plugin or as the `ast-text` frontend's input: an enum without a variant and a `match`
  without an arm (both checked and ran), a binding on a wildcard arm (dropped: `use of
  undeclared variable 'w'` where it was used) or a value arm, and names that are not the
  grammar's: a local `"a b"` ran, an assignment to `"io::x"`, a destructuring target
  `"io::a"`, an import alias `"a::b"` or a module `"../../etc/passwd"` passed `--check-only`,
  and a call of `"$::result::Result::Ok"` named what only the compiler's renamed templates
  name.  It now rejects each, with its position (`an enum needs at least one variant`, `a
  match needs at least one arm`, `(wildcard-arm ...) has no binding: its binding is ""`,
  `expected an identifier: the variable's name, not "a b"`, the name quoted as the format
  writes a string, so a NUL in it prints as `\x00`): a name is an identifier, not a
  keyword or `_`, and a qualified one only where the grammar writes a module path (a
  callee, an enum, a type, a superclass, an `impl`'s type, an import's base)
  (docs/plugins/ast-format.md sections 2 and 3).  The grammar's keywords are
  `frontend::kKeywords` (`include/paykan/Frontend.h`, with `frontend::isIdentifier`), which
  the recursive-descent lexer's table matches.  A `.pkm`'s templates, whose renamed bodies
  name `$::module::name`, are read with `interchange::Origin::Templates`
  (`include/paykan/ast/Interchange.h`), which keeps those names.
- A debug build of `paykan` aborted (`internal compiler error: the same diagnostic was
  reported twice`, exit status 134) on a frontend plugin's AST, or the `ast-text` frontend's
  input, with no locations or coarse ones: two calls of an undeclared function whose nodes
  had no location, or shared `@1:1-1:5`, gave the same error twice, which the guard against
  a construct reported twice (`DiagEngine.h`) took for one repeat.  The guard no longer
  checks a diagnostic without a location, and is off for a frontend plugin's AST
  (`DiagEngine::setExactLocations`), whose locations may be coarse
  (docs/plugins/ast-format.md): both errors are printed and the check fails with status 1,
  as in a release build.
- `paykan --dump-tokens` exited with status 0 on a lexical error, which it printed in a
  format of its own without the file (`error @1.26: invalid character '$'`), and so did a
  frontend plugin's `dump_tokens` that failed or reported an error.  A lexical error is now
  reported as a syntax error is (`l.pkn:1:26: error: invalid character '$'`, with the
  source line and a caret, docs/grammar.md section 9), the listing is still printed whole,
  and `paykan` exits with status 1 (docs/language/17-the-paykan-command.md,
  docs/plugins/plugin-api.md).  A static C++ frontend's `Frontend::dumpTokens`
  (`include/paykan/Frontend.h`) takes the driver's `sema::DiagEngine &` to report them
  through, as `parse` does, so such a frontend is rebuilt for it, as the 0.3.0 version
  requires anyway.
- A `mov` in the operand of another was reported first: `u = mov mov s;` gave the inner
  `'mov' was removed in v0.2.0; ownership transfers are inferred` (at 1:37) before the
  outer one (1:33), though every frontend reports its errors in source order
  (docs/grammar.md section 9), and `mov` before an operand that does not parse (`b = mov
  ;`) was not reported at all.  The removal is now reported at the `mov`, before its
  operand is parsed, and a run of them (`mov mov s`), which was an error a `mov`, is one
  error at the first.
- `impl Box<int> { }` was `error: expected a type parameter name in 'impl Box<...>', not
  the type 'int'` with no position, so no file, line or caret: a builtin type (`int`,
  `Str`, ...) in a type parameter's place has no location of its own.  It is now reported
  at the argument (`impl.pkn:2:10: error: ...`).  docs/grammar.md and
  docs/language/03-functions.md gave a native function's parameter without a type the
  message of a lambda's or a method's; they now give the one the frontends report,
  `parameter 'x' needs a type ('x: int'): a native function's parameters have types`.
- docs/grammar.md's `implMember` gave a `static fn` a block body only, though `static fn
  f() -> int => 1;` is accepted, and documented in docs/language/03-functions.md: it ends
  with `( block | "=>" expression ";" )`, as a `funcDecl` does.
- A field of a conditional `(if c then a else b)` of class type was compiled wrong, though
  the program checked: `io::println((if k == 1 then a else b).name);` printed nothing, `x =
  (if k == 1 then a else b).name;` left `x` undeclared (a later use of it failed an assertion
  in the compiler), `(if k == 1 then a else b).n = 5;` did nothing, an element of a tuple
  field (`(if k == 1 then a else b).t.1`) printed nothing, `(if k == 1 then a else
  Node("b")).name + " "` failed the PIR verifier (`operand of box 0 has type i64, expected
  obj`), and `bump((if k == 1 then a else b).n)` with `fn bump(x: inout int)` was an
  internal compiler error.  The lowering took the class of a field's receiver only from a
  variable, a field, a call or an element; it now takes the type Sema gave any receiver, so
  each of these reads, writes or passes the field of the object the chosen branch gives (a
  method call on a conditional already worked).  A receiver of no class, which Sema lets
  through none of, is an internal compiler error instead of a statement left out.  As the
  field of a call's object is (`bump(pick(k, a, b).n)`), the field of a conditional's object
  is a place an `inout` parameter takes; a conditional's struct is a temporary, whose field
  is not (`a field of a struct that is not a variable or a field (a temporary, which dies
  with the statement) cannot be passed to 'inout' parameter 'n'`).
  `samples/leak-check/31_conditional_receivers.pkn` reads, assigns, passes and matches such
  fields on both branches.
- What a `view` holds could change through a conditional of it: with `a: view Node` and
  `b: Node`, `(if k == 0 then a else b).inc()` called a method that is not a `view fn` on
  `a`'s object, and `(if k == 0 then a else b).child` could be stored, passed to a
  parameter that is not `view` or returned.  Once a field of a conditional's object was
  compiled (above), `(if k == 0 then a else b).n = 7`, `+=` and `bump((if k == 0 then a
  else b).n)` wrote it too, and so did `(if k == 0 then self else o).n = 99` in a `view
  fn`.  Sema's `view` checks looked only at a variable the place is in; a conditional's
  object is now what either branch holds, and it gets the errors that branch would (`'a'
  is a 'view' parameter; cannot assign to its field 'n'`, `'self' is read-only in 'view fn
  peek'; ...`), nested conditionals and `view` locals included.  A method that changes
  `self` through a conditional is no longer told it never changes `self` (`make it a
  'view fn'`).
- `<`, `<=`, `>` and `>=` on two `char`s compared their bytes as signed, so a byte of 128 to
  255 ordered below every ASCII one, against the language's ordering by code, `0` to `255`
  as `int(c)` reads it: `char(200) > char(100)` was `False`, and so was `"é"[0] > 'z'`, on
  every path.  The lowering now zero-extends both chars to `int`s, as `int(c)` does, before
  it orders them; `==` and `!=` are unchanged (docs/pir.md §6: PIR's `cmp` on two chars
  still orders them signed, and the lowering no longer gives it two to order).
  `samples/codegen/69_char_ordering.pkn` orders every pair of eleven codes on both sides of
  128 under each operator, bytes of a UTF-8 string and a sort.
- A value `match` with an arm of `9223372036854775807`, the largest `int`, and any other arm
  failed to compile on every path (`the MLIR pipeline failed: 'scf.index_switch' op has
  duplicate case value: 9223372036854775807`) unless that arm came first: `match n { 0 {
  ... } 9223372036854775807 { ... } _ { ... } }`.  The switch such a match lowers to keeps
  its cases in a set that takes `INT64_MAX` (and `INT64_MIN`) for its empty (and deleted)
  key.  A case of either value is now a test of its own, around the switch of the other
  cases (docs/design/mlir-backend.md, "The shared lowering"); the PIR such a match lowers to
  was already valid.  `samples/codegen/70_match_int64_max.pkn` puts the arm first, second,
  last and alone, with a default arm and without one.
- The two native-function errors that list what crosses into C left out the sized numeric
  types, which cross as `int` and `float` do: `native fn f(s: Str)` said `only int, float,
  bool, char and an enum without fields cross into C by value`, and `native fn g(a: A?)`
  said `cannot cross into C (int, float, bool, char, an enum without fields, Str and Obj
  can, ...)`.  Both now name `a sized numeric type` after `char`
  (docs/language/03-functions.md, "Native Functions").
- Two native functions that bind one C symbol with different C signatures passed the checks:
  in one module (`native fn f(x: int) -> int = "llabs"; native fn g(x: float) -> float =
  "llabs";`, or `view Str` against `int`) the program then failed PIR verification
  (`argument 0 of '@$c.llabs' %2 has type f64, expected i64`), and across two modules it ran
  with a wrong result, calling `llabs` with a double.  A native function bound to a runtime
  function that compiled code calls itself, with another signature (`native fn cat(a: int)
  -> int = "PaykanString_concat"`), failed the MLIR pipeline once both were called
  (`'func.call' op incorrect number of operands for callee`), and one bound to a function an
  executable's entry code or the C of `--emit-c` calls (`native fn hr(x: int) -> int =
  "Paykan_heap_reset"`, `memcpy`) gave C that did not compile (`too few arguments to
  function 'Paykan_heap_reset'`, in C's `main()`).  A C symbol now has one signature in a
  program (docs/language/03-functions.md, "Native Functions"): Sema rejects the later of two
  natives of a module that disagree (`native function 'g' binds C symbol 'llabs', which 'f'
  already binds with a different signature`, and a note where `f` is, which may be another
  file: the program's own native function, against an instance of a generic native function
  of the standard library) and a native bound to a runtime function with a signature other
  than the runtime's (`native function 'cat' binds C symbol 'PaykanString_concat', a
  function of the runtime, with a different signature`), the entry code's
  (`Paykan_heap_reset`, `Paykan_args_free`, ...) and the C library's that compiled code
  calls (`fmod`, `memcpy`, `memmove`, `getenv`) included; no native function has the
  signature of one that takes a C `int` (`memset`, `Paykan_heap_set_tracking`,
  `Paykan_args_new`).  The same signature twice stays fine, and so do the standard library's
  bindings (`Paykan_io_write`, `fmod`, ...).  The driver checks the modules of a program
  against one another once they are lowered (`native functions of modules 'main' and 'na'
  bind C symbol 'llabs' with different signatures`), whether a module is built from source
  or loaded from the module cache: a module's `.pkm` CODE now keeps the native C functions
  its compiled code calls.  Sema computes a native's C signature with the helper the
  lowering declares and calls the C function with (`sema::nativeSignature`), and the PIR
  verifier rejects a native and a runtime extern of one C symbol, in a module or in two,
  with different signatures (docs/pir.md §3, §9).
- `--emit-c` gave C that did not compile for a native function bound straight to a C library
  function, which `run` and `build` call fine: `native fn llabs(x: int) -> int = "llabs"`
  failed with `conflicting types for 'llabs'; have 'int64_t(int64_t)'`, since `<stdlib.h>`
  declares `long long llabs(long long)`, and `native fn quit(code: int) = "exit"` likewise.
  The C now renames each native function's symbol while it includes the headers (`#define
  llabs pk__hidden_llabs` before them, `#undef llabs` after, which also drops a macro a
  header defines for the symbol, such as `isnan`), declares the native function from its own
  signature after them, once, ahead of the C of every module (one that does not bind the
  symbol may call it as the runtime's: `fmod` for a float's `%`, `Paykan_panic_index` for an
  index check), and turns off the compilers' warning of a library function declared with
  other types (docs/c-backend.md, "Native functions").  `Runtime.h`'s `static inline`
  functions, which the rename covers too, call no C library function: `Paykan_vtable_of`
  copies the vtable pointer a byte at a time, not with `memcpy`, which a native function may
  bind.  A call that does not read such a function's result keeps it and casts it to void:
  `floor(x);` was `statement with no effect` to GCC and `ignoring return value of function
  declared with const attribute` to Clang, which know `floor` as pure.
  `samples/codegen/71_native_libc.pkn` binds `llabs`, `labs`, `fabs`, `exit` and `floor`;
  the CStrictC11 check compiles it with GCC and Clang.
- The time and memory it took to compile a function grew with the square of how deeply its
  blocks nest, and an `if` with a long `elif` chain nests one block per arm: the PIR
  verifier, which checks every lowered program, copied the values visible in a block into
  each block in it.  With a Debug build of the compiler, `--emit-pir` of a function of
  4000 arms took 18 s and 703 MB (1.2 s and 64 MB for 1000), and `build` 24 s and 725 MB.
  The verifier now keeps the visible values in a scope stack, so a block costs what it
  defines: closing it takes its own definitions back out.  The 4000 arms take 1.2 s and 33
  MB to `--emit-pir`, most of it spent printing 96 MB of indented PIR, and 7.8 s and 82 MB
  to `build`, most of it in the MLIR and LLVM pipelines.  The verifier reports what it did
  before, and the samples compile as fast as they did.
- `==` and `!=` did not take an `int` with a `float`, though `<`, `<=`, `>`, `>=` and the
  arithmetic did and the docs say the one promotion is `int` with `float`: `x == y` with
  `x = 2; y = 2.0;`, `1 == 1.0` and `2.5 != x` were `operands of '==' have mismatched types
  'int' and 'float'`.  They now compare as floats, the int converted (`2 == 2.0` is `True`,
  a NaN is unequal to every int), on every path; `(1, 2) == (1, 2.0)` stays an error, the
  tuples being of two types.  The pairing is one rule, `Sema::promotedOperandType`, which
  arithmetic, the ordering compares and equality ask (docs/language/02-basics.md,
  "Relational & Equality").  `samples/codegen/73_int_float_equality.pkn` compares the pair
  in each order, through a parameter, a field and a loop's condition.
- A generic enum's variant written without type arguments took them from the other side of
  `==` or `!=`, or the other arm of a conditional expression, only when it came second:
  `x == Maybe::Nothing` and `if c then Maybe::Just(n) else Maybe::Nothing` compiled, but
  `Maybe::Nothing == x`, `Maybe::Nothing != Maybe::Just(1)` and `b = if n > 0 then
  Maybe::Nothing else Maybe::Just(n)` were `cannot infer the type arguments of generic enum
  'Maybe' for 'Maybe::Nothing': ...`, and `Result::Ok(1) == r` could not infer `E`, against
  docs/language/07-enums.md, which names no order.  So could a generic function's call that
  takes its type arguments from the type its result flows into (`fn none<T>() -> Maybe<T>`,
  `res::ok(1)`): `r == res::ok(1)` compiled, `res::ok(1) == r` did not.  The side that
  needs the other's type less is now typed first (`ExprChecker::needsOtherSide`: a variant
  without fields needs it, and so does a variant whose fields leave a type parameter open
  (`Pair::Left(1)` of a `Pair<A, B>`, `Result::Ok(1)`) or whose field needs it
  (`Maybe::Just(Maybe::Nothing)`), and a generic function's call whose parameters leave
  one open; another variant made with fields may infer them from its fields; anything else
  does not need it; of two that need it as much, the first is typed first), in either
  order of `==` and `!=` and of a conditional's arms, nested conditionals and a
  conditional compared with a variant too; so `Maybe::Just(200) == small` with `small:
  Maybe<u8>` compares two `Maybe<u8>`, as its mirror did, and `Maybe::Just(1) == none()`
  and `if c then Maybe::Just(2) else none()` give `none` the variant's type, as they did.
  `Maybe::Nothing == Maybe::Nothing` and `none() == none()` stay errors, having nothing to
  take them from.  `samples/codegen/74_generic_enum_either_side.pkn` runs each form.
- A declaration named like a type an import reached is reported as the conflict it is,
  whatever it declares: `enum ErrorCode { A }` in a program that imports `::io` was
  `redefinition of enum 'ErrorCode'`, and `enum Error { B }`, `struct ErrorCode { ... }`,
  `trait File {}` or `fn Error()` said `'Error' is already declared as a struct` (or a
  class, or an enum), as if the program had declared it; each is now `enum 'ErrorCode'
  conflicts with an imported type of the same name` (`struct ...`, `trait ...`, `function
  ...`), the words `class File` already got.  Two enums of the program stay `redefinition of
  enum 'E'`.  And the docs no longer say a program that imports `::io` cannot declare its
  own `Result`: a generic type of a module is named through it (`res::Result<int, Str>`)
  and leaves its name free, as a program's `class Result` next to the library's showed
  (docs/language/02-basics.md, "Builtin Names Are Reserved"; 15-modules.md;
  docs/stdlib/index.md).
- A module's generic function used as a value, `x = io::println`, was `unknown enum type
  'io'`, and so was a name the module does not have (`x = io::nosuch`, `col::List`).  The
  first is now the error a local generic function gets, `'io::println' is a generic
  function; a generic function cannot be a function value yet (wrap a call to it in a
  lambda)` (one helper, `Sema::errorGenericFunctionValue`, says it for both), and the
  second `'io::nosuch' is not a function of module '::io'`, naming the module as imported;
  `unknown enum type 'Nope'` stays for a qualifier that is no module's
  (docs/language/14-function-types.md, "Function Values").
- Two imports binding one qualifier name each module by its canonical name: `import
  a::util; import ::io as util;` said `import of module 'io': qualifier 'util' already names
  module 'a::util'`, dropping the system module's `::` (and `... already names module 'io'`
  in the other order); it is `import of module '::io': ...` and `... already names module
  '::io'` now (docs/language/15-modules.md, "Aliased Import").
- A `match` arm on an enum whose type is no name, `a: int { }` (or a tuple, an optional, an
  array), was `'' is not a variant of enum 'E'`; it names the type and what an arm names
  now: `'int' is not a variant of enum 'E': an arm names one of its variants, such as 'A'`
  (docs/language/08-match.md, "Semantic Checks").
- A slice type outside a parameter suggests a length only where a fixed-size array would
  do: `struct P { a: int[]; }`, `enum E { A(a: int[]) }` and `t: (int[], int)` were told
  `... a slice can only be a 'view' or 'inout' parameter, and an array has a length
  ('int[3]')`, which those positions reject in turn; they now end `..., and a struct field
  (a variant's field, a tuple element) cannot be an array yet`, an optional's value or a
  type argument (`int[]?`) gets no hint, and a local, a result and a class field keep
  `('int[3]')`.  The fixed-size array error names the field it can be: `... can only be a
  local variable, a parameter, a result or a class field for now`, where it said `a field`
  (docs/language/09-arrays.md, "Slices" and "Limits for now"; 10-tuples.md).
- The error for a local named like a function of the module is reported once, as its
  entry above says: `func = 1; func = 2; func = 3;` reported `'func' is a function of this
  module and cannot be used as a variable` three times, once per assignment, and a
  destructuring target binding the name again (`func, b = (3, 4)`) once more.  The first
  rejected binding poisons the name, and an assignment or a destructuring target binding it
  again is now its follow-on (`Sema::rebindsPoisonedFunctionName`), in an inner block too
  (docs/language/02-basics.md, "Scoping Rules").  The first binding is reported though its
  value has an error of its own: `func = nope; func = 2;` reported `func` at the second
  assignment, `func: int, b: int = nope;` bound the name to an `int`, and `a, func = 5;`
  did not report it at all.
- An error in an imported module's template, reported where the program instantiates it,
  names the module's generic enum, its variants and its generic functions as the program
  spells them: `R::Ok(s)` in `fn wrap<T>(s: view T) -> R<T>` of `lib::g` said `parameter 1
  of '$lib::g::R::Ok' is not one`, with the module's hidden qualifier, and so did a
  variant's wrong field type, a variant written with arguments it has no fields for, the
  type arguments a variant or a generic function's call cannot infer (`'$lib::g::pick'`)
  and their hints; they say `lib::g::R::Ok` and `lib::g::pick` now.
- A subclass's default `__init__` names the superclass constructor's parameters as they
  are declared: `Named(1, 2)` for `class Named : Point { name: Str; }` with `class Point {
  x: int; y: int; }` said `function 'Named' expects 3 argument(s), got 2 (it has no
  '__init__', so it takes arg1: int, arg2: int, name: Str)`, and says `... so it takes x:
  int, y: int, name: Str` now, through a chain of default ones and from a written
  `__init__` too, as does the error of an `__init__` that does not call `__super__` (`its
  default '__init__' takes ...`).  The names are the method's own
  (`MethodDecl::ParamNames`), kept apart from its parameters' modes, which the calling
  convention reads and which named the parameters only when one was `view` or `inout`.  An
  imported superclass's are still `arg1`, `arg2`, ... unless one of them is `view` or
  `inout`, its module recording no other names (docs/language/05-classes.md, "The default
  `__init__`").
- `paykan build` could write its executable over the source file, and `--emit-pkm` and
  `pkm extract --object` their output over their input, each with exit status 0: `build
  noext` replaced `noext`, `build main.pkn.bak` replaced `main.pkn` (another source), and
  `build same.pkn -o ./same.pkn`, `--emit-pkm lib.pkn -o lib.pkn` and `pkm extract
  --object lib.pkm -o lib.pkm` replaced the file they read, as `build main.pkn -o lib.pkn`
  and `--emit-c main.pkn -o lib.pkn` replaced the source of a module `main.pkn` imports
  and `--emit-pkm main.pkn -o geometry/shapes.pkm` a prebuilt one.  The driver now writes
  over none of the command's input: the source file, the sources and module files of the
  modules it imports (the standard library's too), the module file of `pkm extract` and
  the `--object` files, whatever path names it (`./a.pkn`, `d//a.pkn`, `sub/../a.pkn`, a
  symbolic or a hard link, or another case on a case-insensitive file system): `paykan:
  the output './same.pkn' is the input file 'same.pkn' (choose another with -o)`, exit
  status 1.  `build`'s default output drops only a `.pkn` or `.pk` extension and adds
  `.out` to a name with neither (`noext.out`, `main.pkn.bak.out`) and to one that would
  still end in one (`main.pkn.pkn.out`, where `build main.pkn.pkn` replaced `main.pkn`);
  the driver names it, so the backends have no default of their own
  (docs/language/17-the-paykan-command.md, "Commands").
- `paykan pkm dump`, `pkm check` and `pkm extract --object` aborted on a directory
  (`terminate called after throwing an instance of 'std::__ios_failure'`, exit status 134).
  A directory is now an error, `paykan: 'd.pkm' is a directory, not a file`, exit status 1,
  and a module file is read with `istream::read`, which reports a failed read rather than
  throwing: a regular file whole, and a pipe or a device to its end (`cat lib.pkm | paykan
  pkm dump /dev/stdin`).
- `--emit-pkm ./geometry/shapes.pkn` (and `geometry/./shapes.pkn`) named the module
  `shapes`, not `geometry::shapes` as `geometry/shapes.pkn` does, so an importer of
  `geometry::shapes` rejected the file (`names module 'shapes'. Rebuild it from source.`);
  and a path with a part that is no identifier gave a module no import can name
  (`--emit-pkm 1bad/x.pkn` wrote `module 1bad::x`).  The path is now made normal first, and
  each part of the name it gives must be an identifier: `paykan: cannot name the module of
  '1bad/x.pkn' after its path: '1bad' is not an identifier (name it with --module-name)`,
  exit status 1.  An absolute path, or one that goes up (`../shapes.pkn`), still names the
  module by its file name (docs/language/17-the-paykan-command.md, `--emit-pkm`).
- `--emit-pkm -o <path>` made every missing directory of the path, even at the file
  system's root (`-o /nonexistent/x.pkm` made `/nonexistent`), where `build -o` and `pkm
  extract -o` fail.  Only the module cache makes its directories now: `paykan: cannot write
  nd/sub/x.pkm: the directory 'nd/sub' does not exist`, exit status 1.  The standard
  library's build makes its staging directory before it writes the modules there.
- `--emit-pkm -o <path>` replaced what the path named with a regular file when it was not
  one: `-o /dev/null` made `/dev/null` a regular file (for the whole system, when run as
  root), and a FIFO or a symbolic link was replaced, the link's file left as it was, where
  `build -o` and `pkm extract -o` write into it.  A module file is now written through a
  temporary file and a rename only over a regular file or a path that names nothing, so
  that no reader sees half of it, and in place otherwise: a link's file gets the module
  file, and a device or a FIFO stays one.  A module file that cannot be written says why
  (`paykan: cannot write lib.pkm: No space left on device`), where the message named the
  temporary file and gave no reason.
- A command whose standard output could not be written exited with status 0, its output
  lost: `paykan --emit-c hello.pkn > /dev/full`, and the same for `--emit-pir`,
  `--emit-llvm`, `--emit-mlir`, `--emit-ast`, `--dump-ast`, `--dump-tokens`, `--help`,
  `--version`, `--list-*`, `pkm dump` and `pkm check`.  Each now checks standard output once
  it is done: `paykan: cannot write to standard output: No space left on device`, the reason
  the first write that failed gave, exit status 1.  A file `-o` names that cannot be
  written says why too (`paykan: cannot write 'out.c': No space left on device`).
- `PAYKAN_TRACK_HEAP=1 paykan run hello.pkn` printed no heap statistics: only an executable
  `build` wrote read the variable, though the documentation says every program does.  `run`
  now tracks the heap when it is set, as under `--track-heap`.
- `paykan pkm --version`, `pkm -v`, `pkm --list-backends` and `pkm --list-frontends`
  failed with `paykan: cannot open ''`: the `pkm` command ran before the information
  options, which it took for a module file's.  They now answer after `pkm` as they do
  without it, as `pkm --help` already did.
- Malformed command lines got misleading errors: `--emit-mlir hello.pkn` took the file for
  the stage and said `no source file specified`, `--Output hello.pkn` said `invalid
  optimization level 'utput'`, and `build hello.pkn -o ""` said `option '--o' requires a
  value`.  `--emit-mlir <stage>` now takes only a stage's name (``option '--emit-mlir'
  needs a stage: --emit-mlir=<stage> (pir, scf, llvm or emitc)``), only `-O`, `-O<n>` and
  `-O=<n>` are an optimization level (`unknown command line argument '--Output'`), a
  message names an option as it was spelled (`option '-o' requires a value`), and the
  stage of `--emit-mlir=<stage>` is checked as the command line is read (`unknown MLIR
  stage 'bogus' (expected pir, scf, llvm or emitc)`).
- `paykan --object=libtext.a main.pkn` failed with `JIT error: The file was not recognized
  as a valid object file`, though `--object` takes archives and `build` linked the same
  one: the JIT took object files only.  `run` now loads an archive's members as a linker
  does, each when it defines a symbol the program still needs, searching the archives
  before the C library; a file that is neither is named (`JIT error: cannot load 'x.o':
  The file was not recognized as a valid object file`), and one that cannot be read says
  why (`cannot read 'x.a': No such file or directory`).
- A program that could not be started was taken for one that ran and exited with status
  127: a backend plugin's `host->run_executable` of a missing file returned `PAYKAN_OK`
  with exit code 127 (after `exec of '...' failed` on standard error), and `paykan build`
  with a C compiler that is not there said `linking failed (nosuchcc exited with 127)`.
  The child now reports a failed exec through a close-on-exec pipe: `run_executable`
  returns `PAYKAN_ERROR` with `cannot run '<path>': No such file or directory`, and
  `build` says `cannot run 'nosuchcc': No such file or directory`, also when `paykan`'s
  parent left `SIGCHLD` ignored (no wait for a child then succeeds, and `waitpid failed: No
  child processes` was all it said).  The child's environment is made before the fork, so
  between the fork and the exec the child calls only what is safe there, where it called
  `setenv`.  `run_executable` with no arguments was documented to run the program with
  `argc == 0`, which Linux turns into one empty `argv[0]` and other systems refuse; it now
  passes one empty `argv[0]` on every system, as docs/plugins/plugin-api.md now says.
- `CC='ccache cc' paykan build` and `CC='gcc -m64' paykan build` failed (`exec of 'gcc
  -m64' failed: No such file or directory`): the whole of `$CC` was taken for the
  program's name.  As with `make`, its first word is now the program and the others its
  first arguments, the value split at white space with no quoting; the linker's flags
  follow them, before the objects (docs/language/17-the-paykan-command.md, "Environment
  variables").
- An executable lost all its arguments, `args[0]` included, when `PAYKAN_NO_ARGS` was set
  in its environment, to any value: `PAYKAN_NO_ARGS=0 ./prog x y` gave `main` an empty
  `args`, and `args[0]` panicked.  The variable, meant only for the plugin host's
  `run_executable` with no arguments, was public, and every program the executable
  started inherited it.  The host now sets a private one, and the runtime takes away only
  the one empty `argv[0]` the host starts such a program with, and unsets the variable
  once it has read it (on the systems that have `unsetenv`).
- The documented build of `--emit-c`'s C failed for a program that imports a prebuilt
  module (`undefined reference to pk_geometry_3A_3Ashapes_Sarea`): it linked the objects of
  the standard library's module files and the cache entries only, and a prebuilt module has
  neither.  It now links those of the files `paykan --list-modules` names
  (docs/c-backend.md), and the `CStrictC11` test builds every sample that way, and the 13_pkm
  sample with its module prebuilt in a `--module-path` directory.
- A module file's frontend record named the frontend only: its version and its plugin were
  always empty (`frontend "recursive-descent" "" plugin ""`).  It now has the built-in
  frontend's version, the `paykan` version that wrote the file (`frontend
  "recursive-descent" "0.3.0-dev" plugin ""` from a development build), or a plugin
  frontend's plugin, by its descriptor's name, and that plugin's version (docs/design/pkm.md
  §2.2).  The `ast-text` example frontend names the AST format versions it reads and is
  given (for example `ast-text reads AST format 0.3.0 only, not 0.2.0`), where it said
  `format 1`.
- A native function's C symbol was not checked: `native fn f() -> int = "a b";` was
  accepted, and gave C that did not compile (`extern int64_t a b(void);`) and a link that
  failed (`JIT session error: Symbols not found: [ a b ]`); `""` failed PIR verification.
  The parser and the AST reader now take only a C identifier that is not a C keyword, and
  Sema checks it again for an AST made another way: `the C symbol 'a b' of native function
  'f' is not a C identifier`, `the C symbol 'int' of native function 'f' is a C keyword`
  (docs/language/03-functions.md, "Native Functions").
- The Debian package's description still said it held "the C backend, which compiles
  programs with the system C compiler", a backend that is gone.  It now names the mlir
  backend, which runs programs with LLVM's JIT, builds executables (linking them with the
  system C compiler) and prints C11 with `--emit-c`.  Comments in the build files and the
  scripts that named the c and llvm backends or LLVM 17 name the mlir backend and LLVM 22.
- `paykan --help` showed `build <source-file> -o <output>`, though `-o` is optional, and
  listed `pkm dump`'s sections without `tmpl` and `all`, which the command takes.  It now
  shows `build <source-file> [-o <output>]` and
  `--section=manifest|sections|iface|tmpl|code|symidx|obj|payloads|all`, the list the error
  for an unknown section prints, both made from one table.  The lines of the `--emit-*`
  options say they print, not that they write to stdout: `-o` names where.
- The plugin documentation said the plugin API version changes only when `plugin_api.h`
  changes incompatibly, and that a plugin built against an older header keeps loading into a
  newer `paykan`.  The version is the release's, and a `paykan` loads only plugins of its
  own: a loadable plugin is rebuilt for each release, and this one rejects a plugin built
  for 0.2.0 (`built for plugin API 0.2.0; this paykan supports plugin API 0.3.0`) whatever
  the plugin compatibility list holds.  docs/writing-a-backend.md §7,
  docs/plugins/overview.md, docs/plugins/pir-for-backends.md and docs/design/pkm.md now say
  so, and docs/pir.md gives the binary PIR codec's version as the release's, not 1.0.
- A type's conformance to `Hashable` crosses modules and `.pkm` files
  (docs/language/06-traits.md, "Hashable").
  - `impl Hashable for Key` in module `keys` makes `keys::Key` a key of `HashMap` and
    `HashSet`, and an argument of any `T: Hashable` template, in every importer: from its
    source, the module cache (or `--no-module-cache`) or a prebuilt `.pkm`, and through a
    module that only returns one.
  - The interface records the conformance (an `IMPL` record, docs/design/pkm.md §3.5;
    `pkm dump` shows `impl Hashable for Key`).
  - Generic types: `impl Trait for` blocks now go with the template.  An importer's
    `lib::Box<Str>` has their methods and, for `Hashable`, the trait, checked when it is
    instantiated as the module would check it.  An instantiation the module made carries
    the conformance in the interface.
  - The 0.2.0 entries saying a struct is a `HashMap` key across modules, that generic
    structs' `impl Trait for` blocks work across modules, and that an `impl Named for
    Box<T>` block works in an importer now hold.  A module's own trait does not cross yet
    (#147).
- A template of another module bounded by that module's own trait, directly or through a
  template its body uses (an unbounded `fn wrap<T>` calling `fn greet<T: Named>`, a
  generic class's method), is reported where the importer instantiates it:
  `'lib::wrap<int>' cannot be instantiated here: it uses 'lib::greet', which requires 'T'
  to implement trait 'Named' of module 'lib', and another module's traits are not
  supported yet (#147)`, with a note at the use (docs/language/06-traits.md, "Not
  Supported Yet").  Before, it was `unknown trait 'Named'`, for a use inside the module in
  the very file that declares `Named`.  A use two templates deep or more (a generic
  function of `lib` calling one that calls `greet`, or a template of a third module
  calling `lib::wrap`) is still reported at the use in lib's file, with the note of the
  nearest request only, as an instantiation waiting to be checked keeps only that one.
- A bound error that waited for the `impl` blocks to be known (a class field's type that
  instantiates another module's template, whose field instantiates a bounded one) showed
  that template's line under the importer's file name, with no instantiation note:
  `main.pkn:846:3` for a line of collections.pkn.  It now names the template's file, with
  the instantiation notes.  In one module too, such an error in a template a field's
  type instantiates (`h: Holder<int>`, with `class Holder<T> { g: Gen<float>; }` and
  `class Gen<T: Hashable>`) now has the `in instantiation of 'Holder<int>' requested here`
  note, as a bound error found earlier has.
- A bounded generic enum used before the `impl` blocks were checked (a class field `m:
  Maybe<int>` of `enum Maybe<T: Hashable>`) crashed the compiler.  It is now checked like
  a generic class.
- `u8`, `u16` and `u32` parses accepted a sign where `unsigned` did not (`u32("+1")` was
  `1`, `u8("-0")` was `0`). Every unsigned width now rejects one: `"+1".toU8()` is `None`.
- `f16` and `f32` parses gave `inf` for finite text out of their range (`f16("70000")`,
  `f32("1e39")`). The parse is now `None`, as the literal of that text is an error. Text
  below the type's range still rounds to 0, as the literal does.
- The `f32` literal `3.4028235677973366e38`, the midpoint above the largest `f32`, compiled
  and was `inf`. It is now `the literal 3.40282e+38 does not fit in 'f32'`.
- `f32(n)` of an `int` or `unsigned` above 2^53, such an integer literal in an `f32` slot,
  and the `f32` parse of such an integer text (`f32("1152921573326323713")`), were rounded
  twice, through `float` (`f32(1152921573326323713)` was 2^60). They are now rounded once
  (2^60+2^37), as docs/design/sized-types.md says; `s.toF32()` rounds an integer text of
  any size once (`"9223372586610589697".toF32()` is 2^63+2^40, where it was 2^63).

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
  local declarations only; it fixes the variable, not the object a class-typed one refers
  to (an array's elements and a struct's fields are part of its value).  `let` is
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
  variable can no longer be changed through it (a copy of it, the same container, still
  can), and printing one shows the struct's fields.
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

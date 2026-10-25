# The AST interchange format

A frontend plugin returns the program it parsed as text in this format
([`plugin-api.md`](plugin-api.md#frontends)). `paykan` reads it, checks it,
and hands the AST to Sema exactly as if the built-in recursive-descent
frontend had built it. The format is a small S-expression language: easy to
produce from any language, easy to read in a diff.

The format's version is the PaykanLang release's, like every format's
(`PAYKAN_AST_FORMAT_VERSION` in `plugin_api.h`, the release version packed as
major * 10000 + minor * 100 + patch); the text spells it dotted, `0.2.0`. The core's
writer and reader are [`include/paykan/ast/Interchange.h`](../../include/paykan/ast/Interchange.h);
`paykan --emit-ast program.pkn` prints any program in this format, which is
the quickest way to see what a frontend must produce.

```text
$ cat hello.pkn
import ::io;
fn main() -> int {
  io::println("hi");
  return 0;
}
$ paykan --emit-ast hello.pkn
(paykan-ast 0.2.0
  (unit @1:1-5:2
    (import @1:1-1:13 true ""
      (module "io" ""))
    (fn @2:1-5:2 "main"
      (type-params)
      (params)
      (named-type "int")
      (block @2:19-2:19
        (expr @3:3-3:21
          (call @3:3-3:20 "io::println"
            (type-args)
            (string @3:15-3:19 "hi")))
        (return @4:3-4:12
          (int @4:10-4:11 0))))))
```

## 1. Syntax

- A **list** is `(` tag field... `)`. The tag is a symbol naming the node.
- A **symbol** is a run of characters other than whitespace, `(`, `)`, `"`
  and `;` that is not a number or a location: `true`, `false`, `_`, `neg`,
  `inf`.
- A **number** starts with a digit, or `-` and a digit: `42`, `-7`, `1.5`,
  `2e3`. Integers are 64-bit signed decimal.
- A **string** is `"..."`: UTF-8, with the escapes `\"`, `\\`, `\n`, `\t`,
  `\r` and `\xHH` (one byte, two hexadecimal digits). A raw newline is not
  allowed inside a string. The writer escapes every byte outside printable
  ASCII as `\xHH`, so its output is pure ASCII; the reader also accepts raw
  UTF-8.
- A **location** is `@line:column-line:column` (1-based; the start and the
  end of the node in the source). It is optional and, when present, is the
  first field of a list. A missing location means "unknown"; diagnostics
  about such a node have no position, so a frontend should give every node
  one.
- `_` stands for an absent field ("none").
- Whitespace (space, tab, newline, carriage return) separates atoms; `;`
  starts a comment that runs to the end of the line.
- Nesting is limited to 4112 lists (`kMaxDepth`), which holds every AST
  within the frontends' limits; deeper input is rejected.

The whole text is one list:

```text
(paykan-ast VERSION UNIT)
```

`VERSION` is the dotted release version, and the reader accepts exactly its
own (`0.2.0` for paykan 0.2.0). Nothing but whitespace and comments may follow.

## 2. Nodes

In the tables, `LOC` is the optional location, `NAME` a non-empty string,
`STR` any string, `INT` an integer, `TYPE`, `EXPR` and `STMT` one node of
that class, `BLOCK` a `(block ...)`, `X?` means "X or `_`", and `X...` zero
or more.

### Declarations

| Node | Meaning |
|---|---|
| `(unit LOC DECL...)` | the file. Each `DECL` is an `import`, `enum`, `struct`, `class`, `fn`, `trait` or `impl`, in any order |
| `(import LOC SYSTEM BASE MODULE...)` | `import`. `SYSTEM` is `true` for `import ::name`, else `false`; `BASE` is the path before the last segment (`"a::b"` for `import a::b::m`, `""` for none); at least one `MODULE` |
| `(module NAME ALIAS)` | one imported module; `ALIAS` is a string, `""` for none (`import m as x` has `(module "m" "x")`; `import a::{m, n}` has two) |
| `(enum LOC NAME (type-params NAME...)? VARIANT...)` | `enum E { A, B }`: the name, then the variants. A generic enum's type parameters follow its name, at least one (`enum M<T>` is `(enum "M" (type-params "T") ...)`); an enum that is not generic has no `type-params`. A variant without fields is its `NAME`; one with fields is `(variant NAME VAR...)`, each field a `var` with a type and no initialiser (`B(x: int)` is `(variant "B" (var "x" (named-type "int") _))`) |
| `(class LOC NAME SUPER (type-params NAME...) (fields VAR...) (methods FN...))` | a class. `SUPER` is the superclass's name or `""`; a generic class has type parameters. Each field is a `var` with a type and no initialiser. A method may have `_` in place of its body: a pure virtual method, `fn area() -> float;` ([`../language/05-classes.md`](../language/05-classes.md#pure-virtual-methods)) |
| `(struct LOC NAME (type-params NAME...) (fields VAR...))` | a struct ([`../language/11-structs.md`](../language/11-structs.md)): the name, the type parameters of a generic one, and its fields (at least one), each a `var` with a type and no initialiser. A struct body has no methods: they are in its `impl` blocks |
| `(bound NAME TYPE...)` | in `type-params`, in place of `NAME`: a type parameter with bounds, the traits as types (`T: Show + Into<int>` is `(bound "T" (named-type "Show") (generic-type "Into" ...))`) |
| `(fn LOC NAME (type-params NAME...) (params PARAM...) TYPE? QUAL? BLOCK QUAL? INFERRED?)` | a function or method; the `TYPE` is the return type, `_` for none. A borrowed result has `(qual view)` or `(qual inout)` after its `TYPE` (`-> view Str` is `(named-type "Str") (qual view)`), a `view fn` ends with `(qual view)` and a `static fn`, an `impl` block's only, with `(qual static)`. `INFERRED` is `(inferred)` for `fn double(x: int) => x * 2;`, whose result type Sema infers: `_` for its `TYPE` and a body of one `return` of a value, as a lambda's |
| `(impl LOC TYPE (type-params NAME...)? (for TRAIT)? (methods FN...))` | an `impl` block: `TYPE` is the type's name as written (`"A"`, `"geo::Point"`). The block of a generic type has its type parameters (`impl Box<T>` is `(impl "Box" (type-params "T") ...)`). `impl Show for A` has `(for TRAIT)`, the trait as a type |
| `(trait LOC NAME (type-params NAME...) (methods FN...))` | a trait. Its methods are signatures: each `fn` has `_` in place of its body, and none is generic |
| `(fn LOC NAME (type-params NAME...) (params PARAM...) TYPE? QUAL? (native STRING))` | a `native fn` (#198), a top-level function only: the C symbol in place of the body; a generic one has type parameters |
| `(param NAME TYPE QUAL?)` | one parameter; `QUAL` is its mode, `(qual view)` or `(qual inout)`, absent for an ordinary (by-value) parameter: `n: inout int` is `(param "n" (named-type "int") (qual inout))` |
| `(var LOC NAME TYPE? EXPR? MODE? LET? WEAK?)` | a variable or field declaration: `x: int = 1` is `(var "x" (named-type "int") (int 1))`. `MODE` is `(qual view)` or `(qual inout)` for a local borrow, which has an initialiser and is never a field or `let`: `v: view = k;` is `(var "v" _ (ident "k") (qual view))`. `LET` is `(let)` for a `let` local, which has an initialiser and is never a field: `let n = 1;` is `(var "n" _ (int 1) (let))`. `WEAK` is `(weak)` for a `weak` local or class field ([`../language/16-memory.md`](../language/16-memory.md)), which has a type or an initialiser and is never a borrow or `let`: `weak w: Node? = n;` is `(var "w" (optional-type (named-type "Node")) (ident "n") (weak))` |

### Types

| Node | Meaning |
|---|---|
| `(named-type LOC NAME)` | a type by name: `int`, `float`, `bool`, `char`, `void`, `Str`, `Obj`, a class, struct or enum, or a qualified `module::Type` |
| `(array-type LOC TYPE)` | `T[]`, the slice of a `view` or `inout` parameter (`xs: view int[]`) |
| `(fixed-array-type LOC TYPE INT)` | `T[n]`, a fixed-size array of `INT` (at least 1) elements: `int[3]` is `(fixed-array-type (named-type "int") 3)`. `INT` can be the symbol `_` instead, a length inferred from the initializer (`int[_]`) |
| `(optional-type LOC TYPE)` | `T?` (not of `void`, not of another optional) |
| `(tuple-type LOC TYPE TYPE TYPE...)` | `(T1, T2, ...)`, at least two |
| `(fn-type LOC (params (param TYPE QUAL?)...) TYPE)` | a function type ([`../language/14-function-types.md`](../language/14-function-types.md)): `(int, view Str) -> bool` is `(fn-type (params (param (named-type "int")) (param (named-type "Str") (qual view))) (named-type "bool"))`; the result type is never `_` (`void` is `(named-type "void")`) |
| `(generic-type LOC NAME TYPE TYPE...)` | `Box<int>`, `Pair<Str, int>`, `m::Box<int>` |

A `named-type` is resolved the way the recursive-descent frontend resolves a
name: a builtin type or a class the compiler predefines (`Str`, `Obj`, ...)
is that type, and any other name (and every qualified one) is left for Sema
to resolve, which reports unknown names.

### Statements

| Node | Meaning |
|---|---|
| `(block LOC STMT...)` | `{ ... }` |
| `(decl LOC VAR)` | a declaration statement, `x: T = e;` |
| `(assign LOC (ident LOC NAME) EXPR)` | `x = e;` (declares `x` on first assignment, as in the language) |
| `(member-assign LOC EXPR NAME EXPR)` | `recv.field = e;` |
| `(subscript-assign LOC EXPR EXPR EXPR)` | `arr[i] = e;` |
| `(destructure LOC (targets TARGET TARGET...) EXPR)` | `a, b: int, _ = e;`, at least two targets |
| `(target LOC STR TYPE?)` | one destructuring target: the name (`""` for `_`) and its annotation |
| `(expr LOC EXPR)` | an expression statement |
| `(return LOC EXPR?)` | `return;` / `return e;` |
| `(if LOC EXPR BLOCK ELSE)` | `ELSE` is a `block`, another `if` (`elif`), or `_` |
| `(while LOC EXPR BLOCK)` | |
| `(for LOC VAR EXPR EXPR EXPR BLOCK (inclusive)?)` | `for i in a..b step s { ... }`: the loop's variable as a `var` with no initialiser (`(var "i" TYPE _)`, `TYPE` its annotation or `_`; the name is `""` for `for _ in ...`), the range's start and end, the step or `_` (none written), the body, and a trailing `(inclusive)` for `..=` |
| `(break LOC)`, `(continue LOC)` | |
| `(match LOC EXPR ARM...)` | `match e { ... }` |
| `(type-arm LOC STR TYPE QUAL? BLOCK)` | `b: T { ... }`; the binding `STR` is `""` for `T { ... }`. `QUAL` is a binding's mode, `(qual view)` for `b: view T { ... }` or `(qual inout)`, and needs a binding |
| `(value-arm LOC STR EXPR BLOCK)` | `42 { ... }`, `"hi" { ... }`; binding as above |
| `(wildcard-arm LOC STR BLOCK)` | `_ { ... }` |

### Expressions

| Node | Meaning |
|---|---|
| `(int LOC INT)` | an integer literal |
| `(float LOC FLOAT)` | a float literal: a number, or `inf`, `-inf`, `nan`. The writer prints 17 significant digits, so the value reads back exactly |
| `(bool LOC true)`, `(bool LOC false)` | |
| `(char LOC INT)` | a character literal: its byte value, 0 to 255 (`'a'` is 97) |
| `(string LOC STR)` | a string literal: its value after the source's escapes are applied |
| `(none LOC)` | `None` |
| `(ident LOC NAME)` | a name |
| `(unary LOC OP EXPR)` | `OP` is `neg` (`-`) or `not` (`!`) |
| `(binary LOC OP EXPR EXPR)` | `OP` is one of `add sub mul div mod lt gt le ge eq ne and or` |
| `(ternary LOC EXPR EXPR EXPR)` | `c ? a : b` |
| `(call LOC NAME (type-args TYPE...) EXPR...)` | `f(a, b)`, `first<int>(xs)`, `Point(1, 2)`, `m::f(x)` (the qualified name as one string) |
| `(method-call LOC EXPR NAME EXPR...)` | `recv.m(a, b)` |
| `(member LOC EXPR NAME)` | `recv.field` |
| `(subscript LOC EXPR EXPR)` | `arr[i]` |
| `(array LOC EXPR...)` | `[a, b]` (`[]` parses, and Sema rejects it) |
| `(array-repeat LOC EXPR INT)` | `[v; n]`, `INT` (at least 1) copies of `EXPR`: `[0; 3]` is `(array-repeat (int 0) 3)` |
| `(tuple LOC EXPR EXPR EXPR...)` | `(a, b)`, at least two |
| `(tuple-index LOC EXPR INT)` | `t.0` |
| `(lambda LOC (params PARAM...) TYPE? BLOCK INFERRED?)` | a lambda, `fn(x: int) -> int { ... }`: the parameters and the result type (`_` for none) as a `fn`'s, then the body. `INFERRED` is `(inferred)` for `fn(x: int) => x * 2`, whose result type Sema infers: it has `_` for its result type and a body of one `return` of a value, `(block (return EXPR))` |
| `(call-value LOC EXPR EXPR...)` | a call of a function value that is not named, `xs[0](1)`, `t.0(x)`: the callee, then the arguments (`f(1)` with `f` a variable is a `call`, `obj.cb(1)` with `cb` a field a `method-call`) |
| `(enum-value LOC NAME NAME (type-args TYPE...)?)` | `Color::Red`: the enum's name (qualified if spelled so) and the variant; a generic enum's variant written with its type arguments has them after (`M<int>::B` is `(enum-value "M" "B" (type-args (named-type "int")))`) |

Parentheses in the source are not nodes: `(a + b) * c` is
`(binary mul (binary add (ident "a") (ident "b")) (ident "c"))`.

## 3. What the reader checks

`paykan` treats the text as untrusted input. The reader rejects, with the
line and column in the text and a message, anything that does not follow
the rules above: unknown tags, missing or extra fields, a field of the wrong
kind, an empty name where one is required, a tuple or destructuring with
fewer than two elements, an `array-repeat` count below 1, a byte value out
of range, a malformed location, an unknown escape, a version it does not
read, and nesting beyond the limits: more than 4112 lists, and more than
the frontends' 512 levels or 2048 links of a chain (`nesting too deep (more
than 512 levels)`, `nesting too deep (more than 2048 levels)`), counted in
the AST as [`../grammar.md`](../grammar.md) section 9 counts them in the
source. A block's statements, the members of a declaration's body, a
`match`'s arms, an `if` that is an `else` branch, the operand of a `unary`,
a `ternary`'s fields, the arguments of a call, the elements of an `array`
or a `tuple`, a `subscript`'s index, a `lambda`'s result type, a type's
arguments, elements, parameters and result, and the type an `array-type`,
`fixed-array-type` or `optional-type` wraps are each a level in; a
`binary`, `call-value`, `member`, `method-call`, `tuple-index` and
`subscript` (and the target of a `member-assign` or a `subscript-assign`)
is a link over the deepest of its fields. It
also makes the grammar's own checks that the recursive-descent frontend
makes while parsing (`void?`, nested optionals, `_` for the body of a
function that is neither a class's method nor a trait's, `(native ...)` on
a method, `(qual static)` outside an `impl` block). Everything else (unknown
names, types that don't match, ...) is Sema's job, with the same
diagnostics as for any frontend.

When the reader rejects a frontend's output, compiling fails with

```text
error: frontend 'mine' returned an invalid AST: 12:7: (frob ...) is not an expression
```

The positions are in the AST text, not the source: use `--emit-ast` and a
frontend that reads the format back (`src/Frontends/ASTText`) to look at
it.

## 4. Writing a producer

- Build your own tree and print it; the format is a direct rendering of the
  grammar ([`../grammar.md`](../grammar.md)), one node per construct.
- Every frontend must build exactly the same AST as the recursive-descent
  frontend for the same input. Compare your output with
  `paykan --emit-ast` (or `--dump-ast`) over many programs; the test
  support does it over the whole corpus
  ([`../writing-a-frontend-plugin.md`](../writing-a-frontend-plugin.md)).
- Locations: the recursive-descent frontend's rules are in
  [`../grammar.md`](../grammar.md); `--emit-ast` shows them for any program.
- The writer's layout (one list per line, two-space indentation) is not
  part of the format: any whitespace works.

## 5. Stability

The format is versioned by the release: every PaykanLang release has its own
version (`PAYKAN_AST_FORMAT_VERSION`), and a reader accepts only its own, so
a frontend plugin's AST is read by the release whose headers it was built
with (a plugin is accepted only by a release that lists its build version,
[`overview.md`](overview.md)). The core's tests write and read back the AST of every program in the
samples corpus and check that the result is identical
(`tests/AST/InterchangeTests.cpp`).

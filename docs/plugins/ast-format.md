# The AST interchange format

A frontend plugin returns the program it parsed as text in this format
([`plugin-api.md`](plugin-api.md#frontends)). `paykan` reads it, checks it,
and hands the AST to Sema exactly as if the built-in recursive-descent
frontend had built it. The format is a small S-expression language: easy to
produce from any language, easy to read in a diff.

**Version 1** (`PAYKAN_AST_FORMAT_VERSION` in `plugin_api.h`). The core's
writer and reader are [`include/paykan/ast/Interchange.h`](../../include/paykan/ast/Interchange.h);
`paykan --emit-ast program.pkn` prints any program in this format, which is
the quickest way to see what a frontend must produce.

```text
$ cat hello.pkn
fn main() -> int {
  println("hi");
  return 0;
}
$ paykan --emit-ast hello.pkn
(paykan-ast 1
  (unit @1:1-4:2
    (fn @1:1-4:2 "main"
      (type-params)
      (params)
      (named-type "int")
      (block @1:19-1:19
        (expr @2:3-2:17
          (call @2:3-2:16 "println"
            (type-args)
            (string @2:11-2:15 "hi")))
        (return @3:3-3:12
          (int @3:10-3:11 0))))))
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
- Nesting is limited to 2048 lists (`kMaxDepth`); deeper input is rejected.

The whole text is one list:

```text
(paykan-ast 1 UNIT)
```

The version must be one the reader supports (today: 1). Nothing but
whitespace and comments may follow.

## 2. Nodes

In the tables, `LOC` is the optional location, `NAME` a non-empty string,
`STR` any string, `INT` an integer, `TYPE`, `EXPR` and `STMT` one node of
that class, `BLOCK` a `(block ...)`, `X?` means "X or `_`", and `X...` zero
or more.

### Declarations

| Node | Meaning |
|---|---|
| `(unit LOC DECL...)` | the file. Each `DECL` is an `import`, `enum`, `class` or `fn`, in any order |
| `(import LOC SYSTEM BASE MODULE...)` | `import`. `SYSTEM` is `true` for `import ::name`, else `false`; `BASE` is the path before the last segment (`"a::b"` for `import a::b::m`, `""` for none); at least one `MODULE` |
| `(module NAME ALIAS)` | one imported module; `ALIAS` is a string, `""` for none (`import m as x` has `(module "m" "x")`; `import a::{m, n}` has two) |
| `(enum LOC NAME NAME...)` | `enum E { A, B }`: the name, then the variants |
| `(class LOC NAME SUPER (type-params NAME...) (fields VAR...) (methods FN...))` | a class. `SUPER` is the superclass's name or `""`; a generic class has type parameters. Each field is a `var` with a type and no initialiser |
| `(fn LOC NAME (type-params NAME...) (params PARAM...) TYPE? BLOCK)` | a function or method; the `TYPE` is the return type, `_` for none |
| `(param NAME TYPE QUAL?)` | one parameter; `QUAL` is its mode, `(qual view)` or `(qual inout)`, absent for an ordinary (by-value) parameter: `inout n: int` is `(param "n" (named-type "int") (qual inout))` |
| `(var LOC NAME TYPE? EXPR? LET?)` | a variable or field declaration: `x: int = 1` is `(var "x" (named-type "int") (int 1))`. `LET` is `(let)` for a `let` local, which has an initialiser and is never a field: `let n = 1;` is `(var "n" _ (int 1) (let))` |

### Types

| Node | Meaning |
|---|---|
| `(named-type LOC NAME)` | a type by name: `int`, `float`, `bool`, `char`, `void`, `Str`, `Obj`, a class or enum, or a qualified `module::Type` |
| `(array-type LOC TYPE)` | `T[]` |
| `(optional-type LOC TYPE)` | `T?` (not of `void`, not of another optional) |
| `(tuple-type LOC TYPE TYPE TYPE...)` | `(T1, T2, ...)`, at least two |
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
| `(if LOC EXPR BLOCK ELSE)` | `ELSE` is a `block`, another `if` (`else if`), or `_` |
| `(while LOC EXPR BLOCK)` | |
| `(break LOC)`, `(continue LOC)` | |
| `(match LOC EXPR ARM...)` | `match e { ... }` |
| `(type-arm LOC STR TYPE BLOCK)` | `b: T { ... }`; the binding `STR` is `""` for `T { ... }` |
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
| `(array LOC EXPR...)` | `[a, b]`, `[]` |
| `(tuple LOC EXPR EXPR EXPR...)` | `(a, b)`, at least two |
| `(tuple-index LOC EXPR INT)` | `t.0` |
| `(enum-value LOC NAME NAME)` | `Color::Red`: the enum's name (qualified if spelled so) and the variant |

Parentheses in the source are not nodes: `(a + b) * c` is
`(binary mul (binary add (ident "a") (ident "b")) (ident "c"))`.

## 3. What the reader checks

`paykan` treats the text as untrusted input. The reader rejects, with the
line and column in the text and a message, anything that does not follow
the rules above: unknown tags, missing or extra fields, a field of the wrong
kind, an empty name where one is required, a tuple or destructuring with
fewer than two elements, a byte value out of range, a malformed location, an
unknown escape, a version it does not read, and nesting beyond the limit. It
also makes the grammar's own checks that the recursive-descent frontend
makes while parsing (`void?`, nested optionals). Everything else (unknown
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

`PAYKAN_AST_FORMAT_VERSION` is bumped on any incompatible change: a node or
field removed, renamed or changed in meaning, or a node a reader must handle
added (a new language construct). Within one version the format does not
change, except for optional trailing items that only new constructs use: a
parameter's `(qual ...)` and a local's `(let)`. They keep the version, because every document that
was valid stays valid and means the same, and a reader that predates an item
rejects a document that uses it (an unexpected field) instead of misreading
it. The core's tests write and read back the AST of every program in the
samples corpus and check that the result is identical
(`tests/AST/InterchangeTests.cpp`).

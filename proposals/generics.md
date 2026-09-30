# Proposal: Generics for PaykanLang (prototype, issue #2)

Status: **prototype** — a coherent, tested, end-to-end slice on branch
`proto/generics`. Everything below marked *implemented* is exercised by
`tests/{Parser,Sema,CodeGen}/GenericsTests.cpp`, `samples/sema/generics/` and
`samples/codegen/29_generics.pkn`. Everything marked *out of scope* is not
implemented and is listed with the reason and the intended direction.

---

## 1. Goals

- Parametric classes and free functions with a syntax that reads like the rest
  of the language: `class Box<T> { … }`, `fn first<T>(xs: T[]) -> T`.
- Type arguments usable in every type position (`Box<int>[]`, `Box<Box<int>>`,
  `Pair<Str, int>`), on constructor calls (`Box<int>(3)`) and on calls
  (`first<int>(xs)`), with inference of a function's type arguments from its
  argument types.
- Zero new runtime or CodeGen machinery: an instantiation must be an ordinary
  class or function once Sema is done, so ARC, `match`, `==`/`equals`, arrays,
  fields, `mov` and module export all keep working unchanged.
- Diagnostics that name the instantiation being checked.

## 2. Syntax (implemented)

```
classDecl      ::= 'class' IDENT typeParams? (':' modulePath)? '{' … '}'
funcDecl       ::= 'fn' IDENT typeParams? '(' params ')' ('->' type)? block
typeParams     ::= '<' IDENT (',' IDENT)* '>'

type           ::= … | IDENT '<' typeArgs '>' | modulePath '::' IDENT '<' typeArgs '>'
typeArgs       ::= type (',' type)*

primaryExpr    ::= … | IDENT '<' typeArgs '>' '(' args ')'
                     | modulePath '::' IDENT '<' typeArgs '>' '(' args ')'
```

The grammar is LALR(1) and `IDENT <` is ambiguous in expression position
(`i < n` vs. `first<int>(xs)`). The decision is made by a one-token-deep
wrapper around the Flex scanner (`yylex` in `src/Parser/ParserDriver.cpp`, the
raw scanner is `yylex_raw`): on a `<` that directly follows an identifier it
scans ahead over the tokens a type-argument list may contain (`IDENT`, `::`,
`,`, `[`, `]`, nested `<`/`>`) to the matching `>`; when that `>` is directly
followed by `(` the `<` is delivered as a distinct `TYPELESS` token, otherwise
as the relational `LESS`. The scanned tokens are queued and replayed. This is
the C#/TypeScript approach rather than C++'s (which needs declarations to be
visible while parsing). Consequences:

- `a < b` in every ordinary shape still parses as a comparison (tests:
  `Parser/GenericsTests.cpp: LessThanStaysRelational`).
- `a < f<int>(x)` parses correctly: the outer `<` is relational, the queued
  inner `<` is re-examined when it is handed out.
- Known ambiguity, same as C++: `f(a < b, c > (d))` is read as a generic call
  `a<b, c>(d)`; parenthesise either comparison. The grammar rejects
  `a < b > (c)` anyway (relational operators do not chain).
- `>>` is not a token in Paykan, so `Box<Box<int>>` needs no special casing.
  `>=` is a token, so `x: Box<int>=Box<int>(1)` needs a space before `=`.
- In *type* position `<` is unambiguous; `typeArgOpen` accepts both tokens so a
  declaration such as `fn first<T>(`, which matches the lookahead pattern, still
  parses.

Parser AST additions (`include/AST.h`): `ClassDecl::TypeParams`,
`FuncDecl::TypeParams`, `CallExpr::TypeArgs` (plus `setCalleeName`), a new
`GenericType` node (`NK_GenericType`: template name + argument types) for
type applications, and separate `TranslationUnit::GenericClassDecls` /
`GenericFuncDecls` lists so no downstream pass sees a template unless it asks.

## 3. Semantics: monomorphisation in Sema (implemented)

A generic declaration is a **template**: Sema registers it by name
(`Sema::ClassTemplates`, `FuncTemplates`) and never type-checks it as such.

**Instantiation.** Every use with a distinct tuple of *canonical* type
arguments instantiates the template once (`Sema::instantiateClass` /
`instantiateFunction`, `src/Sema/SemaClass.cpp`):

1. The canonical name is `Name<arg, …>` spelled with `ast::typeName` —
   `Box<int>`, `Pair<Str, int>`, `Box<int[]>`, `Box<Box<int>>`. Arguments are
   canonical types (builtin singletons, registered classes/enums, interned
   `ArrayType`s, earlier instantiations), so equal spellings are equal tuples
   and the name doubles as the cache key (`ClassInstantiations`,
   `FuncInstantiations`) — the same idea as `ASTContext::getArrayType` and the
   `Array<T>` specialisation cache. A second `Box<int>` anywhere in the module
   yields the same `ClassType*` (test `SameInstantiationIsSameClassType`).
2. The declaration is deep-copied with the type parameters substituted
   (`ast::ASTCloner`, `include/ASTClone.h`, implemented in `src/AST/AST.cpp`):
   a `ClassType` stub naming a parameter becomes the argument type; `T[]` and
   `Box<T>` are rebuilt around the substituted types; everything else is
   shared. Every statement/expression node is fresh, so Sema's per-node
   annotations never leak between instantiations. Source locations are kept.
3. The copy is registered exactly like a hand-written declaration: for a class,
   the pre-register / populate fields and methods / declare constructor phases
   of `checkClassDecls` (factored into `populateClassType` and
   `declareConstructor`); for a function, `declareFunctionSignature`. The cache
   entry is created *before* the fields are populated so a self-referential
   signature (`fn link(n: Node<T>)`) resolves to the type being built.
4. Its bodies are queued (`PendingInstantiations`) and checked after all
   hand-written bodies, with a worklist so bodies that instantiate further
   templates are handled transitively — the C++ "instantiate at the end of the
   translation unit" model.
5. The instantiated `ClassDecl`s / `FuncDecl`s are appended to the
   `TranslationUnit` (`injectInstantiations`): classes ahead of the hand-written
   ones and in post-order over the "class A's methods construct class B"
   edges Sema recorded, functions at the end. CodeGen therefore emits them as
   ordinary declarations — no CodeGen changes were needed. (Ordering matters
   because CodeGen creates a constructor while emitting its class and resolves
   calls by name; see "Known limitations".)

**Where instantiation is triggered.** `Sema::resolveType` on a `GenericType`
(variable annotations, fields, parameters, return types, match arms, array
elements, nested type arguments) and `Sema::resolveGenericCall` for calls
(`Box<int>(3)`, `first<int>(xs)`, `first(xs)`, `Box(3)`). After a call is
resolved the `CallExpr`'s callee is rewritten to the instantiation's name, so
CodeGen sees a plain call to `first<int>` / a plain constructor call to
`Box<int>`.

**Canonical write-back.** Sema writes each resolved annotation back into the
AST (`VarDecl::setType`, `FuncDecl::setReturnType`, `getMutableParams()`; the
existing `MatchArm::setArmType` was the precedent). CodeGen consequently never
sees a `GenericType` or a parser stub where a canonical type is expected.

**Type-argument inference** (`Sema::inferTypeArgs` / `unifyTypes`): the
template's declared parameter types are unified structurally against the
canonical argument types — a parameter `T` binds; `T[]` binds through the
array element; `Box<T>` finds an instantiation of the same template in the
argument's class or one of its ancestors and unifies argument-wise; concrete
types bind nothing (the ordinary argument check reports mismatches afterwards).
A parameter deduced twice must agree exactly (`pick(1, 2.0)` against
`pick<T>(a: T, b: T)` is a conflict, not `float`); an empty array literal
carries no information. Failure and conflict produce
`cannot infer type parameter 'T' of 'pick' …` plus a note suggesting explicit
arguments. The same unification gives constructor-argument inference for free:
`Box(3)` unifies `__init__`'s parameters.

**Names as LLVM symbols.** `Box<int>` becomes `Box<int>_vtable`,
`Box<int>_struct`, `Box<int>___init__`, `Box<int>` (constructor) and
`first<int>`; LLVM identifiers may contain any character (they are quoted in
textual IR, `@"Box<int>_vtable"`) and ORC resolves them as opaque strings —
the `Array<Str>` specialisations already rely on this.

### Checking per instantiation vs. checking the template once

The prototype checks each instantiation's body (C++ model). The body of a
template that is never instantiated is never checked
(`UninstantiatedTemplateBodyIsNotChecked`), and an operation that is valid for
one argument and not another is reported only for the offending
instantiation, at the template's source, with a note per active instantiation
frame: `error: operator '+' is not defined for types 'bool' and 'bool'` /
`note: in instantiation of 'Adder<bool>' requested here` (`Sema::error`
appends the notes from `InstantiationStack`).

The alternative is to check the template **once**, treating `T` as an opaque
type about which only its declared bounds are known (Java/Rust/Swift model).
That needs constraints (`class Box<T: Comparable>`, `fn max<T: Ord>`) and
therefore interfaces or traits, which Paykan does not have yet; without bounds
an opaque `T` would only support assignment and passing, and `self.v + self.v`
in `Adder<T>` would have to be rejected up front. Checking once gives earlier
and instantiation-independent errors and is the better end state once
interfaces exist; per-instantiation checking is what makes the prototype
useful today (`Adder<int>` works, `Adder<bool>` fails with a clear message)
and is compatible with adding bounds later (a bounded template can be checked
once *and* its instantiations re-checked cheaply). Decision: per-instantiation
now, with the diagnostics shaped so that the switch is invisible to users.

## 4. Diagnostics (implemented)

| Situation | Message |
|---|---|
| wrong arity in a type or call | `generic class 'Box' expects 1 type argument(s), got 2` |
| unknown template | `unknown generic class 'Nope'` / `unknown generic function 'f'` |
| type args on a non-generic | `'f' is not generic and takes no type arguments`; `'Str' is not a generic class …` |
| generic class without args | `variable 'b' names generic class 'Box' without type arguments (write 'Box<...>')` |
| type parameter as value / constructed | `type parameter 'T' cannot be used as a value` |
| duplicate parameter | `duplicate type parameter 'T' in generic class 'P'` |
| parameter shadows a type | `type parameter 'Foo' of generic class 'Bar' shadows a type of the same name` |
| template vs. class/function name | `'Box' is already declared as a generic class`, `… generic function` |
| error inside an instantiation | the error at the template source + `in instantiation of 'Box<bool>' requested here` per frame |
| inference failure / conflict | `cannot infer type parameter 'T' of 'mk' from the call arguments` / `… deduced as both 'int' and 'float'` + `specify the type arguments explicitly: 'mk<...>(...)'` |
| runaway recursion | `instantiating 'Bad<Bad<…>>' exceeds the maximum instantiation depth (16)` (`kMaxInstantiationDepth`) |
| imported template | `generic types cannot be imported yet: 'gen::Box<...>' names a generic class of another module` (and the function/constructor variants) |
| name clash with an exported instantiation | `instantiation 'Box<int>' conflicts with an imported class of the same name (type names are global across imports)` |

## 5. Modules (implemented)

- Templates are **not exported**: `TranslationUnit::GenericClassDecls` is
  invisible to the exporter, and `mod::Box<int>` / `mod::Box<int>(1)` /
  `mod::first<int>(xs)` are rejected with the messages above.
- A module's own instantiations *are* exported, as the concrete classes they
  are: `fn mk() -> Box<int>` serialises its return type as `Box<int>` via
  `ast::typeName`, the importer rebuilds a class named `Box<int>` through
  `resolveExportedType` → `lookupType("Box<int>")`, and `b = gen::mk();
  b.get()` works end to end (`CodeGen/GenericsTests.cpp:
  ExportedInstantiationAcrossModules`). The instantiation's canonical name is
  exactly what makes this round-trip.
- Because type names are global across the import graph, an importer that
  instantiates its **own** `Box<T>` as `Box<int>` while an import exports a
  `Box<int>` gets a hard error rather than a silent merge of two unrelated
  types. Exporting templates (so both sides share one `Box<T>`) is the real
  fix and is future work.

## 6. Out of scope (documented decisions)

- **Constraints / bounds and interfaces.** No `T: Bound`; a template body may
  do anything, and validity is decided per instantiation. Interfaces are the
  prerequisite; see §3.
- **Variance.** `Box<Dog>` is not a `Box<Animal>`; instantiations are unrelated
  nominal classes (same as arrays today). Use-site or declaration-site
  variance is a later design.
- **Default type arguments, explicit specialisation, partial specialisation.**
  Not supported; every instantiation comes from the one primary template.
- **Generic methods on non-generic classes** (`class C { fn m<T>(x: T) }`).
  Not supported: the parser accepts only free generic functions and generic
  classes. It would need per-instantiation method emission and a name
  mangling scheme for vtable slots (a generic method cannot be virtual without
  a dictionary-passing or JIT-on-demand model) — not "trivial", hence left out.
- **Generic superclass** (`class Sub<T> : Base<T>`). The superclass clause is a
  plain class name; a generic one is a parse error. Needs the superclass name
  to become a type and instantiation ordering (superclass populated before the
  subclass's vtable is inherited).
- **Recursion limit.** A simple depth guard (`kMaxInstantiationDepth = 16`)
  stops runaway expansions such as `class Bad<T> { inner: Bad<Bad<T>>; }`;
  nothing smarter (e.g. detecting the growing-type pattern early).
- **Type parameters as `match` arms** (`match o { T { … } }`) are substituted
  and then subject to the ordinary rule that an arm must be a class or array
  type, so `T = int` is rejected as any `int` arm would be.

## 7. Known limitations of the prototype

- **Emission order.** CodeGen resolves constructor calls by name at the point
  of emission (a pre-existing constraint), so instantiations are placed ahead
  of hand-written classes and sorted by the recorded "constructs" edges. An
  instantiation whose method body constructs a hand-written class declared
  *later* hits the pre-existing "constructor emitted after its first call"
  CodeGen bug, exactly as a hand-written class would.
- **Superclass ordering.** A generic class extending a hand-written class of
  the same module can only be instantiated once that superclass has been
  populated (its vtable prefix must be complete); instantiating it from a
  field type of a class sorted before the superclass is rejected with an
  explicit message rather than miscompiled.
- **Inference is exact.** No promotion (`int` → `float`) and no subtype
  widening during unification; explicit arguments always work.
- **Templates are only syntax-checked.** A template that is never instantiated
  can contain nonsense in its body (undeclared names) without a diagnostic.
- **One shared registry name space.** Instantiation names live next to class
  names; an imported class literally named `Box<int>` is a clash (reported).
- The `--dump-ast` output shows templates (with `<T>`) but not instantiations,
  because the dump happens before Sema.

## 8. Open questions

1. Should `mod::Box<int>` resolve to the module's exported instantiation when
   the module happens to have created it? Today it is always an error, which
   keeps the rule simple ("templates are not importable") at the cost of not
   being able to *name* the type of `gen::mk()` in the importer (inference
   `b = gen::mk()` works).
2. Which checking model to commit to once interfaces exist (§3): keep
   per-instantiation errors as a fallback for unconstrained templates, or
   require bounds for every operation on a type parameter.
3. Should unification promote `int` to `float` and pick the join of two class
   arguments (`pick(dog, cat)` → `Animal`) instead of reporting a conflict?
4. Whether to mangle instantiation names for LLVM (`Box$int`) instead of
   relying on quoted identifiers, for friendlier `--emit-llvm` output and
   symbol names in AOT binaries.
5. Exporting templates: serialise the template AST (or its source) into the
   module record so importers instantiate it themselves, which also removes
   the `Box<int>` cross-module identity problem (§5).

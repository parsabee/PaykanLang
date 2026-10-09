# Paykan grammar

This document is the specification of Paykan's concrete syntax. Every
frontend implements it: the in-tree recursive-descent frontend
(`src/Frontends/RecursiveDescent`, the default) and every out-of-tree
frontend plugin ([writing-a-frontend-plugin.md](writing-a-frontend-plugin.md)). An
out-of-tree frontend must follow this specification: it must build exactly
the same AST as the recursive-descent frontend for every input and must
accept and reject the same inputs. The parser and Sema suites and the
differential check (`tests/Frontend/DifferentialTests.cpp`), which a plugin
runs against its frontend with `paykan_add_frontend_tests`, enforce it.
When the grammar changes, this file changes first, then the
recursive-descent frontend; out-of-tree frontends follow.

Notation: EBNF. `"x"` is a literal, `A?` optional, `A*` zero or more, `A+`
one or more, `A | B` alternatives, `( ... )` grouping. Terminals are in
upper case; nonterminals in camelCase.

## 1. Lexical structure

A source file is a sequence of bytes. Line and column numbers count bytes:
lines are separated by `\n`, columns start at 1 on every line, and a tab
counts as one column.

### Whitespace and comments

```
blank    ::= " " | "\t" | "\r"
newline  ::= "\n"
comment  ::= "//" [^\n]*
```

Whitespace, newlines and comments separate tokens and are otherwise ignored.
There are no block comments.

### Tokens

Longest match wins; among equal-length matches a keyword beats an
identifier. Tokens are scanned left to right with one exception, the
tuple-index rule below.

```
IDENT    ::= [_a-zA-Z] [_a-zA-Z0-9]*         (except the keywords and "_")
INT      ::= [0-9]+
FLOAT    ::= [0-9]+ "." [0-9]* exponent?
           | [0-9]+ exponent
exponent ::= ("e" | "E") ("+" | "-")? [0-9]+
CHAR     ::= "'" ( [^\\'\n] | "\\" [^\n] ) "'"
STRING   ::= '"' ( [^\\"\n] | "\\" [^\n] )* '"'
```

Keywords (reserved; never identifiers):

```
import  as  class  enum  match  mov  fn  return  if  then  else  while
break  continue  True  False  None  _  view  inout  let
```

`_` alone is the `UNDERSCORE` token (destructuring skip, match wildcard);
`_x` is an identifier. `True`/`False` are the `BOOL` literals and `None` the
`NONE` literal.

Punctuation and operators:

```
=  -  +  *  /  %  <  >  <=  >=  ==  !=  !  ?  :  ::  .  ;  ,
(  )  {  }  [  ]  ->  &&  ||
```

Any other byte is a lexical error (`invalid character 'X'`).

### Literal values

- `INT` is a 64-bit signed integer. The scanner never sees a sign (`-` is a
  separate token), so `9223372036854775808` is out of range
  (`integer is out of range: ...`); `INT64_MIN` is written as the expression
  `-9223372036854775807 - 1`.
- `FLOAT` is converted with `strtod` and must denote a finite float:
  a literal that overflows to infinity (`1e999`) or a nonzero one that
  underflows to zero (`1e-400`) is an error (`float is out of range: ...`).
  Subnormal values (`5e-324`) and zero with any exponent (`0e-999`) are
  fine. `1.` is a float; `1e` is the integer `1` followed by the
  identifier `e`.
- `CHAR` escapes: `\n` `\t` `\r` `\\` `\'` `\0`; any other `\X` is `X`.
  A character literal missing its closing quote before the end of the line
  or file is an error (`unterminated character literal`).  With the
  recursive-descent frontend, a literal holding more than one character
  before its closing quote (`'ab'`) is one error (`character literal must
  contain exactly one character ...`); another frontend may report it as
  unterminated.
- `STRING` escapes: `\n` `\t` `\r` `\\` `\"` `\0`; any other `\X` is kept
  verbatim as the two characters `\X`. String literals are single-line: a
  raw newline before the closing quote is an error (`unterminated string
  literal ...`); embed a newline as `\n`.

### Tuple indices

A run of digits immediately after a `.` (no whitespace between) is a tuple
index, not a number: `t.0.1` is `t` `.` `0` `.` `1`, never `t` `.` `0.1`. A
tuple index must not have leading zeros (`tuple index must not have leading
zeros: .00`) and must fit in 64 bits. `t. 0` (whitespace after the dot) is
a syntax error. A scanner-generated frontend can implement the rule with
one tuple-index token (`\.[0-9]+`, longest match at the dot).

## 2. Translation unit

```
translationUnit ::= topLevelDecl*
topLevelDecl    ::= importDecl | classDecl | funcDecl | enumDecl
```

Declarations may appear in any order. The AST keeps them in four lists
(imports, enums, classes, functions), each in source order, with generic
classes and generic functions in two further lists.

## 3. Imports

```
importDecl ::= "import" "::"? modulePath ";"
             | "import" "::"? modulePath "as" IDENT ";"
             | "import" "::"? modulePath "::" "{" importedModuleList "}" ";"
             | "import" "::" "{" importedModuleList "}" ";"

modulePath         ::= IDENT ( "::" IDENT )*
importedModuleList ::= importedModule ( "," importedModule )*
importedModule     ::= IDENT ( "as" IDENT )?
```

A leading `::` marks a system (standard library) import. For the single
forms the path is split at its last `::` into the base path and the module
name; for the list form the whole path is the base path.

## 4. Declarations

```
classDecl   ::= "class" IDENT typeParams? ( ":" modulePath )?
                "{" classMember* "}"
classMember ::= varDecl ";" | funcDecl
typeParams  ::= "<" IDENT ( "," IDENT )* ">"

enumDecl    ::= "enum" IDENT "{" IDENT ( "," IDENT )* ","? "}"

funcDecl    ::= "fn" IDENT typeParams? "(" paramList? ")"
                ( "->" typeAnnotation )? block
paramList   ::= param ( "," param )*
param       ::= IDENT ":" ( "view" | "inout" )? typeAnnotation

varDecl     ::= IDENT ":" typeAnnotation
```

An enum needs at least one variant; a single trailing comma is permitted.
A class or function with `typeParams` is generic (a template that Sema
instantiates); the superclass of a generic class is a plain class name.
Methods are `funcDecl`s inside a class body.

A parameter's type may start with its mode, `view` or `inout`
([language/02-functions-and-calling.md](language/02-functions-and-calling.md)):
`fn bump(n: inout int)`, `fn __init__(start: view int)`.  The keywords are
reserved and appear nowhere else: `view` or `inout` in any other position (a
statement, a local's or a field's type, before the parameter's name) is a
syntax error; before the name, the error shows the parameter rewritten.
The AST records the mode on the parameter (`Param::Mode`); whether it is
allowed there (the parameter's type, an override) is Sema's job.

## 5. Types

```
typeAnnotation ::= primaryType ( "[" "]" | "?" )*
primaryType    ::= IDENT
                 | modulePath "::" IDENT
                 | IDENT "<" typeArgList ">"
                 | modulePath "::" IDENT "<" typeArgList ">"
                 | "(" typeAnnotation ( "," typeAnnotation )+ ")"
typeArgList    ::= typeAnnotation ( "," typeAnnotation )*
```

- `T[]` is an array type, `T?` an optional type; the suffixes nest left to
  right (`int[][]`, `Node?[]`, `(int, Str)[]`).
- `T?` is rejected at parse time when `T` is `void` (`optional type 'void?'
  is not supported`) or already optional (`nested optional type 'Str??' is
  not supported`). The other builtin value types have optional forms
  (`int?`, `float?`, `bool?`, `char?`). An enum spelled `Color?` is only
  known to Sema, which rejects it there.
- A tuple type has at least two elements; `(int)` is a syntax error (there
  are no parenthesised types).
- `Box<int>` is a generic type application (a `GenericType` node that Sema
  instantiates). The qualified form `lib::Box<int>` parses so that Sema can
  reject it with a dedicated diagnostic.
- An unqualified name resolves at parse time to the builtin type or
  bootstrap class of that name (`int`, `Str`, `Obj`, ...) if there is one;
  any other name becomes a `ClassType` stub carrying the name and its
  location, which Sema resolves (the class may be declared later in the
  file). A qualified name always becomes a stub named `module::Type`.

## 6. Statements

```
block     ::= "{" statement* "}"

statement ::= ";"
            | expression ";"
            | expression "=" expression ";"
            | varDecl "=" expression ";"
            | letDecl
            | destructureTargets "=" expression ";"
            | "return" expression? ";"
            | block
            | ifStmt
            | whileStmt
            | "break" ";"
            | "continue" ";"
            | matchStmt

letDecl   ::= "let" IDENT ( ":" typeAnnotation )? "=" expression ";"

ifStmt    ::= "if" "(" expression ")" block ( "else" ( block | ifStmt ) )?
whileStmt ::= "while" "(" expression ")" block

destructureTargets ::= destructureTarget ( "," destructureTarget )+
destructureTarget  ::= IDENT | IDENT ":" typeAnnotation | "_"

matchStmt ::= "match" expression "{" matchArm+ "}"
matchArm  ::= typeAnnotation block
            | IDENT ":" typeAnnotation block
            | literal block
            | "_" block
literal   ::= INT | FLOAT | BOOL | CHAR | STRING | NONE
```

- `;` on its own is an empty statement and produces no AST node.
- The left-hand side of `expression "=" expression` must be an identifier
  (`AssignStmt`), a field access `recv.field` (`MemberAssignStmt`), a
  subscript `arr[i]` (`SubscriptAssignStmt`) or a tuple index `t.0` (a
  `MemberAssignStmt` whose field name is the index, which Sema rejects as
  immutable). Anything else: `left-hand side of '=' must be an identifier,
  field access, or subscript`.
- `varDecl "=" expression` is a declaration with an initializer; a bare
  `x: int;` is not a statement.
- `letDecl` declares one local that cannot be reassigned
  (`VarDecl::isLet`); its type, when not written, is its initializer's.
  The `VarDecl` and its `DeclStmt` start at `let`.  `let` starts nothing
  else: a parameter, a field, a type, a destructuring (`let a, b = t;`) or a
  `let` without an initializer is a syntax error.
- A statement that starts with `if` is an if-statement when the
  parenthesised condition is followed by `{`; otherwise it is an expression
  statement whose expression is a ternary (`if c then a else b;`).
- An `else` is followed by a block or by another if-statement (`else if`
  chains nest in the AST).
- Match arms: a wildcard `_`, a literal pattern (bare literal tokens only,
  so `-1` is not a pattern), a type with an optional binding, in any order.
  `None` is accepted as a pattern so that a match over an optional can name
  the absent case.

## 7. Expressions

```
expression ::= ternary
ternary    ::= "if" logicalOr "then" expression "else" ternary
             | logicalOr

logicalOr      ::= logicalAnd ( "||" logicalAnd )*
logicalAnd     ::= relational ( "&&" relational )*
relational     ::= additive ( relOp additive )?
relOp          ::= "<" | ">" | "<=" | ">=" | "==" | "!="
additive       ::= multiplicative ( ( "+" | "-" ) multiplicative )*
multiplicative ::= unary ( ( "*" | "/" | "%" ) unary )*
unary          ::= ( "!" | "-" ) unary | postfix

postfix ::= primary postfixOp*
postfixOp ::= "." IDENT "(" argumentList? ")"      -- method call
            | "." IDENT                            -- field access
            | "." INT                              -- tuple index (see §1)
            | "[" expression "]"                   -- subscript

primary ::= literal
          | IDENT
          | IDENT "(" argumentList? ")"                      -- call / constructor
          | IDENT "<" typeArgList ">" "(" argumentList? ")"  -- generic call /
                                                             -- conversion
          | modulePath "::" IDENT                            -- enum variant
          | modulePath "::" IDENT "(" argumentList? ")"      -- qualified call
          | modulePath "::" IDENT "<" typeArgList ">" "(" argumentList? ")"
          | "(" expression ")"
          | "(" expression ( "," expression )+ ")"           -- tuple literal
          | "[" argumentList? "]"                            -- array literal

argumentList ::= expression ( "," expression )*
```

Precedence, lowest to highest: ternary; `||`; `&&`; relational; `+ -`;
`* / %`; unary `! -`; postfix. Binary operators are left-associative
except the relational operators, which do not associate: `a < b < c` is a
syntax error (parenthesise one comparison). The condition of a ternary is
a `logicalOr`, so a nested ternary there needs parentheses; the `else`
branch may be another ternary without them.

`mov` is reserved but no longer part of the grammar (removed in v0.2.0).
A frontend reports `'mov' was removed in v0.2.0; ownership transfers are
inferred` (`frontend::kMovRemoved`) once per use and keeps parsing the
operand after it, so that is the only error.

A parenthesised expression is just the inner expression (no node is
created); `(a)` is a 1-tuple nowhere. A tuple literal has at least two
elements.

### Generic call versus comparison

`name <` in expression position is ambiguous between a comparison and a
generic call. It is a generic call exactly when a `typeArgList` followed by
`>` `(` fits at that point (`first<int>(xs)`, `Box<Str>("v")`,
`lib::Box<int>(1)`); otherwise `<` is the relational operator.

The recursive-descent frontend decides this by speculatively parsing the type
argument list. An LALR(1) frontend can decide it in its scanner instead: a
`<` right after an identifier is scanned ahead over type-list tokens to the
matching `>`, and opens type arguments if `(` follows. Both readings agree on
every input: in `f(a < b, c) > (d)` the `)` closes the call's own parenthesis
before any `>`, so it is a comparison of `f(a < b, c)` with `(d)` either
way, with the same AST.

### Conversion constructors

A conversion `Target<Source>(value)` (`Str<int>(n)`, `int<float>(f)`,
`int<Str>(s)`, ..., see [`language/01-language-basics.md`](language/01-language-basics.md)) is not
a separate production: it is the generic-call form above. The builtin type
names `int`, `float`, `bool`, `char` and `void` are ordinary `IDENT`s (they
are resolved as type names, not reserved by the scanner), so `int<Str>(s)`
parses exactly like `Box<int>(1)`. Every frontend builds a `CallExpr` whose
callee is the target name, with one type argument and one argument. Sema
tells a conversion apart from a generic constructor by its callee.

## 8. AST and source locations

Every frontend produces the node kinds in `include/AST.h`. Locations are
`<line:col-line:col>` with 1-based lines and columns and an exclusive end
column, as `--dump-ast` prints them. Rules that every frontend follows:

- A token's location is its first byte to one past its last byte.
- A composite node spans from the first byte of its first token to one past
  the last byte of its last token, parentheses included: in `(a + b) * c`
  the `BinaryExpr '*'` starts at `(`, while the inner `BinaryExpr '+'`
  starts at `a` (the parentheses create no node).
- Statement nodes include their terminating `;`; the `VarDecl` inside a
  `DeclStmt` ends with its initializer, and a class field's `VarDecl` ends
  with its type.
- A `CompoundStmt`'s location is the empty range just after its `{`
  (the location of an empty production in an LALR parser), regardless of
  its contents.
- The `TranslationUnit` spans `1:1` to the end of its last declaration;
  an empty file gives `<1:1-1:1>`.
- Builtin and bootstrap types resolved at parse time (`int`, `Str`, ...)
  carry no location; `ClassType` stubs, `ArrayType`, `OptionalType`,
  `TupleType` and `GenericType` nodes created by the parser carry the
  location of their annotation.

## 9. Diagnostics and recovery

Diagnostics go through `DiagEngine` in the clang-style
`file:line:col: error: message` format with a source snippet and caret.
The messages listed in this document are binding (tests assert on them);
the wording of other syntax errors (`unexpected X; expected Y`) is up to
each frontend, and the differential check compares ASTs and accept/reject,
not message text.

The recursive-descent frontend recovers from a syntax error at the next
statement boundary (`;`, or the `}` of the enclosing block, matching braces
on the way), at the next class member (`fn`, `;`, `}`) and at the next
top-level declaration (`import`, `class`, `enum`, `fn`), so one file can
report several independent errors. Another frontend may recover less (for
example only at `;` inside a block). Any error makes the parse fail; a file
with errors is never handed to Sema.

Nesting (blocks, parentheses, brackets, type applications, prefix
operators and conditional expressions) deeper than 512 levels is rejected
with `nesting too deep` by every frontend, so pathological inputs cannot
overflow the stack of the parser or of the passes after it.

Every frontend reports lexical and syntax errors in source order, and
locate the common ones alike (a tuple index with leading zeros at its `.`,
an unclosed `[]` type suffix where the `]` is missing). Beyond that the
diagnostics are not part of the differential check: each frontend words its
syntax errors and recovers in its own way, so the location of the first
error can differ by a token, and the follow-on errors can differ.

## 10. Forms that are easy to accept by accident

A generated grammar can accept a few forms by accident that this
specification does not. Every frontend must reject them (the
`GrammarEdge` parser tests check it), and the samples never use them:

- A leading comma in an argument list, array literal or parameter list:
  `f(, 1)`, `[, 1]`, `fn f(, a: int)`.
- Repeated commas in an enum body after the first variant: `enum E { a,, b }`.

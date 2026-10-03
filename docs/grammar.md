# Paykan grammar

This document is the specification of Paykan's concrete syntax. Every
frontend implements it: the recursive-descent frontend (`src/Frontends/RecursiveDescent`,
the default) and the Bison/Flex frontend (`src/Frontends/Bison`, optional).
Both must build exactly the same AST for every input and must accept and
reject the same inputs; the differential check in CI
(`tests/Frontend/DifferentialTests.cpp` and `scripts/diff_frontends.py`)
enforces it. When the grammar changes, this file changes first, then both
frontends.

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
break  continue  True  False  None  _
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
- `FLOAT` is converted with `strtod`. `1.` is a float; `1e` is the integer
  `1` followed by the identifier `e`.
- `CHAR` escapes: `\n` `\t` `\r` `\\` `\'` `\0`; any other `\X` is `X`.
  A character literal missing its closing quote before the end of the line
  or file is an error (`unterminated character literal`).
- `STRING` escapes: `\n` `\t` `\r` `\\` `\"` `\0`; any other `\X` is kept
  verbatim as the two characters `\X`. String literals are single-line: a
  raw newline before the closing quote is an error (`unterminated string
  literal ...`); embed a newline as `\n`.

### Tuple indices

A run of digits immediately after a `.` (no whitespace between) is a tuple
index, not a number: `t.0.1` is `t` `.` `0` `.` `1`, never `t` `.` `0.1`. A
tuple index must not have leading zeros (`tuple index must not have leading
zeros: .00`) and must fit in 64 bits. `t. 0` (whitespace after the dot) is
a syntax error. The Bison frontend implements the same rule with a
`TUPLE_INDEX` token (`\.[0-9]+`, longest match at the dot).

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
param       ::= IDENT ":" typeAnnotation

varDecl     ::= IDENT ":" typeAnnotation
```

An enum needs at least one variant; a single trailing comma is permitted.
A class or function with `typeParams` is generic (a template that Sema
instantiates); the superclass of a generic class is a plain class name.
Methods are `funcDecl`s inside a class body.

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
            | destructureTargets "=" expression ";"
            | "return" expression? ";"
            | block
            | ifStmt
            | whileStmt
            | "break" ";"
            | "continue" ";"
            | matchStmt

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
unary          ::= ( "!" | "-" | "mov" ) unary | postfix

postfix ::= primary postfixOp*
postfixOp ::= "." IDENT "(" argumentList? ")"      -- method call
            | "." IDENT                            -- field access
            | "." INT                              -- tuple index (see §1)
            | "[" expression "]"                   -- subscript

primary ::= literal
          | IDENT
          | IDENT "(" argumentList? ")"                      -- call / constructor
          | IDENT "<" typeArgList ">" "(" argumentList? ")"  -- generic call
          | modulePath "::" IDENT                            -- enum variant
          | modulePath "::" IDENT "(" argumentList? ")"      -- qualified call
          | modulePath "::" IDENT "<" typeArgList ">" "(" argumentList? ")"
          | "(" expression ")"
          | "(" expression ( "," expression )+ ")"           -- tuple literal
          | "[" argumentList? "]"                            -- array literal

argumentList ::= expression ( "," expression )*
```

Precedence, lowest to highest: ternary; `||`; `&&`; relational; `+ -`;
`* / %`; unary `! - mov`; postfix. Binary operators are left-associative
except the relational operators, which do not associate: `a < b < c` is a
syntax error (parenthesise one comparison). The condition of a ternary is
a `logicalOr`, so a nested ternary there needs parentheses; the `else`
branch may be another ternary without them.

`mov` is a unary operator: `mov a + b` is `(mov a) + b`.

A parenthesised expression is just the inner expression (no node is
created); `(a)` is a 1-tuple nowhere. A tuple literal has at least two
elements.

### Generic call versus comparison

`name <` in expression position is ambiguous between a comparison and a
generic call. It is a generic call exactly when a `typeArgList` followed by
`>` `(` fits at that point (`first<int>(xs)`, `Box<Str>("v")`,
`lib::Box<int>(1)`); otherwise `<` is the relational operator.

The recursive-descent frontend decides this by speculatively parsing the type
argument list. The Bison frontend approximates it in its scanner (a `<`
right after an identifier is scanned ahead over type-list tokens to the
matching `>`; if `(` follows, it opens type arguments). The two agree on
every input except `f(a < b, c) > (d)`, which the scanner mis-scans and
the Bison frontend rejects while the recursive-descent frontend accepts it as a
comparison. Writing `(f(a < b, c)) > (d)` works in both.

## 8. AST and source locations

Both frontends produce the node kinds in `include/AST.h`. Locations are
`<line:col-line:col>` with 1-based lines and columns and an exclusive end
column, as `--dump-ast` prints them. Rules that both frontends follow:

- A token's location is its first byte to one past its last byte.
- A composite node spans from the first byte of its first token to one past
  the last byte of its last token, parentheses included: in `(a + b) * c`
  the `BinaryExpr '*'` starts at `(`, while the inner `BinaryExpr '+'`
  starts at `a` (the parentheses create no node).
- Statement nodes include their terminating `;`; the `VarDecl` inside a
  `DeclStmt` ends with its initializer, and a class field's `VarDecl` ends
  with its type.
- A `CompoundStmt`'s location is the empty range just after its `{` (the
  Bison frontend's location of an empty production), regardless of its
  contents.
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
report several independent errors. The Bison frontend recovers only at
`;` inside a block. Any error makes the parse fail; a file with errors is
never handed to Sema.

Nesting (blocks, parentheses, brackets, type applications) deeper than
512 levels is rejected with `nesting too deep`, so pathological inputs
cannot overflow the stack.

## 10. Known differences and intentional non-copies

The Bison grammar accepts a few forms by accident that this specification
does not; the recursive-descent frontend rejects them and the samples never use
them:

- A leading comma in an argument list, array literal or parameter list:
  `f(, 1)`, `[, 1]`, `fn f(, a: int)`.
- Repeated commas in an enum body after the first variant: `enum E { a,, b }`.
- `f(a < b, c) > (d)` is rejected by the Bison scanner heuristic (see §7)
  but is a valid comparison.

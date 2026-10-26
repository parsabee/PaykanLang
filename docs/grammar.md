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
import  as  class  struct  enum  match  mov  fn  native  return  if  then  else
elif  while  for  break  continue  True  False  None  _  view  inout  let  weak  impl
static  trait
```

`_` alone is the `UNDERSCORE` token (destructuring skip, match wildcard);
`_x` is an identifier. `True`/`False` are the `BOOL` literals and `None` the
`NONE` literal.

Punctuation and operators:

```
=  -  +  *  /  %  <  >  <=  >=  ==  !=  !  ?  :  ::  .  ..  ..=  ;  ,
(  )  {  }  [  ]  ->  =>  &&  ||  +=  -=  *=  /=  %=
```

`=>` starts an expression body (`fn double(x: int) => x * 2;`); `= >` is two
tokens.

Any other byte is a lexical error (`invalid character 'X'`).

### Literal values

- `INT` is a 64-bit signed integer. The scanner never sees a sign (`-` is a
  separate token), so `9223372036854775808` is out of range
  (`integer is out of range: ...`); `INT64_MIN` is written as the expression
  `-9223372036854775807 - 1`.  Right after a `-`, the scanner takes that
  magnitude too, for the match pattern `-9223372036854775808` (§6), which is
  `INT64_MIN`; the parser rejects it anywhere else, with the same error.
- `FLOAT` is converted with `strtod` and must denote a finite float:
  a literal that overflows to infinity (`1e999`) or a nonzero one that
  underflows to zero (`1e-400`) is an error (`float is out of range: ...`).
  Subnormal values (`5e-324`) and zero with any exponent (`0e-999`) are
  fine. `1.` is a float; `1e` is the integer `1` followed by the
  identifier `e`. A `.` followed by another `.` is not part of a number:
  `0..n` is `0` `..` `n` (a range), never the float `0.` and a `.`.
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
- Adjacent `STRING` tokens, with only whitespace, newlines or comments
  between them, are **one** string literal, as in C and C++
  (`stringLiteral ::= STRING+`): its value is the pieces' values, each with
  its escapes decoded, joined in order, and its source range covers every
  piece. `"this is " "one literal"` is `"this is one literal"`, and a long
  string is split across lines this way, at no cost at run time: the AST
  holds the one joined literal. The joined literal is one primary
  expression, so a postfix applies to all of it (`"ab" "c"[2]` is `'c'`). A
  `CHAR` does not join: `"a" 'b'` is a syntax error.

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
topLevelDecl    ::= importDecl | classDecl | structDecl | funcDecl
                  | nativeFuncDecl | enumDecl | traitDecl | implDecl
```

Declarations may appear in any order. The AST keeps them in lists by kind
(imports, enums, structs, classes, functions, traits, `impl` blocks), each in
source order, with generic structs, generic classes and generic functions in
three further lists.

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
classMember ::= "weak"? varDecl ";" | funcDecl      -- funcDecl without typeParams
              | "view"? "fn" IDENT "(" paramList? ")"
                ( "->" ( "view" | "inout" )? typeAnnotation )? ";"   -- pure virtual
typeParams  ::= "<" typeParam ( "," typeParam )* ">"
typeParam   ::= IDENT ( ":" typeAnnotation ( "+" typeAnnotation )* )?

structDecl  ::= "struct" IDENT typeParams? "{" ( varDecl ";" )+ "}"

enumDecl    ::= "enum" IDENT typeParams? "{" variant ( "," variant )* ","? "}"
variant     ::= IDENT ( "(" varDecl ( "," varDecl )* ")" )?   -- its fields

traitDecl   ::= "trait" IDENT ( "<" IDENT ( "," IDENT )* ">" )?
                "{" traitMember* "}"
traitMember ::= "view"? "fn" IDENT "(" paramList? ")"
                ( "->" ( "view" | "inout" )? typeAnnotation )? ";"

implDecl    ::= "impl" ( typeAnnotation "for" )? modulePath
                ( "<" IDENT ( "," IDENT )* ">" )? "{" implMember* "}"
implMember  ::= funcDecl                          -- funcDecl without typeParams
              | "static" "fn" IDENT "(" paramList? ")"
                ( "->" ( "view" | "inout" )? typeAnnotation )?
                ( block | "=>" expression ";" )

funcDecl    ::= "view"? "fn" IDENT typeParams? "(" paramList? ")"
                ( "->" ( "view" | "inout" )? typeAnnotation )?
                ( block | "=>" expression ";" )
nativeFuncDecl ::= "native" "fn" IDENT typeParams? "(" paramList? ")"
                   ( "->" ( "view" | "inout" )? typeAnnotation )? "=" stringLiteral ";"   -- the C symbol
paramList   ::= param ( "," param )*
param       ::= IDENT ":" ( "view" | "inout" )? typeAnnotation
              | IDENT                                   -- a free function's only

varDecl     ::= IDENT ":" typeAnnotation
```

An enum needs at least one variant; a single trailing comma is permitted.
A variant's fields, when it has any, are at least one `name: Type` in
parentheses, without a trailing comma (`a variant without fields is written
without parentheses: 'B'`; `a variant's field is a value: write 'x: T'` for
a mode); a variant is made with a call of its qualified name, `E::B(1, s)`,
and bound by a `match` arm, `b: B { ... }`
([language/07-enums.md](language/07-enums.md#variants-with-fields)).
A struct body holds fields only, at least one: a method goes in an `impl`
block, and a struct has no superclass
([language/11-structs.md](language/11-structs.md)); both are syntax errors
(`a struct body holds fields only ('name: Type;'); a method goes in an 'impl'
block`, `a struct has no superclass`), and so is an empty body (`struct 'S'
has no fields: a struct has at least one`).
A class, struct, enum or function with `typeParams` is generic (a template that
Sema instantiates); the superclass of a generic class is a plain class name.
Only a free function is generic: a method in a class body or an `impl` block,
pure virtual or not, and a `static fn` have no `typeParams` of their own (the
methods of a generic class or struct use its), and `<` after their name is a
syntax error there (`a method cannot have type parameters of its own: only a
free function is generic ...`, `a 'static fn' cannot have type parameters of
its own: ...`, `frontend::kGenericMethod` and `kGenericStaticFn`).
A type parameter's bounds (`T: Show + Into<int>`) name the traits its type
argument must implement.  `impl Show for A` implements trait `Show` for `A`.
A function or method written `=> expression ;` has for its body a block of one
`return` spanning the expression (`fn double(x: int) -> int => x * 2;` is
`fn double(x: int) -> int { return x * 2; }`); without a result type, `fn
double(x: int) => x * 2;`, the result type is the expression's, which Sema
infers (`FuncDecl::hasInferredResult`; a method's is not inferred).  A
`native fn`'s `=` is followed by its C symbol instead; after any other
function's signature `=` is a syntax error that names `=>` (`an expression
body starts with '=>', not '='`).  A trait method's signature has neither
(`a trait method has no body: end its signature with ';'`).
A free function's parameter may be its name alone, `fn double(x) => x * 2;`:
the parser gives it the type parameter `typeof(x)`, after the written type
parameters (`fn double<typeof(x)>(x: typeof(x)) => x * 2;`, a name no program
can write), so the function is generic.  A lambda's, method's, `static fn`'s
or trait method's parameter without a type is a syntax error (`parameter 'x'
needs a type ('x: int'): only a free function's parameters can leave it
out`), and so is a native function's (`parameter 'x' needs a type ('x: int'):
a native function's parameters have types`).
The `impl` block of a generic type names its type parameters: `impl Box<T>`.
A trait's methods are signatures that end with `;`.
Methods are `funcDecl`s inside a class body.  `view fn` marks a method that
does not change `self` ([language/05-classes.md](language/05-classes.md));
on a free function it is a semantic error, not a syntax error.  A class
body's method whose signature ends with `;` is pure virtual: a vtable slot
with no definition, the `FuncDecl` without a body
([language/05-classes.md](language/05-classes.md#pure-virtual-methods)).
After the signature of any other function but a trait method and a `native
fn`, `;` is a syntax error: `function 'f' has no body: only a method in a
class body can end its signature with ';' (a pure virtual method)` for a
free function, `'f' in an 'impl' block has no body: an 'impl' block's
methods are called directly, so only a method in a class body can end its
signature with ';' (a pure virtual method)` for an `impl` block's method,
and `'f' has no body: a 'static fn' needs one` and `'__init__' has no body:
an initializer needs one` for a `static fn` and an initializer (named as
written, `___init__` too); `destroy`, which is never declared, gets
`'destroy' is reserved for the compiler-generated destructor and cannot be
declared` at the function, an `impl` block's method or `static fn`, as
Sema reports one with a body.

A parameter's type may start with its mode, `view` or `inout`
([language/03-functions.md](language/03-functions.md)):
`fn bump(n: inout int)`, `fn __init__(start: view int)`.  The keywords are
reserved: besides a parameter's type, they only start a result's type
(`-> view T`), a function type's parameter (`paramType`, below), a local
borrow's type or a `match` binding's type (`borrowDecl` and `matchArm`,
below), and `view` a `view fn`.  In any other
position (a
statement, a field's type, before the parameter's name) they are a syntax
error; before the name, the error shows the parameter rewritten.
The AST records the mode on the parameter (`Param::Mode`); whether it is
allowed there (the parameter's type, an override) is Sema's job.

## 5. Types

```
typeAnnotation ::= primaryType ( "[" "]" | "[" ( INT | "_" ) "]" | "?" )*
primaryType    ::= IDENT
                 | modulePath "::" IDENT
                 | IDENT "<" typeArgList ">"
                 | modulePath "::" IDENT "<" typeArgList ">"
                 | "(" typeAnnotation ( "," typeAnnotation )+ ")"   -- tuple type
                 | functionType
                 | "(" functionType ")"
functionType   ::= "(" ( paramType ( "," paramType )* )? ")" "->" typeAnnotation
paramType      ::= ( "view" | "inout" )? typeAnnotation
typeArgList    ::= typeAnnotation ( "," typeAnnotation )*
```

- `(int, view Str) -> bool` is a function type (docs/language/14-function-types.md):
  the `->` after the `)` tells it from a tuple type, so a tuple type's elements have
  no mode (`a tuple element has no mode`).  `() -> int` takes no parameter; the result
  may be `void`, never `view` or `inout`.  `->` is right-associative, and a function
  type takes the `[]`, `[n]` and `?` suffixes only in parentheses: `(int) -> int[]`
  returns an `int[]`, `((int) -> int)?` is an optional function.  `(T)` with any other
  `T` stays an error: there are no parenthesised types.

- `T[n]` is an array of `n` elements (an integer literal, at least 1: `a
  fixed-size array has at least one element`), `T[]` a slice and `T?` an
  optional type; the suffixes nest left to right (`Node?[3]`,
  `(int, Str)[2]`, `int[3][]` is a slice of `int[3]`s).  A run of lengths is
  one array of arrays whose first length is its first index's, as in C:
  `int[2][3]` is two `int[3]`s, indexed `grid[i][j]` with `i < 2` and
  `j < 3`.  A length of `_` (`int[_]`) is the initializer's; Sema allows it
  only in a variable declaration with one.
- `T[]` parses wherever a type does, but it is only the spelling of a
  `view` or `inout` parameter's slice
  ([language/09-arrays.md](language/09-arrays.md#slices)): Sema rejects it
  anywhere else (`variable 'x' has type 'int[]': a slice can only be a 'view'
  or 'inout' parameter, and an array has a length ('int[3]')`, or `..., and a
  struct field cannot be an array yet` where no fixed-size array would do).
- `T?` is rejected at parse time when `T` is `void` (`optional type 'void?'
  is not supported`) or already optional (`nested optional type 'Str??' is
  not supported`). The other builtin value types have optional forms
  (`int?`, `float?`, `bool?`, `char?`, `u8?`, `f32?`, ...). An enum spelled `Color?` is only
  known to Sema, which resolves it there (an enum, a tuple and a struct have
  optionals; a fixed-size array does not).
- A tuple type has at least two elements; `(int)` is a syntax error (there
  are no parenthesised types).
- `Box<int>` is a generic type application (a `GenericType` node that Sema
  instantiates). The qualified form `lib::Box<int>` names a generic class of
  an imported module.
- An unqualified name resolves at parse time to the builtin type or
  bootstrap class of that name (`int`, `Str`, `Obj`, ...) if there is one;
  any other name becomes a `ClassType` stub carrying the name and its
  location, which Sema resolves (the class, struct or enum may be declared
  later in the file). A qualified name always becomes a stub named
  `module::Type`.

## 6. Statements

```
block     ::= "{" statement* "}"

statement ::= ";"
            | expression ";"
            | expression "=" expression ";"
            | expression assignOp expression ";"
            | varDecl "=" expression ";"
            | letDecl
            | weakDecl
            | borrowDecl
            | destructureTargets "=" expression ";"
            | "return" expression? ";"
            | block
            | ifStmt
            | whileStmt
            | forStmt
            | "break" ";"
            | "continue" ";"
            | matchStmt

assignOp  ::= "+=" | "-=" | "*=" | "/=" | "%="
letDecl   ::= "let" IDENT ( ":" typeAnnotation )? "=" expression ";"
weakDecl  ::= "weak" IDENT ( ":" typeAnnotation )? ( "=" expression )? ";"
borrowDecl ::= IDENT ":" ( "view" | "inout" ) typeAnnotation? "=" expression ";"

ifStmt    ::= "if" expression block elseTail?
elseTail  ::= "elif" expression block elseTail? | "else" block
whileStmt ::= "while" expression block
forStmt   ::= "for" ( IDENT | "_" ) ( ":" typeAnnotation )? "in" expression
              ( ".." | "..=" ) expression ( "step" expression )? block

destructureTargets ::= destructureTarget ( "," destructureTarget )+
destructureTarget  ::= IDENT | IDENT ":" typeAnnotation | "_"

matchStmt ::= "match" expression "{" matchArm+ "}"
matchArm  ::= typeAnnotation block
            | IDENT ":" ( "view" | "inout" )? typeAnnotation block
            | literal block
            | "-" ( INT | FLOAT ) block          -- a negative number
            | "_" block
literal   ::= INT | FLOAT | BOOL | CHAR | stringLiteral | NONE
stringLiteral ::= STRING+                  -- adjacent pieces, joined (§1)
```

- `;` on its own is an empty statement and produces no AST node.
- The left-hand side of `expression "=" expression` must be an identifier
  (`AssignStmt`), a field access `recv.field` (`MemberAssignStmt`), a
  subscript `arr[i]` (`SubscriptAssignStmt`) or a tuple index `t.0` (a
  `MemberAssignStmt` whose field name is the index, which Sema rejects as
  immutable). Anything else: `left-hand side of '=' must be an identifier,
  field access, or subscript`.
- A compound assignment `target op= value` is the assignment
  `target = target op value`, built by the parser: its value is a
  `BinaryExpr` (`+=` is `+`, ..., `%=` is `%`) spanning `target op= value`,
  whose left operand is a copy of the target's expression and whose right
  operand is `value` whole (`k *= k + 1` is `k = k * (k + 1)`).  The target
  is checked as `=`'s is (`left-hand side of '+=' must be ...`), and is
  evaluated twice, once to read it and once to assign it: `xs[f()] += 1`
  calls `f` twice.
- `varDecl "=" expression` is a declaration with an initializer; a bare
  `x: int;` is not a statement.  Only a `weakDecl` may leave out its
  initializer, and Sema rejects a `DeclStmt` of any other `VarDecl` without
  one, whichever frontend built it (`variable 'x' needs an initial value
  ...`).
- `letDecl` declares one local that cannot be reassigned
  (`VarDecl::isLet`); its type, when not written, is its initializer's.
  The `VarDecl` and its `DeclStmt` start at `let`.  `let` starts nothing
  else: a parameter, a field, a type, a destructuring (`let a, b = t;`) or a
  `let` without an initializer is a syntax error.
- `weakDecl` declares one local holding a weak reference
  (`VarDecl::isWeak`); its type, when not written, is its initializer's, and
  without an initializer it starts as `None`.  It needs a type or an
  initializer (`a 'weak' declaration needs a type or an initial value`), and
  is neither a borrow (`weak x: view = e;`) nor a destructuring.
- `borrowDecl` declares a local borrow (`VarDecl::getMode`); its type, when
  not written, is its initializer's.  It needs an initializer, is never
  `let` (`let x: view = e;`) and is not a destructuring target.  With the
  mode before the name (`view x = e;`) the syntax error shows the fix.
- A condition is not parenthesised (`if x > 0 {`, `while i < n {`); it ends
  at the block's `{`, which no expression consumes outside brackets except a
  lambda's body.  Parentheses around it are a parenthesised expression, so
  `if (x > 0) {` still parses.
- A statement that starts with `if` is an if-statement when its condition is
  followed by `{`, and an expression statement whose expression is a ternary
  (`if c then a else b;`) when it is followed by `then`: the first of the two
  outside any brackets, not counting a nested ternary's `then`, decides.
- An if-statement chains with `elif`, never `else if` (a syntax error that
  names `elif`); an `else` is followed by a block.  Each `elif` is an
  `IfStmt` nested as the else branch of the one before it.  The conditional
  expression keeps `else if` (`if a then x else if b then y else z`), and
  `elif` there is a syntax error that names `else if`.
- A `forStmt` (`ForStmt`) declares its variable, a `VarDecl` with no
  initializer that spans the name and its annotation (`_` is one with an
  empty name, which binds nothing); the statement spans `for` to the block.  The range's ends are any expressions: `..` and `..=`
  are not operators, so each end is a whole expression (`0..n - 1` is
  `0..(n - 1)`).  `in` and `step` are not keywords: an identifier spelled
  `in` right after the variable (and its annotation) separates it from the
  range, one spelled `step` right after the range's end starts the step,
  and anywhere else each is an ordinary name (`for in in 0..step step step {`
  parses).  The step is null in the AST when it is not written.
- Match arms: a wildcard `_`, a literal pattern, a type with an optional
  binding, in any order.  A literal pattern is a literal, or a negative
  number: `-` and an `INT` or a `FLOAT` (`-1`, `- 2.5`) is one
  `IntegerLiteral` or `FloatLiteral` of the negated value, spanning both
  tokens, not a `UnaryExpr`; `-9223372036854775808` is `INT64_MIN` (§1).
  Anything else after the `-` is a syntax error (`unexpected '-'; expected
  a type`).  Sema rejects a pattern that is not a literal, whichever
  frontend built it (`a value-match arm's pattern must be a literal ...`).
  A binding may borrow the subject, `n: view T` or `n: inout T`
  (`MatchArm::getMode`); with
  the mode before the name (`view n: T`) the syntax error shows the fix.
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
postfixOp ::= "(" argumentList? ")"                -- call of a function value
            | "." IDENT "(" argumentList? ")"      -- method call
            | "." IDENT                            -- field access
            | "." INT                              -- tuple index (see §1)
            | "[" expression "]"                   -- subscript

primary ::= literal
          | IDENT
          | IDENT "(" argumentList? ")"                      -- call / constructor
          | IDENT "<" typeArgList ">" "(" argumentList? ")"  -- generic call /
                                                             -- conversion
          | IDENT "<" typeArgList ">" "::" IDENT "(" argumentList? ")"
                                                             -- static fn of an
                                                             -- instantiation, or
                                                             -- a generic enum's
                                                             -- variant with fields
          | IDENT "<" typeArgList ">" "::" IDENT             -- a generic enum's
                                                             -- variant
          | modulePath "::" IDENT                            -- enum variant, or a
                                                             -- qualified function
                                                             -- as a value
          | modulePath "::" IDENT "(" argumentList? ")"      -- qualified call
          | modulePath "::" IDENT "<" typeArgList ">" "(" argumentList? ")"
          | modulePath "::" IDENT "<" typeArgList ">" "::" IDENT "(" argumentList? ")"
          | modulePath "::" IDENT "<" typeArgList ">" "::" IDENT
          | "(" expression ")"
          | "(" expression ( "," expression )+ ")"           -- tuple literal
          | "[" argumentList? "]"                            -- array literal
          | "[" expression ";" INT "]"                       -- repeated value
          | lambda

lambda  ::= "fn" "(" paramList? ")" ( "->" typeAnnotation )?
            ( block | "=>" expression )

argumentList ::= expression ( "," expression )*
```

A lambda is a function without a name, in expression position
(docs/language/14-function-types.md): `fn(x: int) -> int { return x + 1; }`.
Its parameters are written like a function's, modes included, its result type
is written or `void`, never `view` or `inout`, and its body is a block.  With
`=>` its body is an expression instead, which the parser makes a block of one
`return` spanning the expression: `fn(x: int) -> int => x + 1` is
`fn(x: int) -> int { return x + 1; }`.  Without a result type, `fn(x: int) =>
x + 1`, the result type is the expression's, which Sema infers
(`LambdaExpr::hasInferredResult`); it is not `void`.  The expression extends as
far as an expression can, so `fn(x: int) => fn(y: int) => x + y` nests.

Precedence, lowest to highest: ternary; `||`; `&&`; relational; `+ -`;
`* / %`; unary `! -`; postfix. Binary operators are left-associative
except the relational operators, which do not associate: `a < b < c` is a
syntax error (parenthesise one comparison). The condition of a ternary is
a `logicalOr`, so a nested ternary there needs parentheses; the `else`
branch may be another ternary without them.

`mov` is reserved but no longer part of the grammar (removed in v0.2.0).
A frontend reports `'mov' was removed in v0.2.0; ownership transfers are
inferred` (`frontend::kMovRemoved`) once per use, and once for a run of
them (`mov mov s`), and keeps parsing the operand after it, so that is the
only error.

A parenthesised expression is just the inner expression (no node is
created); `(a)` is a 1-tuple nowhere. A tuple literal has at least two
elements.

An array literal `[e1, ..., en]` is a `T[n]`, an array of its own length.
Where it flows into a typed slot (a declaration's type, an argument, a
return, a field), `T` is the slot's element type, and into a slice it is a
`T[n]` of the slice's `T`. Anywhere else `T` is its elements' common type:
`int` and `float` give `float` (`[1, 2.5]` is a `float[2]`), and classes
their nearest common ancestor. `[]` parses, but Sema rejects it (`an empty
array literal has no type: an array has at least one element`).

`[v; n]` is `n` copies of `v`. `n` is an `INT` of at least 1: `[0; 0]` is a
syntax error (`an array has at least one element: the count must be 1 or
more`), and so is any other token after the `;` (`[0; n]`: `expected the
number of copies, an integer literal`). It types as the literal `[v, ...,
v]` of `n` elements would, so `[0; 4]` flowing into a `float[4]` is four
`0.0`s and `[[0; 3]; 2]` is an `int[2][3]`. `v` is evaluated once and every
element is a copy of its value: `[Node(); 3]` holds one object three times.

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
`int<Str>(s)`, ..., see [`language/02-basics.md`](language/02-basics.md)) is not
a separate production: it is the generic-call form above. The builtin type
names `int`, `float`, `bool`, `char`, `void` and the sized numeric types
(`u8`, `i32`, `unsigned`, `f32`, ...) are ordinary `IDENT`s (they are
resolved as type names, not reserved by the scanner), so `int<Str>(s)`
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
  carry no location; `ClassType` stubs, `ArrayType` (`T[]`),
  `FixedArrayType` (`T[n]`), `OptionalType`, `TupleType` and `GenericType`
  nodes created by the parser carry the location of their annotation.

## 9. Diagnostics and recovery

Diagnostics go through `DiagEngine` in the clang-style
`file:line:col: error: message` format with a source snippet and caret (a
line over 120 characters is shown in 120 around the caret, cut with `...`),
and at most `--error-limit=N` errors (20 by default) are printed
([The paykan command](language/17-the-paykan-command.md)).
So do the lexical errors `--dump-tokens` finds: it lists every token, then
fails.
The messages listed in this document are binding (tests assert on them);
the wording of other syntax errors (`unexpected X; expected Y`) is up to
each frontend, and the differential check compares ASTs and accept/reject,
not message text.

The recursive-descent frontend recovers from a syntax error at the next
statement boundary (`;`, or the `}` of the enclosing block, matching braces
on the way), at the next class, struct, `impl` or trait member (`fn`,
`view fn`, `static`, `;`, `}`) and at the next top-level declaration
(`import`, `class`, `struct`, `enum`, `trait`, `impl`, `fn`, `view fn`,
`native`), so one file can
report several independent errors. Past the nesting limit it skips the
block, the brackets or the parentheses one level too deep whole, through
their closer, and after an `if`'s or an `elif`'s block one level too deep
the rest of its `elif` and `else` branches, deeper still, so the limit is
the only error the construct gives. Another
frontend may recover less (for example only at `;` inside a block). Any
error makes the parse fail; a file with errors is never handed to Sema.

Nesting deeper than 512 levels is rejected with `nesting too deep (more
than 512 levels)` by every frontend, so that pathological input can
overflow the stack neither of the parser nor of the passes after it. Each
of these is a level: a block, and a function's or a lambda's `=>` body,
which stands for one; an `elif`, which is the `if` of an else branch (so
an `if` takes at most 510 `elif`s in a function's body); a class, struct,
`impl` or trait body; the arms of a `match`; a parenthesised expression or
a tuple's elements; the arguments
of a call and the elements of an array literal; a subscript's index; the
operand of a prefix operator; a conditional expression; a type
application's arguments; a tuple type's elements; a function type's
parameters and its result, and a lambda's result type; and each type
suffix (`[]`, `[N]`, `?`), around the levels of the type it wraps, so that
`B<int>[][]` is three levels deep, as its AST is. The AST interchange
reader counts the same levels in the AST it reads, which has no
parentheses: there an operand that needs them where it stands (`a + (b +
c)`, `(a == b) == c`, `-(a + b)`, `(a + b).m()`, `(if c then a else b) +
d`) is the level its parenthesis is
([`plugins/ast-format.md`](plugins/ast-format.md) section 3).

A chain of more than 2048 links is rejected with `nesting too deep (more
than 2048 levels)` by every frontend, for the same reason. Each binary
operator is a link over its two operands, and each postfix suffix (a call
`(...)`, a field `.f`, a method call `.m(...)`, a tuple index `.0`, a
subscript `[i]`) over the expression it applies to and its arguments or
index; `x += e` is a link over `x` and `e`. The links are counted along
every path down the AST, through parentheses, operands and arguments, and
the levels of nesting are not: `1 + 1 + ...` takes 2049 terms, `s.trim()
.trim()...` 2048 calls, and `(a + b + ...) + c + ...` counts the links on
both sides of the parenthesis together. The AST interchange reader counts
the same links.

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

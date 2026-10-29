# Bitwise operators

The design behind the bitwise operators on the unsigned types and the
hexadecimal and binary integer literals in
[`../language/02-basics.md`](../language/02-basics.md): `&`, `|`, `^`, `~`,
`<<` and `>>` with their compound assignments, defined on `u8`, `u16`, `u32`
and `unsigned` (`u64`) only, and `0xFF`, `0b1010` and `0xFFFF_0000`, which
reach every value of `unsigned`.  It builds on
[`sized-types.md`](sized-types.md).  Every value stays in its 8-byte slot, a
narrow unsigned value zero-extended, so no layout, runtime structure or
`.pkm` record changes.  PIR gains five instructions.  A shift by the type's
width or more gives 0 and never panics, on every backend.  The open
questions at the end each have the default this implementation takes.

The project owner fixed five points (2026-10-27): the operators are
unsigned-only; hexadecimal and binary literals take `_` separators and are
typed by context like decimal ones; an overshift gives 0 (Swift's `<<` and
`>>`); the precedence is Swift's; and this is its own change after the
stability sweep's chunk A.  Everything else below is this design's choice.

## Summary of the decisions

| Topic | Decision |
|-------|----------|
| Operators | `a & b`, `a \| b`, `a ^ b` (and, or, exclusive or), `~a` (complement), `a << n`, `a >> n` (shift left, logical shift right); the compound assignments `&=`, `\|=`, `^=`, `<<=` and `>>=`.  There is no `>>>`, no rotate and no masking shift (`&<<`) |
| Types | defined on `u8`, `u16`, `u32` and `unsigned` only.  `& \| ^` take one type for both operands, the one-type rule of `+`.  A shift's value is any unsigned type T and its count any unsigned type, of any width; the result is T.  `~a` has `a`'s type |
| Literal operands | the existing rule: only a literal adapts to its context.  A literal operand of `& \| ^` takes the other operand's type; a literal shift count is an `unsigned`; a literal shifted value takes no type from its count; `~literal` takes an unsigned context's type, as `-literal` takes a signed one's.  Two literals are `int`, which has no bitwise operator: an error with a hint |
| Errors | every rejected operand type names its fix: the unsigned type of the same width, `&&`, `\|\|`, `!=` or `!` for a `bool`, `unsigned(...)` for a signed count |
| Overshift | `x << n` and `x >> n` are 0 for every `n` at or above the width (8, 16, 32 or 64), up to 2^64-1.  A literal count at or above the width also gives a warning |
| Precedence | Swift's: `<< >>` above `* / % &`, above `+ - \| ^`, above the comparisons.  Shifts do not associate (`a << b << c` is an error); `& \| ^` are left-associative |
| `>>` | not a token: two `>` with nothing between them.  `>>=` is a `>` followed directly by `>=`.  Type argument lists keep closing with single `>` tokens.  `<<` and `<<=` are tokens |
| Literals | decimal, `0x` hexadecimal and `0b` binary, prefixes in lower case.  `_` stands between two digits of a hexadecimal or binary literal; not in decimal and float literals yet.  An integer literal is 0 to 18446744073709551615 in every base; Sema checks it against the type the context gives it.  A hexadecimal literal is a value, not a bit pattern (`y: i8 = 0xFF` is an error) |
| AST | `BinaryOpcode` gains `BitAnd BitOr BitXor Shl Shr`, `UnaryOpcode` gains `BitNot`, each before `Count`; `IntegerLiteral` gains a flag for a value above 9223372036854775807.  No new node, and no compound-assignment marker (the parser desugars as for `+=`) |
| AST interchange | names `bitand bitor bitxor shl shr` and `bitnot`; `(int LOC INT)` reads -9223372036854775808 to 18446744073709551615.  The format version stays the release's |
| PIR | new `and or xor shl shr` (i64 only), binary codes 58-62.  `shl` and `shr` shift by the count's low six bits, so they are defined for every count.  The language's overshift rule is the lowering's: a guard `ucmp lt %n, 64` and a `select` with 0, left out when the count is known to be below 64 |
| Narrow types | `& \| ^ >>` of zero-extended values are zero-extended; only `<<` on `u8`, `u16` and `u32` ends in `mask N`; `~a` is `xor a, 255` (`u8`), `xor a, 65535` (`u16`), `xor a, -1` then `mask 32` (`u32`), `xor a, -1` (`unsigned`) |
| Backends | the five map 1:1 onto `arith.andi ori xori shli shrui`, the count of a shift masked with `& 63`.  LLVM never sees a shift by 64 or more (no poison); C never shifts by 64 or more (no undefined behaviour) |
| Versions | no format, ABI or plugin-API version changes: every format carries the release's version, and 0.3.0 is unreleased |
| Not in this change | signed bitwise operators, arithmetic shifts, `_` in decimal and float literals, octal literals, literal suffixes, bit-counting and rotate methods, unsigned array indexes, parsing `"0xFF"` at run time, the standard library's use of the operators |

## What changes, by layer

| Layer | Change |
|-------|--------|
| Lexer (`src/Frontends/RecursiveDescent/Lexer.{h,cpp}`) | nine tokens: `&` `\|` `^` `~` `<<` `&=` `\|=` `^=` `<<=`.  `0x`/`0b` literals with `_`; integer literals in a `uint64_t` up to 2^64-1; lexer errors for malformed literals.  `>` unchanged |
| Parser (`Parser.cpp`) | one operator classifier for every binary operator (it reads `>>` as two adjacent `>`); precedence 4/5/6; non-associative shifts; `~` prefix; the five compound assignments; a literal above 9223372036854775807 builds a flagged `IntegerLiteral` |
| AST (`include/AST.h`, `src/AST/AST.cpp`) | six opcodes; `IntegerLiteral::isAboveInt64Max()`, copied by the cloner; the unsigned types register the operators (`BuiltinType::initOps`), which is the whole unsigned-only rule |
| AST interchange (`src/ASTInterchange`) | the six names, the reader's precedence table and nesting count, the 64-bit `(int ...)` |
| Sema (`src/Sema/Sema.cpp`) | shift typing; result types; `~literal`; the hints; the overshift warning; wide literals checked by context |
| Lowering (`src/Lowering/LoweringExpr.cpp`) | the five operators and `~` into PIR, with the shift guard and the folds |
| PIR (`include/paykan/pir`, `src/PIR`) | five opcodes: text, binary codes, verifier rules |
| MLIR backends (`src/Backends/MLIR`) | five dialect ops and their import; the shared lowering to `arith`; a C pattern that prints a masked shift without upstream's second guard |
| Runtime | none |
| Formatter (`scripts/pkfmt.py`) | the new spellings in its lexer and its break priorities |

## Syntax

### Operators

| Form | Meaning | Operands | Result |
|------|---------|----------|--------|
| `a & b` | bitwise and | one unsigned type T | T |
| `a \| b` | bitwise or | one unsigned type T | T |
| `a ^ b` | exclusive or | one unsigned type T | T |
| `~a` | complement within T's width | an unsigned T | T |
| `a << n` | shift left; bits shifted out are lost | `a`: an unsigned T; `n`: any unsigned type | T |
| `a >> n` | logical shift right, zeros shifted in | `a`: an unsigned T; `n`: any unsigned type | T |
| `t &= v`, `t \|= v`, `t ^= v`, `t <<= v`, `t >>= v` | `t = t op v` | as the operator | |

The complement is `~`; `!` stays the `bool` negation.  A compound
assignment is the parser's desugaring that `+=` already uses
(Parser.cpp, `parseExprOrAssignStatement`): `t op= v` becomes the
assignment of `t op v` to `t`, the first `t` a copy of the target's
expression.  Its target is evaluated twice, as for `+=`.

### Tokens

| Spelling | `Tok` | `--dump-tokens` |
|----------|-------|-----------------|
| `&` | `Amp` | `AMP` |
| `\|` | `Pipe` | `PIPE` |
| `^` | `Caret` | `CARET` |
| `~` | `Tilde` | `TILDE` |
| `<<` | `LessLess` | `LESS_LESS` |
| `&=` | `AmpAssign` | `AMP_ASSIGN` |
| `\|=` | `PipeAssign` | `PIPE_ASSIGN` |
| `^=` | `CaretAssign` | `CARET_ASSIGN` |
| `<<=` | `LessLessAssign` | `LESS_LESS_ASSIGN` |

The lexer takes the longest match: `&&` `&=` `&`; `||` `|=` `|`; `^=` `^`;
`~`; `<<=` `<<` `<=` `<`.  `>` lexes as today, `>=` or `>`, so `>>` is two
`GREATER` tokens and `>>=` is `GREATER GREATER_EQ`.  A lone `&` or `|` is no
longer `invalid character`, nor are `^` and `~`.

No program that is valid today changes meaning.  `& | ^ ~` alone were
invalid characters; `1 << 2` was `unexpected '<'`, since no expression or
type starts with `<`; `a >> b` was `a > (> b)`, an error; `x >>= 1` was an
error.

### `>>`, `>>=` and type arguments

Nested type arguments close with two `>` tokens: `Box<Box<int>>`,
`id<Box<Box<int>>>(a)`, `fn best<T: Cmp<T>>`, `Option<Box<int>>::Nothing`.
Seven places close a type argument or parameter list (the class, struct,
enum, trait and function parameter lists, `parsePrimaryType`, and the
speculative `tryGenericCallArgs`), and the generic-call speculation rewinds
the token position, while the lexical error lists hold token indices.
Splitting a `>>` token in the buffer would be unsafe there.  So the rule
is:

- **`>>` is a `>` directly followed by a `>`**: the second token starts at
  the first one's offset plus one, with no space, newline or comment
  between them.
- **`>>=` is a `>` directly followed by a `>=`.**  It is a statement's
  compound assignment only; an expression ends before it.
- **Type argument lists take single `>` tokens**, as today, before any
  expression parser sees them.

The parser classifies the operator at its position once (`OpInfo
binaryOperatorAt()`: precedence, opcode, token count, whether it is a
comparison or a shift, or whether it ends the expression).  The binary
operator loop, the comparison-chain check, the `>>=` stop and the error
reporter all use it, so the first `>` of a `>>` is never taken for a
comparison.

| Source | Parse |
|--------|-------|
| `Box<Box<int>>`, `id<Box<Box<int>>>(a)`, `fn best<T: Cmp<T>>`, `x: List<List<u8>> = l`, `Option<Box<int>>::Nothing` | as today |
| `id<Box<u8>>(x) >> 2` | a generic call, then a shift |
| `a < b >> c` | `a < (b >> c)`: the speculation finds `>` where it needs `(` or `::`, and rewinds |
| `f<a>>(b)`, `a<b>>c` | `f < (a >> b)`, a comparison Sema types (today a syntax error) |
| `a >> b > c`, `a > b >> c` | `(a >> b) > c`, `a > (b >> c)` |
| `x >>= 1;` | `x = x >> 1` |
| `a < b >>= c;` | `left-hand side of '>>=' must be an identifier, field access, or subscript` |
| `a > > b`, `a >` newline `> b` | `unexpected '>'; expected an expression ('>>' is written without a space)` |
| `x > >= 1;` | `unexpected '>='; expected an expression ('>>=' is written without a space)` |
| `a >>> b` | `unexpected '>'; expected an expression (there is no '>>>': '>>' of an unsigned value shifts in zeros)` |
| `f(x >>= 1)` | `unexpected '>>='; expected ')' to close the argument list` |
| `a: Box<Box<int>>= v` | as today: `unexpected '>='; expected '>' to close the type argument list` |

The space hints are added where an expression was expected and the
current `>` or `>=` follows a `>` with something between them.  The error
reporter names a `>` directly followed by `>` or `>=` as `'>>'` or `'>>='`.

### Grammar

```
relational     ::= additive ( relOp additive )?
additive       ::= multiplicative ( ( "+" | "-" | "|" | "^" ) multiplicative )*
multiplicative ::= shift ( ( "*" | "/" | "%" | "&" ) shift )*
shift          ::= unary ( shiftOp unary )?          -- does not associate
shiftOp        ::= "<<" | ">" ">"                     -- the two '>' adjacent
unary          ::= ( "!" | "-" | "~" ) unary | postfix
assignOp       ::= "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<="
                 | ">" ">="                           -- the two adjacent: ">>="
```

## Precedence

Highest first:

| Level | Operators | Associativity | Parser and reader precedence |
|-------|-----------|---------------|------------------------------|
| 1 | postfix: `.method()`, `.field`, `f()`, `[i]`, `.0` | left | |
| 2 | prefix `!`, `-`, `~` | right | |
| 3 | `<<`, `>>` | **none** | 6 |
| 4 | `*`, `/`, `%`, `&` | left | 5 |
| 5 | `+`, `-`, `\|`, `^` | left | 4 |
| 6 | `<`, `>`, `<=`, `>=`, `==`, `!=` | **none** | 3 |
| 7 | `&&` | left | 2 |
| 8 | `\|\|` | left | 1 |
| 9 | `if ... then ... else` | right | |

Shifts do not associate, as Swift's `BitwiseShiftPrecedence` declares no
associativity: `a << b << c` and `a >> b << c` are `shift operators cannot
be chained; parenthesise the shift`, the mechanism of the comparisons'
`relational operators cannot be chained`.  `(a << b) << c` is fine.  Making
shifts left-associative later breaks no program.

The AST interchange reader uses the same numbers.  Its nesting count treats
a shift as the left operand of a shift as parenthesised, as it treats a
comparison under a comparison, so `(a << b) << c` is one level deeper on
every frontend ([`../grammar.md`](../grammar.md) section 9).

Swift's grouping differs from C's.  All of these type-check on unsigned
operands, so the reference lists them:

| Source | Paykan (Swift) | C |
|--------|----------------|---|
| `a + b << 2` | `a + (b << 2)` | `(a + b) << 2` |
| `1 << n - 1` | `(1 << n) - 1` | `1 << (n - 1)` |
| `a & b * c` | `(a & b) * c` | `a & (b * c)` |
| `a + b & c` | `a + (b & c)` | `(a + b) & c` |
| `a \| b + c` | `(a \| b) + c` | `a \| (b + c)` |
| `a \| b ^ c` | `(a \| b) ^ c` | `a \| (b ^ c)` |
| `x & m == 0` | `(x & m) == 0` | `x & (m == 0)` |

The last is C's trap; here it reads the intended way.

## Integer literals

```
INT          ::= decimal | hexadecimal | binary
decimal      ::= [0-9]+
hexadecimal  ::= "0x" hexDigit ( "_"? hexDigit )*
binary       ::= "0b" [01] ( "_"? [01] )*
hexDigit     ::= [0-9a-fA-F]
```

- **Prefixes are lower case** (`0x`, `0b`), as in Swift and Rust; a
  hexadecimal digit is either case.  There are no octal literals; a
  leading zero in a decimal is still decimal (`017` is 17).
- **`_` stands between two digits** of a hexadecimal or binary literal, one
  at a time.  Decimal and float literals take none yet: long bit patterns
  are where separators matter, and a decimal `_` raises float and exponent
  questions.  Allowing it later is compatible, since `1_000` is an error
  today and stays one, with a message that says so.
- **A hexadecimal or binary literal never takes a `.`**: `0x10..0x20` is a
  range, `0xFF.toString()` a method call on 255 (the decimal
  `255.toString()` still lexes the float `255.`), and `0x1e5` is 485, never
  a float.  A tuple index after a `.` is digits only, as today, so `t.0x1`
  stays an error.
- **After `0x` or `0b` the lexer takes the whole run** of letters, digits
  and `_` and checks it, so `0xFG` is one error and not `0xF` then `G`.  A
  decimal literal does not take a following letter run: `for i in 0..10step 2`
  and `if c then 1else 2` are valid today and stay so.
- **The value** is accumulated in a `uint64_t`, in every base: 0 to
  18446744073709551615.  `Token::IntValue` becomes a `uint64_t`, and
  `Token::Text` keeps the spelling for `--dump-tokens` (`INT 0xFF_FF`).
  The scanner never sees a sign, so the special case that took INT64_MIN's
  magnitude right after a `-` (Lexer.cpp:574-591) is gone: 2^63 is now an
  ordinary value.

Lexer errors.  Each is one error; the lexer goes on after the literal, whose
token has the value 0.  Every input here is an error today too.

| Input | Error |
|-------|-------|
| `0x`, `0x.` | `expected a hexadecimal digit after '0x'` |
| `0b` | `expected a binary digit after '0b'` |
| `0xFG` | `invalid digit 'G' in hexadecimal literal '0xFG'` |
| `0b102` | `invalid digit '2' in binary literal '0b102'` |
| `0x_FF`, `0xFF_`, `0x1__0` | `a '_' in a number must stand between two digits: '0x_FF'` |
| `0XFF`, `0B1` | `the prefix of a hexadecimal literal is '0x', in lower case: '0XFF'` (`... a binary literal is '0b' ...`) |
| `0o17` | `there are no octal literals: '0o17' (write the value in hexadecimal, as '0x...', or in decimal)` |
| `0x1.5`, `0b1.0` | `a hexadecimal literal has no fraction: '0x1.5'` (`a binary literal ...`) |
| `1_000`, `3.141_592`, `1e1_0`, `1_` | `'_' separates digits only in hexadecimal and binary literals: '1_000'` |
| `255u8`, `1f32`, `2.5f32` (a number directly followed by a numeric type's name) | `there are no literal suffixes: '255u8' (a literal takes its type from the context, as in 'x: u8 = 255')` |
| 2^64 or more, any base | `integer is out of range: 18446744073709551616 (an integer literal is at most 18446744073709551615)` |

### Where a literal's range is checked

1. **The lexer**: only 2^64 and above, and the malformed forms.
2. **The parser**, where the grammar needs an `int64` and gets more:
   - a fixed-size array's length `T[n]` and a repeat count `[v; n]` above
     9223372036854775807: `integer is out of range: <text>`, as today;
   - a negative match pattern `-INT` whose magnitude is above 2^63:
     `integer is out of range: -9223372036854775809`, at the `-`.  A
     magnitude of exactly 2^63 is the pattern INT64_MIN, as today.
3. **Sema**, everything else, by the context's type (below), on every
   frontend alike.

`IntegerLiteral` keeps its `int64_t` value, the bits, and gains
`AboveInt64Max`: set, the literal is `getUnsignedValue()`, 2^63 to 2^64-1;
clear, it is `getValue()`, signed (a negative one comes only from a match
pattern or an interchange `(int -5)`).  The parser and the interchange
reader (a TMPL template's body too) set it, and the cloner copies it, for a
generic instantiation and a compound assignment's copy of its target; the
printer and the interchange writer print a flagged value unsigned.

### Typing a literal

A literal's value is checked against the type its context gives it, the
rule of [`sized-types.md`](sized-types.md).  The ranges are computed from
the type's width as unsigned bounds, which removes the `unsigned` special
case in `integerRangeText` (it printed `18446744073709551615` while the
check stopped at 9223372036854775807).

- **An unsigned or `u64` slot** takes 0 to 18446744073709551615:
  `u: unsigned = 0xFFFF_FFFF_FFFF_FFFF`, `x == 0x8000_0000_0000_0000`, a
  `match` arm, an argument, a field.
- **A float slot** (`float`, `f32`, `f16`) takes a literal above
  9223372036854775807 as its value, rounded once, as it takes every other
  integer literal: `x: f32 = 18446744073709551615` is `1.84467e+19`.
- **A narrower slot** gives the existing error, with the value in decimal:
  `x: u8 = 0x100` is `the literal 256 does not fit in 'u8' (0 to 255)`, and
  `y: i8 = 0xFF` is `the literal 255 does not fit in 'i8' (-128 to 127)`.
  A hexadecimal literal is a value: the reinterpretation is spelled
  `i8(u8(0xFF))`, which is -1.
- **An `int` slot, or no context**, takes a literal above
  9223372036854775807 as an error: `the literal 18446744073709551615 does
  not fit in 'int' (-9223372036854775808 to 9223372036854775807; a literal
  above 9223372036854775807 needs an unsigned context, as in 'u: unsigned =
  18446744073709551615')`.
- **The argument of an inferred conversion**, `unsigned(...)`, `u8(...)`,
  `float(...)`, is an `unsigned` when it is a literal above
  9223372036854775807: `unsigned(0xFFFF_FFFF_FFFF_FFFF)` is that value, and
  `u8(0xFFFF_FFFF_FFFF_FFFF)` is 255 (a conversion keeps the low bits).
  A smaller literal argument is an `int`, as today (`u8(300)` is 44).
- **Under a `-`** the literal is negated first: `x: i8 = -0x80` is -128, and
  `-0x8000_0000_0000_0000` in a `u8` slot is `the literal
  -9223372036854775808 does not fit in 'u8' (0 to 255)`.  With no sized
  context, `-9223372036854775808` stays an error, as today, now Sema's:
  `the literal 9223372036854775808 does not fit in 'int'
  (-9223372036854775808 to 9223372036854775807; write the most negative
  'int' as '-9223372036854775807 - 1')`.  A wide literal therefore never
  reaches `ForStmt::constantStep` or `Sema::checkFixedIndex`, which negate a
  value they take for at most 9223372036854775807: a step and an index are
  `int`s.

A `match` on an `unsigned` takes arms up to `0xFFFF_FFFF_FFFF_FFFF`,
compared by their bits; `0x8000_0000_0000_0000` and
`0x7FFF_FFFF_FFFF_FFFF` are INT64_MIN's and INT64_MAX's bits, which the
switch lowering already takes out of its `scf.index_switch` (the dense
set's empty and tombstone keys, [`mlir-backend.md`](mlir-backend.md)).
`~0` and other expressions are not patterns.

Parsing a `Str` stays decimal: `"0xFF".toUnsigned()` is `None` while `0xFF`
is a literal (`"0x10".toFloat()` is 16, as C's `strtod` reads it).

## Typing

`BuiltinType::initOps` registers `BitAnd`, `BitOr`, `BitXor`, `Shl`, `Shr`
and the unary `BitNot` in the case the four unsigned kinds share.  Sema's
`hasBinaryOp` and `hasUnaryOp` checks then make the operators unsigned-only
with no rule of their own.

### `&`, `|`, `^`

The existing path of `+`: a literal operand is typed from the other
operand, both operands have one type (`operator '&' is not defined for
types 'u8' and 'u32' (the operands must have one type: convert one with
'u8(...)' or 'u32(...)')`), and the result is that type.  `x & 0xF0` with
`x: u8` is a `u8`; `x & 0x100` is `the literal 256 does not fit in 'u8' (0
to 255)`.

### Shifts

1. The value is typed first, with no expectation from the count: a literal
   value is `int`.
2. The count is typed next: a numeric literal count is an `unsigned` when
   the value's type is unsigned (`x << 3`, `x << ~0`), otherwise it is typed
   on its own.
3. Both must have an unsigned type; the one-type check is not applied.  The
   result is the value's type.

A count of any unsigned width is read from its zero-extended slot, so it
costs nothing and never wraps.  With the one-type rule, `x << u8(n)` would
wrap a count of 256 to 0 and give `x`, where the overshift rule promises 0;
Swift, Rust and C take a count of another type too.  A literal value takes
no type from the count, so `m = 1 << b` with `b: u8` is an error that asks
for the value's type, never a silent `u8` that is 0 for `b >= 8`.  `x <<
300` with `x: u8` is valid and 0, with the warning below; `x << -1` is `the
literal -1 does not fit in 'unsigned' (0 to 18446744073709551615)`.

### `~`

`~a` has `a`'s type.  `~` directly over an integer literal is a numeric
literal for the context rule, as `-literal` is, but only an unsigned context
gives it a type: the inner literal is checked against that type, and the
value is the complement within its width.  So `x & ~0x0F` with `x: u8` is
`x & 240`, `y: u8 = ~0` is 255, `~0` in an `unsigned` slot is
18446744073709551615, and `~256` in a `u8` slot is `the literal 256 does
not fit in 'u8' (0 to 255)`.  With no unsigned context, `~5` is `~` of an
`int`, an error.  `-~5` and `~~5` are not literals.

### Errors

The base texts are the existing ones, `operator 'OP' is not defined for
types 'A' and 'B'` and `unary 'OP' is not defined for type 'A'`.  A bitwise
operator adds the first hint that applies:

| Operands | Hint |
|----------|------|
| a `bool` with `&`, `\|`, `^` | ` (for a logical and, write '&&')`, ` (for a logical or, write '\|\|')`, ` (for a logical exclusive or, write '!=')` |
| a shift whose value is unsigned and whose count is a signed integer or a `char` | ` (a shift count is unsigned: convert it with 'unsigned(...)')` |
| a shift whose value is a literal and whose count is unsigned | ` (the value shifted takes no type from the count: write its type, as in 'unsigned(1)')`, with the literal's value |
| two literals | ` (two literals are 'int': give one a type, as in 'unsigned(1) << 5')`, with the operator and the values |
| `&`, `\|` or `^` with one unsigned operand of type U, the other a signed integer or `char` of type S | ` (the bitwise operators take unsigned operands: convert the 'S' with 'U(...)')` |
| a signed integer or `char` operand, a shift's value too whatever its count | ` (the bitwise operators take unsigned operands: convert with 'W(...)')`, W the unsigned type of its width: `int` gives `unsigned`, `i32` `u32`, `i16` `u16`, `i8` and `char` `u8` |
| a float operand | ` (the bitwise operators take unsigned operands, not floats)`; for `^`, ` ('^' is exclusive or, not a power)` |
| anything else | none |

With `^` and numeric operands that are not unsigned, the hint ends in `;
'^' is exclusive or, not a power`.  `~` takes ` (for a logical not, write
'!')` for a `bool`, the same-width conversion for a signed integer or
`char`, and the float text for a float.  A shift's count never decides its
result's type, so a signed value with an unsigned count takes the
same-width conversion, not the count's type: `i << n` with `i: i32` and
`n: u8` names `u32(...)`, since `u8(i)` would drop the value's upper bits.

| Program | Error |
|---------|-------|
| `a & b`, both `int` | `operator '&' is not defined for types 'int' and 'int' (the bitwise operators take unsigned operands: convert with 'unsigned(...)')` |
| `x \| n`, `x: u8`, `n: int` | `operator '\|' is not defined for types 'u8' and 'int' (the bitwise operators take unsigned operands: convert the 'int' with 'u8(...)')` |
| `i ^ j`, both `i32` | `operator '^' is not defined for types 'i32' and 'i32' (the bitwise operators take unsigned operands: convert with 'u32(...)'; '^' is exclusive or, not a power)` |
| `2 ^ 8` | `operator '^' is not defined for types 'int' and 'int' (two literals are 'int': give one a type, as in 'unsigned(2) ^ 8'; '^' is exclusive or, not a power)` |
| `m: u32 = 1 << 5` | `operator '<<' is not defined for types 'int' and 'int' (two literals are 'int': give one a type, as in 'unsigned(1) << 5')` |
| `1 << n`, `n: u8` | `operator '<<' is not defined for types 'int' and 'u8' (the value shifted takes no type from the count: write its type, as in 'unsigned(1)')` |
| `x << i`, `x: u32`, `i: int` | `operator '<<' is not defined for types 'u32' and 'int' (a shift count is unsigned: convert it with 'unsigned(...)')` |
| `p & q`, both `bool` | `operator '&' is not defined for types 'bool' and 'bool' (for a logical and, write '&&')` |
| `~i`, `i: int` | `unary '~' is not defined for type 'int' (the bitwise operators take unsigned operands: convert with 'unsigned(...)')` |
| `~b`, `b: bool` | `unary '~' is not defined for type 'bool' (for a logical not, write '!')` |
| `f ^ 2.0`, `f: float` | `operator '^' is not defined for types 'float' and 'float' ('^' is exclusive or, not a power)` |

A compound assignment's error is the binary operator's, located at the
statement's start, as `+=`'s is.  An optional operand gives the existing
`match` hint.  A generic body is checked per instantiation: `fn clear<T>(x:
T, m: T) -> T => x & ~m` works with `u8` and is the `int` error with `int`.

### The overshift warning

A shift whose count is an integer literal at or above the value's width is
valid and always 0; Sema warns: `the shift count 8 is not less than the
width of 'u8' (8 bits): the result is always 0`.  A location is warned
about once, though several instantiations of a generic reach it.  The
warning goes to standard error, which the strict C check compares with the
C program's (`scripts/c_strict.py`), so the samples and the documentation's
examples shift by variables when they show an overshift.

## Semantics

A value of an unsigned type of width W (8, 16, 32 or 64) is held
zero-extended in its 8-byte slot.

| Expression | Value |
|------------|-------|
| `a & b`, `a \| b`, `a ^ b` | the bitwise operation |
| `~a` | `2^W - 1 - a` |
| `a << n` | `(a * 2^n) mod 2^W` for `n < W`; **0** for every `n >= W` |
| `a >> n` | `floor(a / 2^n)` for `n < W`; **0** for every `n >= W` |

`n` is read as an unsigned number of its own type, so a count of 2^63 or
2^64-1 is an overshift, never a negative count.  Nothing panics and nothing
checks for overflow: bits shifted out are lost, as unsigned arithmetic
wraps.  The value is evaluated before the count, as every binary operator's
left operand is.

| Type | W | 0 for counts |
|------|---|--------------|
| `u8` | 8 | 8 to 18446744073709551615 |
| `u16` | 16 | 16 to 18446744073709551615 |
| `u32` | 32 | 32 to 18446744073709551615 |
| `unsigned` | 64 | 64 to 18446744073709551615 |

### How each layer gives 0

- **Lowering, a constant count.**  At or above W the shift is the constant
  0 (the value is still evaluated); below W it is a plain `shl` or `shr`.
  Two constant operands fold.
- **Lowering, a count known to be below 64.**  A count written `e & k`
  with an integer literal `k` of 0 to 63, or `e % k` with `k` 1 to 64, is a
  plain `shl` or `shr`: masked counts and the rotate idiom stay one
  instruction.
- **Lowering, any other count.**

  ```
  %ok = ucmp lt %n, 64        ; unsigned: a count of 2^63 or more is not negative
  %s  = shl %a, %n            ; or shr
  %r  = select %ok, %s, 0
  ```

  For `u8`, `u16` and `u32`, counts W to 63 need no comparison of their
  own: `shl` followed by `mask W` clears them, and `shr` of a value below
  2^W is 0.  So one comparison with 64 covers every width.
- **PIR.**  `shl` and `shr` shift by the count's low six bits (`n mod 64`),
  which is what x86-64 and AArch64 shift instructions do; every count is
  defined, so a plugin backend that prints PIR has no undefined case.
- **The shared MLIR lowering** masks the count: `arith.shli %a,
  (arith.andi %n, 63)` (a constant count becomes `c & 63`).
- **LLVM** sees only counts 0 to 63, so `shl` and `lshr` never give poison.
  Instruction selection drops the `and 63` on x86-64 and AArch64, and the
  `select` becomes `cmov` or `csel`; LLVM -O2 removes the guard where the
  count's range is known (a loop over `0..64`).
- **C** prints `(uint64_t)a << ((uint64_t)n & 63)`, defined for every
  `n`, and the lowering's `select` as `?:`.  Upstream's EmitC conversion
  wraps every shift in `n < 64 ? ... : 0` of its own; a pattern of the C
  backend prints a shift whose count is a constant below 64 or masked with
  `& 63` without it (any other shift falls back to upstream's, which is
  correct too).

### Narrow types and masks

`& | ^` and `>>` of zero-extended operands are zero-extended, so they take no
`mask`.  `<<` can set bits at and above W, so on `u8`, `u16` and `u32` it
ends in `mask W`.  `~a` is one instruction: `xor %a, 255` for `u8` and
`xor %a, 65535` for `u16` (LLVM cannot see that `a` is zero-extended, so
`xor -1` then `mask` stays a `not` and a zero-extension); `xor %a, -1` then
`mask 32` for `u32` (x86-64's 32-bit `not` zero-extends for free); `xor %a,
-1` for `unsigned`.  A constant folds: `~u8(0x0F)` is the constant 240.

## Lowering to PIR

New PIR instructions, appended to `enum Opcode` after `Owned`, so a C++
backend plugin's existing values keep their numbers:

```
%r = and %a, %b | or | xor         ; i64, i64 -> i64: bitwise
%r = shl %a, %n                    ; i64: %a << (%n mod 64)
%r = shr %a, %n                    ; i64: %a >> (%n mod 64), zeros shifted in
```

| Opcode | Code | Name |
|--------|------|------|
| `And` | 58 | `and` |
| `Or` | 59 | `or` |
| `Xor` | 60 | `xor` |
| `Shl` | 61 | `shl` |
| `Shr` | 62 | `shr` |

Codes 63 to 255 stay reserved.  The verifier takes two `i64` operands and
gives an `i64` (`operand of and / or / xor`, `operand of shl / shr`); none
has extras, so the printer and the binary codec need no case of their own.
`not` stays `bool`-only, and `~` needs no instruction.  The passes (lend,
move at last use, RC elimination, dead code) treat any instruction on
`i64`s alike.

| Source (W the result's width) | PIR |
|-------------------------------|-----|
| `a & b`, `a \| b`, `a ^ b` | `and`, `or`, `xor`; two constants fold |
| `~a` | `xor %a, 255` / `xor %a, 65535` / `xor %a, -1` then `mask 32` / `xor %a, -1`; a constant folds |
| `a << c`, constant `c < W` | `shl %a, c`, then `mask W` when `W < 64` |
| `a >> c`, constant `c < W` | `shr %a, c` |
| `a << c`, `a >> c`, constant `c >= W` (read unsigned) | the constant 0 |
| `a << n`, `a >> n`, `n` known below 64 | `shl` / `shr`, then `mask W` after `shl` |
| any other count | `ucmp lt %n, 64`, `shl` / `shr`, `select %ok, %s, 0`, then `mask W` after `shl` |

## Formats and versions

| Format | Change | Version |
|--------|--------|---------|
| AST interchange (`--emit-ast`, a `.pkm`'s TMPL) | binary names `bitand bitor bitxor shl shr`, unary `bitnot`; `(int LOC INT)` from -9223372036854775808 to 18446744073709551615 | the release's (`kFormatVersion = kVersionNumber`, 300 for 0.3.0) |
| PIR text and binary (a `.pkm`'s CODE) | `and or xor shl shr`, codes 58-62 | the release's (`kPIRVersion`) |
| IFACE, runtime ABI, plugin API | none (the C++ enums are appended to) | unchanged |

`and`, `or` and `not` are the logical operators' names, so the bitwise ones
are prefixed.  The writer names the unary operators with a switch: a
fall-through that wrote every operator but `neg` as `not` would have turned
a template's `~` into `!`.  The reader parses `(int ...)` as an `int64`, or
a value without a sign as a `uint64`, which sets the flag; the other
integer fields (a `char`, an `array-repeat` count, a `tuple-index`) stay
`int64`.

The binary codes and names are new, so a `.pkm` or cache entry written
after this change is unreadable by an older 0.3.0-dev compiler, as any file
of another build is: a development build takes only files of its own build,
so its manifest check rejects a prebuilt file (`... build ID ...; this
paykan is development build ... and takes only files of its own build`)
and finds a cache entry stale before any section is read
([`pkm.md`](pkm.md)).  The older readers' own errors for the new names and
codes (`unknown binary operator 'bitand'`, `reserved opcode`) are therefore
never printed for such a file.  The readers still take every older name and
code.  `IntegerLiteral`'s layout changes within the dev cycle.

## Not in this change

- **Signed operands.**  `i32 & i32` is an error that names `u32(...)`; the
  owner chose unsigned-only.  An arithmetic right shift would be a new PIR
  instruction (`shr` is the logical one).
- **`_` in decimal and float literals**, octal literals and literal
  suffixes: each is a dedicated error today, so adding them breaks nothing.
- **A literal-only expression taking its context's type.**  `m: u32 = 1 <<
  5` is an error with a hint; `u32(1) << 5` is the spelling.
- **INT64_MIN as an expression literal**: it is still
  `-9223372036854775807 - 1`.
- **Methods** such as `popCount`, `leadingZeros`, `rotateLeft`.  The
  masked rotate idiom `(x << (k & 63)) | (x >> ((0 - k) & 63))` compiles to
  one `rol` already.
- **Unsigned array indexes**: an index is an `int`, so `t[int(x & 0xF)]`
  for a `u8`, `u16` or `u32` (`int(u32(x & 0xF))` for an `unsigned`).
- **Runtime parsing of `0x` texts.**
- **The standard library**, after the sweep's chunk E: `collections::_bucket`
  as `unsigned(h) & (n - 1)` (the bucket count is a power of two), and
  SplitMix64 in Paykan with hexadecimal constants.

## Open questions

1. **Should shifts be left-associative, as in C?**  No: Swift's shifts do
   not associate, and an error now can be relaxed later without breaking
   programs.
2. **Should a shift count need the value's type?**  No: a count of any
   unsigned type keeps the overshift rule true for every count a program
   holds; requiring one type wraps the count.
3. **Should an expression made only of literals take its context's type?**
   No: only a literal adapts, the documented rule; the hint names the
   spelling.  It could be added later without breaking programs.
4. **Should a literal count at or above the width be an error?**  No: the
   owner's rule makes it 0, so it is a warning.
5. **`_` in decimal literals?**  Not yet; the error says so, and allowing
   it later is compatible.
6. **Should `-9223372036854775808` be an expression?**  Not yet: accepting
   it needs a negation that is safe for INT64_MIN in `ForStmt::constantStep`
   and `Sema::checkFixedIndex`.

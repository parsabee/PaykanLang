# Sized primitive types

The design behind the sized integer and floating-point types in
`docs/language/02-basics.md`: `i8`, `i16`, `i32`, `i64`, `u8`, `u16`,
`u32`, `u64`, `unsigned`, `f16`, `f32` and `f64`, next to the `int` and
`float` the language had.  The decisions keep every value in the 8-byte slot
`int` and `float` already use, so no layout, runtime structure or `.pkm`
record changes, and add to PIR only the few instructions the backends need
for wrapping, unsigned arithmetic and rounding.  The open questions at the
end each have the default this implementation takes.

## Summary of the decisions

| Topic | Decision |
|-------|----------|
| Names | `i8 i16 i32 i64` signed, `u8 u16 u32 u64` unsigned, `f16 f32 f64` floating-point; `int` is `i64`, `float` is `f64` and `unsigned` is `u64`: the same types, spelled either way (`int` and `i64` are one type, as are `unsigned` and `u64`, `float` and `f64`), printed as `int`, `unsigned` and `float` |
| Storage | every value takes 8 bytes, 8-byte aligned, like an `int`: a narrower integer is held sign-extended (signed) or zero-extended (unsigned) in an `i64` slot, an `f32` or `f16` as the `f64` it exactly is; `bool` and `char` keep their byte |
| Integer arithmetic | `+ - * / %` on one type give that type; the result wraps to the width (two's complement, the low bits), as `int` wraps today; `/` and `%` by zero panic; `/` and `%` of unsigned types are unsigned; unary `-` is not defined on unsigned types |
| Float arithmetic | `+ - * /` on `f32` or `f16` are computed in `f64` and rounded to the type's precision after each operation (exact for these operations, as `f64` has enough bits); comparisons are IEEE as for `float` |
| Mixing | an operation needs both operands of one type; the only promotion is the existing `int` with `float` (to `float`); anything else is spelled with a conversion (`operator '+' is not defined for types 'i32' and 'int' (the operands must have one type: convert one with 'i32(...)' or 'int(...)')`) |
| Literals | an integer literal (and `-literal`) takes the integer type the context expects when it fits that type (`x: u8 = 255`, `x + 1`, `x == 0`, a `match` arm, an argument, a field, an element, a `T?` slot), an error when it does not (`the literal 256 does not fit in 'u8' (0 to 255)`); a float literal, or an integer literal, takes `f32` or `f16` the same way (an error when its magnitude rounds to infinity); only the literal itself adapts (`100 + 100` is an `int` expression); without a context an integer literal is `int` and a float literal `float` |
| Conversions | `i8(x)` … `f16(x)`, `unsigned(x)` and `int(x)`, `float(x)` from any numeric type: integer to integer wraps to the width; float to integer truncates toward zero and panics outside the target's range (as `int(x)` does); integer to float rounds to nearest; `Str(x)` prints; `s.toI8()` … parse a `Str` to `i8?` …, `None` when the text is not a number of that type |
| Methods and traits | `toString()` and `hash()` on every numeric type; every numeric type is `Show`, `Hashable` and `ValueType`, as `int` and `float` are |
| Optionals and match | `i32?`, `u8?`, `f32?` are value optionals like `int?`; `match` on a sized integer compares with literals of its type |
| Runtime | one new type kind, `PAYKAN_KIND_UINT`, so a `u64` in a struct, tuple or array prints unsigned; `f16` rounding and the unsigned `toString`/parse are runtime functions; the ABI version stays |
| PIR | new instructions `wrap N`, `mask N`, `udiv`, `urem`, `ucmp`, `utof`, `ftou` and `fround32`; `f16` rounding is a runtime call; the format version stays (caches are cleared) |
| Not in this change | bit operations and shifts (the language has none), integer literal suffixes (`1u8`), hexadecimal literals, `char` conversions to the new types (`u8(int(c))`), implicit widening (`i32` to `int`), overflow checks |

## Types and spelling

`int`/`i64`, `unsigned`/`u64` and `float`/`f64` are three types with two
names each; the parser resolves the type name to the one `BuiltinType`, and
`typeName` prints `int`, `unsigned` and `float`.  The other nine are new
`BuiltinType` kinds.  All twelve are value types: copied on assignment, held
in place in structs, tuples and arrays, compared by value.

## Representation

Every numeric value occupies an 8-byte slot (`i64` or `f64` in PIR), so a
struct, tuple or array holding sized types has the layout it would have with
`int` and `float`, and an `i32?` is an `opt_i64`.  The slot holds the value
in *canonical form*: a signed integer sign-extended to 64 bits, an unsigned
one zero-extended, a float as the `f64` equal to its `f32` or `f16` value.
Reading a value never adjusts it; every operation that could produce a
non-canonical result (an arithmetic operation, a conversion) is followed by
the instruction that restores the form: `wrap 8` sign-extends the low 8 bits,
`mask 16` zero-extends the low 16, `fround32` rounds an `f64` to the nearest
`f32`, and a call to `Paykan_round_f16` rounds to the nearest `f16`
(ties-to-even, subnormals and overflow to infinity included).

`u64` is the one type whose canonical form is not its `int` reading: its
division, remainder, ordering, conversions to and from `f64` and printing
are unsigned (`udiv`, `urem`, `ucmp`, `utof`, `ftou`, `PaykanString_from_uint`,
`PaykanString_parse_uint`); addition, subtraction, multiplication, equality
and hashing are the same bits as `int`'s.  The narrower unsigned types are
non-negative in canonical form, so their signed division and ordering are
right.

An `f32` literal is rounded by the compiler; an `f16` literal lowers to the
`Paykan_round_f16` call on its `f64` value, so the compiler and the runtime
share one rounding.  An integer converts to `f32` or `f16` through `f64`
(`itof` then the rounding), which is the correctly rounded result for every
integer an `f32` or `f16` can distinguish.  The `INT64_MIN / -1` guard of
`int` division is not emitted for a narrower type, whose quotient wraps
(`i8(-128) / i8(-1)` is `-128`).  An integer literal is at most
`9223372036854775807`, so the upper half of `unsigned` is reached through a
conversion or a parse (`unsigned(-1)`, `"18446744073709551615".toUnsigned()`).

## Typing

A binary operation on two numeric operands requires one type for both, with
the existing exception that `int` with `float` is computed in `float`.  An
integer literal (and `-literal`) is typed by its context: the other operand
of a binary operation, the declared type of a variable, a parameter, a field,
an element or a result, the subject of a `match`.  When the context's type is
an integer type and the literal fits it, the literal has that type; when it
does not fit, the literal is an error; when the context is not an integer
type, the literal is `int` and the usual checks apply.  A float literal is
`f32` or `f16` in such a context, else `float`.  Nothing else converts
implicitly: `x + y` with `x: i32` and `y: int` is `operator '+' is not
defined for types 'i32' and 'int'`, written `x + i32(y)` or `int(x) + y`.

## Conversions

The conversion call `T(x)` takes every numeric type for every numeric `T`,
with `Str(x)` as for `int` and `float`; a `Str` is parsed with its method
`s.toT()` (giving `T?`), not converted.  Integer to integer keeps the low bits of the two's-complement
value (`u8(300)` is `44`, `i8(200)` is `-56`, `unsigned(-1)` is
`18446744073709551615`, `int(unsigned(-1))` is `-1`); float to integer
truncates toward zero and panics when the value is outside the target's
range, `NaN` included, as `int(x)` does; integer to float rounds to nearest,
and `f16(70000.0)` is infinity.  `char` converts to and from `int` only, as
before.

## Printing and parsing

`toString()` and `Str(x)` print signed integers as `int` does, `u64` with
`%llu`, and every float with the `%g` form `float` uses.  Str's methods
parse, each into the optional of its type (`s.toI8()` … `s.toF32()`,
docs/language/04-strings-and-conversions.md, "Parsing a string"; `i8(s)` …
are errors that name them).  A signed type's parse reads a 64-bit number and
refuses one outside the type's range.  An unsigned type's reads an unsigned
64-bit number, which has no sign (`"+1"` and `"-0"` are refused, as
`unsigned` always refused them), and refuses one outside the type's range.
An `f32` or `f16` parse is the `f64` nearest the text rounded to the type,
the value the literal of that text has, and refuses a finite text that
rounds to infinity, whose literal is an error; a text below the type's
range is 0 or a subnormal, as its literal is.

## Open questions

1. **Should `i32` widen to `int` implicitly?**  No: one rule, explicit
   conversions, keeps the type of every expression visible; the literal rule
   covers the common cases.  Widening could be added later without breaking
   programs.
2. **Should narrow integer arithmetic check for overflow?**  No: `int`
   wraps today and the backends compute in 64 bits; a checked variant would
   be a different operator or a runtime mode.
3. **Should `f32` and `f16` print with their own precision?**  No: `%g`
   prints the value the type holds, with the six significant digits `float`
   prints; a rounded `f32` shows the same text as the `f64` it is.
4. **Literal suffixes (`1u8`) and hexadecimal literals?**  Not yet; the
   context rule makes suffixes rare, and hexadecimal literals are a lexer
   change on their own.
5. **Should `u8` convert from `char`?**  Not yet: `char` keeps its single
   conversion to `int`; `u8(int(c))` spells a byte.
6. **Bitwise operators?**  The language has none; they are the natural next
   step for the unsigned types and a separate change.

# `::math`

The C library's math functions on `float`, imported with `import ::math;`. Each function
but the constants and the classifications is a [native
function](../language/03-functions.md#native-functions) bound to the C99 `<math.h>`
function named in the tables below, so it computes exactly what C's does: a domain error
(`sqrt(-1.0)`, `log(-1.0)`) is NaN, a result out of range is an infinity, and nothing
panics or reports an error. The trigonometric functions work in radians.

```pkn
import ::io;

import ::math;

fn main() -> int {
  r = 2.0;
  io::println("area " + Str(math::pi() * math::pow(r, 2.0)));
  io::println("hypot " + Str(math::hypot(3.0, 4.0)) + ", 45 degrees is "
          + Str(math::atan2(1.0, 1.0)) + " radians");
  io::println(Str(math::floor(-2.5)) + " " + Str(math::round(-2.5)) + " "
          + Str(math::gamma(5.0)));
  io::println(Str(math::isNaN(math::sqrt(-1.0))) + " " + Str(math::log(0.0)));
  return 0;
}
```

Output:

```
area 12.5664
hypot 5, 45 degrees is 0.785398 radians
-3 -3 24
True -inf
```

Every function takes and returns `float`. An `int` argument converts to `float` on its own
(`math::sqrt(4)` is `2`); a sized float (`f32`, `f16`) converts with `float(x)` first
(`argument 1 of 'math::sqrt' has type 'f32', expected 'float'`). `::math` has no generic
declarations, so it is built once into the compilation cache (see
[Where the Library Is](index.md#where-the-library-is)).

---

## Constants

The language has no global constants, so these are functions.

| Function | Value |
|----------|-------|
| `fn pi() -> float` | π, `3.141592653589793` |
| `fn e() -> float` | e, `2.718281828459045` |
| `fn inf() -> float` | positive infinity, `1.0 / 0.0` (prints `inf`) |
| `fn nan() -> float` | a quiet NaN, `0.0 / 0.0` (prints `nan`) |

## Classification

| Function | Is `True` when |
|----------|----------------|
| `fn isNaN(x: float) -> bool` | `x` is NaN (a NaN is the one value that is not `==` itself) |
| `fn isInf(x: float) -> bool` | `x` is positive or negative infinity |
| `fn isFinite(x: float) -> bool` | `x` is neither NaN nor an infinity |

## Powers, roots, exponentials and logarithms

| Function | C | Returns |
|----------|---|---------|
| `fn sqrt(x: float) -> float` | `sqrt` | the square root; NaN for `x < 0` |
| `fn cbrt(x: float) -> float` | `cbrt` | the cube root (of a negative `x` too) |
| `fn pow(x: float, y: float) -> float` | `pow` | `x` to the power `y` |
| `fn hypot(x: float, y: float) -> float` | `hypot` | `sqrt(x*x + y*y)`, without overflow in between |
| `fn exp(x: float) -> float` | `exp` | e to the power `x` |
| `fn exp2(x: float) -> float` | `exp2` | 2 to the power `x` |
| `fn expm1(x: float) -> float` | `expm1` | `exp(x) - 1`, accurate near 0 |
| `fn log(x: float) -> float` | `log` | the natural logarithm; `-inf` for 0, NaN for `x < 0` |
| `fn log2(x: float) -> float` | `log2` | the base-2 logarithm |
| `fn log10(x: float) -> float` | `log10` | the base-10 logarithm |
| `fn log1p(x: float) -> float` | `log1p` | `log(1 + x)`, accurate near 0 |

## Trigonometric and hyperbolic functions

All in radians.

| Function | C | Returns |
|----------|---|---------|
| `fn sin(x: float) -> float` | `sin` | the sine |
| `fn cos(x: float) -> float` | `cos` | the cosine |
| `fn tan(x: float) -> float` | `tan` | the tangent |
| `fn asin(x: float) -> float` | `asin` | the arc sine, in `[-π/2, π/2]`; NaN outside `[-1, 1]` |
| `fn acos(x: float) -> float` | `acos` | the arc cosine, in `[0, π]`; NaN outside `[-1, 1]` |
| `fn atan(x: float) -> float` | `atan` | the arc tangent, in `[-π/2, π/2]` |
| `fn atan2(y: float, x: float) -> float` | `atan2` | the angle of the point `(x, y)`, in `[-π, π]`: note `y` first |
| `fn sinh(x: float) -> float` | `sinh` | the hyperbolic sine |
| `fn cosh(x: float) -> float` | `cosh` | the hyperbolic cosine |
| `fn tanh(x: float) -> float` | `tanh` | the hyperbolic tangent |
| `fn asinh(x: float) -> float` | `asinh` | the inverse hyperbolic sine |
| `fn acosh(x: float) -> float` | `acosh` | the inverse hyperbolic cosine; NaN for `x < 1` |
| `fn atanh(x: float) -> float` | `atanh` | the inverse hyperbolic tangent; NaN outside `[-1, 1]` |

## Rounding and remainders

| Function | C | Returns |
|----------|---|---------|
| `fn floor(x: float) -> float` | `floor` | the largest whole `float` not above `x` |
| `fn ceil(x: float) -> float` | `ceil` | the smallest whole `float` not below `x` |
| `fn trunc(x: float) -> float` | `trunc` | `x` rounded toward zero |
| `fn round(x: float) -> float` | `round` | `x` rounded to the nearest whole `float`, halves away from zero (`round(-2.5)` is `-3`) |
| `fn fmod(x: float, y: float) -> float` | `fmod` | the remainder of `x / y` with the quotient truncated: the sign of `x`, like `%` (`fmod(-7.0, 3.0)` is `-1`) |
| `fn remainder(x: float, y: float) -> float` | `remainder` | the remainder with the quotient rounded to the nearest, halves to even (`remainder(7.0, 4.0)` is `-1`) |

The rounding functions return a `float`; `int(x)` converts one to an `int` (see
[Conversions](../language/04-strings-and-conversions.md#conversions)).

## Magnitude and sign

| Function | C | Returns |
|----------|---|---------|
| `fn abs(x: float) -> float` | `fabs` | the absolute value |
| `fn min(x: float, y: float) -> float` | `fmin` | the smaller; when one operand is NaN, the other |
| `fn max(x: float, y: float) -> float` | `fmax` | the larger; when one operand is NaN, the other |
| `fn copysign(x: float, y: float) -> float` | `copysign` | the magnitude of `x` with the sign of `y` |

## Special functions

| Function | C | Returns |
|----------|---|---------|
| `fn erf(x: float) -> float` | `erf` | the error function |
| `fn erfc(x: float) -> float` | `erfc` | `1 - erf(x)`, accurate for large `x` |
| `fn gamma(x: float) -> float` | `tgamma` | the gamma function (`gamma(5.0)` is `24`) |
| `fn lgamma(x: float) -> float` | `lgamma` | the natural logarithm of the absolute value of `gamma(x)` |

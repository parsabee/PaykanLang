# Basics

This chapter covers the everyday core of PaykanLang: variables, the built-in types,
operators, control flow and functions. By the end of it you can write small programs, and
we write the first functions of `tasks`, the to-do list manager this manual builds.

## Variables

A variable is declared with a name, a colon, a type and an initial value:

```pkn
fn main() -> int {
  count: int = 3;
  price: float = 2.5;
  ready: bool = True;
  name: Str = "Ada";
  println(name + " ordered " + Str(count) + " items at " + Str(price));
  println(Str(ready));
  return 0;
}
```

Output:

```
Ada ordered 3 items at 2.5
True
```

Every variable needs an initial value: there are no uninitialised variables. When the type
is obvious from the value, leave it out and the compiler **infers** it:

```pkn
fn main() -> int {
  count = 3;          // int
  price = 2.5;        // float
  name = "Ada";       // Str
  total = count * 4;  // int
  println(name + ": " + Str(total));
  return 0;
}
```

Output:

```
Ada: 12
```

Once a variable exists, `name = value` assigns a new value to it. The new value must have
the variable's type: a variable never changes type.

```pkn
fn main() -> int {
  count = 3;
  count = count + 1;
  count = "four";
  return 0;
}
```

Error:

```
error: cannot assign value of type 'Str' to variable 'count' of type 'int'
```

A name can be declared only once per block (`redeclaration of variable 'count'`), but an
inner block may declare its own variable with the same name, which hides the outer one
until the block ends. Assigning without a type, on the other hand, assigns to the outer
variable:

```pkn
fn main() -> int {
  x = 1;
  {
    x: int = 10;  // a new x, local to this block
    println("inner x = " + Str(x));
  }
  {
    x = 2;        // assigns the outer x
  }
  println("outer x = " + Str(x));
  return 0;
}
```

Output:

```
inner x = 10
outer x = 2
```

A variable declared with `let` keeps its first value: assigning to it again is an error. It
always has an initial value, and its type can be left out as usual:

```pkn
fn main() -> int {
  let rate = 3;
  let label: Str = "rate";
  println(label + " = " + Str(rate));
  return 0;
}
```

Output:

```
rate = 3
```

```pkn
fn main() -> int {
  let rate = 3;
  rate = 4;
  return 0;
}
```

Error:

```
error: 'rate' is declared with 'let' and cannot be reassigned
```

`let` is for local variables only (not parameters or fields). It fixes the variable, not
the object it refers to: `let p = Point(1, 2);` can still be followed by `p.x = 5;`.

## Types

These are the built-in value types:

| Type    | Values                                  | Literals                    |
|---------|-----------------------------------------|-----------------------------|
| `int`   | 64-bit signed integers                  | `0`, `42`, `-17`            |
| `float` | 64-bit IEEE 754 floating point          | `3.14`, `0.5`, `1e9`        |
| `bool`  | truth values                            | `True`, `False`             |
| `char`  | a single byte                           | `'a'`, `'\n'`               |

Note the capitals in `True` and `False`. An `int` converts to a `float` automatically where a
`float` is expected (`f: float = 3;` is fine), but nothing else converts implicitly: a
`float` never silently becomes an `int`, and neither ever becomes a `bool`.

Besides these, `Str` is the string type, `void` is the "returns nothing" type of functions,
and every class you declare is a type. Later chapters add arrays (`int[]`), tuples
(`(int, Str)`), optionals (`int?`) and generic types (`Box<int>`).

### Printing values

`print` and `println` take **one** argument, and it must be an object such as a `Str`, not
an `int` or a `bool`. To print a number, convert it to a string with `Str(...)` and join the
pieces with `+`:

```pkn
fn main() -> int {
  n = 6;
  println("n squared is " + Str(n * n));
  print("no newline, ");
  println("then a newline");
  return 0;
}
```

Output:

```
n squared is 36
no newline, then a newline
```

Passing an `int` directly is a compile-time error (`argument 1 of 'println' has type 'int',
expected 'Obj'`). `printerr` and `printerrln` work the same way but write to standard error.
[Strings and conversions](03-strings-and-conversions.md) covers `Str(...)` and the other
conversions.

## Operators

The arithmetic operators are `+`, `-`, `*`, `/` and `%`. If either operand is a `float` the
result is a `float`; otherwise it is an `int`. Integer division truncates toward zero, and
`%` takes the sign of the left operand:

```pkn
fn main() -> int {
  println(Str(7 / 2) + " " + Str(-7 / 2) + " " + Str(7 % 3) + " " + Str(-7 % 3));
  println(Str(7.0 / 2) + " " + Str(2 + 3 * 4) + " " + Str((2 + 3) * 4));
  println(Str(7.5 % 2.0));
  return 0;
}
```

Output:

```
3 -3 1 -1
3.5 14 20
1.5
```

Integer arithmetic wraps around on overflow, and dividing an `int` by zero is a panic that
stops the program. Dividing a `float` by zero gives an infinity, as in C.

There are no compound assignments (`+=`) and no increment operators (`++`): write
`i = i + 1`.

Comparisons (`<`, `>`, `<=`, `>=`, `==`, `!=`) produce a `bool`. They do not chain:
`1 < x < 10` is a syntax error, so write `1 < x && x < 10`. The logical operators are `&&`,
`||` and `!`; `&&` and `||` short-circuit, evaluating their right side only when it is needed.

```pkn
fn main() -> int {
  x = 7;
  inRange = 1 < x && x < 10;
  odd = x % 2 != 0;
  println(Str(inRange) + " " + Str(odd) + " " + Str(!odd || x > 100));
  return 0;
}
```

Output:

```
True True False
```

### The conditional expression

`if c then a else b` is an expression: it evaluates to `a` when `c` is true and to `b`
otherwise. It chains naturally:

```pkn
fn main() -> int {
  score = 84;
  grade = if score >= 90 then "A" else if score >= 80 then "B" else "C";
  println("grade: " + grade);
  return 0;
}
```

Output:

```
grade: B
```

From highest to lowest precedence: member access, calls and indexing (`a.b`, `f()`, `a[i]`);
the unary `!` and `-`; `*` `/` `%`; `+` `-`; the comparisons; `&&`; `||`; and finally
`if ... then ... else`. Parenthesise when in doubt.

## Control flow

### if and else

The condition goes in parentheses, must be a `bool`, and the branches are always blocks in
braces:

```pkn
fn main() -> int {
  temperature = 23;
  if (temperature > 30) {
    println("hot");
  } else if (temperature > 15) {
    println("pleasant");
  } else {
    println("cold");
  }
  return 0;
}
```

Output:

```
pleasant
```

### while, break and continue

`while` is PaykanLang's only loop: there is no `for`. Counting loops use an index variable.
`break` leaves the innermost loop and `continue` starts its next iteration:

```pkn
fn main() -> int {
  i = 0;
  sum = 0;
  while (True) {
    i = i + 1;
    if (i % 2 == 0) { continue; }  // skip even numbers
    if (i > 9) { break; }
    sum = sum + i;
  }
  println("sum of odd numbers below 10: " + Str(sum));
  return 0;
}
```

Output:

```
sum of odd numbers below 10: 25
```

`match` is the third way to branch; it gets its own chapter,
[Enums and match](05-enums-and-match.md).

## Functions

A function declares its parameters with their types, and its return type after `->`. A
function without `->` returns nothing (`void`), and may use a bare `return;` to leave early:

```pkn
fn add(a: int, b: int) -> int {
  return a + b;
}

fn greet(name: Str) {
  if (name == "") {
    return;
  }
  println("Hello, " + name + "!");
}

fn main() -> int {
  greet("Grace");
  greet("");
  println(Str(add(2, 3)));
  return 0;
}
```

Output:

```
Hello, Grace!
5
```

Some rules to know:

- The compiler checks that every path through a function with a return type ends in a
  `return` with a value of that type.
- Functions can be declared in any order, and they can be recursive.
- There is no overloading: each function name is used once. Names of the built-in functions
  (`print`, `println`, `printerr`, `printerrln`, `open`) and built-in types (`Str`, `Obj`,
  `File` and others) are reserved.
- Arguments are passed by value for `int`, `float`, `bool` and `char`. Strings, arrays and
  objects are passed by reference: a function receives the same object as its caller (more
  in [Classes](04-classes.md) and [Memory](12-memory.md)).

A parameter of any type can say how it is passed. An `inout` parameter is the caller's
variable (or a field of an object): the function changes it in place, and assigning an object
to it gives the caller that object. A `view` parameter is read-only: the function cannot
assign to it, nor change the object, string or array it holds, and it can only be passed on
to other `view` parameters. The call looks the same either way:

```pkn
fn addTax(price: inout float, rate: view float) {
  price = price + price * rate;
}

fn main() -> int {
  price = 20.0;
  addTax(price, 0.5);
  println("price = " + Str(price));
  return 0;
}
```

Output:

```
price = 30
```

An `inout` argument must be a variable or a field of exactly the parameter's type: `addTax(20.0,
0.5)` is an error, and so is passing the same variable to two `inout` parameters of one
call. The full rules are in
[the language reference](../language/02-functions-and-calling.md#parameter-modes-view-and-inout).

```pkn
fn fib(n: int) -> int {
  if (n < 2) { return n; }
  return fib(n - 1) + fib(n - 2);
}

fn main() -> int {
  println("fib(20) = " + Str(fib(20)));
  return 0;
}
```

Output:

```
fib(20) = 6765
```

The value `main` returns is the program's exit status. A non-zero status tells the shell
that something went wrong, which matters for scripts: `paykan check.pkn && echo ok` prints
`ok` only when `main` returned 0.

## Project: the first pieces of `tasks`

Our to-do list manager will need to show how far along the list is. Here are its first two
functions: `percent` computes how many tasks are done, as a percentage, and `progressBar`
draws it with a loop:

```pkn
// Percentage of `done` out of `total`, rounded down; 0 for an empty list.
fn percent(done: int, total: int) -> int {
  if (total == 0) { return 0; }
  return done * 100 / total;
}

// A bar of `width` cells, filled in proportion to done/total: [####------]
fn progressBar(done: int, total: int, width: int) -> Str {
  filled = if total == 0 then 0 else done * width / total;
  bar = "[";
  i = 0;
  while (i < width) {
    bar = bar + (if i < filled then "#" else "-");
    i = i + 1;
  }
  return bar + "]";
}

fn main() -> int {
  println(progressBar(0, 4, 10) + " " + Str(percent(0, 4)) + "%");
  println(progressBar(3, 4, 10) + " " + Str(percent(3, 4)) + "%");
  println(progressBar(4, 4, 10) + " " + Str(percent(4, 4)) + "%");
  return 0;
}
```

Output:

```
[----------] 0%
[#######---] 75%
[##########] 100%
```

Notice the parentheses around the conditional expression in `bar + (if ... else "-")`. The
`if` expression has the lowest precedence of all, so it cannot appear as an operand of `+`
without them: `bar + if ...` is a syntax error.

For the precise rules of everything in this chapter, see
[Language basics](../language/01-language-basics.md) and
[Functions](../language/02-functions-and-calling.md) in the reference. Next:
[Strings and conversions](03-strings-and-conversions.md).

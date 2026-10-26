# Function types and lambdas

A function can be stored in a variable, passed to another function or returned from one.
This chapter introduces function values, then gives the rules for function types, lambdas,
what a lambda captures, calling a function value, and how function values are copied and
compared.

## Functions as values

A function type names what a function value takes and returns. It lists the parameter
types in parentheses and the result after `->`: `(int) -> int` is a function taking an
`int` and returning an `int`, and `(Str, int) -> void` takes two arguments and returns
nothing. A value of a function type is a *closure*: a named function used without calling
it (a function is a value just by its name), or a *lambda*, `fn(...) -> ... { ... }`
without a name, written in place. It is stored, passed and returned like any other value
and called like any function:

```pkn
import ::io;

fn double(x: int) -> int { return x * 2; }

fn apply(f: (int) -> int, x: int) -> int {
  return f(x);
}

fn main() -> int {
  step = 3;
  addStep = fn(x: int) -> int { return x + step; };   // uses `step` from main
  io::println(Str(apply(double, 5)) + " " + Str(apply(addStep, 5)));
  step = 100;
  io::println(Str(addStep(5)));                           // still 8: the lambda copied `step`
  return 0;
}
```

Output:

```
10 8
8
```

A lambda copies the variables it uses from the surrounding function at the moment it is
written, and cannot assign to them: `addStep` keeps its own `step`, which the later
assignment in `main` does not change. The copies, like every argument and result of a
function value, are plain copies for value types and references for objects (see
[Captures](#captures)).

## Function Types

```pkn
import ::io;

fn square(x: int) -> int { return x * x; }
fn greet(name: view Str) { io::println("hi " + name); }

fn apply(f: (int) -> int, x: int) -> int { return f(x); }
fn twice(f: (int) -> int) -> (int) -> int {
  return fn(x: int) -> int { return f(f(x)); };
}

class Button {
  label: Str;
  onClick: (Str) -> void;            // a field of function type
}

fn main() -> int {
  g: (int) -> int = square;          // a named function as a value
  log: (view Str) -> void = greet;   // the modes are part of the type
  fs: ((int) -> int)[2] = [square, twice(square)];
  pair: ((int) -> int, Str) = (g, "square");
  log("there");
  io::println(Str(apply(g, 3)) + " " + Str(fs[1](2)) + " " + Str(pair.0(5)) + " " + pair.1);
  return 0;
}
```

Output:

```
hi there
9 16 25 square
```

The type is written as a parenthesised list of parameter types, `->`, and the result
type.  `(int)` alone is not a type (there are no parenthesised types; `(int) -> int` is
one), `() -> int` takes no parameter, and the result may be `void`: `(view Str) -> void`
is the type of `greet` above.  A parameter's mode is written before its type and is part
of the type: `(view Str) -> void` is not `(Str) -> void`.  The result is always a copy:
`-> view T` and `-> inout T` are not supported in a function type (`a function type's
result is a copy`).

`->` is right-associative, so `(int) -> (int) -> int` is a function returning a function.
A suffix binds to the result: `(int) -> int[]` returns an `int[]`, which no result may be;
write the function type in parentheses to give it a suffix, `((int) -> int)[3]` for an
array of three functions, `((int) -> int)?` for an optional one.

Function types are structural: a diagnostic spells them as written and two spellings of
the same shape are one type, wherever they are written.  A function type goes anywhere
a type does: a field, a parameter, a result, an element of an array or a tuple, a type
argument (`List<(int) -> int>`), a `view` or `inout` parameter or local, and an optional.

## Function Values

A **named function** used without an argument list is a value of its function type: a
free function of the module (`square`), an imported one (`m::square`), a `static fn`
(`Point::origin`).  Not a constructor or a conversion (`'Point' is a constructor, not a
function value`, and so is `Str`), not a method (`p.shift` is a field access, and there is no such field), not a
generic function (`'first' is a generic function; a generic function cannot be a function
value yet`), and not a function returning a borrow (`'peek' returns a borrow ('-> view');
it cannot be a function value yet`).

A **lambda** is a function without a name, written where a value goes:

```pkn
import ::io;

fn main() -> int {
  n = 10;
  add = fn(x: int) -> int { return x + n; };          // captures n
  log = fn(s: view Str) { io::println(s); };              // returns void
  cmp = fn(a: view Str, b: view Str) -> bool { return a.len() < b.len(); };
  make = fn() -> () -> int { return fn() -> int { return n * 2; }; };
  log("start");
  io::println(Str(add(1)) + " " + Str(cmp("a", "bb")) + " " + Str(make()()));
  return 0;
}
```

Output:

```
start
11 True 20
```

Its parameters are written like a function's, with their types and modes (there is no
inference of them from the context yet), its result type after `->` or left out for
`void`, and its body is a block, checked like a function's: a `return` matches the result
type, a non-void body returns on every path (`the lambda does not always return a value`),
and `break` and `continue` do not reach a loop outside it.  `view fn` is for methods;
a lambda is never one.

### Expression-bodied lambdas

A lambda whose body only returns an expression can be written with `=>` and the expression
in place of the block: `fn(x: int) -> int => x * 2` is `fn(x: int) -> int { return x * 2; }`.
Leave the result type out and it is the expression's: `fn(x: int) => x * 2` is an
`(int) -> int`, `fn(s: view Str) => s + "!"` a `(view Str) -> Str`, and `fn(x: u8) => x + 1`
a `(u8) -> u8`.

```pkn
import ::io;

fn apply(f: (int) -> int, x: int) -> int { return f(x); }

fn main() -> int {
  double = fn(x: int) => x * 2;
  n = 10;
  addN = fn(x: int) -> int => x + n;        // captures n, as a block would
  add = fn(x: int) => fn(y: int) => x + y;   // a lambda returning a lambda
  io::println(Str(double(4)) + " " + Str(addN(1)) + " " + Str(add(1)(2)) + " "
      + Str(apply(fn(x: int) => x - 1, 10)));
  return 0;
}
```

Output:

```
8 11 3 9
```

- The expression reaches as far as an expression can: `fn(x: int) => x * 2 + 1` returns
  `x * 2 + 1`, and the lambda ends at the `,`, `)` or `;` after it.
- The result type comes from the expression alone, not from where the lambda goes:
  `f: (int) -> u8 = fn(x: int) => 5;` is an error (the lambda is an `(int) -> int`); write
  `fn(x: int) -> u8 => 5` to give the literal the type.
- The expression must have a value: `fn() => io::println("x")` is an error (`the lambda's
  result is the value of its expression, which has none`), and a block is the way to
  write a lambda that returns nothing, `fn() { io::println("x"); }`.
- Everything else is a block's: the expression is the one `return`, so it is checked as
  one (a `view` parameter cannot be returned, `fn(s: view Str) => s`).

A named function takes the same forms, ending with `;` ([Expression
Bodies](03-functions.md#expression-bodies)).

## Captures

A lambda may use the variables of the function (or method, or lambda) it is written in.
Each one it uses is **captured by copy when the lambda expression is evaluated**: the
closure holds a copy of the variable's value as it was then, as a struct's field would.  A
reference (an object, a `Str`, an array, an optional of one) is retained, so the closure
and the variable share the object; a value held in place (a struct, a tuple, a fixed-size
array) is copied, with its references retained.  Later assignments to the variable do not
reach the closure, and the closure does not change the variable.  A closure is an object:
one that captures an object and is stored in a field of it is a reference cycle, which
[is not collected](16-memory.md#reference-cycles) — unless the captured variable is a
[`weak`](16-memory.md#weak-references) one, which the closure captures weakly: it reads
as the object while the object is alive and as `None` afterwards (`weak me = self;
self.onClick = fn() -> Str { match me { ... } };`).

```pkn
import ::io;

class Counter { n: int; }
struct P { x: int; }

fn main() -> int {
  n = 1;
  c = Counter(1);
  p = P(1);
  f = fn() -> int { c.n = c.n + 1; return n + p.x + c.n; };   // n, p copied; c shared
  n = 100;
  p.x = 100;
  io::println(Str(f()) + " " + Str(f()) + " " + Str(c.n));
  return 0;
}
```

Output:

```
4 5 3
```

Inside the lambda a captured name is read-only: it cannot be assigned (`cannot assign to
'n': the function captures it by copy, and the copy is read-only (assign it before the
function is made, or keep it in an object)`), passed to an `inout` parameter, or, when
it holds a value in place, have a field or an element assigned (`'p' is captured by copy
by the function; cannot assign to its field 'x'`).  An object reached through a captured
reference is changed through it as through any other reference (`c.n = c.n + 1` above):
to share a counter with a closure, keep it in an object. Read, a captured name is a value
like any other: the lambda may return it, store it in a local or pass it to any parameter,
a struct, a tuple or a tagged union as well as a string or an object (`fn constant<T>(x:
T) -> () -> T { return fn() -> T { return x; }; }` works for every `T`), and the copy holds
the references the capture does.

What cannot be captured: a `view` or `inout` parameter or local, or a `match` binding of
one (`'v' is a 'view' parameter; a function cannot capture a borrow (it may outlive the
call that holds it)`); a variable that a live [local borrow](02-basics.md#local-borrows)
names (`'k' is borrowed by 'inout' local 'x' until 'x' is last used; a function cannot
capture it`), or a `view` local views when it holds an object, a string or an array;
`self` in a `view fn` (`'self' is read-only in 'view fn m'; a
function cannot capture it (its copy could change the object)`); and `self` in a struct's
method (`'self' is the receiver's storage in a method of struct 'P'; a function cannot
capture it (copy it into a variable first)`).  `self` in a class method that is not a
`view fn` is captured as a reference to the object, like any object, and the method then
counts as one that changes `self` (the closure may):

```pkn
import ::io;

class Counter {
  n: int;
  fn adder() -> (int) -> void { return fn(d: int) { self.n = self.n + d; }; }
  fn reader() -> () -> int { return fn() -> int { return self.n; }; }
}

fn main() -> int {
  c = Counter(5);
  add = c.adder();
  get = c.reader();
  add(10);
  io::println(Str(get()) + " " + Str(c.n));
  return 0;
}
```

Output:

```
15 15
```

A lambda inside a lambda captures the outer lambda's copy, so a closure returned from a
closure holds what the outer one held.

## Calling a Function Value

A function value is called with the syntax of a function call: `f(1)` when `f` is a
variable or a parameter (which cannot take the name of a function of the module, so
`f(1)` never means both: see [Scoping Rules](02-basics.md#scoping-rules)), `obj.cb(1)`
when `cb` is a field of a class or a struct, and `fs[0](1)`,
`t.1(x)`, `make()(2)` for any other expression of function type.  The arguments are
checked as for a function with the same parameters: their number and types, and their
modes (`argument 2 of 'v' must be a variable or a field: parameter '2' is 'inout'`; a
function type's parameters are known by number).  A call through a `view` of a function
value is fine: calling changes no capture.

```pkn
import ::io;

struct Op { name: Str; run: (int, int) -> int; }
class Handler { cb: ((Str) -> Str)?; }

fn plus(a: int, b: int) -> int { return a + b; }

fn main() -> int {
  ops: Op[2] = [Op("plus", plus), Op("times", fn(a: int, b: int) -> int { return a * b; })];
  h = Handler(None);
  h.cb = fn(s: Str) -> Str { return s + "!"; };
  match h.cb {
    f: (Str) -> Str { io::println(f(ops[0].name) + " " + f(ops[1].name)); }
    None { io::println("no handler"); }
  }
  i = 0;
  while i < 2 {
    io::println(Str(ops[i].run(3, 4)));
    i = i + 1;
  }
  return 0;
}
```

Output:

```
plus! times!
7
12
```

An optional function value (`((Str) -> Str)?`) is unwrapped with `match`, the arm naming
the function type ([Unwrapping with `match`](12-optionals.md#unwrapping-with-match)).

## Function Values Are Values

A function value is held in place, like a tuple: the code it runs and its *closure*, a
reference-counted object holding the captures (none at all for a named function or a
lambda that captures nothing).  Copying one (assigning it, passing it, storing it in a
field or an element) copies the code and shares the closure; the last copy releases the
closure and its captures.  Since captures are read-only, two copies cannot tell they
share one.

Two function values are `==` when they run the same code with the same closure:

- the same named function, whatever spelling names it (`lib::square` too): `square ==
  square` is `True`, and `List<T>.contains(square)` finds it. A value of the function made
  in another module is a different value for now;
- the same lambda expression capturing nothing: `same() == same()` is `True` when `same`
  returns `fn(x: int) -> int { return x + 1; }`;
- a copy of a value: `g = f; g == f` is `True`.

Each evaluation of a lambda that captures something makes a new closure, so `adder(1) ==
adder(1)` is `False`, and two different lambdas are never equal, even with the same body.

A function type is a [`ValueType`](06-traits.md#valuetype) (`fn keep<T: ValueType>(x: T)`
takes `square`).  It is not an `Obj`: it cannot be printed, converted to a `Str`, stored
in an `Obj` slot, matched by class, or given to a `T: Show` or `T: Hashable` bound.  Nor
can a tuple, struct, enum or optional holding one, which does not implement `Show` either
(`io::println((1, f))` is an error naming the element).  An
optional function value, `((int) -> int)?`, takes the same 16 bytes: `None` is the value
with no code.

Parameter modes work on a function value as on any value held in place: a plain
parameter is the callee's own copy, a `view` one reads it (it can be called, not stored),
an `inout` one is the caller's variable, which the callee may set to another function.

## Generics and Modules

A function type may name type parameters, and a function value is a type argument:

```pkn
import ::io;

import ::collections;

fn mapFirst<T, U>(xs: view T[], f: (view T) -> U) -> U { return f(xs[0]); }

class Handler<T> { on: (T) -> T; }
impl Handler<T> {
  fn __init__(on: (T) -> T) { self.on = on; }
  view fn run(x: T) -> T { return self.on(x); }
}

fn main() -> int {
  n = mapFirst([1, 2], fn(x: view int) -> int { return x + 1; });
  h = Handler(fn(s: Str) -> Str { return s + "!"; });
  fs = collections::List<(int) -> int>();
  fs.push(fn(x: int) -> int { return x * 2; });
  fs.push(fn(x: int) -> int { return x + 2; });
  k: view = fs.at(1);
  io::println(Str(n) + " " + h.run("a") + " " + Str(k(1)) + " " + Str(fs.contains(k)));
  return 0;
}
```

Output:

```
2 a! 3 True
```

A function type crosses a module boundary in a signature or a field like any type, an
imported function is a value under its qualifier (`ops::square`), and a closure made in
one module is called in another:

```pkn
// file: ops.pkn
fn square(x: int) -> int { return x * x; }
fn twice(f: (int) -> int) -> (int) -> int { return fn(x: int) -> int { return f(f(x)); }; }
class Button {
  label: Str;
  onClick: (Str) -> Str;
  fn click() -> Str { return self.onClick(self.label); }
}
```

```pkn
import ::io;

import ops;

fn main() -> int {
  t = ops::twice(ops::square);
  b = ops::Button("b", fn(l: Str) -> Str { return l + "!"; });
  io::println(Str(t(3)) + " " + b.click());
  return 0;
}
```

Output:

```
81 b!
```

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Not a type | `x: (int) = 1` |
| A mode in a tuple type | `(view int, int)` |
| A borrowed result | `(int) -> view Str`; `fn(x: int) -> view Str { ... }` |
| A `void` parameter | `(void) -> int`; `fn(v: void) {}` |
| Mismatched function types | `f: (int) -> int = fn(x: float) -> int { ... }`; a mode dropped; `int` where `float` is expected |
| Not a function value | `p = Str`; `c = Point`; `g = first`; `q = peek` (`-> view`); `m = p.shift` |
| Calling a non-function | `n = 1; n(2)`; `obj.n(1)` with `n: int`; an optional function value without `match` |
| Wrong arguments | `f(1, 2)` for `f: (int) -> int`; `f("a")`; a `let` or a `view` to an `inout` parameter |
| Assigning a captured name | `n = 1; g = fn() { n = 2; };`; `bump(n)` inside the lambda; `p.x = 2` for a captured struct |
| Capturing a borrow or `self` | a `view` or `inout` parameter or local, a `view fn`'s `self`, a struct method's `self` |
| Capturing a borrowed variable | `x: inout = k;` then `fn() -> int { return k; }` while `x` is live; a reference a live `view` local views |
| A lambda body's rule | `break` outside its own loop, a missing `return`, a duplicate parameter |
| A closure as an object | `io::println(f)`, `o: Obj = f`, `f.toString()`, `show(f)` with `T: Show`; the same for a tuple, struct or enum holding `f` |

## Not Yet Supported

A borrowed result (`-> view T`, `-> inout T`) in a function type; a method as a value
(`p.shift` bound to `p`: write `fn(d: int) { p.shift(d); }`); a generic function as a
value (`first<int>`); a lambda with inferred parameter types (`fn(x) { ... }`); a capture by
reference (a counter shared with a closure lives in
an object); capturing a `view`, an `inout` or a struct method's `self`; a function value
crossing into C, except as a type argument of a generic native function; `toString`,
`print` and `hash` on a function value.

## Notes

- At run time a function value is laid out as the tuple `(int, Obj?)`: the address of
  its code, then its closure, or null when it captures nothing.  A closure is an object
  of a class the compiler makes for the lambda, with one field per capture and an
  object's vtable `[destroy, toString, equals]`.  A call through a function value loads
  the code and the closure and calls the code through its address (`icall`,
  [PIR](../pir.md)), the closure first; `Paykan_release` destroys a closure through
  `destroy` as it does any object, releasing its captures.  A named function used as a
  value has no closure: its code forwards to the function.
- The design and its open questions are in [`docs/design/function-types.md`](../design/function-types.md).

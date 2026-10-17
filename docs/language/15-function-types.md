# PaykanLang — Function Types and Lambdas

A function type names what a function value takes and returns: `(int, float) -> bool`
is the type of a function of an `int` and a `float` returning a `bool`.  A value of a
function type is a *closure*: a lambda written in place, `fn(x: int) -> int { return x +
1; }`, or a named function used without calling it.  It is stored, passed and returned
like any other value and called like any function.

---

## Quick Summary

- Type: `(T1, T2) -> R`; `() -> R` for no parameters; `-> void` for no result; a
  parameter may have a mode, `(view Str, inout int) -> bool`.  `->` is right-associative,
  and a function type takes a suffix in parentheses: `((int) -> int)[3]`, `((int) ->
  int)?`.
- Values: a lambda `fn(x: int, y: int) -> int { return x + y; }` (its parameters are
  annotated, its result type written or `void`, its body a block); a named function,
  `square`, `m::square`, `Point::origin`; the field of a class or a struct, an element of
  an array or a tuple, a parameter, a result, a type argument.
- Captures: a lambda uses the variables of the function it is written in, which it
  **copies when it is made**: a value is copied, an object or a `Str` is shared (the
  reference retained).  A captured name is read-only inside the lambda, and a `view` or
  `inout` cannot be captured.
- Calls: `f(1)` with `f` a variable, `obj.cb(1)` with `cb` a field, `fs[0](1)` for any
  other expression of function type; the arguments are checked and passed as for a
  function with the same parameters, modes included.
- A closure is a reference: copying one shares it, `==` is identity, and it is released
  like an object.  It is not an `Obj`: it cannot be printed, stored in an `Obj` or given to
  a `Show` or `Hashable` bound.
- Structural: two function types are the same when their parameter types, modes and
  result are.
- Not yet: borrowed results in a function type, a method as a value (`p.shift`), a generic
  function as a value, a lambda with inferred parameter types, a capture of a `view`, an
  `inout` or a struct method's `self`.

---

## Function Types

```pkn
fn square(x: int) -> int { return x * x; }
fn greet(name: view Str) { println("hi " + name); }

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
  println(Str(apply(g, 3)) + " " + Str(fs[1](2)) + " " + Str(pair.0(5)) + " " + pair.1);
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

Function types are structural: `typeName` spells them as written and two spellings of
the same shape are one type, wherever they are written.  A function type goes anywhere
a type does: a field, a parameter, a result, an element of an array or a tuple, a type
argument (`List<(int) -> int>`), a `view` or `inout` parameter or local, and an optional.

---

## Function Values

A **named function** used without an argument list is a value of its function type: a
free function of the module (`square`), an imported one (`m::square`), a `static fn`
(`Point::origin`).  Not a builtin (`'println' is a builtin and cannot be a function value
(wrap it in a lambda)`), not a constructor (`'Point' is a constructor, not a function
value`), not a method (`p.shift` is a field access, and there is no such field), not a
generic function (`'first' is a generic function; a generic function cannot be a function
value yet`), and not a function returning a borrow (`'peek' returns a borrow ('-> view');
it cannot be a function value yet`).

A **lambda** is a function without a name, written where a value goes:

```pkn
fn main() -> int {
  n = 10;
  add = fn(x: int) -> int { return x + n; };          // captures n
  log = fn(s: view Str) { println(s); };              // returns void
  cmp = fn(a: view Str, b: view Str) -> bool { return a.len() < b.len(); };
  make = fn() -> () -> int { return fn() -> int { return n * 2; }; };
  log("start");
  println(Str(add(1)) + " " + Str(cmp("a", "bb")) + " " + Str(make()()));
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

---

## Captures

A lambda may use the variables of the function (or method, or lambda) it is written in.
Each one it uses is **captured by copy when the lambda expression is evaluated**: the
closure holds a copy of the variable's value as it was then, as a struct's field would.  A
reference (an object, a `Str`, an array, an optional of one) is retained, so the closure
and the variable share the object; a value held in place (a struct, a tuple, a fixed-size
array) is copied, with its references retained.  Later assignments to the variable do not
reach the closure, and the closure does not change the variable.  A closure is an object:
one that captures an object and is stored in a field of it is a reference cycle, which
[is not collected](08-memory-model.md#reference-cycles):

```pkn
class Counter { n: int; }
struct P { x: int; }

fn main() -> int {
  n = 1;
  c = Counter(1);
  p = P(1);
  f = fn() -> int { c.n = c.n + 1; return n + p.x + c.n; };   // n, p copied; c shared
  n = 100;
  p.x = 100;
  println(Str(f()) + " " + Str(f()) + " " + Str(c.n));
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
to share a counter with a closure, keep it in an object.

What cannot be captured: a `view` or `inout` parameter or local, or a `match` binding of
one (`'v' is a 'view' parameter; a function cannot capture a borrow (it may outlive the
call that holds it)`); `self` in a `view fn` (`'self' is read-only in 'view fn m'; a
function cannot capture it (its copy could change the object)`); and `self` in a struct's
method (`'self' is the receiver's storage in a method of struct 'P'; a function cannot
capture it (copy it into a variable first)`).  `self` in a class method that is not a
`view fn` is captured as a reference to the object, like any object, and the method then
counts as one that changes `self` (the closure may):

```pkn
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
  println(Str(get()) + " " + Str(c.n));
  return 0;
}
```

Output:

```
15 15
```

A lambda inside a lambda captures the outer lambda's copy, so a closure returned from a
closure holds what the outer one held.

---

## Calling a Function Value

A function value is called with the syntax of a function call: `f(1)` when `f` is a
variable or a parameter (a variable of function type shadows a function of the same
name), `obj.cb(1)` when `cb` is a field of a class or a struct, and `fs[0](1)`,
`t.1(x)`, `make()(2)` for any other expression of function type.  The arguments are
checked as for a function with the same parameters: their number and types, and their
modes (`argument 2 of 'v' must be a variable or a field: parameter '2' is 'inout'`; a
function type's parameters are known by number).  A call through a `view` of a function
value is fine: calling changes no capture.

```pkn
struct Op { name: Str; run: (int, int) -> int; }
class Handler { cb: ((Str) -> Str)?; }

fn plus(a: int, b: int) -> int { return a + b; }

fn main() -> int {
  ops: Op[2] = [Op("plus", plus), Op("times", fn(a: int, b: int) -> int { return a * b; })];
  h = Handler(None);
  h.cb = fn(s: Str) -> Str { return s + "!"; };
  match h.cb {
    f: (Str) -> Str { println(f(ops[0].name) + " " + f(ops[1].name)); }
    None { println("no handler"); }
  }
  i = 0;
  while (i < 2) {
    println(Str(ops[i].run(3, 4)));
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
the function type (`10-optionals.md`).

---

## Closures Are References

A function value is a *closure*: a reference-counted object holding the code and the
captures.  Copying one (assigning it, passing it, storing it in a field or an element)
shares it, as with an object, and the last reference releases it and its captures.  Two
closures are `==` when they are the same closure: each evaluation of a lambda expression
makes a new one, so `fn() {} == fn() {}` is `False`, and so does each use of a named
function as a value (`square == square` is `False` too), while a stored closure equals
itself, which is what `List<T>.contains` needs.  A closure is not an `Obj`: it cannot be printed,
converted to a `Str`, stored in an `Obj` slot, matched by class, or given to a `T: Show`
or `T: Hashable` bound.

Parameter modes work on a function value as on an object: a plain parameter shares the
closure, a `view` one reads it (it can be called, not stored), an `inout` one is the
caller's variable, which the callee may point at another closure.

---

## Generics and Modules

A function type may name type parameters, and a function value is a type argument:

```pkn
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
  k = fs.get(1);
  println(Str(n) + " " + h.run("a") + " " + Str(k(1)) + " " + Str(fs.contains(k)));
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
import ops;

fn main() -> int {
  t = ops::twice(ops::square);
  b = ops::Button("b", fn(l: Str) -> Str { return l + "!"; });
  println(Str(t(3)) + " " + b.click());
  return 0;
}
```

Output:

```
81 b!
```

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Not a type | `x: (int) = 1` |
| A mode in a tuple type | `(view int, int)` |
| A borrowed result | `(int) -> view Str`; `fn(x: int) -> view Str { ... }` |
| A `void` parameter | `(void) -> int`; `fn(v: void) {}` |
| Mismatched function types | `f: (int) -> int = fn(x: float) -> int { ... }`; a mode dropped; `int` where `float` is expected |
| Not a function value | `p = println`; `c = Point`; `g = first`; `q = peek` (`-> view`); `m = p.shift` |
| Calling a non-function | `n = 1; n(2)`; `obj.n(1)` with `n: int`; an optional function value without `match` |
| Wrong arguments | `f(1, 2)` for `f: (int) -> int`; `f("a")`; a `let` or a `view` to an `inout` parameter |
| Assigning a captured name | `n = 1; g = fn() { n = 2; };`; `bump(n)` inside the lambda; `p.x = 2` for a captured struct |
| Capturing a borrow or `self` | a `view` or `inout` parameter or local, a `view fn`'s `self`, a struct method's `self` |
| A lambda body's rule | `break` outside its own loop, a missing `return`, a duplicate parameter |
| A closure as an object | `println(f)`, `o: Obj = f`, `f.toString()`, `show(f)` with `T: Show` |

---

## Not Yet Supported

A borrowed result (`-> view T`, `-> inout T`) in a function type; a method as a value
(`p.shift` bound to `p`: write `fn(d: int) { p.shift(d); }`); a generic function as a
value (`first<int>`); a lambda with inferred parameter types (`fn(x) { ... }`); a lambda
with an expression body; a capture by reference (a counter shared with a closure lives in
an object); capturing a `view`, an `inout` or a struct method's `self`; a function value
crossing into C, except as a type argument of a generic native function; `toString`,
`print` and `hash` on a function value.

---

## Notes

- At run time a closure is an object of a class the compiler makes for the lambda (or
  for the named function), with one field per capture and the vtable `[destroy,
  toString, equals, call]`: the first three an object's, `call` the function's code with
  the closure first.  A call through a function value is a virtual call of `call`
  (`docs/pir.md`); `Paykan_release` destroys a closure through `destroy` as it does any
  object, releasing its captures.  A closure of a named function holds no captures and
  forwards to the function.
- The design and its open questions are in `docs/design/function-types.md`.

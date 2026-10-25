# Function types and lambdas

The design behind `docs/language/14-function-types.md`: what a function value
is, what it may capture, how it is called, and how it is laid out at run time.
The decisions were made to fit the language as it is (values held in place,
objects behind reference-counted boxes, `view` / `inout` borrows checked at
compile time).  They add two PIR instructions, `fn.addr` and `icall`, and no
change to the runtime ABI or the `.pkm` format; the versions stay fixed until
0.2.0.  The open questions at
the end record the alternatives; each has the default this implementation
takes.

## Summary of the decisions

| Topic | Decision |
|-------|----------|
| Type syntax | `(T1, T2) -> R`; `() -> R` for no parameters; `-> void` for no result; a parameter may have a mode, `(view Str, inout int) -> bool`; `->` is right-associative, and a function type is parenthesised to take a suffix (`((int) -> int)[3]`, `((int) -> int)?`) |
| Values | a lambda `fn(x: int) -> int { return x + 1; }`, or a named function used without calling it (`square`, `m::square`): a free function of the module or an import, a `static fn` (`Point::origin`), not a method, a constructor, a builtin or an uninstantiated generic |
| Representation | a value held in place, 16 bytes: the address of its code, and the box of its *closure*, a reference-counted object holding the captures (null when there are none); copying one copies the code and shares the closure; `==` is true for the same code and the same closure; it is a `ValueType`, not an `Obj`, and has no `toString` or `hash`; an optional one is the same 16 bytes, `None` having no code |
| Captures | a lambda copies, when it is made, each variable of the enclosing function it uses (an `int` is copied, an object or a `Str` is retained, a struct or a tuple is copied like an assignment); only a plain variable (a local or a by-value parameter) or a class method's `self` can be captured, never a `view` or `inout` name, a `view fn`'s `self` or a struct's `self`; inside the lambda a captured name is read-only |
| Calls | `f(args)` where `f` is a variable or parameter of function type, `obj.f(args)` where `f` is a field of function type (and `self.f(args)`), and any other expression of function type called in place (`fs[0](1)`, `pair.0(x)`, `twice(g)(2)`), with the arguments checked and passed exactly as for the function's own parameters, modes included |
| Nominal? | structural: two function types are the same when their parameter types, modes and result type are |
| Generics | a function type may name type parameters (`fn map<T, U>(xs: view T[], f: (view T) -> U)`), and a function value is a type argument (`List<(int) -> int>`) |
| Optionals | `((int) -> int)?` is the same 16 bytes, `None` having no code; unwrapped with `match` like an optional object |
| Modules | a function type crosses a module boundary in a signature or a field like any type; a function value made in one module is called in another through its code's address |
| Not yet | borrowed results (`-> view T`) in a function type; methods as values (`p.shift` bound to `p`); generic lambdas; a lambda capturing a `view`, an `inout` or a struct's `self`; printing a function value |

## Syntax

```
functionType ::= "(" ( paramType ( "," paramType )* )? ")" "->" typeAnnotation
paramType    ::= ( "view" | "inout" )? typeAnnotation
lambda       ::= "fn" "(" paramList? ")" ( "->" typeAnnotation )? block
```

`(int) -> bool` is a function type and `(int, Str)` a tuple type: after the
closing `)`, `->` decides.  `(T)` alone stays an error (there are no
parenthesised types) unless `T` is a function type: `((int) -> int)[3]` is an
array of three functions, `((int) -> int)?` an optional function, and
`(int) -> int[]` a function returning a slice-less error (its result is
`int[]`, which is not a type a result may have).  The result of a function
type is any type, `void` included (`(int) -> void` is the type of
`fn log(n: int) { ... }`); `-> view T` and `-> inout T` are rejected.

A lambda is written like a function without a name, in expression position:
`fn(x: int) -> int { return x * 2; }`.  Its parameters are annotated (there is
no inference of a lambda's parameter types from the context yet), its result
type is written or `void`, and its body is a block (no expression bodies).
`view fn(...)` is not a lambda: `view` qualifies methods.

## Typing

* A function type is canonical: `ASTContext::getFunctionType(params, modes,
  result)` returns one node per shape, so type identity is pointer identity,
  as for tuples and optionals.  `typeName` spells it as written, so it round
  trips through a `.pkm` interface and reads naturally in diagnostics.
* Assignability is identity: no variance in parameters or results (a
  `(Dog) -> int` is not an `(Animal) -> int`), no mode dropping.  `int ->
  float` promotion does not apply to function types.
* A named function used as a value has the type of its signature.  A
  generic function must be instantiated to be a value, and that is not
  supported yet (`first<int>` as a value; `io::println` is generic too).  A
  builtin (a conversion, a constructor) is not a value: it has no single
  signature or is not a function.
* A lambda's type is the function type of its signature.  Its body is
  checked as a function body: `return` matches its result type, a non-void
  body returns on every path, `break` and `continue` do not reach out of it.
* `sharesStorage` is true for a function type: a `view` of one is read or
  passed on, never stored, like a `view` of an object.

## Captures

A lambda may use the variables of the function (or method, or lambda) it is
written in.  Each such variable is *captured by copy* when the lambda
expression is evaluated: the closure holds a copy of the variable's value as
it was then, as a struct field would.  A reference (an object, a `Str`, an
array, an optional of one) is retained; a value held in place (a struct, a
tuple) is copied with its references retained.  Later assignments to the
variable do not reach the closure, and the closure does not change the
variable.

Inside the lambda a captured name is read-only: it cannot be assigned, passed
to an `inout` parameter, have a field or an element assigned (when it is a
value held in place) or be the subject of a changing method call.  An object
reached through a captured reference can still be changed, as an object held
by two variables can.  The reason: the closure's copy would be the one
changed, which the program could not see and which would persist across calls
(a closure is not meant to be a mutable cell; use an object for that).

What cannot be captured: a `view` or `inout` parameter, a `view` or `inout`
local, a `match` binding of one (they are borrows that end with the function;
a closure may outlive it), `self` in a `view fn` (the copy could change the
object the `view fn` promised not to), and `self` in a struct's method (it is
the receiver's storage, not a value).  `self` in a class's method that is not
a `view fn` is captured as a reference to the object, like any object.

The capture set is computed by Sema (the names the body uses that resolve
outside the lambda) and recorded on the `LambdaExpr`, in first-use order.

## Run-time representation

A function value is held in place, laid out as the tuple `(int, Obj?)`: the
address of its *code* (kept as an `i64`), then the box of its *closure*, or
null.  The lowering treats it as that tuple everywhere a value held in memory
is handled: a variable is a `mem(16, 8)` local, a field or an element 16
bytes in place, a parameter or a result passed by address, a copy a
`mem.copy` that retains the closure, a release the closure's release.  An
optional function value is the same 16 bytes; `None` is all zeros, so a new
object's field of type `((int) -> int)?` is `None` already.

* The code of a lambda is a function `fn.<module>.N.call(obj closure,
  params...) -> result`, the parameters passed as a method's are (a `view`
  reference as a borrowed box, an `inout` as the address of the caller's
  storage, a value held in memory by address, a result held in memory
  through a trailing pointer).  It reads its captures from the closure.
* The closure of a lambda that captures something is an object of the class
  `fn.<module>.N`: the object header, one field per capture (a `weak`
  variable's a `weak` field, docs/design/weak-references.md), and an
  object's vtable, the runtime's `toString` and `equals` and a generated
  `destroy` that releases the captures.  A lambda that captures nothing has
  no closure: its value is its code and null.
* A named function used as a value is the code `fn.ref.<module>.<f>.call`,
  which ignores the closure and forwards its parameters to the function
  (ownership passes through), and no closure.
* A lambda expression or a named function writes its two words into a
  temporary: `fn.addr` of the code, cast to `i64`, then the closure, `new`
  of its class with a `field.store` per capture (a reference retained, a
  value held in memory copied and its references retained) and `box`, or
  null.
* `f(args)` loads the code word and the closure from the value's address
  before evaluating the arguments (an argument may rebind `f`), retains the
  closure for the call, and calls the code with `icall`, the closure object
  first, the function type's signature written out (the verifier checks it,
  which is all a call site knows).
* `==` passes the two addresses and the tuple's layout to
  `PaykanTuple_equals`: the code words are compared as integers and the
  closures by the object's `equals`, identity, two nulls being equal.
* A `PaykanType` descriptor for a function type (a generic native function's
  argument, a `Buffer<(int) -> int>`'s element) is the tuple's: 16 bytes,
  one optional reference at offset 8.  It also names the code word, at
  offset 0, as the word that tells storage never set (a buffer's new slot):
  it is 0 only there and in None, so an optional function type's names
  none.

At `-O2` LLVM turns an `icall` whose code is known into a direct call, often
inlined: a lambda passed straight to the function that calls it costs no
indirect call.  The code and closure classes are module items like any
other: names no Paykan identifier can spell, mangled by the C backend like
`Box<int>` (now by the link names of the `mlir` backend, which replaced the
C and LLVM backends; [the MLIR backend](mlir-backend.md)).  A function
value crosses modules as its two words: the module that calls it needs only
its signature.  Nothing is added to the runtime: `Paykan_release` destroys a
closure through slot 0 as it does any object.

## Diagnostics

| Error | Example |
|-------|---------|
| a `(T)` that is not a function type | `x: (int) = 1` |
| `-> view` in a function type | `f: (int) -> view Str` |
| a parameter type of `void` | `(void) -> int` |
| a mode in a tuple type | `(view int, int)` |
| mismatched function types | `f: (int) -> int = fn(x: float) -> int { ... }` |
| calling a non-function | `n = 1; n(2)` |
| wrong arguments to a function value | `f(1, 2)` for `f: (int) -> int`; `f("a")`; a `view` argument to an `inout` parameter |
| a builtin, constructor or generic as a value | `p = Str;`, `c = Point;`, `g = first;` |
| a method as a value | `m = p.shift;` |
| capturing a borrow | `fn f(v: view Str) { g = fn() -> int { return v.len(); }; }` |
| capturing `self` in a `view fn` or a struct method | `view fn m() { g = fn() { self.x = 1; }; }` |
| assigning a captured name | `n = 1; g = fn() { n = 2; };` |
| a captured name as an `inout` argument | `g = fn() { bump(n); };` |
| a lambda's `break` outside its own loop | `while c { g = fn() { break; }; }` |
| `toString`, `print` on a function value | `io::println(f)`, `f.toString()` |

## Open questions

Each with the default taken here; a different answer changes the
implementation in the place noted.

1. **Capture by copy or by reference?**  By copy (above): it keeps the
   language's rule that only objects are shared, needs no cells for captured
   locals and no escape analysis, and makes a closure safe to store and call
   later.  By-reference captures would let a lambda count calls into a local
   (`n = n + 1`), the way JavaScript or Swift closures do; that needs the
   captured local to live in a shared cell (an object) for both the function
   and the closure, which is a bigger change to locals.  Changes: Sema's
   capture rules and the lowering of locals.
2. **Should a lambda's parameter types be inferable from the context?**
   Not yet: `xs.map(fn(x) { return x + 1; })` would need bidirectional
   inference through the call's parameter type.  The expected type is at hand
   in Sema (`resolveExprType(e, expected)`), so this can be added later
   without a syntax change.  Changes: Sema's `visitLambdaExpr`.
3. **Should function types be `Obj`s?**  No: an `Obj` is an object with
   `toString` and `equals` and a class, and `match` dispatches on classes; a
   function value has none of that.  Keeping it out of `Obj` keeps `match`
   and printing simple.  Making it an `Obj` would let it be stored in
   `Obj`-typed slots and printed.  Changes: `isAssignable` and the runtime
   vtable's `toString`.
4. **Should methods be values (`p.shift` bound to `p`)?**  Not yet: a bound
   method is a closure capturing the receiver, which is the by-copy capture
   of `self`, but a struct's method needs its receiver's storage, which a
   closure cannot hold.  A class's bound method can be added as sugar for
   `fn(d: int) { p.shift(d); }`.
5. **Should a named generic function be a value with explicit arguments
   (`first<int>`)?**  Not yet; the instantiation machinery can produce the
   function and the value would be a `fn.ref` closure of it.  Changes: the
   parser (a type argument list on an identifier that is not called) and
   `visitIdentifier`.
6. **Variance?**  None: identity.  Parameter contravariance and result
   covariance for reference types would be sound but the existing conversion
   rules are representation-preserving (a tuple element must match exactly),
   so function types follow those.
7. **Equality and printing?**  `==` compares the code and the closure: a
   named function equals itself, as does a lambda that captures nothing,
   while each evaluation of a capturing lambda makes a different closure.  A
   stored value equals itself, which is what `List<T>.contains` and
   `indexOf` need to take function values.  No `toString`: it would have
   nothing to show but an address.  An alternative is no `==` at all, which
   keeps `List<(int) -> int>` out.
8. **Where does a lambda's `self` come from in a class method?**  Captured as
   a reference, like any object; the lambda's body then treats `self` as an
   ordinary variable holding the object (private methods are still the
   class's to call, so a lambda inside the class may call them).

# PIR: the Paykan intermediate representation

PIR is the small, backend-neutral IR that sits between Sema's typed AST and
every backend (the in-tree `mlir` backend, plugins).  It is produced by
**one** lowering pass (`src/Lowering`), which is the single home of the
ownership rules: by the time a backend sees PIR, every
`retain`/`release`/`box`/`unbox`, every scope cleanup, every `match` dispatch
and every vtable is explicit.  A
backend never re-derives any of that; it translates PIR ops one-to-one.

PIR has a textual form (printer + parser, so backend tests need no frontend),
a binary form for module files (§11) and a verifier.  The data structures
live in `include/paykan/pir/PIR.h`.

Design rules:

* **Explicit.**  No implicit conversions, no implicit ownership.  Every value has
  exactly one PIR type; every ARC operation is an instruction.
* **Structured control flow.**  Blocks nest (`if`, `while`); there is no CFG and
  no `phi`.  Values that merge across branches go through mutable `local`
  slots.  `break`/`continue`/`ret` never carry cleanup: the lowering emits the
  releases *before* them.
* **No target concepts.**  No LLVM types, no C types, no calling-convention
  detail.  LP64 is assumed by the runtime ABI (a pointer and a box are 8
  bytes), and PIR inherits that assumption.
* **Runtime by name.**  Anything the runtime does is a `call` to an `extern fn`
  named by its C symbol under the `$rt.` prefix (`@$rt.PaykanString_new`; the
  list below is the ABI).  Only ops that *every*
  backend wants to map specially (ARC, allocation, vtables, field access) are
  first-class.

---

## 1. Types

| PIR   | Meaning                                                           | C            |
|-------|-------------------------------------------------------------------|--------------|
| `void`| no value (function returns only)                                  | `void`       |
| `i64` | 64-bit signed integer; also enum values                           | `int64_t`    |
| `f64` | IEEE-754 double                                                   | `double`     |
| `bool`| true/false                                                        | `int64_t` or `_Bool` (backend's choice; `0`/`1` at the ABI) |
| `char`| one byte                                                          | `int8_t`     |
| `box` | `PaykanShared*`: an owned-or-borrowed reference-counted box; may be `null` (an optional's `None`) | `PaykanShared *` |
| `obj` | raw `PaykanObject*`: an unboxed heap object of any class (also `PaykanString*`, `PaykanFile*`, `PaykanError*`) | `PaykanObject *` (cast at runtime calls) |
| `ptr` | any other pointer: vtable addresses, literal data, type descriptors and tuple layouts, the address of a slot | `void *`  |
| `opt_i64` `opt_f64` `opt_bool` `opt_char` | an inline optional: a presence flag, then the payload (`i64`, `f64`, `bool`, `char`) at its natural alignment, held and passed by value (#96): 16 bytes for `opt_i64` and `opt_f64`, 2 for `opt_bool` and `opt_char` | `struct { bool present; <payload> value; }` |
| `mem(N, A)` | N bytes aligned to A (1, 2, 4 or 8; N a positive multiple of A): the storage of an aggregate, a fixed-size array or a tuple. Only a `local` or a field has this type, never a value, a parameter or a result; it is reached through its address (§6) | `_Alignas(A) uint8_t name[N]` |
| `weak` | `PaykanWeak*`: a weak reference to an object (docs/language/16-memory.md, "Weak references"), the type of a `weak` local or field; may be `null`. Made from a `box` by `weak.new`, read as a `box` by `weak.load`, dropped by `weak.release` (§6); never addressed, cast or compared | `PaykanWeak *` |

Paykan types lower as: `int`→`i64`, `float`→`f64`, `bool`→`bool`,
`char`→`char`, every enum→`i64`, `int?`→`opt_i64`, `float?`→`opt_f64`,
`bool?`→`opt_bool`, `char?`→`opt_char`, and every reference type (class,
`Str`, and the optional of one)→`box` when stored (variable, field,
parameter, return value, array or tuple element) and `obj` when used
as a raw receiver.  A `T?` of a reference type is a `box` that may be `null`.
A fixed-size array `T[n]` is `mem` storage holding its `n` elements one after
another, each laid out as its type is stored (8 bytes for an `i64`, `f64` or
`box`, 1 for a `bool` or a `char`, an optional's size, a row's `mem`):
`int[3]` is `mem(24, 8)`, `bool[2]` is `mem(2, 1)`, `int[2][3]` is
`mem(48, 8)`.  A tuple is `mem` storage holding its elements as C lays out a
struct of them: each, laid out as its type is stored (a nested tuple as its
`mem`), at the next offset its alignment allows, the whole padded to the
largest alignment: `(int, Str)` is `mem(16, 8)`, `(bool, char)` is `mem(2,
1)`, `(int?, (Str, bool))` is `mem(32, 8)` (the `opt_i64` at 0, the `box` at
16, the `bool` at 24).

An optional type holds a presence flag and a payload: `none` is the absent
value (presence false, a zero payload), `some %v` the present one (presence
true).  In memory (a field, an array or tuple element, a native function's
`PaykanType`) it is laid out as C lays out that struct, so all zero is
`none`.
It is a value like an `i64`: stored in locals and fields, passed and returned,
`select`ed and read and written through addresses, with no ARC.  It has no
constants, is never compared (`is_some`, then the payloads) and never `cast`,
and a runtime or native function never takes or returns one (its C ABI is
not the same in the C and in the LLVM IR the backend makes: it crosses into
C as its parts).  A new object's optional fields are absent, as its other
fields are zero.

## 2. Values and constants

Instruction results are SSA names `%name` (unique per function).  The printer
writes every value as `%name.N` where `N` is its id (`%N` when it has no name);
hand-written text may use bare `%name`s and the parser numbers them.  Function
parameters are values.  Operands are either values or constants:

```
42        i64        1.5  / 1.0e3   f64 (a float literal always has a '.' or an exponent)
true / false  bool   'a' / '\n' / '\x41'   char
null box / null obj  a null box or obj (a bare `null` is a box)
@sym      the address of a module-level symbol (see §4): cstr/data/bytes globals
          are `ptr`, extern objects are `obj`, extern vtables are `ptr`
```

An `f64` constant is printed with 17 significant digits (`%.17g`, plus `.0`
when that has neither `.` nor exponent), so it reads back to the same bits;
that covers subnormals (`4.9406564584124654e-324`) and `-0.0`.  The non-finite
constants are written `inf`, `-inf` and `nan` (a NaN's sign and payload are
not kept).  Text naming a value outside the `f64` range (`1e999`, or a
nonzero value that rounds to zero such as `1e-400`) is a parse error.

A value defined inside a nested block is visible only inside that block (and
its children): there is no dominance analysis, structure *is* dominance.  To
carry a value out of a branch, store it into a `local`.

## 3. Functions and locals

```
fn @name(%p0: T0, %p1: T1) -> R {
  local %x: T          ; mutable slot, function-scoped, uninitialised
  ...statements...
}
extern fn @$rt.Paykan_retain(box) -> void          ; runtime ABI symbol `Paykan_retain`
extern fn @add(i64, i64) -> i64 module "lib::math" ; defined in another PIR module
extern fn @"x::tag"() -> box module "x" symbol @tag ; named `tag` in module "x"
```

Locals are declared at the top of the function body (the lowering hoists them,
like LLVM entry-block allocas) and read/written with `load`/`store`; a `mem`
local (`local %a: mem(24, 8)`) is read and written through its address
instead (`local.addr`, §6).  Several
locals may share a name (two match arms binding `a`, a name declared in sibling
scopes), so the printer writes a local as `%name.I` with `I` its index;
hand-written text may use any spelling as long as declaration and uses agree.
Every Paykan variable is a local.  A slot can hold `null` (an absent optional),
so `release` of a null box is a no-op in the runtime.

A function is `extern` when it is defined elsewhere: in the runtime (no
`module` clause; the name is `$rt.` followed by the C symbol, see below) or in
another PIR module of the same
program (`module "<module name>"`, written on the declaration's line: a
`module` on a later line starts the next module).  A module extern may add
`symbol @<name>` (on the same line): the function's name in its defining
module, when that differs from the name this module's calls use.  The lowering
names every function it imports as its call sites qualify it (`@"x::tag"`,
`@"r::tag"` for `import y as r`) with `symbol @tag`, so two modules' `tag`s and
the importer's own `@tag` are distinct symbols; a function reached through
several qualifiers is declared once.  Backends link a module extern through
its `(module, symbol)` pair.  Backends may mangle the
names of module-defined symbols (C needs to: `Box<int>`, `first<int>` and
`helper::add` are not C identifiers) but must link a runtime extern by its C
symbol, the name without `$rt.` (`pir::runtimeSymbol`, `PIR.h`); a
`(module, name)` pair must mangle the same way in every module of the
program.

**Runtime names.**  Runtime externs (`extern fn` without a `module`, `extern
obj`, `extern vtable`), and only they, are named `$rt.<C symbol>`:
`@$rt.PaykanString_new` is the runtime's `PaykanString_new`.  No Paykan
identifier can spell `$` or `.`, so the runtime and the program never share a
PIR name: a program function `fn PaykanString_new(x: int) -> int` is the
ordinary `@PaykanString_new` (the mlir backend's link name
`pk_<module>_SPaykanString__new`), whatever its signature, and the program's own string
literals still call `@$rt.PaykanString_new`.  The verifier enforces both
directions (§9).

**Native names.**  The C function a `native fn` calls (#198) is a module-less
extern named `$c.<C symbol>`: `native fn put(fd: int, s: view Str) -> int =
"pk_put"` declares `extern fn @$c.pk_put(i64, obj) -> i64`.  Its signature is
not the runtime's, so backends declare it from the PIR signature (C:
`int64_t pk_put(int64_t, PaykanObject *);`) and link it by the name without
`$c.` (`pir::externCSymbol`).  A call in the declaring module calls
`@$c.pk_put` itself, with the caller's own references unboxed (and an
`inout` argument's address).  The `native fn` is also an ordinary function
`@put`, for other modules' callers, whose body does the same with its
parameters, releases them and returns the result; a reference result is the
C function's owned box.  The objects that define the
`$c.` symbols are not part of PIR: the driver hands them to the backends
(`backend::Input::Objects`, `--object`).

## 4. Module-level items

```
module "<name>"                     ; the canonical module name (`geometry::shapes`; the main module: its file stem)

cstr  @.str0  = "hello\n"  len 6   ; NUL-terminated literal data (`ptr`)
data  @.type0 = [8, 8, 0, 0]       ; constant i64 words: a `PaykanType` descriptor (`ptr`)
data  @.tuple0 = [2, 0, 0, 4, 8]   ; constant i64 words: a tuple layout (`ptr`)
bytes @.b0    = [0, 4]             ; constant bytes (`ptr`)

extern obj    @$rt.PaykanObject_None   ; runtime object singletons (`obj`)
extern vtable @$rt.PaykanString_vtable ; runtime vtable globals (`ptr`)

class Point {                      ; layout + vtable, see §5 (`: Super` names
                                   ; the superclass; none for a root class)
  field x: i64
  field name: box
  vtable {
    destroy  = @Point.destroy  : (obj) -> void
    toString = @$rt.PaykanObject_toString : (obj) -> box
    equals   = @$rt.PaykanObject_equals   : (obj, box) -> i64
    move     = @Point.move : (obj, i64, i64) -> void
  }
}
extern class Adder module "helper" { field n: i64 }   ; layout only

fn / extern fn                      ; §3
```

The lowering emits a `.typeN` `data` global for each type argument of a
generic native function, and a `.tupleN` one, the layout of a tuple type
(`Runtime.h`, "Tuples": the element count, then each element's
`PaykanTypeKind` and byte offset, an optional's payload kind, a nested tuple's
layout), for each tuple type that is printed, compared or converted with
`toString()`, and a `.structN` one for each struct type that is (`Runtime.h`,
"Structs": a tuple's layout with the struct's name after the count and each
field's name before its entry, a name being its byte length then its bytes
packed eight to a word; a struct nested in a tuple or a struct is an entry of
kind `STRUCT`, its layout inline).  A `bytes` global (raw constant bytes) is
part of the format, though the lowering emits none.

Every symbol a function body references must be declared in its module: a call
to a runtime function needs the `extern fn`, a field access needs the `class`
(or `extern class`), a `vtable.addr` needs the class or `extern vtable`.  The
lowering emits these declarations; the verifier checks them.

A **program** is the list of PIR modules reachable from the main module
(main module first, each module exactly once).  Linking them is a backend
concern.  The entry point is the main module's `@main`, which takes either no
parameters or the program's arguments as a slice, `(ptr, i64)`: the address of
their strings' `box` slots and their number.  It returns `i64`.  The backend's
entry code makes the slice before it calls `@main` and drops it after:
`@main` borrows the strings, as any slice parameter borrows its elements.  An
executable's `main(argc, argv)` (the one the mlir backend adds for `paykan
build`, and the C of `--emit-c`) makes it with `Paykan_args_new(argc, argv,
&count)` and drops it with `Paykan_args_free(args, count)`; the backend's JIT
(`paykan run`) boxes the same strings itself.

## 5. Classes, layouts and vtables

A `class` item gives the complete object layout and the complete vtable; the
backend does no inheritance walk.

* **Layout**: the 16-byte runtime header (vtable pointer, 32-bit strong
  count, 32-bit weak count; `struct PaykanObject` in `Runtime.h`: an object carries its
  own reference counts, so a `box` is the object's pointer) followed by
  *all* fields in declaration order, ancestors first.  Field types are `i64
  f64 bool char box weak`, the optionals and `mem(N, A)` (laid out at its
  alignment, as C lays out the `uint8_t` array; reached through
  `field.addr`).
* **Vtable**: one entry per slot in slot order, each a function symbol (a
  function of this module, an `extern fn`, or `null` for an abstract slot) plus
  its signature.  Slot 0 is always `destroy`.  The vtable's address is the
  class's runtime type identity (`vtable.addr`).
* A class marked `extern` belongs to another module: its fields are known (for
  `field.load`/`field.store` on imported instances) and its vtable address may
  be taken, but its vtable contents and functions are defined in that module.
* Runtime classes (`Obj`, `Str`) are never
  declared as classes; their vtables are
  `extern vtable` globals and their methods are called through `vcall` with an
  explicit slot index, exactly like user classes.

Constructors and destructors are ordinary functions produced by the lowering:
`@Point(args...) -> box` allocates with `new`, installs the vtable, zeroes
the counts and the fields, boxes the object (`box`, before `__init__`, so
`self` inside `__init__` is the caller's box) and calls
`@Point.__init__(obj, args...)`; `@Point.destroy(obj)` releases every
ref-typed field (`weak.release` for a `weak` one) and returns: the runtime
frees the object after the destroy, or keeps it for its weak references
(Runtime.h, "Shared"), so a destroy never `free`s its object.  `destroy` is
entirely compiler-generated: it is final, so a class cannot write or
override it.  Every
method `m` of a class `C` is the function `@C.m(obj self, params...)`.  The
'.' cannot occur in a Paykan identifier, so these names never clash with a
program function (`fn Point_move`, `fn Point_destroy`); backends must keep
them apart too (the mlir backend escapes the '.' as `_2E` in its link
names, and a vtable's starts `pkvt_` where a function's starts `pk_`).

## 6. Statements and instructions

Statement forms (each on its own line; blocks are `{ ... }`):

```
if %c { ... } else { ... }          ; else optional
while { ...; cond %c } { ... }      ; condition region ends with `cond`; body may break/continue
break                               ; leave the innermost while
continue                            ; re-enter the innermost while's condition region
ret %v   |   ret                    ; return from the function
unreachable                         ; after a noreturn call (panic)
<instruction>
```

Nothing may follow `break`, `continue`, `ret` or `unreachable` in the same
block.  An `if` whose `then` and `else` blocks both end that way is itself a
terminator (nothing may follow it either, and a non-void function may end
with one); a `while` never is.  Instructions (`%r = op ...` when they produce a value):

**Arithmetic and comparison** (both operands the same type; `i64` or `f64`
unless stated):

```
%r = add %a, %b | sub | mul | div | rem        ; `div`/`rem` on i64 are plain:
                                               ; the lowering guards 0 and INT64_MIN/-1
%r = neg %a                                    ; i64 or f64
%r = not %a                                    ; bool
%r = cmp eq %a, %b | ne | lt | le | gt | ge    ; -> bool; also on bool and char (signed);
                                               ; eq/ne also on box/obj/ptr
%r = select %c, %a, %b                         ; bool ? a : b (no side effects)
%r = itof %a                                   ; i64 -> f64 (numeric)
%r = ftoi %a                                   ; f64 -> i64 (numeric, toward zero);
                                               ; only for %a in [-2^63, 2^63):
                                               ; the lowering guards the range
%r = cast %a to T                              ; reinterpret / resize, see table
```

**Sized numeric types** (`docs/design/sized-types.md`): every sized integer is
held in an `i64` in canonical form (a signed one sign-extended, an unsigned one
zero-extended) and every sized float in an `f64`; these restore the form after
an operation, and give `unsigned` (`u64`) its unsigned semantics:

```
%r = wrap 8 %a                                 ; i64 -> i64: the low 8 bits, sign-extended (8, 16 or 32)
%r = mask 16 %a                                ; i64 -> i64: the low 16 bits, zero-extended (8, 16 or 32)
%r = udiv %a, %b | urem                        ; i64: unsigned quotient / remainder (the lowering guards 0)
%r = ucmp lt %a, %b | le | gt | ge | eq | ne   ; i64 -> bool: unsigned comparison
%r = utof %a                                   ; i64 -> f64: the unsigned reading of the bits
%r = ftou %a                                   ; f64 -> i64: toward zero, as uint64 bits; only for
                                               ; %a in [0, 2^64): the lowering guards the range
%r = fround32 %a                               ; f64 -> f64: rounded to the nearest f32
```

(`f16` rounding is the runtime call `@$rt.Paykan_round_f16(f64) -> f64`.)

On `f64`, `cmp` follows IEEE 754: `ne` is *unordered* (true when either
operand is NaN; it is exactly `not (eq a, b)`), and `eq`, `lt`, `le`, `gt`,
`ge` are *ordered* (false when either operand is NaN).  So `x != x` is true
for a NaN `x`, and every other comparison involving a NaN is false.  These
are LLVM's `fcmp une` / `oeq olt ole ogt oge` and C's `!= == < <= > >=`.

`cast` pairs: `f64->i64` and `i64->f64` reinterpret the bits; `bool->i64`,
`char->i64` zero-extend; `i64->bool`, `i64->char` truncate; `box<->i64`,
`obj<->i64`, `ptr<->i64` are pointer/int casts; `obj<->ptr`, `box<->ptr`,
`box<->obj` are free.  Nothing else.

**Calls**

```
%r = call @f(%a, %b)                          ; direct; `call @f(...)` when void
%r = vcall %recv : Point [3] (%a)             ; virtual: slot 3 of the vtable of
                                              ; `%recv`'s class (static class and
                                              ; signature come from the class item or,
                                              ; for runtime classes, are written out:
%r = vcall %recv : (obj, box) -> i64 [2] (%b) ; explicit signature form)
%p = fn.addr @f                               ; the address of function @f: ptr
%r = icall %p : (obj, i64) -> i64 (%c, %a)    ; indirect: a call through the
                                              ; function address %p (a function
                                              ; value's code), signature written out
```

The callee signature is always statically known: for `call` it is the
declaration, for `vcall` it is the vtable slot's signature, for `icall` the
one written (the verifier checks the arguments against it; that the address
names a function of that signature is the lowering's guarantee).  `fn.addr`
names a function of the module or one it declares.  Arguments are
passed as-is; the lowering has already done every conversion (`bool`→`i64` for
runtime `int64_t` params, boxing, unboxing).

**ARC and objects**

```
retain %b            ; box -> void              Paykan_retain
release %b           ; box -> void              Paykan_release (null-safe)
%b = box %o          ; obj -> box               PaykanShared_new: the object, strong +1
%o = unbox %b        ; box -> obj               the identity (null -> null)
%w = weak.new %b     ; box -> weak              Paykan_weak_new: a weak reference (null -> null)
%b = weak.load %w    ; weak -> box              Paykan_weak_load: a +1 box while alive, else null
weak.release %w      ; weak -> void             Paykan_weak_release (null-safe)
%o = new Point       ; allocate + header init + zeroed fields (Paykan_malloc)
free %o              ; Paykan_free of the object struct (an object never boxed)
%v = field.load %o, Point.x
field.store %o, Point.x, %v                    ; plain store: ARC is explicit around it
%p = vtable.load %o                            ; the object's vtable pointer (ptr)
%p = vtable.addr Point   |   vtable.addr @$rt.PaykanString_vtable
```

**Locals**

```
%v = load %x
store %x, %v
```

**Inline optionals**

```
%o = some %v                                   ; i64/f64/bool/char -> its optional
%n = none opt_i64                              ; the absent optional of that type
%b = is_some %o                                ; optional -> bool
%v = unwrap %o                                 ; optional -> payload; zero when absent
```

**Addresses** (`inout` parameters, `mem` slots)

```
%p = local.addr %x                             ; the address of local %x (ptr)
%p = field.addr %o, Point.x                    ; the address of a field of %o (ptr)
%v = ptr.load i64, %p                          ; the i64 at %p
ptr.store %p, %v                               ; store %v at %p
%q = ptr.add %p, %off                          ; %p plus %off (i64) bytes (ptr)
mem.copy %dst, %src, %n                        ; copy %n (i64) bytes; the two may overlap
mem.zero %p, %n                                ; set %n (i64) bytes to zero
```

An `inout` parameter is a `ptr` parameter: the caller passes the address of
its storage, and the callee reads and writes it in place with `ptr.load` and
`ptr.store`.  Scalar, optional and box slots have addresses: `local.addr`
takes an `i64`, `f64`, `bool`, `char`, optional, `box` or `mem` local, `field.addr` a
field of any type, and `ptr.load` / `ptr.store` read and write every one of
those types but `mem`.  A `mem` slot is the bytes of an aggregate: its parts
are reached with `ptr.add` from its address and read and written with
`ptr.load` / `ptr.store` at their own types, and the whole is copied with
`mem.copy` and cleared with `mem.zero`.  A new object's `mem` fields are
zero, like its other fields; a `mem` local starts uninitialised.  Like
`ptr.store`, `mem.copy` and `mem.zero` do no ARC: the lowering retains what
a copy duplicates and releases what a store or a clear overwrites.  A
fixed-size array or a tuple is passed as the `ptr` address of its bytes in
every mode (a by-value callee copies it into a `mem` local of its own
first), and a function returning one takes a last `ptr` parameter, the
address of the caller's temporary it writes the result to, and returns
`void`.  A slice
parameter (`view T[]`, `inout T[]`) is two: the `ptr` address of the
elements and their number, an `i64`.  The verifier cannot
know what an address points to, so the lowering always reads and writes it
with the slot's own type.  A local's address is valid until its function
returns; a field's address while its object is alive, which the caller
guarantees by keeping the object retained for the duration of the call.  An
`inout` local (`x: inout = k;`) is a `ptr` local too, holding the address of
what it names; a hidden owned local keeps a field's object alive for as long
as the local can be used.  An address is only ever kept in a `ptr` local,
never in a field or an array or tuple element.  Like `store` and `field.store`,
`ptr.store` does no ARC.  An `inout` of a reference type holds the address of
a `box` slot, not a reference of its own: it retains nothing, and an
assignment through it releases the box it loads from the slot before storing
the new one, just as an assignment to the variable would.

## 7. What the lowering makes explicit (the ownership rules)

This is the contract backends rely on.  It is the same set of rules the LLVM
CodeGen enforced, now in one place.

* **Variables** hold `box` values and own one reference.  Declaring a variable
  from an expression yields an owned box: a fresh +1 box (call, method call,
  ternary, call-rooted field read) is stored as-is; a borrowed
  box (plain field read, tuple element, array element, another variable) is
  `retain`ed first; a raw object
  (string literal, concat result, `self`, a match-arm binding) is `box`ed, which
  creates the object's unique box or acquires the existing one.
* **Scope exit** releases the scope's owned variables in reverse declaration
  order, then any pending temporaries (a match subject), on every path:
  fall-through, `ret` (all scopes), `break`/`continue` (scopes inside the loop).
* **Arguments** to user functions/methods, `__super__` and the virtual `equals`
  are owned (+1) boxes that the callee releases on exit (callee-consumes).
  Arguments to runtime builtins are borrowed; temporaries are released after the
  call.
* **Returns** of reference types are +1 boxes.  Runtime methods returning a
  reference also return a +1 box.
* **Fixed-size arrays of references** own one reference per element slot,
  as variables do: a copy (`mem.copy`) is followed by a `retain` of every
  element, an assignment releases the elements it overwrites, and scope
  exit releases a variable's (one by one, or in a `while` past four).  A
  call's result or a literal owns its elements: they move into the place it
  initialises, and are released after any other use (an element read from
  it is retained first).  A `[v; n]` literal stores `v` in slot 0 and
  copies it into the others (one by one for up to four more, else in a
  `while`), retaining a reference once per extra slot and a nested array's
  references after its `mem.copy`; a zero `v` (`0`, `0.0`, `False`, `'\0'`)
  of a scalar element type is one `mem.zero` instead.  A field is a `mem`
  field reached with `field.addr`, and `C.destroy` releases its references
  too.
* **Tuples** follow the same rules: a tuple's reference elements (a nested
  tuple's included) are owned, one reference per copy.  A tuple literal is
  built in place, each element stored at its offset (a literal assigned to
  an existing tuple is built in a temporary first, then copied); copying a
  tuple is a `mem.copy` followed by a `retain` of each reference element;
  an assignment releases the ones it replaces; scope exit releases a
  variable's, `C.destroy` a field's, and an array releases its tuples'
  with its other elements.  Printing a tuple, `toString()` and `==`/`!=`
  pass its address (both operands' for `==`) and its `.tupleN` layout to
  `PaykanTuple_toString` (a new string `obj`, a temporary like a
  concatenation's: destroyed after a print, boxed when kept) and
  `PaykanTuple_equals` (`i64`, nonzero when equal: `==` is its `cmp ne
  ..., 0`, `!=` the `not` of that).
* **Structs** (docs/language/11-structs.md) are tuples with names: the same
  `mem` storage, copies, retains and releases, with a field read as
  `ptr.add` of the field's offset from the struct's address and a field
  assignment a `ptr.store` there (a `mem` field a `mem.copy`), after the
  release of the reference it replaces.  A method `S.m` takes the
  receiver's address as its first `ptr` parameter (`self`, declared as an
  `inout` parameter: it reads and writes the receiver's storage), then its
  parameters, then the result slot of a `mem` or `inout` result; a `static
  fn` is a function `S::f`; the constructor `S` takes the `__init__`'s
  parameters and a last `ptr`, the caller's storage, which it zeroes before
  calling `S.__init__` on it, and returns `void`.  Printing, `toString()`
  and `==`/`!=` pass the address and the `.structN` layout to
  `PaykanStruct_toString` and `PaykanStruct_equals`, with the conventions
  of the tuple calls.
* **Function values** (docs/language/14-function-types.md,
  docs/design/function-types.md) are held in memory, 16 bytes laid out as
  the tuple `(i64, box)`: the code's address cast to `i64`, then the box of
  the closure, or null.  The code of a lambda is `fn.<module>.N.call` and of
  a named function used as a value `fn.ref.<module>.<function>.call`, each
  taking the closure object first, its parameters as a method's.  A lambda
  that captures something has a closure, an object of the class
  `fn.<module>.N` with a field per capture and an object's vtable.  Making
  one is `fn.addr` of the code and, for a closure, `new`, a `field.store`
  per capture and `box`; a call loads both words, retains the closure for
  the call and `icall`s the code with the function type's signature.  An
  optional function value is the same 16 bytes, None all zeros.
* **Fields** own their box; `field.store` is preceded by a retain-or-steal
  of the new value and the old value is released (null-checked).
* **Weak references** (docs/language/16-memory.md): a `weak` local is
  a `local` of type `weak`, a `weak` field a field of type `weak`.  A
  declaration or an assignment takes the value as a +1 box (as a variable's
  initializer is taken), stores `weak.new` of it and releases the box; an
  assignment `weak.release`s the old value first (a field's too).  A read
  (`load` or `field.load`, then `weak.load`) is a fresh +1 box, or null:
  it is owned like a call's result (released after a print or a comparison,
  pending for a `match`, stored as-is in a variable or a field, passed as
  an argument).  Scope exit `weak.release`s a `weak` local in declaration
  order with the owned variables, and `C.destroy` `weak.release`s a `weak`
  field.  A closure captures a `weak` variable as a `weak` field: stored
  with `weak.new` from the variable's read, released by the closure's
  destroy, and loaded (`weak.load`) into an owned local at the start of a
  call.
* **Indexing** an array or a slice checks the index first: when it is
  negative or not below the length, `call @$rt.Paykan_panic_index(%i, %len)`
  and `unreachable`.  The element is then read or written in place, with
  `ptr.load` / `ptr.store` at `ptr.add` of the index times the element size.
* **Temporaries**: a raw `PaykanString*` produced by a literal, `Str<int>` & co.
  or concatenation is destroyed and freed (`Paykan_dispose`) by the consumer
  unless it is boxed; a fresh box used as a receiver or borrowed argument is
  released after the use.
* **Match**: the subject is evaluated once; a boxed subject is owned for the
  match's duration (pending release at match end, also on early exits).  Class
  arms compare `vtable.load` against `vtable.addr`; optional subjects test
  `null` first; value arms compare literals (Str through
  `PaykanString_equals` with a boxed literal); enum arms compare `i64`
  constants.  Arm bindings are raw `obj` aliases (unowned).
* **Optionals**: `None` into a reference `T?` is `null`.  An optional is
  never an `Obj` (Sema rejects that).  An optional
  primitive (`int?`, `float?`, `bool?`, `char?`) is a value
  (`opt_i64` & co.) with no ARC: a primitive that Sema marked for an
  optional primitive slot is `some` of it (after `int`→`float` promotion for
  a `float?`), `None` there is `none`, `x == None` is `not (is_some x)`, two
  of them are equal when their `is_some` agree and, both present, their
  `unwrap`s are `==`, and a `match` arm binding the primitive is its
  `unwrap`.  An optional's `toString()` (optionals are `Show`) is a branch
  on `is_some`: the value's text (`PaykanString_from_int` & co. for a
  primitive) when present, `"None"` otherwise.  An
  element of an `int?[n]` & co. is the optional itself, the optional's size
  in the array's `mem` (16 bytes for `int?` and `float?`, 2 for `bool?` and
  `char?`), read and written in place, and so is an `int?` element of a
  tuple, at its offset.
* **Conversions** (`Target<Source>(value)`, #64): `Str<…>` calls
  `PaykanString_from_int` & co. (an owned string temporary); `int<Str>` /
  `float<Str>` / `bool<Str>` call `PaykanString_parse_int` /
  `PaykanString_parse_float` / `PaykanString_parse_bool` with the
  `local.addr` of a local the value is parsed into, and are the `select` of
  `some` of that value (a valid string) or `none` (`bool<Str>` accepts
  exactly `"True"` and `"False"`).  The numeric ones are inline: `int<float>` checks
  `cmp ge %f, -2^63` and `cmp lt %f, 2^63` (both false for NaN) and calls
  `@$rt.Paykan_panic_float_to_int(%f)` then `unreachable` otherwise, before the
  `ftoi`; `float<int>` is `itof`; `int<bool>` and `int<char>` are `cast`s to
  `i64` (zero-extending, so a char's code is 0..255); `bool<int>` is
  `cmp ne %n, 0`; `char<int>` panics outside 0..255
  (`@$rt.Paykan_panic_int_to_char`) and maps 128..255 to the same byte's signed
  value before the truncating `cast`, so no backend converts an out-of-range
  value.
* **Division**: integer `div` is preceded by `if %b == 0 { call @$rt.Paykan_panic_div_by_zero(); unreachable }`
  and the `INT64_MIN / -1` check; `rem` replaces a `-1` divisor by `1`.
* **Sized numeric types**: an arithmetic result of a narrow type is followed
  by its `wrap N` / `mask N` (`u8` and `i8` & co.), `fround32` (`f32`) or
  `@$rt.Paykan_round_f16` call (`f16`); an `unsigned` uses `udiv`, `urem`
  and `ucmp`, and the `INT64_MIN / -1` check is only emitted for `int`.  The
  conversions between numeric types are the same instructions (`u8<int>` is
  `mask 8`, `float<unsigned>` is `utof`, `unsigned<float>` guards `[0, 2^64)`
  then `ftou`, `i8<float>` guards `(-129, 128)` and calls
  `@$rt.Paykan_panic_float_to_type(%f, @.strN, 2)` otherwise, then `ftoi`);
  `Str<unsigned>` and `unsigned<Str>` call `PaykanString_from_uint` /
  `PaykanString_parse_uint`, and a parse into a narrow integer compares the
  parsed `i64` against the type's range before the `some`.

## 8. Runtime ABI (extern declarations the lowering may emit)

Signatures in PIR types, by C symbol: each is declared under its runtime
name (`extern fn @$rt.Paykan_free(ptr) -> void`, `extern obj
@$rt.PaykanObject_None`, §3).  `bool` never appears at the ABI: a Paykan `bool`
crossing into the runtime is `cast` to `i64` first.

```
; memory / RC (first-class ops, listed for completeness)
Paykan_malloc(i64) -> ptr        Paykan_free(ptr) -> void
Paykan_retain(box) -> void       Paykan_release(box) -> void
PaykanShared_new(obj) -> box     PaykanShared_get(box) -> obj
Paykan_weak_new(box) -> weak     Paykan_weak_load(weak) -> box
Paykan_weak_release(weak) -> void
Paykan_dispose(obj) -> void      ; destroy and free an object never boxed (a string temporary)
; panics (noreturn)
Paykan_panic_div_by_zero() -> void      Paykan_panic_div_overflow() -> void
Paykan_panic_float_to_int(f64) -> void  Paykan_panic_int_to_char(i64) -> void
Paykan_panic_index(i64, i64) -> void    ; the index, the length
Paykan_panic_float_to_type(f64, ptr, i64) -> void   ; the value, the target type's name and its length
; sized numeric types
Paykan_round_f16(f64) -> f64            ; rounded to the nearest f16
; strings
PaykanString_new(ptr, i64) -> obj       PaykanString_destroy(obj) -> void
PaykanString_concat(obj, obj) -> obj    PaykanString_char_at(obj, i64) -> char
PaykanString_from_int(i64) -> obj       PaykanString_from_float(f64) -> obj
PaykanString_from_bool(i64) -> obj      PaykanString_from_char(char) -> obj
PaykanString_equals(obj, box) -> i64    PaykanString_toString(obj) -> box
PaykanString_length(obj) -> i64
PaykanString_parse_int(obj, ptr) -> i64     PaykanString_parse_float(obj, ptr) -> i64
PaykanString_parse_bool(obj, ptr) -> i64    ; 1 and the value at ptr, or 0 (and 0 there)
PaykanString_from_uint(i64) -> obj      PaykanString_parse_uint(obj, ptr) -> i64   ; `unsigned`
; hash (the builtin Hashable's `hash()`)
PaykanInt_hash(i64) -> i64              PaykanString_hash(obj) -> i64
; tuples: the address of the value (of both, for equals), then its layout
PaykanTuple_toString(ptr, ptr) -> obj   PaykanTuple_equals(ptr, ptr, ptr) -> i64
; structs: likewise, with a struct layout
PaykanStruct_toString(ptr, ptr) -> obj  PaykanStruct_equals(ptr, ptr, ptr) -> i64
; vtable slot implementations (inherited into user vtables)
PaykanObject_destroy(obj) -> void   PaykanObject_toString(obj) -> box
PaykanObject_equals(obj, box) -> i64
; globals
extern obj PaykanObject_None
; runtime classes' vtables, their type identities for `match`
extern vtable PaykanObject_vtable   extern vtable PaykanString_vtable
```

An executable's entry code, not PIR, calls the two functions that make and
drop the program's arguments (§4):

```
Paykan_args_new(int argc, char **argv, int64_t *count) -> PaykanShared **
Paykan_args_free(PaykanShared **args, int64_t count) -> void
```

`names::kCodeGenRequiredSymbols` in `include/Names.h` is the authoritative
list; the mlir backend's JIT symbol table must cover it, and an executable links
`libpaykan_runtime.a`.

Vtable slot layouts of runtime classes (slot index = position):

* `Obj`: `destroy, toString, equals`
* `Str`: `destroy, toString, equals, len, concat`

Printing and files are not runtime classes or builtins: they are the
standard library's `::io` module, whose `native fn`s call the runtime's
`Paykan_print` & co. and `Paykan_io_*` functions like any native function.

An array is not an object and has no vtable: `len()` is its type's length,
or a slice's `i64` length.  Nor is a tuple: its `toString()` and `==` are the
`PaykanTuple_*` calls above.

## 9. Verifier

The verifier rejects a program when:

* a value is used before its definition or outside the block that defines it;
  a `local` is loaded/stored with the wrong type or used without a declaration;
* an instruction's operand types do not match the op (arithmetic type mismatch,
  `cmp` on different types or on optionals, a disallowed `cast` pair,
  non-`bool` condition, `some` of anything but `i64`/`f64`/`bool`/`char`,
  `none` of a type that is not an optional, `is_some`/`unwrap` of a value that
  is not one; `wrap`/`mask`/`udiv`/`urem`/`ucmp`/`utof` of anything but `i64`,
  `ftou`/`fround32` of anything but `f64`, a `wrap`/`mask` width other than 8,
  16 or 32);
* a `call` names an undeclared function or passes the wrong arity/types; a
  `vcall` slot is out of range of the named class's vtable or the argument list
  does not match the slot's signature; an `icall`'s callee is not a `ptr` or
  its arguments do not match the signature it writes; `fn.addr` names a
  function the module does not declare;
* `field.load`/`field.store`/`field.addr`/`new`/`vtable.addr` name an
  unknown class or field; a field store has the wrong type;
* a class extends an unknown class or declares a field twice, a field has
  a type that is not a field type (§5), a defined class's vtable slot 0 is
  not `destroy`, or an `extern class` names no module;
* `weak.new` takes anything but a `box`, `weak.load` or `weak.release`
  anything but a `weak`; a `weak` is cast or its address taken (that a
  `weak` is never compared, §1, is the lowering's guarantee: `cmp` does not
  check it);
* `local.addr` or `field.addr` takes the address of a slot that is not `i64`,
  `f64`, `bool`, `char`, an optional, `box` or `mem`; `ptr.load`/`ptr.store`
  take an address that is not a `ptr`, or read or write a value of another
  type (`mem` included); `ptr.add`, `mem.copy` or `mem.zero` take an operand
  that is not a `ptr` address or an `i64` count;
* a value, a parameter or a result has type `mem`, or a `mem` local or field
  is read or written with `load`/`store`/`field.load`/`field.store`; a `mem`
  slot's size is not a positive multiple of its alignment, or its alignment
  is not 1, 2, 4 or 8;
* a runtime or native extern takes or returns an optional;
* `break`/`continue` appear outside a `while` or in its condition region,
  or a condition region ends the function; a statement follows a
  terminator in the same block (a terminator is `break`, `continue`, `ret`,
  `unreachable`, or an `if` whose two branches both end in one); `ret`
  carries the wrong type; a non-void function's body can fall off the end
  (its last statement is not a `ret`, `unreachable` or terminating `if`);
* a module-level symbol is defined twice, or a vtable entry's signature does not
  match the named function's declaration;
* a runtime extern (`extern fn` without a `module`, `extern obj`, `extern
  vtable`) is not named `$rt.<symbol>` (a module-less `extern fn` may instead
  be a native `$c.<symbol>`), or anything else (a function, a module extern's
  `symbol`, a `cstr`/`data`/`bytes` global) has either name;
* `@main` has a signature other than `() -> i64` or `(ptr, i64) -> i64`.

Verifying a whole program (`verify(const Program &)`) adds the checks
across modules: a module appears twice; a module extern (`extern fn ...
module`, `extern class`) names a module that is not in the program, or a
function or class that module does not define, or one whose signature or
field layout differs from the definition; the main module defines no
`@main`.

## 10. Text format summary

The text form is also what a loadable backend plugin receives
([`plugins/pir-for-backends.md`](plugins/pir-for-backends.md)), so it is
versioned, by the release like every format: `PAYKAN_PIR_TEXT_VERSION` in
`include/paykan/plugin_api.h` is the release version packed as
major * 10000 + minor * 100 + patch (200 for 0.2.0), the same number the binary
form stores.

```
module "01"
cstr @.str0 = "Hello" len 5
extern fn @$rt.PaykanString_new(ptr, i64) -> obj
extern fn @$rt.Paykan_println(obj) -> void
extern fn @$rt.Paykan_dispose(obj) -> void

fn @main() -> i64 {
  %s = call @$rt.PaykanString_new(@.str0, 5)
  call @$rt.Paykan_println(%s)
  call @$rt.Paykan_dispose(%s)
  ret 0
}
```

Comments start with `;` and run to the end of the line.  Identifiers after `@`
and `%`, class names and field names may contain any character except
whitespace, `(`, `)`, `,`, `[`, `]`, `{`, `}`, `:` and `"` (so `@Box<int>` and
`@first<int>` are valid); a name containing those characters is written quoted:
`@"helper::add"`, `"helper::Adder".n`.  A named value or local keeps its
`.N` index outside the quotes: `%"Pair<Str, int>.shared".4`.  The printer
(`paykan::pir::print`) emits exactly this format and the parser (`paykan::pir::parseProgram`) reads
it back; `paykan::pir::verify` checks §9.  A printed module separates its
extern declarations from its definitions with blank lines, which the parser
ignores.

## 11. Binary form

A module also has a binary encoding, the `CODE` section of a `.pkm` module
file: `paykan::pir::binary::encode` / `decode` in
`include/paykan/pir/Binary.h`.  It is a one-to-one encoding of the in-memory
form, not of the text, so a decoded module goes through the same verifier
and backends and `print(decode(encode(m))) == print(m)` byte for byte.  The
format is specified in [`design/pkm.md`](design/pkm.md) §5 (layout, type and
opcode codes, records, determinism) and §6.1 (the symbol index); in short:

- **Layout.** Magic `PIRB`, codec version (1.0), PIR version
  (`pir::kPIRVersion`, `include/paykan/pir/Version.h`; the same number as
  `PAYKAN_PIR_TEXT_VERSION`), flags, a string table, the module name, then
  the item tables in the printer's order (extern globals, cstrs, datas,
  bytes, classes, functions; two reserved tables are empty today), and the
  trailer `BRIP`.  Integers are minimal LEB128, `f64` constants are their
  raw bits (a NaN payload and `-0.0` survive, which the text form does not
  guarantee), and every name is an index into the string table.
- **Codes.** Types and opcodes are written as the fixed codes of
  `include/paykan/pir/Codes.h`, the one table that also holds the mnemonics
  of §10, never as the C++ enumerator's value; a reader rejects a code it
  does not know.
- **Functions** are length-prefixed records, so a reader can skip or decode
  one function on its own (`decodeFunction`), and the symbol index
  (`index`, `encodeSymbolIndex`) lists every record with its offset, length
  and SHA-256.
- **Determinism.** One module has one encoding: strings are interned in
  first-use order, reserved fields are zero, and `encode(decode(b)) == b`
  for every blob a reader accepts.  `EncodeOptions::StripNames` drops value
  and local names (flag bit 0); symbols keep theirs.
- **Reading is not verifying.**  `decode` checks the framing (bounds,
  versions, reserved codes and bits, slack bytes, nesting depth) and
  nothing else; run `paykan::pir::verify` on the result as on a parsed
  module.

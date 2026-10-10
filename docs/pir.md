# PIR: the Paykan intermediate representation

PIR is the small, backend-neutral IR that sits between Sema's typed AST and
every backend (C, LLVM, out-of-tree).  It is produced by **one** lowering
pass (`src/Lowering`), which is the single home of the ownership rules: by the
time a backend sees PIR, every `retain`/`release`/`box`/`unbox`, every scope
cleanup, every `match` dispatch and every vtable is explicit.  A
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
  detail.  LP64 is assumed by the runtime ABI (every array/tuple slot is 8
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
| `i64` | 64-bit signed integer; also enum values and raw 8-byte slot bits  | `int64_t`    |
| `f64` | IEEE-754 double                                                   | `double`     |
| `bool`| true/false                                                        | `int64_t` or `_Bool` (backend's choice; `0`/`1` at the ABI) |
| `char`| one byte                                                          | `int8_t`     |
| `box` | `PaykanShared*`: an owned-or-borrowed reference-counted box; may be `null` (an optional's `None`) | `PaykanShared *` |
| `obj` | raw `PaykanObject*`: an unboxed heap object of any class (also `PaykanString*`, `PaykanArray*`, `PaykanTuple*`, `PaykanFile*`, `PaykanError*`, boxed primitives) | `PaykanObject *` (cast at runtime calls) |
| `ptr` | any other pointer: vtable addresses, literal data, kind descriptors | `void *`  |

Paykan types lower as: `int`→`i64`, `float`→`f64`, `bool`→`bool`,
`char`→`char`, every enum→`i64`, every reference type (class, `Str`, array,
tuple, optional)→`box` when stored (variable, field, parameter, return value,
array/tuple ref slot) and `obj` when used as a raw receiver.  A `T?` is a `box`
that may be `null`.

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
like LLVM entry-block allocas) and read/written with `load`/`store`.  Several
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
ordinary `@PaykanString_new` (C `pk_<module>_PaykanString_new`, LLVM
`pk.PaykanString_new`), whatever its signature, and the program's own string
literals still call `@$rt.PaykanString_new`.  The verifier enforces both
directions (§9).

**Native names.**  The C function a `native fn` calls (#198) is a module-less
extern named `$c.<C symbol>`: `native fn put(fd: int, s: Str) -> int =
"pk_put"` declares `extern fn @$c.pk_put(i64, obj) -> i64`.  Its signature is
not the runtime's, so backends declare it from the PIR signature (C:
`int64_t pk_put(int64_t, PaykanObject *);`) and link it by the name without
`$c.` (`pir::externCSymbol`).  The `native fn` itself is an ordinary function
`@put` whose body unboxes each reference parameter (borrowed by the C call),
calls `@$c.pk_put`, releases its parameters and returns the result; a
reference result is the C function's owned box.  The objects that define the
`$c.` symbols are not part of PIR: the driver hands them to the backends
(`backend::Input::Objects`, `--object`).

## 4. Module-level items

```
module "<name>"                     ; the canonical module name (`geometry::shapes`; the main module: its file stem)

cstr  @.str0  = "hello\n"  len 6   ; NUL-terminated literal data (`ptr`)
data  @.arr0  = [1, 2, 3]          ; constant i64 words, primitive array literals (`ptr`)
bytes @.tk0   = [0, 4]             ; tuple slot-kind descriptor, PaykanTupleKind bytes (`ptr`)

extern obj    @$rt.PaykanObject_None   ; runtime object singletons (`obj`)
extern vtable @$rt.PaykanArray_vtable  ; runtime vtable globals (`ptr`)

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

Every symbol a function body references must be declared in its module: a call
to a runtime function needs the `extern fn`, a field access needs the `class`
(or `extern class`), a `vtable.addr` needs the class or `extern vtable`.  The
lowering emits these declarations; the verifier checks them.

A **program** is the list of PIR modules reachable from the main module
(main module first, each module exactly once).  Linking them is a backend
concern.  The entry point is the main module's `@main`, which takes either no
parameters or one `box` parameter (an owned `Str[]` of program arguments, which
`main` releases like any owned parameter) and returns `i64`.

## 5. Classes, layouts and vtables

A `class` item gives the complete object layout and the complete vtable; the
backend does no inheritance walk.

* **Layout**: the two-word runtime header (vtable pointer, unique-box
  backpointer; `struct PaykanObject` in `Runtime.h`) followed by *all* fields in
  declaration order, ancestors first.  Field types are `i64 f64 bool char box`.
* **Vtable**: one entry per slot in slot order, each a function symbol (a
  function of this module, an `extern fn`, or `null` for an abstract slot) plus
  its signature.  Slot 0 is always `destroy`.  The vtable's address is the
  class's runtime type identity (`vtable.addr`).
* A class marked `extern` belongs to another module: its fields are known (for
  `field.load`/`field.store` on imported instances) and its vtable address may
  be taken, but its vtable contents and functions are defined in that module.
* Runtime classes (`Obj`, `Str`, `File`, `Error`, `Int`, `Float`, `Bool`,
  arrays, tuples) are never declared as classes; their vtables are
  `extern vtable` globals and their methods are called through `vcall` with an
  explicit slot index, exactly like user classes.

Constructors and destructors are ordinary functions produced by the lowering:
`@Point(args...) -> box` allocates with `new`, installs the vtable, nulls the
backpointer, zeroes the fields, boxes the object (`box`, before `__init__`, so
`self` inside `__init__` recovers the caller's box) and calls
`@Point.__init__(obj, args...)`; `@Point.destroy(obj)` releases every
ref-typed field and frees the struct (`free`).  `destroy` is entirely
compiler-generated: it is final, so a class cannot write or override it.  Every
method `m` of a class `C` is the function `@C.m(obj self, params...)`.  The
'.' cannot occur in a Paykan identifier, so these names never clash with a
program function (`fn Point_move`, `fn Point_destroy`); backends must keep
them apart too (the C backend escapes the '.' and uniquifies clashing C
names; the LLVM backend names a vtable `<class>..vtable`, which no method
symbol spells).

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
%r = cmp eq %a, %b | ne | lt | le | gt | ge    ; -> bool; eq/ne also on box/obj/ptr
%r = select %c, %a, %b                         ; bool ? a : b (no side effects)
%r = itof %a                                   ; i64 -> f64 (numeric)
%r = ftoi %a                                   ; f64 -> i64 (numeric, toward zero);
                                               ; only for %a in [-2^63, 2^63):
                                               ; the lowering guards the range
%r = cast %a to T                              ; reinterpret / resize, see table
```

On `f64`, `cmp` follows IEEE 754: `ne` is *unordered* (true when either
operand is NaN; it is exactly `not (eq a, b)`), and `eq`, `lt`, `le`, `gt`,
`ge` are *ordered* (false when either operand is NaN).  So `x != x` is true
for a NaN `x`, and every other comparison involving a NaN is false.  These
are LLVM's `fcmp une` / `oeq olt ole ogt oge` and C's `!= == < <= > >=`.

`cast` pairs: `f64->i64` and `i64->f64` reinterpret the bits (array/tuple
slots); `bool->i64`, `char->i64` zero-extend; `i64->bool`, `i64->char`
truncate; `box<->i64`, `obj<->i64`, `ptr<->i64` are pointer/int casts (slot
bits); `obj<->ptr`, `box<->ptr` are free.  Nothing else.

**Calls**

```
%r = call @f(%a, %b)                          ; direct; `call @f(...)` when void
%r = vcall %recv : Point [3] (%a)             ; virtual: slot 3 of the vtable of
                                              ; `%recv`'s class (static class and
                                              ; signature come from the class item or,
                                              ; for runtime classes, are written out:
%r = vcall %recv : (obj, box) -> i64 [2] (%b) ; explicit signature form)
```

The callee signature is always statically known: for `call` it is the
declaration, for `vcall` it is the vtable slot's signature.  Arguments are
passed as-is; the lowering has already done every conversion (`bool`→`i64` for
runtime `int64_t` params, boxing, unboxing).

**ARC and objects**

```
retain %b            ; box -> void              Paykan_retain
release %b           ; box -> void              Paykan_release (null-safe)
%b = box %o          ; obj -> box               PaykanShared_new: create-OR-acquire (+1)
%o = unbox %b        ; box -> obj               PaykanShared_get (pure read; null -> null)
%o = new Point       ; allocate + header init + zeroed fields (Paykan_malloc)
free %o              ; Paykan_free of the object struct (destructors only)
%v = field.load %o, Point.x
field.store %o, Point.x, %v                    ; plain store: ARC is explicit around it
%p = vtable.load %o                            ; the object's vtable pointer (ptr)
%p = vtable.addr Point   |   vtable.addr @$rt.PaykanArray_obj_vtable
```

**Locals**

```
%v = load %x
store %x, %v
```

**Addresses** (`inout` parameters)

```
%p = local.addr %x                             ; the address of local %x (ptr)
%p = field.addr %o, Point.x                    ; the address of a field of %o (ptr)
%v = ptr.load i64, %p                          ; the i64 at %p
ptr.store %p, %v                               ; store %v at %p
```

An `inout` parameter is a `ptr` parameter: the caller passes the address of
its storage, and the callee reads and writes it in place with `ptr.load` and
`ptr.store`.  Scalar and box slots have addresses: `local.addr` takes an
`i64`, `f64`, `bool`, `char` or `box` local, `field.addr` a field of one of
those types (every field type), and `ptr.load` / `ptr.store` read and write
those types.  The verifier cannot
know what an address points to, so the lowering always reads and writes it
with the slot's own type.  A local's address is valid until its function
returns; a field's address while its object is alive, which the caller
guarantees by keeping the object retained for the duration of the call.  An
`inout` local (`x: inout = k;`) is a `ptr` local too, holding the address of
what it names; a hidden owned local keeps a field's object alive for as long
as the local can be used.  An address is only ever kept in a `ptr` local,
never in a field or an array or tuple slot.  Like `store` and `field.store`,
`ptr.store` does no ARC.  An `inout` of a reference type holds the address of
a `box` slot, not a reference of its own: it retains nothing, and an
assignment through it releases the box it loads from the slot before storing
the new one, just as an assignment to the variable would.

## 7. What the lowering makes explicit (the ownership rules)

This is the contract backends rely on.  It is the same set of rules the LLVM
CodeGen enforced, now in one place.

* **Variables** hold `box` values and own one reference.  Declaring a variable
  from an expression yields an owned box: a fresh +1 box (call, method call,
  ternary, array/tuple literal, call-rooted field or element read) is stored
  as-is; a borrowed box (plain field read, tuple element, array element,
  another variable) is `retain`ed first; a raw object
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
* **Fields and array/tuple ref slots** own their box; `field.store` is preceded
  by a retain-or-steal of the new value and the old value is released
  (null-checked); `PaykanArray_set_obj`/`push_obj`/`PaykanTuple_set_obj` retain
  internally, so the +1 temporary passed to them is released afterwards.
* **Temporaries**: a raw `PaykanString*` produced by a literal, `Str<int>` & co.
  or concatenation is destroyed (`PaykanString_destroy`) by the consumer unless
  it is boxed; a fresh box used as a receiver or borrowed argument is released
  after the use.
* **Match**: the subject is evaluated once; a boxed subject is owned for the
  match's duration (pending release at match end, also on early exits).  Class
  arms compare `vtable.load` against `vtable.addr`; optional subjects test
  `null` first; value arms compare literals (Str through
  `PaykanString_equals` with a boxed literal); enum arms compare `i64`
  constants.  Arm bindings are raw `obj` aliases (unowned).
* **Optionals**: `None` into a `T?` is `null`; a `T?` into an `Obj` slot is
  replaced by a +1 box of `@$rt.PaykanObject_None` when null.  An optional
  primitive (`int?`, `float?`, `bool?`, `char?`) boxes a present value: a
  primitive that Sema marked for an optional primitive slot is lowered as
  `PaykanInt_new` (etc.) followed by `box`, a fresh +1 box for that slot; a
  `match` arm binding the primitive reads it with `PaykanInt_value` (etc.)
  into a plain local.
* **Conversions** (`Target<Source>(value)`, #64): `Str<…>` calls
  `PaykanString_from_int` & co. (an owned string temporary); `int<Str>` /
  `float<Str>` / `bool<Str>` call `PaykanInt_from_str` /
  `PaykanFloat_from_str` / `PaykanBool_from_str`, whose result is the
  `int?` / `float?` / `bool?` box (a fresh +1 box, `null` for an invalid
  string; `bool<Str>` accepts exactly `"True"` and `"False"`).  The boxed
  forms `Int<Str>` & co. are the same calls.  The numeric ones are inline: `int<float>` checks
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
; panics (noreturn)
Paykan_panic_div_by_zero() -> void      Paykan_panic_div_overflow() -> void
Paykan_panic_float_to_int(f64) -> void  Paykan_panic_int_to_char(i64) -> void
; strings
PaykanString_new(ptr, i64) -> obj       PaykanString_destroy(obj) -> void
PaykanString_concat(obj, obj) -> obj    PaykanString_char_at(obj, i64) -> char
PaykanString_from_int(i64) -> obj       PaykanString_from_float(f64) -> obj
PaykanString_from_bool(i64) -> obj      PaykanString_from_char(char) -> obj
PaykanString_equals(obj, box) -> i64    PaykanString_toString(obj) -> box
PaykanString_length(obj) -> i64
; arrays (slots are i64 bits; object slots hold a box cast to i64)
PaykanArray_new(i64) -> obj             PaykanArray_new_obj(i64) -> obj
PaykanArray_new_from_data(i64, ptr) -> obj
PaykanArray_get(obj, i64) -> i64        PaykanArray_set(obj, i64, i64) -> void
PaykanArray_set_obj(obj, i64, box) -> void
PaykanArray_push(obj, i64) -> void      PaykanArray_push_obj(obj, box) -> void
PaykanArray_pop(obj) -> i64             PaykanArray_pop_obj(obj) -> box
; tuples
PaykanTuple_new(i64, ptr) -> obj        PaykanTuple_get(obj, i64) -> i64
PaykanTuple_set(obj, i64, i64) -> void  PaykanTuple_set_obj(obj, i64, box) -> void
; files / boxed primitives
PaykanFile_open(obj, obj) -> box
PaykanInt_from_str(obj) -> box          PaykanFloat_from_str(obj) -> box
PaykanBool_from_str(obj) -> box         ; null box (None) for an invalid string
; boxes of the optional primitives (int? / float? / bool? / char?)
PaykanInt_new(i64) -> obj               PaykanInt_value(obj) -> i64
PaykanFloat_new(f64) -> obj             PaykanFloat_value(obj) -> f64
PaykanBool_new(i64) -> obj              PaykanBool_value(obj) -> i64
PaykanChar_new(char) -> obj             PaykanChar_value(obj) -> char
; I/O
Paykan_print(obj) -> void   Paykan_println(obj) -> void
Paykan_printerr(obj) -> void   Paykan_printerrln(obj) -> void
; vtable slot implementations (inherited into user vtables)
PaykanObject_destroy(obj) -> void   PaykanObject_toString(obj) -> box
PaykanObject_equals(obj, box) -> i64
PaykanFile_destroy(obj) -> void     PaykanFile_toString(obj) -> box
PaykanFile_equals(obj, box) -> i64  PaykanFile_write(obj, obj) -> void
PaykanFile_readln(obj) -> box
; globals
extern obj PaykanObject_None     extern obj PaykanFile_Stdin
extern vtable PaykanArray_vtable  extern vtable PaykanArray_obj_vtable
extern vtable PaykanInt_vtable    extern vtable PaykanFloat_vtable
extern vtable PaykanBool_vtable   extern vtable PaykanChar_vtable
```

`names::kCodeGenRequiredSymbols` in `include/Names.h` is the authoritative
list; the LLVM backend's JIT symbol table must cover it and the C backend links
`libpaykan_runtime.a`.

Vtable slot layouts of runtime classes (slot index = position):

* `Obj`, `Error`, `Int`/`Float`/`Bool`/`Char`, tuples: `destroy, toString, equals`
* `Str`: `destroy, toString, equals, len, concat`
* `File`: `destroy, toString, equals, write, readln, readbytes, read`
* arrays: `destroy, toString, equals, len` (`push`/`pop` are direct runtime calls)

## 9. Verifier

The verifier rejects a program when:

* a value is used before its definition or outside the block that defines it;
  a `local` is loaded/stored with the wrong type or used without a declaration;
* an instruction's operand types do not match the op (arithmetic type mismatch,
  `cmp` on different types, a disallowed `cast` pair, non-`bool` condition);
* a `call` names an undeclared function or passes the wrong arity/types; a
  `vcall` slot is out of range of the named class's vtable or the argument list
  does not match the slot's signature;
* `field.load`/`field.store`/`field.addr`/`new`/`vtable.addr` name an
  unknown class or field; a field store has the wrong type;
* `local.addr` or `field.addr` takes the address of a slot that is not `i64`,
  `f64`, `bool` or `char`; `ptr.load`/`ptr.store` take an address that is not
  a `ptr`, or read or write a value of another type;
* `break`/`continue` appear outside a `while`; a statement follows a
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
* `@main` has a signature other than `() -> i64` or `(box) -> i64`.

## 10. Text format summary

The text form is also what a loadable backend plugin receives
([`plugins/pir-for-backends.md`](plugins/pir-for-backends.md)), so it is
versioned: this is **PIR text version 1** (`PAYKAN_PIR_TEXT_VERSION` in
`include/paykan/plugin_api.h`), bumped on any incompatible change.

```
module "01"
cstr @.str0 = "Hello" len 5
extern fn @$rt.PaykanString_new(ptr, i64) -> obj
extern fn @$rt.Paykan_println(obj) -> void
extern fn @$rt.PaykanString_destroy(obj) -> void

fn @main() -> i64 {
  %s = call @$rt.PaykanString_new(@.str0, 5)
  call @$rt.Paykan_println(%s)
  call @$rt.PaykanString_destroy(%s)
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

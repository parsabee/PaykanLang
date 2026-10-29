# Private names: the `_` and `__` prefixes

The design behind the visibility rules in `docs/language/05-classes.md`,
`11-structs.md` and `15-modules.md`: which names an underscore prefix makes
private, from where they can be reached, and how a private name crosses a
module boundary.  There are two levels, each spelled by its prefix and neither
by a keyword: two underscores make a member private to its class or struct,
one underscore makes a name private to its module.  Neither changes the `.pkm`
format.  The open questions at the end each have the default this
implementation takes.

## Summary of the decisions

| Topic | Decision |
|-------|----------|
| The markers | two underscores (`__count`, `__find`) make a member private to its type; one underscore (`_Entry`, `_panic`, `_cache`) makes a name private to its module; the reserved `__init__` and `__super__` keep their meaning |
| A `__` field | `__x` in a class or a struct is reachable only from the methods of that class or struct: its body, its `impl` blocks and its trait impls, in whichever module they are instantiated; not from a subclass, not from the rest of the module, not from an importer |
| A `__` method | `__m` (`fn`, `view fn` or `static fn`) is reachable only from the same places; `__init__` and `__super__` are not methods in this sense |
| A `_` field or method | `_x`, `_m` or `static fn _s` is reachable from any code of the module that declares its type (a subclass, a free function, another type, the module's templates wherever they are instantiated), and from no importer |
| `__` at the top level | an error: a free function, class, struct, enum or trait cannot be private to a class; it is spelled with one underscore |
| A free function | `_f` is callable anywhere in its module and nowhere else: an importer cannot write `m::_f` |
| A type | a class, struct, enum or trait `_T` is nameable anywhere in its module and nowhere else: an importer cannot write `m::_T`, instantiate it, inherit from it or implement it |
| What is still exported | everything: a private field is part of an object's layout and a private function or type may be used by an exported generic template, so the `.pkm` interface carries them as before; the importer rejects a *written* reference to a private name through a qualifier, while injected template bodies keep using them |
| A public surface naming a private type | an error in the declaring module: a free function, a public field, or a public method of a public type cannot have a private type in its signature (`fn make() -> _Node` is rejected: an importer could receive what it cannot name); a private member or a private type's own members may |
| Constructors | the default `__init__` takes every field in order, private ones included: `Counter(0)` constructs from outside, as a constructor is the class's own code; an initializer written `___init__` is private to the module: `T(...)`, and `__super__(...)` into it, only from the module's code (its templates included, wherever they are instantiated), and an importer makes a `T` through the module's functions (`io::open` for `io::File`) |
| Runtime | nothing changes: `toString`, `==` and `hash` see every field as before |

## The markers

A member is private to its type when its name starts with `__`, and a name is
private to its module when it starts with exactly one `_`.  Nothing else is
looked at: not the type of the member, not an attribute.  The reserved method
names `__init__` and `__super__` are the one exception; they are keywords in
all but spelling, and a class's constructor is reachable from anywhere its
class is.  A name of exactly `_` or `__` is not private.

`__` names a member: it is rejected on a top-level declaration, whose
narrowest scope is its module, with

```
function '__f': the '__' prefix makes a member private to its class or struct; a module's own function is spelled '_f'
```

## Fields and methods: private to the type

`self.__x` reads and writes a private field in any method of the type that
declares it, including methods in a later `impl` block, in a trait impl
(`impl Show for Counter`), and in an instantiation of a generic class or
struct (`List<int>`'s methods are `List<T>`'s).  Outside those methods every
use is an error, with the same wording for a read, an assignment, an `inout`
or `view` argument, a method call and a `static fn` call:

```
'__items' is private to 'List<int>': it is reachable only from its own methods
'__check' is private to 'List<int>': it is reachable only from its own methods
```

A subclass cannot reach a superclass's private field or method: `self.__x` in
`Sub`'s method, with `__x` declared in `Base`, is the same error.  Nor can it
declare a field `__x` of its own: the shadowing check does not look at
privacy, so it is `field '__x' in class 'Sub' shadows a field in superclass
'Base'`, as for a public name.  (Letting a subclass declare its own `__x`, a
different field with the superclass's still in the layout, is not
implemented.)

The check is made where a member is resolved, on the owner of the member (the
class or struct that declares it, found by walking the superclass chain) and
the class context of the code (`CurrentClassCtx`, or the type a `static fn`
belongs to): the two must be the same type, or instantiations of the same
template (`List<int>`'s methods reach `List<Str>`'s private members: the
methods are the template's).  A free function, the body of another type and
a subclass's methods all fail it; a lambda inside a method keeps the
method's class context.  A private method is still a method: a subclass's
`__m` overrides it as any method would, a later change may make private
methods non-virtual.

## Fields and methods: private to the module

A field or method whose name starts with one `_` is reachable from any code of
the module whose type declares it: its own methods, a subclass's, another
type's, a free function.  From another module every use is an error, with the
module named:

```
'_w' of 'Pub' is private to module 'lib::shapes'
'_make' of 'Pub' is private to module 'lib::shapes'
```

The check is made where `__` members are checked, on the same owner, but it
compares modules rather than types: the module that declares the owner (its
template's module for an instantiation, `ImportedTypeOrigins` for an imported
type, this module otherwise) against the module whose code is being checked.
The body of another module's template, instantiated here, is that module's
code (`Sema::CurrentOrigin`, set wherever its diagnostics are pointed at its
file), so `Gen<T>`'s methods reach `Pub::_make()` in the importer as they do in
`lib::shapes`.  An inherited `_` member is its declaring class's: a subclass in
an importer does not reach it.

## Free functions and types: private to the module

Within its module a private function or type is an ordinary one: `_panic("x")`
and `_Entry<K, V>(k, v)` are written anywhere in the module.  An importer
gets the module's interface with every name in it, but a *written* qualified
reference to a private name is an error:

```
'collections::_bucket' is private to module '::collections'
'collections::_Entry' is private to module '::collections'
```

This covers calls, function values, type annotations, instantiations
(`m::_T<int>`), superclasses, `impl m::_Trait for X`, enum values and
`match` arms.  A generic template exported by the module keeps using private
names inside its bodies when it is instantiated in the importer, because the
clone is checked in the exporting module's namespace (the hidden qualifier),
not the importer's.

A public declaration that would hand a private type to an importer is rejected
where it is declared, since the importer could receive a value it cannot
name: a free function, a public field of a public class or struct, or a
public method's parameter or result naming a private type (`_Node`,
`_Node?`, `List<_Node>`, `(int) -> _Node`).  A private function, a private
field, a private method and the members of a private type may name private
types freely, which is how the standard library's `HashMap<K, V>` holds a
`Buffer<_Entry<K, V>?>`.

## What the exporter does

Nothing new.  The `.pkm` interface lists private fields (an importer lays out
and constructs the object), private methods (a vtable slot is a vtable slot),
private functions and private types (an exported template may use them).
Privacy is a property of the name, so the importer needs no new record: it
reads the underscores.

## Diagnostics

| Situation | Message |
|-----------|---------|
| A private field or method used outside its type | `'__x' is private to 'C': it is reachable only from its own methods` |
| A subclass using a superclass's private member | the same |
| An importer using a module-private member | `'_x' of 'C' is private to module 'm'` |
| An importer naming a private function | `'m::_f' is private to module 'm'` |
| An importer naming a private type | `'m::_T' is private to module 'm'` |
| A top-level declaration named `__f` | `function '__f': the '__' prefix makes a member private to its class or struct; a module's own function is spelled '_f'` |
| A public signature naming a private type | `'make' is public but its result names the private type '_Node'; make the function private or the type public` (likewise `parameter 'n'`, `field 'head' of 'List'`, `method 'peek' of 'List'`) |

## Open questions

Each with the default taken here.

1. **Should a private method be reachable from a subclass (protected)?**  No:
   the `__` prefix means "mine", and one level is enough for a language with
   no `protected` keyword.  A superclass that wants to share a member with its
   subclasses leaves the underscores off.
2. **Should privacy be per module rather than per type for fields?**  Both
   are offered: `__x` is the type's own, `_x` the module's.  Per-type is what
   the generic instantiation model gives for free (the instantiated methods
   are the type's); per-module needs the module of the code being checked,
   which a template body instantiated elsewhere takes from its origin.
3. **Should a private type be allowed in a public signature as an opaque
   handle?**  No: an importer that receives a value it cannot name can only
   pass it back, and every such API can be written with a public wrapper.
   Rejecting it keeps the exported surface nameable.
4. **Should private names be left out of the `.pkm` interface?**  No: a
   private field is part of the layout and a private function or type may be
   reached by an exported template.  Leaving them out would need a layout
   record of its own in the format.
5. **Should a single underscore mean anything?**  Yes: private to the
   module.  A top-level `__` name is an error rather than a synonym, so each
   prefix has one meaning.
6. **Should a private type's name be the module's own too?**  Not yet: type
   names are global across an import graph (one canonical name per type, as
   `docs/language/15-modules.md` says), so two modules still cannot both
   declare a `_Node`.  Privacy here is about reach, not naming; module-local
   canonical names would be a change of their own (`docs/design/pkm.md`
   §3.6).

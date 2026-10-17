# Private members: the `__` prefix

The design behind the visibility rules in `docs/language/04-classes.md`,
`14-structs.md` and `10-modules.md`: which names a double underscore makes
private, from where they can be reached, and how a private name crosses a
module boundary.  The rules were chosen to turn the convention the standard
library already follows ("names starting with `__` are the module's own") into
a check, without a keyword, without a change to the `.pkm` format and without
breaking a program that follows the convention.  The open questions at the end
each have the default this implementation takes.

## Summary of the decisions

| Topic | Decision |
|-------|----------|
| The marker | a name that starts with two underscores (`__count`, `__find`, `__Entry`, `__panic`); one underscore means nothing, and the reserved `__init__` and `__super__` keep their meaning |
| A field | `__x` in a class or a struct is reachable only from the methods of that class or struct: its body, its `impl` blocks and its trait impls, in whichever module they are instantiated; not from a subclass, not from the rest of the module, not from an importer |
| A method | `__m` (`fn`, `view fn` or `static fn`) is reachable only from the same places; `__init__` and `__super__` are not methods in this sense |
| A free function | `__f` is callable anywhere in its module and nowhere else: an importer cannot write `m::__f` |
| A type | a class, struct, enum or trait `__T` is nameable anywhere in its module and nowhere else: an importer cannot write `m::__T`, instantiate it, inherit from it or implement it |
| What is still exported | everything: a private field is part of an object's layout and a private function or type may be used by an exported generic template, so the `.pkm` interface carries them as before; the importer rejects a *written* reference to a private name through a qualifier, while injected template bodies keep using them |
| A public surface naming a private type | an error in the declaring module: a free function, a public field, or a public method of a public type cannot have a private type in its signature (`fn make() -> __Node` is rejected: an importer could receive what it cannot name); a private member or a private type's own members may |
| Constructors | the default `__init__` takes every field in order, private ones included: `Counter(0)` constructs from outside, as a constructor is the class's own code |
| Runtime | nothing changes: `toString`, `==` and `hash` see every field as before |

## The marker

A name is private when it starts with `__`.  Nothing else is looked at: not
the type of the member, not where it is declared, not an attribute.  The
reserved method names `__init__` and `__super__` are the one exception; they
are keywords in all but spelling, and a class's constructor is reachable from
anywhere its class is.  A name of exactly `__` or of underscores only is
rejected like any other unusable name.

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
`Sub`'s method, with `__x` declared in `Base`, is the same error.  A
subclass may declare its own `__x`; it is a different field (the superclass's
stays in the layout, unreachable from the subclass), reported as the usual
shadowing error if the name is public in the superclass.

The check is made where a member is resolved, on the owner of the member (the
class or struct that declares it, found by walking the superclass chain) and
the class context of the code (`CurrentClassCtx`): the two must be the same
type.  A free function, a lambda inside a method (which keeps the method's
class context), a trait's default method and the body of another type all
fail it.

## Free functions and types: private to the module

Within its module a private function or type is an ordinary one: `__panic("x")`
and `__Entry<K, V>(k, v)` are written anywhere in the module.  An importer
gets the module's interface with every name in it, but a *written* qualified
reference to a private name is an error:

```
'collections::__bucket' is private to module 'collections'
'collections::__Entry' is private to module 'collections'
```

This covers calls, function values, type annotations, instantiations
(`m::__T<int>`), superclasses, `impl m::__Trait for X`, enum values and
`match` arms.  A generic template exported by the module keeps using private
names inside its bodies when it is instantiated in the importer, because the
clone is checked in the exporting module's namespace (the hidden qualifier),
not the importer's.

A public declaration that would hand a private type to an importer is rejected
where it is declared, since the importer could receive a value it cannot
name: a free function, a public field of a public class or struct, or a
public method's parameter or result naming a private type (`__Node`,
`__Node?`, `List<__Node>`, `(int) -> __Node`).  A private function, a private
field, a private method and the members of a private type may name private
types freely, which is how the standard library's `HashMap<K, V>` holds a
`Buffer<__Entry<K, V>?>`.

## What the exporter does

Nothing new.  The `.pkm` interface lists private fields (an importer lays out
and constructs the object), private methods (a vtable slot is a vtable slot),
private functions and private types (an exported template may use them).
Privacy is a property of the name, so the importer needs no new record: it
reads the two underscores.

## Diagnostics

| Situation | Message |
|-----------|---------|
| A private field or method used outside its type | `'__x' is private to 'C': it is reachable only from its own methods` |
| A subclass using a superclass's private member | the same |
| An importer naming a private function | `'m::__f' is private to module 'm'` |
| An importer naming a private type | `'m::__T' is private to module 'm'` |
| A public signature naming a private type | `'make' is public but its result names the private type '__Node'; make the function private or the type public` (likewise `parameter 'n'`, `field 'head' of 'List'`, `method 'peek' of 'List'`) |

## Open questions

Each with the default taken here.

1. **Should a private method be reachable from a subclass (protected)?**  No:
   the `__` prefix means "mine", and one level is enough for a language with
   no `protected` keyword.  A superclass that wants to share a member with its
   subclasses leaves the underscores off.
2. **Should privacy be per module rather than per type for fields?**  No: a
   field is the type's own; the standard library never reaches another type's
   `__` field.  Per-type is also what the generic instantiation model gives
   for free (the instantiated methods are the type's).
3. **Should a private type be allowed in a public signature as an opaque
   handle?**  No: an importer that receives a value it cannot name can only
   pass it back, and every such API can be written with a public wrapper.
   Rejecting it keeps the exported surface nameable.
4. **Should private names be left out of the `.pkm` interface?**  No: a
   private field is part of the layout and a private function or type may be
   reached by an exported template.  Leaving them out would need a layout
   record of its own in the format, which stays fixed until 0.2.0.
5. **Should a single underscore mean anything?**  No.

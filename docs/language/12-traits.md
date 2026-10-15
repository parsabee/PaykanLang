# PaykanLang — Traits

A trait names the methods a type must have. `impl Trait for Type` declares that a class
implements it.

---

## Quick Summary

- `trait Name { view fn f() -> T; ... }` lists method signatures, each ending with `;`.
  A trait can be generic (`trait Into<T>`), and `Self` in its signatures is the type
  that implements it.
- `impl Name for A { ... }` declares that class `A` implements `Name`. The block's
  methods join `A` like the methods of any `impl` block (see `04-classes.md`), and a
  method that `A` already has meets a requirement too.
- A class implements every trait its superclasses implement. Every class implements the
  built-in trait `Show` (`view fn toString() -> Str`) through `Obj`.
- A trait is not a type: `x: Named` is an error.

---

## Declaring and Implementing

```pkn
trait Named { view fn name() -> Str; }
trait Into<T> { view fn into() -> T; }
trait Same { view fn same(other: view Self) -> bool; }

class A {
  n: int;
  fn __init__(n: int) { self.n = n; }
  view fn name() -> Str { return "A" + Str(self.n); }   // meets Named's requirement
}

impl Named for A {}
impl Into<int> for A { view fn into() -> int { return self.n; } }
impl Same for A {
  view fn same(other: view Self) -> bool { return self.n == other.n; }
}
```

A requirement is met by the method of the same name. Its signature must be the trait's,
with `Self` and the trait's type parameters replaced. That covers the parameter and
result types, the `view` / `inout` modes, and whether it is a `view fn`. An `impl ... for`
block holds only the methods its trait requires, and no `static fn`.

`Self` also names the enclosing class in a class's own methods and in its `impl` blocks.

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Trait name taken | A trait is named like a class, enum, function or another trait, or `Show` |
| Bad requirement | A requirement names an unknown type, repeats a name, or is `__init__` / `destroy` |
| Missing or mismatched method | The class has no method a requirement names, or its signature differs |
| Extra method | An `impl ... for` block has a method, or a `static fn`, its trait does not require |
| Implemented twice | The class, or a superclass, already implements the trait (every class implements `Show`) |
| Not a trait | An `impl ... for` names a class, a built-in type or an unknown name, or gives the wrong number of type arguments |
| Trait as a type | A trait is used where a type is expected |

## Not Supported Yet

Bounds on type parameters (`<T: Show>`, parsed but rejected), traits of another module
(an importer cannot name them, and their `impl ... for` blocks
are not in the module interface), `impl ... for` a built-in type, an enum or a class of
another module, default methods, `static fn` requirements, and trait objects.

# PaykanLang — Functions & Argument Passing

---

## Function Declaration

```
fn name(params) -> ReturnType {
  body
}

fn name(params) {
  // void return — no -> annotation needed
}
```

- No function overloading — each name must be unique in the program.
- Functions must be declared before they are called.
- `main` must return `int`.

---

## Argument-Passing Rules

### Parameters

Each parameter has the form:

```
paramName: TypeName
```
### Builtin Type parameter (`i: int`)

| Argument kind | How to pass | Notes |
|---------------|-------------|-------|
| variable | `f(i)` | copied |
| Bare literal | `f(4)` | Allowed: a literal creates a new allocation

### Obj Type parameter (`s: Obj`)

| Argument kind | How to pass | Notes |
|---------------|-------------|-------|
| Shared variable | `f(s)` | Refcount bumped |
| Bare literal | `f("hi")` | Allowed: a literal creates a new object

---

## Examples

```
fn writeLine(s: Str) {
  std::write(std::out(), s + "\n");
}

fn main() -> int {
  a: Str = "hello";
  b: Str = "world";
  writeLine(a + b);
  return 0;
}
```

---

## Return Values

```
fn add(a: int, b: int) -> int {
  return a + b;
}

fn nothing() {
  return;   // bare return for void
}
```

- Non-void functions must return a value matching the declared return type.
- `return;` in a non-void function is a compile-time error.

---

## Semantic Checks for Functions

| Error | Trigger |
|-------|---------|
| Undeclared function | Calling a function not yet declared |
| Function redefinition | Two functions with the same name |
| Argument count mismatch | Wrong number of arguments |
| Argument type mismatch | Argument type incompatible with parameter type |
| Non-void return without value | `return;` in non-void function |
| Return type mismatch | Returned expression type ≠ declared return type |

---
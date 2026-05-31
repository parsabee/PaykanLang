# Paykan Language Reference

> **Version:** 0.1 (April 2026)
> **Implementation:** LLVM 17.0.6 backend, Bison 3.8 / Flex 2.6.4 frontend, C11 runtime

---

## Table of Contents

1. [Overview](#overview)
2. [Program Structure](#program-structure)
3. [Types](#types)
4. [Variables & Declarations](#variables--declarations)
5. [Ownership System](#ownership-system)
6. [Functions](#functions)
7. [Expressions](#expressions)
8. [Statements](#statements)
9. [Control Flow](#control-flow)
10. [Built-in Functions](#built-in-functions)
11. [String Constructors](#string-constructors)
12. [Scoping Rules](#scoping-rules)
13. [Semantic Checks](#semantic-checks)
14. [Compiler CLI](#compiler-cli)
15. [Grammar Summary](#grammar-summary)

---

## Overview

Paykan (`.pkn`) is a statically-typed, ownership-aware language that compiles to LLVM IR and executes via JIT. It features:

- **Three ownership modes** for heap-allocated objects: unique, shared (reference-counted), and reference (borrowed).
- **Move semantics** (`mov`) for transferring unique ownership.
- **Const correctness** on variables and parameters.
- **Automatic memory management** — unique variables are destroyed at scope exit; shared variables are freed when their reference count drops to zero.
- **No garbage collector** — all memory lifetimes are determined statically or via reference counting.

---

## Program Structure

A Paykan program is a list of **function declarations**. Execution begins at `main`, which must return `int`.

```
fn greet() {
  out("hello!");
}

fn main() -> int {
  greet();
  return 0;
}
```

- There are **no top-level statements** outside functions.
- There is **no function overloading** — each function name must be unique.
- Functions must be **declared before use** (the parser processes declarations in order).
- Comments use `//` (C-style single-line only).

---

## Types

### Builtin Types

| Type    | LLVM Type | Description                        |
|---------|----------|------------------------------------|
| `int`   | `i64`    | 64-bit signed integer              |
| `float` | `double` | 64-bit IEEE 754 floating-point     |
| `bool`  | `i1`     | Boolean — literals `True` / `False`|
| `void`  | `void`   | Used only as function return type  |

### Class Types

| Type     | Description                                              |
|----------|----------------------------------------------------------|
| `Object` | Root of the class hierarchy (vtable-based)               |
| `String` | Heap-allocated, NUL-terminated string; inherits `Object` |

Class types are **heap-allocated** and subject to the ownership system. Builtin types are always **stack-allocated** and not subject to ownership qualifiers.

### Type Hierarchy

```
Object
  └── String
```

`String` is a subtype of `Object`. Subtype values are assignable to supertype locations (e.g., a `String` can be passed where an `Object` is expected).

### Implicit Promotions

- `int` -> `float`: an `int` value is silently promoted to `float` when assigned to a `float` variable or passed to a `float` parameter.

---

## Variables & Declarations

Variables are declared with explicit type annotations using `:` syntax:

```
name: Type = initializer;
```

### Examples

```
x: int = 42;
pi: float = 3.14;
flag: bool = True;
msg: String = "hello";
```

### Declaration by Assignment

If a variable has not been declared in any enclosing scope, a bare assignment `x = expr;` creates it in the current scope with the inferred type.

### Const Variables

The `const` qualifier prevents reassignment:

```
x: const int = 10;
// x = 20;  ← error: cannot assign to const variable 'x'
```

`const` can be combined with any ownership qualifier.

---

## Ownership System

Ownership qualifiers apply to **class types only** (`String`, `Object`). Builtin types (`int`, `float`, `bool`) are always stack-allocated and ownership qualifiers on them are rejected.

### Unique (default)

Single owner. Destroyed automatically at scope exit. Cannot be reassigned (would create two owners).

```
a: String = "hello";       // unique by default
// a = "world";            // error: cannot reassign unique variable
```

### Shared

Reference-counted. Multiple variables can share the same object. The underlying object is destroyed when the last reference is released.

```
b: shared String = "hello";
b = "world";                // OK — old value released, new value wrapped
```

### Reference (`&`)

A borrowed pointer to an existing variable. No ownership — the referent must outlive the reference. Created with `&`:

```
a: String = "owned";
r: String& = &a;           // r borrows a
r: const String& = &a;     // const borrow — cannot assign through r
```

**Rules for reference creation:**
- `&` can only be applied to a **variable** (identifier), not a literal or expression.
- The reference variable must be initialized with `&variable`.

### Ownership Summary Table

| Qualifier   | Syntax              | Cleanup          | Reassignable | Copyable |
|-------------|---------------------|------------------|-------------|----------|
| Unique      | `x: String`         | Scope exit       | No          | No (use `mov`) |
| Shared      | `x: shared String`  | Refcount -> 0     | Yes         | Yes (refcount bump) |
| Reference   | `x: String&`        | None (borrowed)  | No          | N/A      |

---

## Functions

### Declaration

```
fn name(params) -> ReturnType {
  body
}

fn name(params) {
  // void return
}
```

### Parameter Ownership

Each parameter can have its own ownership and const qualifier:

| Syntax                        | Ownership   | Const |
|-------------------------------|-------------|-------|
| `p: String`                   | Unique      | No    |
| `p: shared String`            | Shared      | No    |
| `p: String&`                  | Reference   | No    |
| `p: const String`             | Unique      | Yes   |
| `p: const shared String`      | Shared      | Yes   |
| `p: const String&`            | Reference   | Yes   |

### Argument Passing Rules

When calling a function, how you pass an argument depends on the **parameter's ownership**:

#### Unique parameter (`p: String`)

- Requires `mov` to transfer ownership: `f(mov x)`
- Shared variables cannot be passed.
- References cannot be passed.

#### Shared parameter (`p: shared String`)

- Shared variables pass directly: `f(s)` (refcount bumped)
- `mov` transfers ownership from unique or shared: `f(mov x)`
- References cannot be passed.

#### Reference parameter (`p: String&`)

- **Owned variables** (unique/shared) require `&`: `f(&a)`
- **Reference variables** pass directly: `f(r)` (already borrowed)
- `mov` is not allowed.
- **Non-const `T&`:** rejects rvalues, rejects `&shared_var`
- **`const T&`:** additionally accepts rvalues (literals, constructor results, call results) directly — no `&` needed

#### Passing Rules Summary

| Argument \ Param ->    | `T` (unique) | `shared T` | `T&`            | `const T&`       |
|-----------------------|-------------|-----------|-----------------|------------------|
| Unique variable       | `mov x`     | `mov x`   | `&x`            | `&x`             |
| Shared variable       | ✗           | `s`       | ✗               | `&s`             |
| Reference variable    | ✗           | ✗         | `r` (implicit)  | `r` (implicit)   |
| Literal / rvalue      | ✗           | ✗         | ✗               | `"hi"` (implicit)|

---

## Expressions

### Literals

| Literal         | Type    | Examples                    |
|-----------------|---------|-----------------------------|
| Integer         | `int`   | `0`, `42`, `-17`            |
| Floating-point  | `float` | `3.14`, `0.5`               |
| Boolean         | `bool`  | `True`, `False`             |
| String          | `String`| `"hello"`, `""`             |

### Arithmetic Operators

| Operator | Operation      | Operand Types         | Result Type                |
|----------|---------------|-----------------------|----------------------------|
| `+`      | Addition       | `int`, `float`        | `float` if either is float, else `int` |
| `+`      | Concatenation  | `String + String`     | `String`                   |
| `-`      | Subtraction    | `int`, `float`        | Numeric                    |
| `*`      | Multiplication | `int`, `float`        | Numeric                    |
| `/`      | Division       | `int`, `float`        | Numeric                    |
| `%`      | Modulo         | `int`, `float`        | Numeric                    |

### Unary Operators

| Operator | Operation    | Operand Type | Result Type |
|----------|-------------|-------------|-------------|
| `-`      | Negation     | `int`, `float` | Same as operand |
| `!`      | Logical NOT  | `bool`      | `bool`      |

### Relational & Equality Operators

| Operator | Description       | Result |
|----------|-------------------|--------|
| `<`      | Less than         | `bool` |
| `>`      | Greater than      | `bool` |
| `<=`     | Less or equal     | `bool` |
| `>=`     | Greater or equal  | `bool` |
| `==`     | Equal             | `bool` |
| `!=`     | Not equal         | `bool` |

Equality operators require both operands to have **compatible types** (same type or class subtype relationship).

### Logical Operators

| Operator | Description   | Operand Types | Result Type |
|----------|---------------|---------------|-------------|
| `&&`     | Logical AND   | `bool`        | `bool`      |
| `\|\|`   | Logical OR    | `bool`        | `bool`      |

Both operators use **short-circuit evaluation**:
- `a && b` — if `a` is `False`, `b` is not evaluated.
- `a || b` — if `a` is `True`, `b` is not evaluated.

```
flag: bool = x > 0 && x < 100;
ok: bool = isReady || isFallback;
```

### Ternary Expression

A conditional expression that evaluates to a value:

```
if <condition> then <expr> else <expr>
```

- The condition must be `bool`.
- Both branches must have the **same type**.
- The result type is the type of the branches.
- Ternary expressions are right-associative and can be nested:

```
label: int = if grade > 90 then 1
             else if grade > 80 then 2
             else 3;

result: int = if x > 0 then x else 0 - x;  // abs(x)
```

### Operator Precedence (highest to lowest)

| Level | Operators                 | Associativity |
|-------|---------------------------|---------------|
| 1     | `!`, `-` (unary), `mov`, `&` | Right (prefix) |
| 2     | `*`, `/`, `%`             | Left          |
| 3     | `+`, `-`                  | Left          |
| 4     | `<`, `>`, `<=`, `>=`, `==`, `!=` | Left    |
| 5     | `&&`                      | Left          |
| 6     | `\|\|`                    | Left          |
| 7     | `if...then...else` (ternary) | Right      |

### Function Calls

```
functionName(arg1, arg2, ...)
```

- Arguments are type-checked against the declared parameter types.
- Variadic builtins (`out`, `err`) accept any number of arguments.
- No function overloading — each name maps to exactly one function.

### Move Expression

```
mov variable
```

Transfers ownership of a unique or shared variable. After a `mov`, the variable is considered **moved** and cannot be used again.

```
a: String = "hello";
consume(mov a);
// out(&a);  ← error: use of moved variable 'a'
```

### Reference Expression

```
&variable
```

Borrows a variable, producing a reference. Can only be applied to **identifiers** (not literals or expressions).

---

## Statements

### Expression Statement

```
expression;
```

### Variable Declaration

```
name: Type = expression;
name: shared Type = expression;
name: Type& = &existing_var;
name: const Type = expression;
```

### Assignment

```
variable = expression;
```

- Cannot assign to `const` variables.
- Cannot reassign unique class-type variables (use shared instead).

### Return

```
return expression;
return;             // void functions
```

### Block (Compound Statement)

```
{
  // new scope
  statements...
}
```

Blocks introduce a new lexical scope. Variables declared inside are not visible outside.

### If / Else Statement

See [Control Flow](#control-flow).

### While Statement

See [Control Flow](#control-flow).

### Empty Statement

```
;
```

---

## Control Flow

### `if` / `else`

Conditional branching. The condition must be a `bool` expression (parentheses required).

```
if (condition) {
  // then branch
}

if (condition) {
  // then branch
} else {
  // else branch
}
```

### `else if` Chains

`else if` chains are supported by placing an `if` statement as the else branch:

```
if (x > 100) {
  out("large");
} else if (x > 10) {
  out("medium");
} else {
  out("small");
}
```

Internally, `else if` is represented as a nested `IfStmt` — there is no separate AST node.

### `while` Loop

Repeats a block while the condition is `True`. The condition must be `bool` (parentheses required).

```
i: int = 0;
while (i < 5) {
  out(StrInt(i));
  i = i + 1;
}
```

```
// infinite loop
while (True) {
  // ...
}
```

### `break`

Exits the innermost enclosing loop immediately.

```
i: int = 0;
while (True) {
  if (i == 5) { break; }
  i = i + 1;
}
// i == 5 here
```

- `break` is only allowed inside a `while` loop body.
- In nested loops, `break` exits only the innermost loop.

### `continue`

Skips the rest of the current iteration and jumps to the loop condition.

```
i: int = 0;
while (i < 10) {
  i = i + 1;
  if (i % 2 == 0) { continue; }
  out(StrInt(i));  // prints 1, 3, 5, 7, 9
}
```

- `continue` is only allowed inside a `while` loop body.
- Using `break` or `continue` outside a loop is a compile-time error.

---

## Built-in Functions

### `out(args...)`

Prints one or more values to **stdout**, each converted to a string via its `toString` vtable method, followed by a newline.

- Signature: `out(arg1, arg2, ...)` — variadic, `const Object&` parameters.
- Accepts: literals, rvalues, references, and `&owned_var` for owned variables.

```
out("count: ", StrInt(42));   // prints: count: 42
```

### `err(args...)`

Same as `out` but prints to **stderr**.

---

## String Constructors

Built-in functions that convert values to `String`:

| Constructor       | Parameter | Description                    |
|-------------------|-----------|--------------------------------|
| `String(s)`       | `const String&` | Identity (wraps string literal) |
| `StrInt(n)`    | `int`     | Integer to string              |
| `StrFloat(f)`  | `float`   | Float to string                |
| `StrBool(b)`   | `bool`    | Boolean to string (`True`/`False`) |

```
s: String = StrInt(42);         // "42"
t: String = StrFloat(3.14);     // "3.14"
u: String = StrBool(True);      // "True"
v: String = String("hello");       // "hello"
```

---

## Scoping Rules

- **Lexical scoping**: variables are resolved by walking the scope chain from innermost to outermost.
- **Block scopes**: `{ }` introduces a new scope.
- **Function scopes**: each function body is its own scope with parameters pre-declared.
- **Shadowing**: inner scopes can shadow names from outer scopes (via declaration-by-assignment).
- **Move tracking**: moved status is tracked through the scope chain — moving a variable in an inner scope marks it as moved in the scope that owns it.

```
fn main() -> int {
  x: int = 1;
  {
    x = 2;          // modifies outer x
    y: int = 3;     // local to this block
  }
  // y is not visible here
  return 0;
}
```

---

## Semantic Checks

The compiler performs the following checks at compile time:

### Variable Checks
- **Undeclared variable** — use of a variable not declared in any enclosing scope.
- **Duplicate declaration** — declaring a variable that already exists in the current scope.
- **Use after move** — reading a variable after its ownership has been transferred via `mov`.
- **Const reassignment** — assigning to a `const` variable.
- **Unique reassignment** — reassigning a unique class-type variable (would create two owners).
- **Ownership on builtins** — `shared` or `&` qualifiers on `int`, `float`, `bool` are rejected.

### Type Checks
- **Type mismatch in initialization** — initializer type incompatible with declared type.
- **Type mismatch in assignment** — assigned value type incompatible with variable type.
- **Operator type errors** — operators applied to unsupported types (e.g., `!` on `int`).
- **Equality type mismatch** — `==`/`!=` on incompatible types (e.g., `int == String`).
- **Return type mismatch** — return value type doesn't match function signature.
- **Argument count/type mismatch** — wrong number or type of arguments in function calls.

### Ownership Checks
- **Missing `mov`** — passing an owned variable to a unique parameter without `mov`.
- **Missing `&`** — passing an owned variable to a reference parameter without `&`.
- **`mov` on reference** — attempting to move a borrowed reference variable.
- **`&` on non-variable** — applying `&` to a literal or expression (only variables allowed).
- **Shared -> unique** — shared variable cannot be passed to a unique parameter.
- **Reference -> unique/shared** — reference variable cannot be passed to owned parameters.
- **Rvalue -> non-const `T&`** — rvalues can only be passed to `const T&`, not `T&`.
- **`&shared_var` -> non-const `T&`** — mutable reference cannot borrow shared variable.
- **Reference binding** — `T& = expr` requires `&identifier` (must borrow a named variable).

### Function Checks
- **Undeclared function** — calling a function that hasn't been declared.
- **Function redefinition** — declaring two functions with the same name.
- **Non-void return without value** — `return;` in a function with a non-void return type.

---

## Compiler CLI

```
paykan <source-file> [options]
```

By default, the compiler parses, type-checks, generates LLVM IR, and **JIT-executes** the program.

### Options

| Flag              | Description                              |
|-------------------|------------------------------------------|
| `--dump-ast`      | Print the AST in tree form and exit      |
| `--emit-llvm`     | Print LLVM IR to stdout and exit         |
| `--trace-parser`  | Enable Bison parser debug traces         |
| `--trace-scanner` | Enable Flex scanner debug traces         |
| `-O0` … `-O3`    | LLVM optimization level (default: `-O0`) |

---

## Grammar Summary

```ebnf
program        = { funcDecl } ;

funcDecl       = "fn" IDENT "(" paramList ")" [ "->" typeAnnotation ] block ;

paramList      = [ param { "," param } ] ;
param          = IDENT ":" [ "const" ] [ "shared" ] typeAnnotation [ "&" ] ;

block          = "{" { statement } "}" ;

statement      = ";"
               | expression ";"
               | expression "=" expression ";"
               | varDecl "=" expression ";"
               | "return" [ expression ] ";"
               | block
               | ifStmt
               | whileStmt
               | "break" ";"
               | "continue" ";" ;

ifStmt         = "if" "(" expression ")" block
               | "if" "(" expression ")" block "else" block
               | "if" "(" expression ")" block "else" ifStmt ;

whileStmt      = "while" "(" expression ")" block ;

varDecl        = IDENT ":" [ "const" ] [ "shared" ] typeAnnotation [ "&" ] ;

typeAnnotation = IDENT ;   (* "int" | "float" | "bool" | "String" | ... *)

expression     = ternaryExpr ;
ternaryExpr    = logicalOrExpr
               | "if" logicalOrExpr "then" expression "else" ternaryExpr ;
logicalOrExpr  = logicalAndExpr { "||" logicalAndExpr } ;
logicalAndExpr = relationalExpr { "&&" relationalExpr } ;
relationalExpr = additiveExpr { relOp additiveExpr } ;
additiveExpr   = multExpr { ( "+" | "-" ) multExpr } ;
multExpr       = unaryExpr { ( "*" | "/" | "%" ) unaryExpr } ;
unaryExpr      = primaryExpr
               | ( "!" | "-" ) primaryExpr
               | "mov" IDENT
               | "&" expression ;
primaryExpr    = INT | FLOAT | BOOL | STRING
               | IDENT "(" argumentList ")"
               | IDENT
               | "(" expression ")" ;

argumentList   = [ expression { "," expression } ] ;

relOp          = "<" | ">" | "<=" | ">=" | "==" | "!=" ;
```

---

## Implementation Details

- **Frontend:** Bison 3.8 LALR(1) parser + Flex 2.6.4 lexer
- **AST:** Arena-allocated (all nodes owned by `ASTContext`), LLVM-style RTTI (`isa<T>`, `dyn_cast<T>`, `cast<T>`)
- **Semantic analysis:** Single-pass visitor with scoped symbol table and ownership tracking
- **Code generation:** Direct LLVM IR emission via LLVM 17.0.6 C++ API
- **Execution:** LLVM ORC JIT (in-process compilation and execution)
- **Runtime:** C11 library linked at JIT time; vtable-based dispatch for `toString`/`equals`; reference counting for shared ownership
- **Optimization:** LLVM's new pass manager with configurable `-O0` through `-O3`

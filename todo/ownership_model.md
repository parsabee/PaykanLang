# Ownership Model Implementation Plan

Paykan supports three ownership qualifiers for class types (builtins like `int`, `float`, `bool` have automatic/stack storage and are unaffected):

| Qualifier | Syntax | Semantics |
|-----------|--------|-----------|
| **unique** (default) | `a: String` / `a: unique String` | Single owner; destroyed at scope exit |
| **shared** | `a: shared String` | Reference-counted; freed when refcount → 0 |
| **reference** | `a: String&` | Borrowed pointer; no ownership transfer; lifetime must not exceed referent |

---

## Phase 1 — AST: Ownership Qualifier

> Add ownership information to the type system and AST nodes.

- [x] Define `enum class Ownership { Unique, Shared, Reference }` (in `AST.h` or a new `Ownership.h`)
- [x] Add `Ownership` field to `VarDecl` (default: `Unique`)
- [x] Add `getOwnership()` / `setOwnership()` accessors to `VarDecl`
- [x] Extend `ASTContext::make<VarDecl>(...)` call sites to propagate ownership
- [x] Update `ASTPrinter` to display ownership qualifier in dump output
- [ ] Add unit tests for AST ownership storage and printing

---

## Phase 2 — Parser: Ownership Syntax

> Teach the parser to recognise `unique`, `shared`, and `&` qualifiers.

- [x] Add `SHARED` and `UNIQUE` keyword tokens to the Flex lexer
- [x] Add `&` as a type-suffix token (if not already present)
- [x] Update Bison grammar for variable declarations:
  - `IDENT ":" type "=" expr` → unique (default, no keyword needed)
  - `IDENT ":" "unique" type "=" expr`
  - `IDENT ":" "shared" type "=" expr`
  - `IDENT ":" type "&" "=" expr`
- [x] Construct `VarDecl` with the correct `Ownership` in parser actions
- [ ] Add parser test cases for each ownership syntax variant
- [ ] Add parser error tests (e.g., `shared int` on a builtin type should be rejected or warned)

---

## Phase 3 — Runtime: Reference-Count Infrastructure

> Extend the C runtime with a refcount field and retain/release API.

- [x] Add `int64_t refCount` field to `PaykanObject` struct (before or after vtable pointer)
  - All subclasses (e.g., `PaykanString`) inherit this automatically since they embed `PaykanObjectVTable *vtable` first — refCount goes second
- [x] Initialise `refCount = 1` in `PaykanObject_new()` and `PaykanString_new()`
- [x] Implement `void Paykan_retain(PaykanObject *obj)` — increment refCount
- [x] Implement `void Paykan_release(PaykanObject *obj)` — decrement refCount; if it reaches 0, call the destructor
  - Destructor dispatch: need a `destroy` function pointer in vtable, or use a type tag, or a simple approach: add a `void (*destroy)(PaykanObject*)` slot to `PaykanObjectVTable`
- [x] Add `destroy` vtable slot:
  - `PaykanObjectVTable.destroy` → `PaykanObject_delete`
  - `PaykanStringVTable.destroy` → `PaykanString_delete`
- [x] Update all existing `_new` functions to set `refCount = 1`
- [ ] Add runtime unit tests for retain/release lifecycle
- [x] Verify `PaykanString_concat` and `PaykanObject_toString` return objects with refCount = 1

---

## Phase 4 — Sema: Ownership Validation

> Enforce ownership rules during semantic analysis.

- [x] Reject ownership qualifiers on builtin types (`shared int` → error)
- [x] Track ownership in the `Sema` symbol table alongside the type
- [x] **Unique rules:**
  - Reject assignment of a unique variable to another variable without `mov`
  - `mov` operator explicitly transfers ownership: `b: String = mov a` invalidates `a`
  - Reject use of a moved-from variable
  - `mov` only allowed on unique variables
- [x] **Shared rules:**
  - Allow free assignment (each assignment increments refcount)
- [x] **Reference rules:**
  - A `&` reference can only bind to an existing variable (not a temporary/rvalue)
  - The referent must outlive the reference (scope-based analysis):
    - Reference cannot escape the scope of its referent
    - Reference cannot be stored in a wider-scoped variable
    - Reference cannot be returned from a function (initially)
  - Assignment through a reference mutates the referent
- [ ] Add Sema error tests for each violation
- [ ] Add Sema pass tests for legal ownership patterns

---

## Phase 5 — CodeGen: Unique Ownership (Scope-Based Destruction)

> Emit destructor calls at scope exit for unique-owned class-type variables.

- [x] In `ScopeGuard` (or scope-exit logic), collect all `unique` class-type `alloca`s in the current scope
- [x] At scope exit, emit `_delete()` / `destroy()` call for each live unique variable (in reverse declaration order)
- [ ] Handle early returns: emit cleanup before every `ret` instruction (or use a cleanup landing pad)
- [x] Handle move semantics: if a variable was moved, skip its destructor (track via a null-check or a boolean flag alloca)
- [x] Register `destroy` vtable slot in the JIT runtime symbol table
- [ ] Add codegen tests: verify IR contains delete calls at scope boundaries
- [ ] Add JIT integration tests: verify no memory leaks with unique objects (e.g., valgrind or manual allocation counter)

---

## Phase 6 — CodeGen: Shared Ownership (Retain/Release)

> Emit retain/release calls for shared variables.

- [x] On `shared` variable initialisation: no extra retain needed (constructor returns refCount = 1)
- [x] On `shared` assignment (`a = b`):
  1. `Paykan_retain(b)`
  2. `Paykan_release(old_a)`
  3. Store `b` into `a`
- [x] On `shared` scope exit: emit `Paykan_release()` for each live shared variable
- [ ] On `shared` variable passed to a function: retain before call, release after (or adopt callee-retain convention)
- [x] Register `Paykan_retain` and `Paykan_release` in JIT symbol table and CodeGen function table
- [x] Bootstrap `retain`/`release` in `CodeGen::bootstrapBuiltins()`
- [ ] Add codegen tests: verify retain/release IR emission
- [ ] Add JIT integration test: allocation counter confirms balanced retain/release

---

## Phase 7 — CodeGen: Reference Ownership (Borrow)

> Emit reference (borrow) codegen — no retain/release, just pointer aliasing.

- [x] `a: String& = b` → `a` alloca stores the same pointer as `b` (no copy, no retain)
- [x] Assignment through a reference: store to the referent's alloca, not the reference's
- [x] No cleanup emitted for references at scope exit
- [ ] Add codegen tests for reference IR shape
- [ ] Add JIT integration test: mutations through reference are visible on the original

---

## Phase 8 — End-to-End Samples & Stress Tests

> Validate the full ownership model with realistic programs.

- [x] `samples/basic/unique.pkn` — unique string created and auto-destroyed
- [x] `samples/basic/shared.pkn` — shared string with multiple aliases
- [x] `samples/basic/reference.pkn` — reference borrowing and mutation
- [x] `samples/errors/move_after_use.pkn` — use of moved unique variable
- [ ] `samples/errors/ref_escapes_scope.pkn` — reference outlives referent
- [x] `samples/errors/shared_builtin.pkn` — `shared int` rejected
- [ ] Memory-leak stress test: loop creating many objects, verify constant memory usage (JIT + allocation counter)

---

## Future Considerations (Out of Scope for Now)

- Weak references for shared objects (break cycles)
- Custom destructors / `drop` trait
- Ownership transfer across function boundaries (move params, owned return values)
- Thread-safe shared (atomic refcount)
- Escape analysis to auto-promote stack allocation

# Skill: PaykanLang — Compiler & Implementation

---

## Compiler CLI

```
paykan <source-file> [options]
```

By default: parse → type-check → generate LLVM IR → JIT-execute.

### Options

| Flag              | Description                                      |
|-------------------|--------------------------------------------------|
| `--dump-ast`      | Print the AST in tree form and exit              |
| `--emit-llvm`     | Print LLVM IR to stdout and exit                 |
| `--trace-parser`  | Enable Bison parser debug traces                 |
| `--trace-scanner` | Enable Flex scanner debug traces                 |
| `-O0` … `-O3`    | LLVM optimization level (default: `-O0`)         |

### Build

The project uses CMake. Binaries are output to `build/bin/`.

```bash
cd build
cmake ..
make
./bin/paykan ../samples/basic/function.pkn
```

Tests:
```bash
./bin/parser_tests
./bin/sema_tests
./bin/codegen_tests
```

Or via CTest:
```bash
ctest --test-dir build
```

---

## Architecture Overview

```
Source (.pkn)
  └─► Flex lexer          (src/Parser/)
  └─► Bison parser        (src/Parser/)
        └─► AST nodes     (include/AST.h, src/AST/)
              └─► Sema    (include/Sema.h, src/Sema/)
                    └─► CodeGen (include/CodeGen.h, src/CodeGen/)
                              └─► LLVM IR
                                    └─► ORC JIT → execution
```

---

## Key Source Files

| Path | Purpose |
|------|---------|
| `include/AST.h` | All AST node definitions (LLVM-style RTTI) |
| `include/ASTContext.h` | Arena allocator that owns all AST nodes |
| `include/ASTVisitor.h` | Visitor base class for AST traversal |
| `include/ASTPrinter.h` | `--dump-ast` implementation |
| `include/Sema.h` | Semantic analysis pass |
| `include/CodeGen.h` | LLVM IR generation pass |
| `include/JIT.h` | ORC JIT wrapper |
| `include/ParserDriver.h` | Driver that connects lexer + parser + AST |
| `include/Names.h` | Interned string table |
| `src/Parser/` | Bison grammar + Flex rules |
| `src/Runtime/` | C11 runtime (vtable, refcounting, string ops) |
| `tests/` | GTest-based unit tests |

---

## AST

- All nodes are arena-allocated via `ASTContext` — no manual `delete` needed.
- LLVM-style RTTI: use `isa<T>(node)`, `dyn_cast<T>(node)`, `cast<T>(node)`.
- Base class: `ASTNode` with an `enum NodeKind`.
- Key node families:
  - **Declarations**: `FuncDecl`, `VarDecl`
  - **Statements**: `CompoundStmt`, `IfStmt`, `WhileStmt`, `ReturnStmt`, `BreakStmt`, `ContinueStmt`
  - **Expressions**: `IntLiteral`, `FloatLiteral`, `BoolLiteral`, `StringLiteral`, `VarExpr`,
    `BinaryExpr`, `UnaryExpr`, `CallExpr`, `MovExpr`, `RefExpr`, `TernaryExpr`

---

## Semantic Analysis (`Sema`)

- Single-pass visitor over the AST.
- Maintains a **scoped symbol table** (stack of maps from name → `VarDecl*`).
- Tracks **moved variables** per scope.
- Tracks **live borrows** per owner (for the aliasing/XOR rules in `skills/02-ownership-system.md`).
- Emits diagnostics as `std::string` error messages with source location.
- Checks performed: see `skills/02-ownership-system.md` and `skills/03-functions-and-calling.md`.

### Cross-Module Signatures

When a module is imported, its function signatures (including parameter ownership and `const`
qualifiers, return ownership, and class hierarchy info) are exported as part of the module's
metadata so callers in other modules see the same signature the defining module did. There is
no implicit weakening of ownership across module boundaries. Type pointers (`ast::Type*`) are
remapped across `ASTContext` boundaries during import.

---

## Code Generation (`CodeGen`)

- Direct LLVM IR emission via the LLVM 17.0.6 C++ API (`llvm::IRBuilder<>`).
- Each function becomes an `llvm::Function`.
- Class instances are represented as `{ vptr, fields... }` aggregates. A bare `T` binding stores the aggregate inline (stack/struct field/array element); `T*` and `shared T` lower to a pointer to a heap cell holding that aggregate (plus a refcount header for `shared`).
- Unique variables: destructor call inserted at scope exit (via LLVM basic-block terminator hooks).
- Shared variables: `retain`/`release` calls inserted at assignment and scope exit.
- Reference variables: raw pointers, no retain/release.

---

## JIT Execution

- Uses LLVM ORC JIT (in-process).
- Runtime C11 library is linked at JIT time.
- Entry point: `main` symbol is resolved and called.
- Optimization level is configurable via `-O0` … `-O3` (LLVM new pass manager).

---

## Runtime (`src/Runtime/`)

Written in C11. Provides:

- **Vtable dispatch**: every `Object` starts with a vtable pointer; `toString`, `equals` are virtual.
- **Reference counting**: `pkn_retain(obj)` / `pkn_release(obj)` for shared ownership.
- **String operations**: allocation, concatenation, copy.
- **Built-in function implementations**: `std::out`, `std::print`, `std::err`, `std::printErr`; `(String)` cast for numeric-to-string conversion.

---

## Testing

Tests live in `tests/` and use **Google Test**.

| File | Tests |
|------|-------|
| `tests/ParserTests.cpp` | Parse valid and invalid programs; check AST structure |
| `tests/SemaTests.cpp` | Run sema on programs; check expected errors |
| `tests/CodeGenTests.cpp` | JIT-compile and run programs; check stdout output |
| `tests/TestUtils.h` | Shared helpers (capture stdout, build AST from string, etc.) |

---

## Third-Party Dependencies

| Dependency | Version | Location |
|------------|---------|----------|
| LLVM       | 17.0.6  | `build/third-party/llvm/` |
| Bison      | 3.8     | `build/third-party/bison/` |
| Flex       | 2.6.4   | `build/third-party/flex/` |
| Google Test| latest  | `build/third-party/gtest/` |

CMake setup scripts are in `cmake/`.

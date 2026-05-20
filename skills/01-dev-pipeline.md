# Skill: PaykanLang — Development Pipeline

---

## 1. Task and Ticketing System

### Structure

```
TODO/
  high/       ← must ship soon
  medium/     ← important but not blocking
  low/        ← nice to have
```

Each priority directory contains **epic directories**. Each epic directory contains **ticket files**.

### Epics

An epic represents a cohesive feature or area of work (e.g. `import-system`, `casting`).

**Naming:** numeric prefix, starting at `1`. Before creating a new epic, look at the highest
existing prefix in the target priority directory and increment by one.

```
TODO/high/
  1-import-system/
  2-casting/
```

### Tickets

A ticket is a single `.md` file inside an epic — one focused, actionable task.
Same numeric prefix scheme, scoped within the epic.

```
TODO/high/1-import-system/
  1-fix-importdecl-ast.md
  2-fix-parser.md
```

**Format:**
```markdown
# Short Title

What needs to change and why (1–2 sentences).

**Changes:**
- Concrete code change bullets.

## Dependencies
- `TODO/<priority>/<epic>/<ticket>.md`   ← omit section if none
```

Keep tickets under ~20 lines. Split if larger.

### Workflow

1. Pick the lowest-numbered ticket in the highest-priority epic.
2. Implement the change.
3. Verify correctness manually (run the compiler / JIT against a `.pkn` sample).
4. Update existing unit tests **or** add new ones in `tests/` to fully cover the change.
   - New tests must exercise every code path introduced or modified.
   - Run the full test suite (`ctest --output-on-failure`) and confirm all tests pass.
5. Commit everything (implementation + tests) with a message referencing the ticket:
   `fix(import): add SelectedNames to ImportDecl [import-system/1]`
6. Delete the ticket file.
7. When all tickets in an epic are done, delete the epic directory.

**Rules:**
- Never skip a prefix — always check the last number before creating.
- One ticket = one commit.
- Resolve dependencies before starting a ticket.
- After deleting a completed ticket, remove its path from any remaining tickets that list it as a dependency.

---

## 2. Build and Test Pipeline

### Build

The project uses **CMake** (minimum 3.24) with C++20. All outputs go to `build/bin/`.

```bash
# Configure (first time or after CMakeLists changes)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

# Build everything
cmake --build build

# Build a specific target
cmake --build build --target paykan
```

Key CMake targets:

| Target | Output | Description |
|--------|--------|-------------|
| `paykan` | `build/bin/paykan` | Compiler / JIT driver |
| `parser_tests` | `build/bin/parser_tests` | Parser unit tests |
| `sema_tests` | `build/bin/sema_tests` | Sema unit tests |
| `codegen_tests` | `build/bin/codegen_tests` | CodeGen + JIT tests |

Third-party deps (LLVM 17.0.6, Bison 3.8, Flex 2.6.4, GTest 1.15.2) are pinned and built
under `build/third-party/` on first configure.

### Test

Tests use **Google Test** and are registered with CTest.

```bash
# Run all tests
cd build && ctest --output-on-failure

# Run a specific suite directly
./build/bin/parser_tests
./build/bin/sema_tests
./build/bin/codegen_tests
```

Test suites:

| Suite | CMake test name | Covers |
|-------|----------------|--------|
| `parser_tests` | `ParserTests` | Lexer + parser |
| `sema_tests` | `SemaTests` | Type checking, ownership, sema errors |
| `codegen_tests` | `CodeGenTests` | LLVM IR emission, JIT execution, end-to-end output |

### Run a `.pkn` file

```bash
./build/bin/paykan path/to/file.pkn
```


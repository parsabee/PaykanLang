# PaykanLang — Engineering Backlog

Generated from expert reviews: Parser/Grammar, LLVM/CodeGen, SW QA.
49 tickets total. Sorted by priority then source.

---

## P0 — Fix Immediately (Correctness / Crash / UB)

### Parser / Lexer (from Parser Review)
- [x] **#1** Fix `fclose(stdin)` UB in `scanEnd()` — `src/Parser/Lexer.lpp`
- [x] **#2** Fix `make_CHAR` silently accepting multi-char literals like `'abc'` — `src/Parser/Lexer.lpp`
- [x] **#3** Fix `float` regex matching bare integers — `src/Parser/Lexer.lpp`

### CodeGen (from LLVM Review)
- [x] **#20** Fix vtable GEP element type in dispatch — `src/CodeGen/CodeGen.cpp:1821`
- [x] **#21** Add `CreateUnreachable()` after dead block `SetInsertPoint` in break/continue — `src/CodeGen/CodeGen.cpp:764`
- [x] **#22** Fix string double-wrap in `visitAssignStmt` for class-type rebind — `src/CodeGen/CodeGen.cpp:544`
- [x] **#23** Register `PaykanObject_None` in JIT symbol table — `src/JIT/JIT.cpp`

### QA / Testing (from QA Review)
- [x] **#36** Add `tests/CodeGen/LeakTests.cpp` using `Paykan_heap_set_tracking` for all leak-check scenarios
- [x] **#37** Add `cmake/SanitizerSetup.cmake` with ASan / UBSan / TSan options
- [x] **#38** Add `.github/workflows/ci.yml` with Debug, ASan, UBSan, and Linux CI jobs
- [x] **#39** Add `tests/CodeGen/CharTests.cpp` — char type end-to-end (14 tests)
- [x] **#40** Add `tests/CodeGen/ArrayTests.cpp` — subscript read/write end-to-end (9 tests)

---

## P1 — Fix Before Adding New Features

### Parser / Grammar (from Parser Review)
- [ ] **#4**  Add `error ";"` recovery rules to Bison grammar — `src/Parser/Parser.ypp` (zero error recovery today)
- [ ] **#5**  Replace assignment LHS `dyn_cast` hack with explicit `lvalue` non-terminal — `src/Parser/Parser.ypp`
- [ ] **#6**  Flatten `translationUnit` to allow any ordering of top-level declarations — `src/Parser/Parser.ypp`
- [ ] **#7**  Add `ResolvedType` to all `Expr` subclasses consistently — `include/AST.h`
- [ ] **#8**  `AssignStmt` should store `Identifier*` not `std::string VarName` — `include/AST.h`

### CodeGen (from LLVM Review)
- [ ] **#24** Clear `OwnedStringTemps` per function body in CodeGen — `src/CodeGen/CodeGen.cpp`
- [ ] **#25** Fix `InternedStrings` key type from `StringRef` to `std::string` — `src/CodeGen/CodeGen.cpp`
- [ ] **#26** Fix `emitDestructor` skipping field release when user body returns early — `src/CodeGen/ClassCodeGen.cpp:526`
- [x] **#27** Verify `kMalloc` and `kPaykanMalloc` resolve to the same symbol name — confirmed identical (`"Paykan_malloc"`)

### QA / Testing (from QA Review)
- [ ] **#41** Fill `tests/Sema/ArrayTests.cpp` — currently empty (30 sample files ready to use)
- [ ] **#42** Add `tests/Runtime/BoxedPrimitiveTests.cpp` for `PaykanInt/Float/Bool/Error`
- [ ] **#43** Add stdin safety test and missing `PaykanFile` / `PaykanString` function tests
- [ ] **#44** Add boxed primitive and `Error` codegen tests

---

## P2 — Quality / Optimization / Scalability

### Parser / Grammar (from Parser Review)
- [ ] **#9**  Introduce a central `DiagEngine` for all compiler diagnostics (replace `std::cerr` everywhere)
- [ ] **#10** Intern identifier strings to eliminate per-use heap allocations
- [ ] **#11** Disallow chained relational operators in grammar (`a < b < c` silently accepted)
- [ ] **#12** Fix `make_INT`: use `strtoll` with `INT64` bounds instead of `strtol` — `src/Parser/Lexer.lpp`

### CodeGen (from LLVM Review)
- [ ] **#28** Add `nounwind` / `readonly` / `argmemonly` attributes to runtime function declarations
- [ ] **#29** Attach `!invariant.load` metadata to vtable pointer loads — `src/CodeGen/CodeGen.cpp:1821`
- [ ] **#30** Fix constant float handling in `emitPrimitiveArrayLiteral` — `src/CodeGen/CodeGen.cpp:1022`
- [ ] **#31** Add post-optimize `verifyModule` in debug builds — `src/Driver/main.cpp`

### QA / Testing (from QA Review)
- [ ] **#45** Add `tests/fuzz/FuzzParser.cpp` libFuzzer target
- [ ] **#46** Replace `dup2` stdout capture in `TestUtils.h` with per-invocation temp file (fix hang-on-crash)
- [ ] **#47** Add `main(args: Str[])` codegen test
- [ ] **#48** Add `tests/Driver/` with `--check-only` and `--dump-ast` smoke tests

---

## P3 — Maintainability / Architecture / Future

### Parser / Grammar (from Parser Review)
- [ ] **#13** Add `NK_StmtBegin` / `NK_StmtEnd` sentinels to `NodeKind` enum — `include/AST.h`
- [ ] **#14** Convert `MatchArm` to a proper `ASTNode` subclass — `include/AST.h`
- [ ] **#15** Switch `ASTContext` pool to `BumpPtrAllocator`; pre-reserve in the meantime — `include/ASTContext.h`
- [ ] **#16** Add reverse map to `ASTContext` for O(1) `getSpecializedArrayElemType` — `src/AST/ASTContext.cpp`
- [ ] **#17** Add `= delete` for copy/move on `ASTContext` — `include/ASTContext.h`
- [ ] **#18** Add `unordered_map` index to `ClassType` for O(1) method lookup — `src/AST/AST.cpp`
- [ ] **#19** Reject empty `match` arms at grammar level — `src/Parser/Parser.ypp`

### CodeGen (from LLVM Review)
- [ ] **#32** Introduce `ExprValue` ownership type to unify ARC retain/release decisions — `src/CodeGen/CodeGen.cpp`
- [ ] **#33** Use `ThreadSafeContext` per imported module in `CodeGenImport` — `src/CodeGen/CodeGenImport.cpp`
- [ ] **#34** Factor `CodeGenFunction` out of `CodeGen` for parallel function compilation — `src/CodeGen/`
- [ ] **#35** Add compile-time sync check between `Runtime.h` declarations and JIT symbol table

### QA / Testing (from QA Review)
- [ ] **#49** Fix hardcoded vtable slot index in `tests/Runtime/StringTests.cpp:268`

---

## Summary

| Priority | Parser/Grammar | CodeGen | QA/Testing | Total |
|----------|---------------|---------|------------|-------|
| P0       | 3             | 4       | 5          | **12** |
| P1       | 5             | 4       | 4          | **13** |
| P2       | 4             | 4       | 4          | **12** |
| P3       | 7             | 4       | 1          | **12** |
| **Total**| **19**        | **16**  | **14**     | **49** |

# PaykanLang — Task Plan to 0.1 Release

> **Historical — superseded.** This is an early plan, written when the compiler was
> LLVM/JIT-only. It no longer describes the v0.1.0 release: MLIR code generation, the AI
> offload/optimization plugin and the alpha/beta/0.1 checkpoints below are not part of it.
> The current v0.1.0 scope and definition of done are tracked in
> [#27](https://github.com/parsabee/PaykanLang/issues/27); see [CHANGELOG.md](CHANGELOG.md)
> for what has shipped. The document is kept for reference only.

The USP (unique selling point) for 0.1: a **clean, sequential, simple** language whose
compiler **generates MLIR** and uses **AI-assisted optimization** to offload/parallelize
code, cleanly integrated **with safe guardrails that prevent erroneous codegen**, exposed
through a **universal plugin for AI models** (Claude Code, ChatGPT, …).

The roadmap has three checkpoints on the way to 0.1: **alpha**, **beta**, **0.1**.
This plan expands each into concrete, dependency-ordered tasks with acceptance criteria.

---

## Current state (v0.0.0 preview) — baseline

| Capability | Status |
|---|---|
| Static typing, Sema, diagnostics | ✅ done |
| Classes, vtable dispatch, ARC (no GC) | ✅ done — the 2026-09-28 triage found ARC crashers (self-as-value, match-binding, array-field double-frees); fixed via the unique-box invariant (`TODO/TICKETS-analysis-2026-09-28.md`, PAY-1) |
| Enums, `match` | ✅ done |
| Dynamic arrays `T[]` (subscript, len/push/pop, 2D) | ✅ done — 2026-09-28 push/pop-receiver and array-equality bugs fixed (PAY-1/PAY-2) |
| Module system + selective/aliased imports + bitcode cache | ✅ done |
| LLVM 17 backend, ORC JIT | ✅ done |
| **`for` loop syntax** | ❌ only `while` |
| **Static / fixed-size arrays** | ❌ only heap-dynamic |
| **MLIR codegen + custom dialect** | ❌ none |
| **AOT binary emission** | ❌ JIT only |
| **Package manager** | ❌ none |
| **AI/MCP optimization integration** | ❌ none |

Prebuilt LLVM 17.0.6 is downloaded (RTTI-off, **ships no MLIR**); build is `-fno-rtti -fno-exceptions`.

---

## Phase A — Finish ALPHA (polish the LLVM+JIT language)

*Goal: "working, no major bugs" language, module system improved. All in the existing
direct-LLVM path — no MLIR yet.*

- [ ] **A1. Bug triage bar.** Enumerate known bugs; define the "no major bugs" gate
  (crashes, miscompiles, leaks). Land fixes. **Accept:** all `samples/` + `tests/` green,
  zero leaks under `--track-heap` on the leak-check corpus.
  *One triage round executed 2026-09-28:* `TODO/TICKETS-analysis-2026-09-28.md` (PAY-1…PAY-5)
  — ARC double-free crashers, mov flow-sensitivity holes, array `==`, parser/lexer fixes all
  landed in the working tree; remaining known issues live in `TODO/TODO.md`.
- [ ] **A2. Module system updates** (explicit alpha item). Audit import resolution,
  diamond imports, qualified `mod::Type`, re-export/visibility gaps; nail down visibility
  rules (public/private), submodule/path conventions, and import diagnostics. **Accept:**
  documented module semantics + tests for diamonds, cycles, aliasing, cross-module types,
  and visibility violations.
- [ ] **A3. C interoperability (FFI).** A first-class way for Paykan to call into C and,
  where useful, expose Paykan functions with the C ABI — building on the fact that the
  runtime is already a C-ABI library and the JIT registers C symbols by name.
  - [ ] **A3.1 `extern` C declarations.** Syntax to declare an external C function
    (name, params, return) — new AST node, lexer/grammar, Sema type-check. Lower to an
    LLVM external `declare` / `func.call` to the symbol.
  - [ ] **A3.2 C type mapping.** Define the Paykan↔C type correspondence (`int`↔`i64`,
    `float`↔`f64`, `bool`, `char`, raw pointers, `Str`↔`const char*` bridging) and the
    marshalling rules; document what is *not* passable across the boundary (boxed/ARC objects
    without an explicit bridge).
  - [ ] **A3.3 Ownership / ARC boundary.** Rules for who owns memory crossing the FFI line;
    prevent ARC double-free / leak on values handed to or received from C. Reuse `--track-heap`
    to verify leak-neutrality across FFI calls.
  - [ ] **A3.4 Linking.** JIT path — register external C symbols (extend the existing
    `absoluteSymbols` mechanism / `dlopen` for shared libs); prepare the hook AOT linking
    (Phase E) will reuse. Optional `#[link]`/pragma to name a library.
  - [ ] **A3.5 Export Paykan → C** *(optional for alpha)*. Mark a Paykan function to emit
    with an unmangled C ABI symbol so C code can call it.
  - [ ] **Accept:** a Paykan program calls a libc function (e.g. `abs`, `strlen`) and a
    user C function linked at JIT time, with correct results and zero leaks under `--track-heap`;
    FFI samples + Sema/CodeGen tests; language-reference section for the FFI.
- [ ] **A4. Test/coverage hardening.** Push CodeGen/Sema/Parser coverage; add fuzz smoke
  for parser. **Accept:** coverage target met in CI (owner: `compiler-devops-engineer`).
- [ ] **A5. Tag `0.0.x` alpha**, update CHANGELOG.

**Exit criteria:** language runs real programs with no major bugs; module system solid;
C FFI can call external C functions leak-cleanly.

---

## Phase B — Language surface for BETA (loops + static arrays)

*Goal: add the two constructs beta requires. Land them in the **existing LLVM path first**
for parity, because they are also the prerequisites that make MLIR analyzable later.*

- [ ] **B1. `for` loop syntax.** New `ForStmt` AST node (induction var, bounds, step; and/or
  range/foreach form). Lexer `FOR` token, grammar rule, `ASTVisitor` X-macro entry, Sema
  type-check, ASTPrinter, LLVM codegen. **Accept:** for-loop samples + Sema/CodeGen/Parser
  tests; leak-clean. *(Design note: shape the AST so it lowers cleanly to `scf.for` later —
  explicit IV + bounds + step, not desugared to `while`.)*
- [ ] **B2. Static / fixed-size arrays.** Syntax + type (`T[N]`), stack or inline storage,
  bounds semantics, interaction with existing dynamic `T[]`. Sema + LLVM codegen. **Accept:**
  static-array samples + tests; leak-clean. *(Design note: fixed extent = analyzable
  `memref<NxT>` later.)*
- [ ] **B3. Docs + language reference** updates for loops and static arrays.

**Exit criteria:** `for` and static arrays work end-to-end on the LLVM path with tests.

---

## Phase C — MLIR migration (BETA core: "generate mlir, with a custom dialect")

*Goal: emit MLIR instead of direct LLVM, via a thin custom `paykan` dialect, lowering back
to the **existing** ORC JIT. Grounded in the codegen review: the JIT seam is a plain
`llvm::Module` calling ~80 runtime symbols by name, so JIT/runtime stay untouched.*

### C1. Toolchain & build (biggest practical hurdle)
- [ ] Build **LLVM+MLIR from source** (`-DLLVM_ENABLE_PROJECTS=mlir`); prebuilt tarballs
  ship no MLIR. Update `cmake/LLVMSetup.cmake`; add CI caching for the source build.
- [ ] **Resolve the RTTI policy up front.** MLIR is conventionally RTTI-on; Paykan is
  `-fno-rtti` (matching prebuilt LLVM). Decide: build MLIR RTTI-off to keep `-fno-rtti`,
  **or** flip Paykan to `-frtti`. Mixing risks vtable/`dyn_cast` mismatches.
- [ ] CMake: `find_package(MLIR CONFIG)`, `AddMLIR`, link `MLIR*` libs; wire `mlir-tblgen`.
- [ ] Add `--emit-mlir` driver flag beside `--emit-llvm`/`-O`. **Accept:** empty
  `mlir::ModuleOp` → `translateModuleToLLVMIR` → `llvm::Module` → existing `runModule`
  executes an empty program end-to-end.

### C2. Custom `paykan` dialect (minimal — only what standard dialects can't express)
- [ ] TableGen dialect: `!paykan.shared<T>` box type, `!paykan.obj`; ops `paykan.box` /
  `paykan.unwrap`, `paykan.dispatch` (vtable), `paykan.array` wrapper (len/cap/push/pop).
- [ ] **ARC as scoped-effect ops** — `paykan.retain` / `paykan.release` whose memory effect
  targets **only the box's refcount resource**, not general memory. *This is the crux:*
  opaque `func.call @Paykan_retain` would block reordering/elimination AND block
  parallelization of any region containing it.
- [ ] **`-paykan-arc-optimize` pass** — Swift/objc-ARC-style redundant retain/release
  elimination. **Accept:** ARC pass cancels provably-redundant pairs; verified leak-neutral.

### C3. Emitter port (bottom-up, direct-LLVM path as differential oracle)
Reuse `--track-heap` live-block counts as the leak oracle at every step.
- [ ] Scalars: `arith`/`func`/`scf.if`, primitive expressions, `if`/`return`, free functions.
- [ ] Control flow: `while` → `scf.while`; `for` (B1) → **`scf.for`**; `break`/`continue`.
- [ ] Strings + `paykan.shared` box + ARC ops; differential-test leaks vs `--track-heap`.
- [ ] Classes: struct layout, `paykan.dispatch`, ctor/dtor, member access/assign.
- [ ] Arrays: first as `paykan.array` + runtime calls (correctness parity), **then** a
  `memref` view for subscripts so `arr[i]` = `memref.load/store` (analyzable form).
- [ ] Imports / multi-module (mirror current `processImports`).

### C4. Lowering pipeline
- [ ] `paykan` + `arith`/`scf`/`memref`/`func` → progressive lowering → **`llvm` dialect** →
  `translateModuleToLLVMIR` → existing `optimize()` → existing JIT. Custom ops materialize
  as C-ABI runtime calls **only at final lowering** (so ARC-opt can run first). `memref`
  loads/stores lower to GEP/load/store (not runtime calls).
- [ ] **Accept:** every `samples/` program produces identical output + identical
  `--track-heap` results on the MLIR path vs the LLVM path. Then flip default to `--emit-mlir`;
  keep LLVM path behind a flag through 0.1 soak.

**Exit criteria:** compiler generates MLIR through a custom dialect, lowers to the existing
JIT, at full parity with the LLVM path.

> **⟶ BETA milestone ends here.** Beta = a solid, MLIR-generating language (loops + static
> arrays + custom `paykan` dialect + JIT). The AI/MCP work below is deferred to 0.1.

---

## Phase D — AI-assisted optimization + MCP (0.1 — USP core)

*Goal: MCP servers that let a pluggable AI detect and safely apply parallel/async/offload
rewrites on the `paykan`/`scf`/`memref` IR — with guardrails that prevent bad codegen.*

- [ ] **D1. MCP server #1 — dialect & lowering knowledge.** Serves the `paykan` dialect
  spec, op semantics, type system, and lowering paths so an AI model can reason about the IR.
  **Accept:** an AI client can query op/type/lowering facts and get accurate, versioned answers.
- [ ] **D2. MCP server #2 — parallelization advisor.** Given a module + call graph +
  dependency info, detects parallelizable / async / offloadable regions and proposes rewrites
  (`scf.for`→`scf.parallel`/`forall`, `async.execute`, `gpu.launch`). **Accept:** on a
  benchmark corpus it flags known-parallel loops and rejects loop-carried-dependent ones.
- [ ] **D3. Safety guardrails (the differentiator — "prevent erroneous codegen").** A
  mandatory verification gate every AI-proposed rewrite must pass before it is accepted:
  (a) `mlir-opt --verify-each` well-formedness; (b) dependence/alias check must *prove*
  independence (conservative — unknown aliasing ⇒ reject); (c) differential execution:
  sequential vs rewritten output must match on representative inputs; (d) leak-neutrality via
  `--track-heap`; (e) FP-associativity changes from parallel reductions flagged/opt-in.
  **Accept:** a deliberately-unsafe rewrite is caught and rejected by the gate.
- [ ] **D4. Universal AI plugin abstraction.** Pluggable LLM backend (Claude Code, ChatGPT,
  …) behind one interface; model choice is config. **Accept:** same optimization flow runs
  through ≥2 backends.
- [ ] **D5. Parallelization passes wired end-to-end.** AI proposes → guardrails gate →
  accepted rewrites lower to CPU (`async`/`omp`) — GPU (`gpu`→`nvvm`/`rocdl`/`spirv`) optional
  stretch. **Accept:** a parallelized sample runs correctly and shows measurable speedup.

**Exit criteria:** AI can propose parallel/offload rewrites; only provably-safe ones land.

---

## Phase E — 0.1 release engineering (binary + package manager + bug sweep)

*Phases D and E together constitute the **0.1 release**.*

- [ ] **E1. AOT binary emission** (0.1 item "generate binary + jit"). `TargetMachine` →
  object file → link runtime → native executable; keep JIT path. Driver `-o` / `--emit-obj`
  / compile-to-exe. **Accept:** `paykan build foo.pkn -o foo && ./foo` matches JIT output;
  works on macOS + Linux.
- [ ] **E2. Package manager** (0.1 item). Manifest format (name/version/deps), dependency
  resolution, fetch/registry (or git sources), lockfile, integration with the module/import
  system + bitcode cache. **Accept:** a project can declare a dependency, resolve, build, run.
- [ ] **E3. Major-bug resolution sweep** (0.1 item "all major bugs resolved"). Full
  regression across LLVM + MLIR paths, sanitizers (ASan/UBSan/TSan for parallel codegen).
- [ ] **E4. Release engineering.** Bump `project(VERSION 0.1.0)`, CHANGELOG, distribution
  (Homebrew formula / Ubuntu packaging — owner: `compiler-devops-engineer`), docs.

**Exit criteria:** 0.1 tagged — MLIR codegen, AI-assisted safe parallelization, native
binaries + JIT, and a working package manager.

---

## Critical path & dependencies

```
   ALPHA          ├──────────── BETA ────────────┤   ├─────────── 0.1 ───────────┤
A (polish) ──► B (for + static arrays) ──► C (MLIR + paykan dialect) ──► D (AI/MCP) ──► E (binary + pkg mgr)
                    │                            │                          │
               prerequisite for          scf.for / memref            guardrails gate
               analyzable IR             from B constructs           every AI rewrite
```

- **B blocks C's payoff:** without a structured `for` (→`scf.for`) and fixed/analyzable
  arrays (→`memref`), MLIR has nothing to parallelize.
- **C blocks D:** the parallelization advisor operates on `paykan`/`scf`/`memref` IR.
- **C2 ARC ops block correctness *and* parallelism:** scoped-effect retain/release is what
  lets ARC survive optimization and not serialize parallel candidates.
- **C1 toolchain (from-source LLVM+MLIR + RTTI decision) is the top risk** — de-risk first.

## Top risks
1. **From-source LLVM+MLIR build** replaces the 1-min prebuilt download (multi-GB, slow CI).
2. **RTTI policy** conflict (`-fno-rtti` vs MLIR conventions) — decide before writing emitters.
3. **Re-expressing ARC** (`OwnedStringTemps`, `ExprValue`, scope cleanup) in MLIR without
   regressing leak tests — riskiest part of the emitter port; `--track-heap` is the oracle.
4. **Guardrail soundness** — the safety gate is the USP; a false "safe" verdict is the worst
   failure mode. Keep it conservative (reject on uncertainty).
5. **Scope of D (AI + 2 MCP servers + plugin)** is large and partly research-y; consider
   landing D1+D3 (knowledge + guardrails) before D2/D5 (automated rewrite).

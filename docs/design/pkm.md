# Design: the compiled Paykan module (`.pkm`)

**Status:** proposal, for review. Tracks #57. No code exists yet.
**Scope:** the file format, what produces and consumes it, and the
phased plan. v0.1.0 gets a minimal, shippable subset. Everything else gets
reserved space in the format, so that it can be added later without a
redesign.

Line references are to `develop` @ `9440486`.

---

## 0. Summary

A `.pkm` file is one compiled Paykan module. Every backend consumes it the
same way. It has two halves:

1. **Interface:** what an importer's Sema and lowering need. That covers the
   exported enums, classes (layout, vtable slot order, slot targets,
   constructor/destructor/vtable symbols), and functions (Paykan signature,
   PIR signature, link symbol). It also lists the module's dependencies, each
   with its canonical name and interface hash. This section is built from
   **Sema's view of the module**, not from its PIR, because PIR has erased
   the Paykan types (`Str`, `Shape` and `T?` are all `box`) and contains no
   enums or templates.
2. **Code:** the module's verified PIR in a compact binary encoding. A symbol
   index maps every defined symbol to its record.

**Key decision: cross-module generics.** Importing a generic template from
another module is rejected today, so it is **new functionality** that the
`.pkm` has to enable or defer. This proposal defers it to v0.2.0 and
reserves a `templates` section for it (§2.1, §12).

In v0.1.0 the driver builds a `.pkm` for every imported module on demand into
`.paykan_cache/` and reads imports **only** from those files. Imports are no
longer re-parsed, re-checked or re-lowered on every build. The backends don't
change: the driver reassembles a `pir::Program` from the main module's fresh
PIR plus the code sections, so the C and LLVM backends keep consuming the
same PIR they see today. The existing per-backend caches stay where they are,
behind the `.pkm`.

The format is a custom little-endian container written and read with
standard C++ only. It is deterministic (byte-identical for identical inputs),
contains no absolute paths (#102), and is integrity-hashed (the #74 lesson).
The reader is bounds-checked and runs the PIR verifier on everything it loads.

---

## 1. Where we are today

| Step | What happens to an import today | Where |
|---|---|---|
| Resolve | `a::b` → `<root>/a/b.pkn` → `std::filesystem::canonical` (absolute path) | `SemaImport.cpp:131-156`, `LoweringProgram.cpp:20-52` |
| Parse + Sema | the imported file is **parsed and fully type-checked again on every compile** | `SemaImport.cpp:399-437` |
| Interface | an in-memory `ModuleInfo` (types spelled as strings) is built from the module's ASTContext and injected into the importer; cached per process only (`static ModuleCache`) | `Sema.h:431-485`, `SemaImport.cpp:439-549`, injection `:235-392` |
| Lower | the importer's lowering lowers the imported AST to a full `pir::Module`, then declares what it uses as `extern fn … module "…" symbol @…` / `extern class` by looking at that PIR | `LoweringProgram.cpp:76-128`, `Lowering.cpp:412-467`, `LoweringClass.cpp:189-213` |
| Verify | `verify(Program)` cross-checks extern signatures and layouts against the definitions | `Verifier.cpp:660-716` |
| Backend cache (C) | `.paykan_cache/<rel>.c/.o/.key`; key = cc + flags + hash of `Runtime.h` + `kVersion` + hash of the module's **generated C** + object stamp | `CBuild.cpp:149-252` |
| Backend cache (LLVM) | `.paykan_cache/<rel>.bc`; key = SHA-256 of the module's **PIR text** + version + LLVM version + compiler build id (exe size/mtime) + `kPaykanABIVersion` (= 5) | `PIRToLLVM.cpp:833-871`, `:968-997` |

Facts that shape the design:

- **Only the last step is cached.** Parse, Sema and lowering of every import
  run on every build (the problem statement of #57).
- **Module identity is an absolute path.** That holds for PIR `module` names
  (`PIR.h:267`, `pir.md:116`), extern clauses, LLVM symbol names
  (`PIRToLLVM.cpp:79-86` mangles as `<module.Name>::<name>`) and cache paths.
  #102 replaces it with canonical module names (`geometry::shapes`), which is
  what `06-modules.md:3-4` already promises. This design **assumes #102 has
  landed**. A `.pkm` never contains a filesystem path.
- **C symbol names depend on the whole program.** `CEmitter.cpp:355-366`
  reserves `pk_<stem>_<name>` and adds `_<n>` on a clash across the program,
  so a module's C, and therefore its `.o`, is only valid for the program it
  was built in. The C cache copes by keying on the generated C text
  (`CBuild.cpp:187`). Native objects can't go into a `.pkm` until mangling is
  stable (§6).
- **Cross-module generics don't exist today; they are new functionality.**
  "Generic classes and functions are not exported" (`11-generics.md:189-210`).
  `b = shapes::Box<int>(7)`, with `class Box<T>` in `geometry/shapes.pkn`,
  is rejected with "error: generic types and functions cannot be imported
  yet: 'shapes::Box<...>' names a template of another module"
  (`SemaClass.cpp:1125`; see also `:802`, `:917`), and the error is printed
  twice. Only a module's own concrete instantiations (`Box<int>`) are
  exported, as ordinary classes. The `.pkm` design therefore isn't preserving
  an existing feature here. It has to **decide whether to enable** importable
  templates, and when. That is one of the key decisions; see §2.1 and §12.
- **PIR loses source-level types, so the interface can't be derived from
  PIR.** In PIR, fields and parameters are only `i64`/`f64`/`bool`/`char`/
  `box`/`obj`/`ptr` (`PIR.h:23-32`): a `Str`, a `Shape` and a `Point?` field
  are all `box`, and a `float` is `f64`. Enums lower to `i64` and don't
  appear in PIR at all (`pir.md:44-48`), and templates are never lowered
  (only their instantiations). Sema needs the Paykan types, so the interface
  section is produced **from Sema's view** (today's `ModuleInfo` data),
  alongside the PIR, never reconstructed from the code section.
- **Ownership conventions are uniform.** Every user-function argument is an
  owned +1 box that the callee releases (`pir.md:287-290`), so v0.1.0 needs
  no per-function convention data. #98 changes this in v0.2.0 (§9).
- **Cross-module subclassing copies vtable targets.** In
  `samples/imports/06_class_inherit`, the importer's `class Circle : Shape`
  emits a full vtable whose inherited slot is
  `perimeter = @Shape.perimeter`, an `extern fn … module "<base>"`. So the
  interface must carry each class's **slot targets**, not only the slot order.
- **The core is standard C++20 with `-fno-exceptions -fno-rtti`**
  (`CMakeLists.txt:75`). The only hash in the core is the 64-bit FNV-1a in
  `CBuild.cpp:83-92`; SHA-256 lives in LLVM.

---

## 2. Goals and non-goals

### v0.1.0 (must ship)

- G1. An importer type-checks, lowers and links against `.pkm` files alone.
  Imported sources are never re-parsed, re-checked or re-lowered when their
  `.pkm` is fresh.
- G2. One format that every backend consumes the same way, through
  `pir::Program`. No backend changes are required.
- G3. Correct invalidation: a module is rebuilt when its source changes or
  when the interface of any module it imports changes. Its importers are
  rebuilt **only** when its interface changes.
- G4. Deterministic, path-free, integrity-checked, versioned output. Garbage
  input never crashes or misleads the compiler.
- G5. Standard C++ only, inside the barebones core.
- G6. A debugging aid: `paykan --dump-pkm f.pkm` prints the file as text
  (header, interface, PIR).

### Later (format space reserved, not implemented in v0.1.0)

| Item | Target | Notes |
|---|---|---|
| Generic templates in the interface | v0.2.0 | §4 |
| Native objects embedded in, or keyed by, the `.pkm` | v0.2.0 | needs stable mangling; §6 |
| Prebuilt module distribution and search paths | v0.2.0 | with the package manager / #23; §7 |
| Per-parameter ownership conventions | v0.2.0 | #98 borrowed parameters; §9 |
| Post-pass PIR and cross-module inlining | v0.2.0 | #97; §5 |
| Lazy per-symbol loading, JIT hot reload | v0.2.0+ | #25; the symbol index exists from v0.1.0 |
| Exported header for C FFI | v0.2.0 | #21; generated from the interface |
| Debug info (source locations of declarations) | later | an optional section |

### 2.1 Key decision: cross-module generics, enable now or defer?

Importing a generic template is new functionality (§1). Today it is
rejected with an error.

| Option | Pros | Cons |
|---|---|---|
| **A. Defer to v0.2.0.** v0.1.0's `.pkm` exports concrete instantiations only (today's behaviour) and reserves the `templates` section kind | keeps v0.1.0 small; no AST serializer; no instantiation-dedup machinery in the driver; the language behaves as it does today | stdlib-style generic containers (`Stack<T>`) can't live in a module yet |
| B. Enable in v0.1.0 | generic libraries become possible at once | needs the template representation (§12.1), cross-module instantiation ownership and dedup (§12.2), and new Sema paths, all before the tag; this widens a release that already has a long list |

**Recommendation: A.** The format reserves room for templates (§3.4, §12),
so B arrives as a minor format bump. This is open question 2.

### Non-goals

- Compatibility across versions before 1.0. A version mismatch means
  rebuild, or a clear error when no source is available (§9).
- Native object containers (ELF/Mach-O/COFF) for the `.pkm` itself (§3.1).
- Cross-target builds (#23). The header records the target assumptions so
  that a mismatch is detected, but v0.1.0 supports only the LP64
  little-endian host.

---

## 3. File layout

### 3.1 Decision: container

| Option | Pros | Cons |
|---|---|---|
| **A. Custom binary container** (header + section table) | Standard C++ only; small; full control over determinism; trivial to fuzz | standard tools (`nm`, `ar`) don't understand it; we need a `--dump-pkm` |
| B. Native object file with a `.paykan.iface` section | "a module is an object"; `ld`/`ar`/`nm` work | needs an ELF/Mach-O/COFF writer in the core (LLVM is not allowed there); per-target; the C backend would have to post-process `cc` output; JIT and out-of-tree backends would have to parse objects |
| C. A zip/tar of text files (interface JSON, `.pir`) | readable, reuses the text parser | needs a container parser anyway; not deterministic without care; slow to load; text is not "lowered to binary" |

**Recommendation: A.** `.pkm` files can still be bundled with `ar` later (a
library is an archive of `.pkm` files, #23), and native objects, once they
exist, live next to the `.pkm` rather than inside an object-file wrapper.

### 3.2 Encoding conventions

- All fixed-width integers are **little-endian**. The header and section
  table use fixed widths so that they can be read with no decoding state.
- Inside sections, counts, ids and indices are **ULEB128** and signed
  constants are **SLEB128**. PIR is dominated by small ids, so this roughly
  halves the size compared with fixed `u32`s. `f64` is stored as its 8 raw
  IEEE-754 bytes, so the binary form keeps NaN payloads and `-0.0` exactly.
  The text form drops NaN payloads (`pir.md:65-70`), so round-trip tests
  compare the text form (§10).
- **Strings** live in one string table section (ULEB128 length + UTF-8
  bytes, no NUL). Every other section references them by index. Index order
  is first use in the writer's deterministic traversal.
- **Hashes are SHA-256** (32 bytes). The core gets a small standard-C++
  SHA-256 (about 150 lines, FIPS 180-4 test vectors) in a shared support
  library. The LLVM cache could then use it too. FNV-1a is fine for a
  private cache, but a `.pkm` hash is an identity that other files record.

### 3.3 Header (fixed 128 bytes)

| Offset | Size | Field | v0.1.0 value / meaning |
|---|---|---|---|
| 0 | 8 | magic | `89 50 4B 4D 0D 0A 1A 0A` (`\x89PKM\r\n\x1a\n`, PNG-style: catches text-mode and truncation damage) |
| 8 | 2 | format major | 1. A reader rejects any other major |
| 10 | 2 | format minor | 0. Bumped for additive changes within a major (§9) |
| 12 | 4 | PIR version | 1. Bumped whenever the PIR instruction set or types change (#96, #99) |
| 16 | 4 | runtime ABI version | the core's `kPaykanABIVersion` (moved out of `PIRToLLVM.cpp:839` into a core header so that every backend and the `.pkm` share it) |
| 20 | 1 | pointer size | 8 |
| 21 | 1 | slot size | 8 (array/tuple slots, `pir.md:22-23`) |
| 22 | 1 | endianness | 1 = little |
| 23 | 1 | reserved | 0 |
| 24 | 4 | flags | bit 0: module defines `@main`; bit 1: system (stdlib) module; others 0 |
| 28 | 4 | section count | |
| 32 | 8 | section table offset | 128 in v0.1.0 |
| 40 | 32 | **content hash** | SHA-256 of every byte after the header |
| 72 | 32 | **source hash** | SHA-256 of the module's source bytes (staleness check, §7) |
| 104 | 4 | compiler version | string-table index of `kVersion` (e.g. `0.1.0`) |
| 108 | 4 | module name | string-table index of the canonical name (`geometry::shapes`) |
| 112 | 16 | reserved | zero |

The interface hash is **not** in the header. It is the SHA-256 of the
interface section's bytes, recomputed on load. Storing it would only invite
disagreement with the section.

### 3.4 Section table

There is one 32-byte entry per section, sorted by `kind`:

| Size | Field |
|---|---|
| 4 | kind |
| 4 | flags: bit 0 = **required** (a reader that doesn't know the kind must reject the file); unknown optional sections are skipped |
| 8 | offset |
| 8 | size |
| 8 | reserved |

Every section must lie inside the file, and sections must not overlap.

| Kind | Name | Required | v0.1.0 |
|---|---|---|---|
| 1 | `strtab` | yes | yes |
| 2 | `deps` | yes | yes |
| 3 | `types` (interface type table) | yes | yes |
| 4 | `iface` (exported decls) | yes | yes |
| 5 | `code` (binary PIR module) | yes | yes |
| 6 | `symidx` (symbol → code record) | no | yes |
| 7 | `templates` | yes when present | reserved (§4) |
| 8 | `native` (per-target objects or references) | no | reserved (§6) |
| 9 | `debug` (decl source locations, relative paths) | no | reserved |

What the interface hash covers: `deps` + `types` + `iface` (+ `templates`),
which is everything an importer's output can depend on. Changing a function
body changes `code` but not the interface hash, so importers are not rebuilt.

---

## 4. Interface section

### 4.1 What goes in

The importer side today consumes exactly two things:

- Sema needs `ModuleInfo`: enums, classes with super, fields, methods
  (including `__init__`), method flags, functions, plus `IsLocal` and
  `OriginPath` for types reached through imports (`Sema.h:431-482`).
- Lowering needs, from the defining module's PIR, the PIR signature and name
  of every function it calls (`Lowering.cpp:438-467`), the layout of every
  class it touches (`LoweringClass.cpp:189-213`), and the vtable targets it
  inherits (§1).

The interface is the union of these two, made path-free:

| Record | Contents |
|---|---|
| **Dependency** (`deps`) | canonical module name, system flag, **interface hash** at build time. Only the module's direct imports are listed; the closure is reached through each dependency's own `deps`. |
| **Enum** | name; variants in declaration order (the value is the index, `Sema.h:473-474`) |
| **Class** | name; superclass as a type ref (or none for `Obj`); **fields** in layout order, ancestors first (name, Paykan type, PIR type); **vtable** in slot order: slot name, Paykan signature, PIR signature, flags (private), **target symbol** (or abstract); `__init__` signature; the symbols of the constructor (`@C`), destructor (`@C.destroy`) and vtable; header kind (v0.1.0: `two-word {vtable, backpointer}`, `pir.md:157-159`; see #93) |
| **Function** | name; Paykan signature; PIR signature; link symbol; per-parameter convention byte (v0.1.0: always `owned`; see #98) |
| **Type table** (`types`) | every Paykan type the records mention, as a DAG of tagged entries: builtin, class ref, enum ref, array(T), optional(T), tuple(T…). A class or enum ref is `(defining module, name)`, so `IsLocal`/`OriginPath` become structural. |

Decls are sorted by name within each kind, so the order is independent of
hash-map iteration. Today's code sorts for the same reason
(`SemaImport.cpp:521-531`).

### 4.2 Decision: how types reached through imports are described

`ModuleInfo` today copies the full description of every class the module
merely reached through its imports (`SemaImport.cpp:463-505`).

| Option | Pros | Cons |
|---|---|---|
| **A. Reference** `(module, name)` only; the importer loads that module's `.pkm` to reconstruct the type | no duplication; one source of truth per type; type identity is structural | the importer must have the whole dependency closure, which it needs anyway to link |
| B. Copy the full description (today's behaviour) | each `.pkm` is self-contained for Sema | copies can disagree; the interface hash changes when a transitive type changes even if this module's use of it didn't |

**Recommendation: A.** Linking already needs the closure. Non-transitivity
(`06-modules.md:100-121`) is unchanged: the importer reconstructs a referenced
type but binds it to **no** qualifier, exactly as `IsLocal = false` does
today.

### 4.3 Decision: how types are encoded

| Option | Pros | Cons |
|---|---|---|
| A. Strings in the `ast::typeName` grammar (today) | reuses `parseExportedType` (`SemaImport.cpp:44-117`) | a string parser in the hot path; `<…>` nesting is ad hoc; class names inside are unqualified, so they depend on "type names are global" |
| **B. Structured type table** with indices | trivial, bounds-checked decoding; qualified refs; easy to extend (`weak T` #99, inline `int?` #96) | slightly more writer code |

**Recommendation: B.**

### 4.4 Why the interface holds both Paykan and PIR views

The interface can't be derived from the `code` section: PIR has erased the
Paykan types and has no enums or templates at all (§1). So the Paykan view
is primary, and it comes from Sema.

Is the PIR view redundant, then?

The PIR signatures and layouts are derivable from the Paykan types
(`toPIRType`), but the vtable **targets** are not, and the lowering's
type-mapping rules may change. Storing the PIR view explicitly makes the
importer's lowering a pure function of the interface. On load, the reader
cross-checks every interface function and class against the `code` section
(the same signature and layout comparisons as `Verifier.cpp:677-712`). An
inconsistent file is rejected rather than silently miscompiled.

---

## 5. Code section

### 5.1 Decision: payload

These are the options from #57:

| Option | Pros | Cons |
|---|---|---|
| **(a) PIR bytecode** | backend-neutral; every backend, including out-of-tree (#61) and the JIT, consumes the same thing; works in the barebones core; cross-target later | the backend still translates on every link (mitigated by the backend caches, §6) |
| (b) native object per target | fastest link | not portable; the barebones core can't produce objects without `cc`; impossible today because C symbols are program-dependent (§1) |
| (c) both: PIR canonical plus optional native objects | best of both | needs (b)'s prerequisites |

**Recommendation: (a) for v0.1.0, (c) in v0.2.0** once mangling is stable.
The `native` section kind is reserved for it.

### 5.2 Decision: PIR encoding

| Option | Pros | Cons |
|---|---|---|
| A. Embed the PIR **text** | the parser and printer exist and are round-tripped over the whole corpus (`tests/Lowering/RoundTripTests.cpp:102`) | larger and slower; text isn't "lowered to binary"; the parser was written for trusted, hand-written input |
| **B. A binary PIR codec** (`paykan/pir/Binary.h`: `encode(const Module&)`, `decode(span) -> StatusOr<Module>`) | compact and fast; a natural fuzz target; it's the "PIR binary serialization" deliverable of #57 | about 800 lines of new code, plus tests |

**Recommendation: B.** It mirrors `PIR.h` one to one:

- Module → its globals (`cstr`, `data`, `bytes`, extern globals), classes and
  functions, in **the lowering's order** (deterministic, and it keeps
  `--emit-pir` output stable).
- Function → name, signature, params, locals, then a body block.
- A block is a statement count followed by statements. Each statement is a
  tag byte (instruction, `if`, `while`, `break`, `continue`, `ret`,
  `unreachable`) plus its fields. An instruction is an opcode byte followed
  by only the per-opcode fields that `Instr` uses (`PIR.h:139-154`).
- An operand is a kind byte plus its payload.
- Value names are kept: they're in the string table, they make `--dump-pkm`
  readable, and they cost little. Dropping them could become a later flag.

If the binary codec slips, the first container PR can ship the payload as
text behind an `encoding` byte in the `code` section and switch before the
tag. The format doesn't change.

### 5.3 Symbol index

`symidx` holds `(symbol string, kind {fn, class, global}, offset, length,
SHA-256 of the record)` for every **defined** symbol, sorted by symbol. This
is the "map symbols to binary definitions" half of the proposal. v0.1.0
decodes whole modules, but the index already supports lazy JIT loading and
hot reload (#25) and per-symbol change detection, at negligible cost.

### 5.4 Pre-pass or post-pass PIR (#97)

There are no PIR passes in v0.1.0, so the `code` section holds the verified
output of the lowering. For v0.2.0:

| Option | Pros | Cons |
|---|---|---|
| **A. Store pre-pass (canonical) PIR; run the pipeline when the program is assembled** | one `.pkm` serves every `-O` level, which suits prebuilt stdlib and libraries; module bodies stay available for future cross-module inlining | the passes rerun on every build (cheap next to `cc`/LLVM, and the backend caches key on the post-pass PIR anyway) |
| B. Store post-pass PIR for the requested level | no pass time on link | one `.pkm` per opt level or pipeline; the header must record the pipeline |
| C. Both (an optional `code.opt` section) | flexible | more bytes, more rules |

**Recommendation: A.** C remains possible later because sections are
additive.

### 5.5 Cross-module inlining

None in v0.1.0 or v0.2.0. If it comes, an importer that inlines from M
depends on M's `code` hash, not just its interface hash. The `deps` record
would gain a "code hash used" field, a minor format bump.

---

## 6. How the backends consume it

`Backend::emit` and `run` take a `pir::Program` (`Backend.h:44-54`). In
v0.1.0 the driver builds that program from the main module's fresh PIR plus
the decoded `code` sections of its transitive imports. The order matches
today's lowering: main first, each module once (`pir.md:146-150`). The result
is verified as a whole before any backend sees it, as today (`main.cpp:168-175`).
**Neither backend changes.**

| Backend | v0.1.0 | Later |
|---|---|---|
| C | unchanged: emits C per module from the PIR; `.o` cache keyed on the generated C (`CBuild.cpp:187`) | with stable, program-independent mangling, key the `.o` on (`.pkm` content hash, cc, flags, `Runtime.h` hash) and skip emitting C for cached modules; optionally record the object in the `native` section |
| LLVM (in tree until v1.0, #61) | unchanged: per-module bitcode keyed on the PIR text (`PIRToLLVM.cpp:860-871`); the decoded PIR is identical, so entries stay valid | key on the `.pkm` content hash instead of re-printing PIR; drop the exe-mtime build id (`:846-858`) in favour of `kVersion` + ABI |
| Out-of-tree (#61, `print-pir`, MLIR) | consume `pir::Program`, as now | may also read `.pkm` directly through the exported `paykan_pkm` library (`find_package(Paykan)`, #37); they pin the format major / PIR / ABI they support |
| JIT (#25) | n/a | load modules lazily through `symidx` |

**Decision: does the `.pkm` replace the backend caches or sit in front of
them?** It **sits in front** in v0.1.0. Replacing them needs native code in
the `.pkm` (§5.1), and that needs stable mangling. The issue's line "this
replaces `.paykan_cache/*.o` and the bitcode cache" becomes the v0.2.0 goal.

Code placement:

- `include/paykan/pkm/` and `src/PKM/` form the `paykan_pkm` library. It
  depends on `paykan_pir` only and defines a neutral `pkm::Interface` struct,
  not AST types. This keeps it consumable by plugins.
- Conversion between `pkm::Interface` and Sema lives in `SemaImport.cpp`. It
  replaces `ModuleInfo` and its string serialization, and reuses the
  existing reconstruction (`SemaImport.cpp:235-392`).
- The lowering's `ProgramLowering::lowerImport` (`LoweringProgram.cpp:76-94`)
  returns the decoded `pir::Module` instead of lowering an AST, and
  `ClassOrigins` is filled from the interface.

---

## 7. Build flow

### 7.1 Decision: who builds `.pkm` files

| Option | Pros | Cons |
|---|---|---|
| **A. On demand:** `paykan run/build/--emit-*` compiles each stale import into `.paykan_cache/<canonical path>.pkm` | no new workflow; users keep running `paykan main.pkn` | the cache is implicit |
| B. Explicit only: `paykan compile-module x.pkn -o x.pkm`, importers find `.pkm`s on a search path | Make/Bazel-friendly | every user must manage build order: a regression in usability |
| **A + an explicit `--emit-pkm`** | the default is unchanged, and tests, the stdlib and future library builds can produce files explicitly | two entry points to keep consistent (they share one code path) |

**Recommendation: A + `--emit-pkm`.** The main module is never cached, as
today (`06-modules.md:158-160`). `--emit-pkm -o out.pkm main.pkn` writes it
for any module, including one with `main`.

### 7.2 Resolution and staleness

For `import a::b` (canonical name `a::b`):

1. If `<root>/a/b.pkn` exists, it is authoritative. Use
   `.paykan_cache/a/b.pkm` when it is **fresh**; otherwise rebuild it, which
   recursively ensures its own dependencies first.
2. If no source exists, look for a prebuilt `a/b.pkm` (v0.1.0: next to where
   the source would be, and under `PAYKAN_STDLIB` for `import ::…`). Use it
   if it is fresh apart from the source check; otherwise report an error.
3. Otherwise report "module not found", as today.

A `.pkm` is **fresh** when all of these hold:

- the magic, format major, PIR version, runtime ABI version, target bytes
  and compiler version match this compiler;
- the content hash verifies;
- the source hash equals SHA-256 of the current source (when a source exists);
- for every `deps` entry, the dependency resolves to the same canonical name
  and its *current* interface hash equals the recorded one.

Consequences:

- Editing a body in `base` rebuilds `base.pkm`. Its interface hash is
  unchanged, so `mid.pkm` stays fresh. This is better than today's LLVM
  cache, which invalidates every transitive importer (`06-modules.md:168-174`).
- Editing a signature in `base` changes its interface hash, so every direct
  importer is rebuilt. Their own interface hashes change only if their
  interfaces actually change.
- Diamonds: each module is loaded once per program, keyed by canonical name.
  If two importers recorded different interface hashes for it, the stale one
  is rebuilt (source available) or reported.
- Writes are atomic (temporary file + rename, as in `CBuild.cpp:46-80`).
  Concurrent builds sharing an import never see a partial file, and the
  content hash catches anything else.

**Version mismatch:** with source, rebuild silently. The cache is purely a
cache (`06-modules.md:175-178`). Without source, report an error and never
crash:

```
error: module 'a::b' (a/b.pkm) was compiled by paykan 0.1.0
       (pkm 1.0, PIR 1, ABI 5); this compiler needs pkm 2.x, PIR 2, ABI 6.
       Rebuild it from source.
```

### 7.3 Distribution (v0.2.0)

A module search path (`--module-path`, `PAYKAN_MODULE_PATH`), prebuilt
stdlib `.pkm`s installed with the toolchain, and libraries as an archive of
`.pkm`s plus a generated header (#21, #23) all belong with the package
manager. The v0.1.0 format already supports them: no paths inside the file,
and a self-describing header.

---

## 8. Determinism and reproducibility (#102)

The rule: identical source trees with the same compiler produce
byte-identical `.pkm` files, whatever the checkout directory, working
directory or machine.

- **Names:** only canonical module names appear (`geometry::shapes`; the
  main module's name derives from its file name, per #102). There are no
  `module "/abs/path"` clauses, no `-I/abs` (the C key already hashes the
  runtime header's content), and no source paths. The optional `debug`
  section may later hold paths **relative to the source root**.
- **Order:** interface decls are sorted by name. `code` keeps the lowering's
  order, which is deterministic. String table indices follow the writer's
  traversal. Nothing iterates an `unordered_map` without sorting first.
- **Content:** no timestamps, PIDs, hostnames or exe mtimes. LLVM's
  `compilerBuildId()` stays a backend-cache detail and never enters a `.pkm`.
  Reserved bytes are zero.
- **Tests:** build a multi-module program from two directories and two
  working directories; the `.pkm` files must be byte-identical (`cmp`) and
  `strings` must show no absolute path. This is the same test shape #102
  asks for.

---

## 9. Versioning and compatibility

The header carries **four independent numbers**, each with one owner:

| Number | Bumped when | Owner |
|---|---|---|
| format major.minor | the container or a section encoding changes; minor for additive, optional sections | `paykan_pkm` |
| PIR version | PIR types, opcodes or their semantics change | `docs/pir.md` |
| runtime ABI | object layout, calling or ownership conventions, `Runtime.h` structs change | `kPaykanABIVersion` (moved into the core) |
| compiler version | each release | `Version.h` |

**Policy before 1.0:** a reader accepts exactly its own format major, PIR
version and ABI. Anything else is stale (§7.2). Minor versions only add
optional sections, so a newer minor stays readable. After 1.0 we can promise
"reads the previous major".

The v0.2.0 ABI work is planned so that **ABI, PIR and format break once,
together**, as #93, #96, #98 and #99 each ask:

| Change | What it touches in the `.pkm` | Room reserved in v0.1.0 |
|---|---|---|
| #93 one allocation per object | class layout semantics (the header, no backpointer) | the class record's **header kind** field; ABI bump |
| #96 inline optional primitives | a new PIR value type and ops; new interface type-table entries; field types | type tags and PIR type bytes are open enums; PIR bump |
| #98 borrowed parameters | per-parameter conventions in function and vtable-slot signatures | the **per-parameter convention byte**, always `owned` today; ABI bump |
| #99 weak references | a `weak T` type, `weak.load`/`weak.store` ops | open type and opcode enums; PIR + ABI bump |
| #97 passes | optional optimized code later | the section table (`code.opt` would be new and optional) |

The expected result is v0.2.0 = pkm 2.0, PIR 2, ABI 6. Every v0.1.0 `.pkm`
in a cache is rebuilt silently on first use.

---

## 10. Robustness and security

A `.pkm` is untrusted input: it may be truncated, stale, hand-edited or
shipped by a third party.

- **Bounds-checked reader.** One `ByteReader` (a span + cursor) with
  `readU8/U32/U64/ULEB/SLEB/F64/Bytes` that fail with a `Status` on overrun.
  It uses no exceptions (the core is `-fno-exceptions`) and never casts a
  raw pointer to a struct. A ULEB is limited to 10 bytes and must fit its
  target type.
- **No allocation from unchecked counts.** Every count is checked against
  the remaining bytes (each element needs at least 1 byte) before any
  `reserve`. String indices, type indices, local and value ids are
  range-checked when decoded.
- **Bounded recursion.** PIR blocks nest (`if`/`while`), so the decoder caps
  nesting depth (e.g. 512) and type-table depth; type-table entries may only
  reference earlier entries, which makes the table acyclic by construction.
- **Validate before trusting.** The order is: magic → versions →
  section-table bounds and overlap → content hash → decode → **`pir::verify`
  on the decoded module** → interface/code cross-check (§4.4) →
  `verify(Program)` after assembly. A file that passes all of these still
  can't make a backend see invalid PIR.
- **Error surface.** Every failure is a normal diagnostic naming the module
  and the reason ("corrupt module file 'a/b.pkm': section 5 extends past end
  of file"). In the cache, a corrupt entry is just rebuilt.
- **Fuzzing.** A deterministic fuzz smoke test in the style of
  `tests/Frontend/FuzzSmokeTests.cpp` (fixed seeds, xorshift) mutates corpus
  `.pkm` files: byte flips, truncation at every offset of a small file, and
  count inflation. With the content-hash check disabled through a test hook
  it can reach the decoder. It runs in the ASan/UBSan CI jobs. A libFuzzer
  target (`-fsanitize=fuzzer`, clang only) is optional and kept out of the
  barebones build.

---

## 11. Testing and phased plan

### 11.1 Tests

| Test | What it proves |
|---|---|
| SHA-256 vectors | the hash implementation |
| `PIRBinary.RoundTrip` over the whole corpus | `print(decode(encode(m))) == print(m)` for every module (the binary analogue of `RoundTripTests.cpp:102`), plus `encode(decode(b)) == b` |
| Hand-written PIR text → binary → text | backend-test PIR (no frontend) works too |
| `.pkm` writer/reader unit tests | header, sections, required/optional kinds, interface records, cross-check failures |
| Determinism | §8: byte-identical across directories and runs |
| Staleness (Driver tests, both backends) | a body edit rebuilds only that module; a signature edit rebuilds its importers; a diamond with a stale side; a deleted cache; a version-bumped file (test hook) |
| Corruption | truncated, flipped, wrong magic or version, overlapping sections, a lying interface: each gives a clean diagnostic (and a rebuild in cache mode), never a crash |
| Fuzz smoke | §10, under ASan/UBSan |
| SamplesParity / SamplesParityBuild | the whole corpus through `.pkm`, run twice (cold and warm cache), on llvm and c: identical stdout and rc, 0 live blocks |
| `--dump-pkm` golden | the debug dump is stable (it documents the format by example) |

### 11.2 Phases (small PRs)

| # | PR | Depends on | In v0.1.0 |
|---|---|---|---|
| 0 | This design doc | — | yes |
| 1 | Core support: SHA-256, `ByteReader`/`ByteWriter` (LE, LEB128), moving `kPaykanABIVersion` into the core | — | yes |
| 2 | Binary PIR codec (`paykan/pir/Binary.h`) + corpus round trip + fuzz smoke; `docs/pir.md` gets a "Binary form" section | 1 | yes |
| 3 | `.pkm` container + `pkm::Interface` + writer/reader + `--emit-pkm` + `--dump-pkm` (still unused by imports) | 2, #102 | yes |
| 4 | Imports via `.pkm`: Sema reconstructs from `pkm::Interface` (replacing `ModuleInfo` and its static cache), the lowering takes decoded modules, the driver does on-demand build and staleness checks | 3 | yes |
| 5 | Docs: rewrite `06-modules.md` "Compilation Cache" and the `11-generics.md` modules note; CHANGELOG; samples for staleness and corruption | 4 | yes |
| 6 | Stable, program-independent mangling for both backends; backend caches keyed by `.pkm` hash; optional `native` section | 4, #102 | v0.2.0 |
| 7 | Generic templates in the interface (§12) | 4 | v0.2.0 |
| 8 | ABI/PIR/format 2 together with #93/#96/#98/#99 | 4 | v0.2.0 |
| 9 | Search paths, prebuilt stdlib, libraries (#23), header generation (#21), lazy JIT loading (#25) | 6 | v0.2.0+ |

PRs 1–2 can start now. PR 3 needs #102's canonical names.

---

## 12. Generics: new functionality (design now, implement in v0.2.0)

Cross-module generics don't work today. Importing a template is a Sema
error (§1). Enabling them is **new language functionality** that the
`.pkm` makes possible, not something it has to preserve. Under the
recommendation in §2.1, v0.1.0 doesn't export templates, so nothing here
blocks the release. The format reserves the `templates` section so that
adding it is a minor change. This section records the design that would be
built on it.

### 12.1 Decision: template representation

Templates are never type-checked as such. Each instantiation is a substituted
AST clone that is checked separately (`Sema.h:548-557`). So there is no
"typed template" to serialize.

| Option | Pros | Cons |
|---|---|---|
| A. Source text of the generic decls | trivial to write | the importer would have to re-parse in the defining module's name context (its imports and qualifiers), with that module's frontend; diagnostics point into a file the importer may not have |
| **B. Serialized (untyped) AST of the generic decls, with every free name resolved** to `(module, symbol)` or to a type ref at export time | instantiation in the importer reuses `ASTCloner` + Sema unchanged; hygienic (a name in the template body means what it meant in its module); frontend-independent | an AST serializer for the decl/stmt/expr node set; the AST becomes a semi-stable format |
| C. "Template PIR" (PIR with type parameters) | lowered, so no Sema in the importer | PIR depends on `T` (`i64` vs `box` vs `f64` slots, ARC only for boxes), so it would need polymorphic ops and type-dependent ARC: a second IR; it also loses the per-instantiation Sema diagnostics that `11-generics.md` promises |

**Recommendation: B.** To keep it tractable, the AST encoding is versioned
with the PIR version and covers only what can appear inside generic decls.

### 12.2 Instantiation ownership and dedup

An importer that instantiates `Box<Point>` generates it in its own
compilation. When two modules in one program both make `Box<Point>`, they
must end up with one class: one vtable, which is the type identity for
`match` (`pir.md:162-163`).

| Option | Pros | Cons |
|---|---|---|
| COMDAT / `linkonce_odr` | the standard C++ answer | not expressible in strict ISO C11 (#62 forbids `__attribute__`/weak symbols), so the C backend can't do it |
| "First importer owns it" | simple | depends on program order, so the same `.pkm` differs between programs and determinism breaks |
| **One synthetic unit per instantiation**, with canonical identity `<template module>::Box<geometry::point::Point>`, cached like a module (its own `.pkm` keyed by the template's interface hash and the arguments' interface hashes); the driver links each identity once | works on every backend and in ISO C; deterministic; naturally shared across modules and programs | the driver must collect the instantiations requested by every module (listed in each module's `iface` as "instantiations used") |

**Recommendation: the per-instantiation unit.** Its symbol names are those
of a class defined by the synthetic module, so they follow the ordinary
mangling. Type arguments are always spelled with fully qualified canonical
names, so `Box<Point>` from two modules with different `Point`s can't
collide.

This also lifts today's restriction that two modules can't instantiate the
same generic (`11-generics.md:208-210`), but only for importable templates.
A module's own `class Box<T>` keeps today's global-name rule.

---

## 13. Open questions for the owner

1. **Doc location.** #57's deliverable says `docs/modules.md`; this proposal
   is `docs/design/pkm.md`. Keep it here, or move it there (or make
   `docs/modules.md` the user-facing summary once implemented)?
2. **Cross-module generics (key decision, §2.1).** Importing a template is
   rejected today, so enabling it is new functionality. Defer to v0.2.0
   (recommended; v0.1.0 exports concrete instantiations only), or enable it
   in v0.1.0 as part of #57?
3. **v0.1.0 scope.** Is "imports go through `.pkm`, backends and their
   caches unchanged" (PRs 1–5) enough for #57 in v0.1.0, with native code,
   stable mangling and templates in v0.2.0? Or must the stable mangling /
   cache replacement (PR 6) ship in v0.1.0?
4. **Binary PIR in v0.1.0.** Is it OK to land the container first with a
   text-PIR payload (an encoding byte) and switch to binary before the tag
   if the codec slips? Or is binary a hard requirement for v0.1.0?
5. **Container.** Custom container (recommended) vs. a native object with a
   `.paykan.iface` section, which the issue also lists.
6. **Transitive types.** Reference-only (recommended; it requires the whole
   dependency closure to be present) vs. copying them as today?
7. **Prebuilt modules without source in v0.1.0.** Allow them (a `.pkm` found
   where the source would be), or require sources until the package manager
   exists?
8. **Dev-build staleness.** A `.pkm` records `kVersion` only. Should
   development builds also record a configure-time git revision so that two
   builds of `0.1.0-dev` with different codegen don't share caches? (The
   LLVM cache uses the exe mtime, which must not go into a `.pkm`.)
9. **Pre-pass PIR (#97).** Store canonical pre-pass PIR and rerun the
   pipeline per build (recommended), or store post-pass PIR per opt level?
10. **Generics in v0.2.0.** Serialized resolved AST (recommended) vs. source
   text vs. template PIR; and the per-instantiation unit for dedup?
11. **Hash.** SHA-256 in the core (recommended) vs. a 128-bit
    non-cryptographic hash. A `.pkm` hash identifies a module to *other*
    files, so collisions matter more than they do for a private cache.
12. **Frontend.** Should the `.pkm` record which frontend parsed the module?
    The two frontends are required to produce identical ASTs, so this
    proposal records nothing.

---

## Appendix A: worked example

`samples/imports/06_class_inherit`: `shapes/base.pkn` defines `class Shape`,
and `main.pkn` subclasses it.

`.paykan_cache/shapes/base.pkm` (as shown by `--dump-pkm`, abridged):

```
pkm 1.0  pir 1  abi 5  ptr 8 slot 8 le  paykan 0.1.0
module shapes::base   source sha256:3f9a…  content sha256:c1d2…
deps: (none)
iface:
  class Shape : <root>  header two-word
    fields: (none)
    vtable:
      0 destroy   ()      (obj) -> void  = @Shape.destroy
      1 toString  () -> Str  (obj) -> box = @PaykanObject_toString
      2 equals    (Obj) -> bool  (obj, box) -> i64 = @PaykanObject_equals
      3 area      () -> int  (obj) -> i64 = @Shape.area
      4 perimeter () -> int  (obj) -> i64 = @Shape.perimeter
    init: ()   ctor @Shape () -> box   dtor @Shape.destroy
  interface sha256: 77e0…
code: module "shapes::base" { class Shape {…} fn @Shape.area … }   (binary PIR)
symidx: @Shape @Shape.area @Shape.destroy @Shape.perimeter class Shape
```

Compiling `main.pkn`:

- Sema reconstructs `Shape` from `iface` and binds `base::Shape` and
  `shapes::base::Shape`.
- The lowering declares `extern class Shape module "shapes::base"` and
  `extern fn @Shape.perimeter(obj) -> i64 module "shapes::base"` from the
  same records, and copies slot 4's target into `Circle`'s vtable.
- The driver appends the decoded `code` module and verifies the program.
- The C or LLVM backend runs exactly as today.

Editing `Shape.area`'s body rebuilds `base.pkm`, and `main.pkn` (never
cached) is compiled as always. Changing `area`'s return type changes the
interface hash. Any cached importer of `shapes::base` is then rebuilt.

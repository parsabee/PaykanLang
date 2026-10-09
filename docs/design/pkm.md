# The Paykan module file (`.pkm`) and the module system

Design for v0.2.0. Tracks #57 (module file), #106 (backend payloads) and
#160 (generics across modules); touches #19, #21, #23, #25, #93, #96, #98,
#99, #113, #144, #147, #153, #162, #167, #170, #171.

Status: **design, not implemented.** Every "Decision" below is proposed;
the "Open questions" at the end need the owner. Line references are to
`develop` after the #28 cleanup.

---

## 0. Goals, the three parts, terminology

A `.pkm` is the one module artefact every part of the toolchain consumes:
the driver finds it, Sema type-checks against it, the lowering declares
externs from it, backends generate code from it. It has three parts, which
map onto three *classes* of section rather than three literal sections:

| Part (owner's words) | Section class | Sections | Who reads it |
|---|---|---|---|
| 1. "a header that tells the system, backend, frontend, core, libraries that it depends on and their versions" | **header** | `MANIFEST` | the driver, before anything else is decoded |
| 2. "loadable by Sema for semantic analysis" | **sema** | `IFACE`, `TMPL` | Sema (and the lowering, to declare externs) |
| 3. "everything not needed for semantic analysis; binary" | **binary** | `CODE`, `SYMIDX`, `DEBUG`, `PAYLOAD`* | the driver (PIR) and the active backend (payload) |

A file with **no `PAYLOAD`** is the **portable** form: it is complete, and
any backend whose PIR version and runtime ABI match regenerates native
code from `CODE`. A payload is only ever a cache of `CODE` for one backend,
target and configuration.

Terms: *canonical name* is the module identity (`include/ModuleName.h`:
`geometry::shapes`, `::io`, main's stem); *interface* is what Sema sees;
*PIR* is the verified intermediate representation (`docs/pir.md`);
*payload* is a backend's native translation; *synthetic unit* is a
per-program compilation unit the driver creates for one generic
instantiation.

Fixed constraints: the core is standard C++20, `-fno-exceptions -fno-rtti`,
`Status`/`StatusOr` errors; every reader is bounds-checked, version-gated and
never trusts a count or an index; files contain no paths, timestamps or
host state; the writer has exactly one encoding of a given module
(determinism is tested byte for byte).

---

## 1. Container

### 1.1 Conventions

- Fixed-width integers in the header and section table are little-endian,
  assembled byte by byte (never a struct `memcpy`).
- Inside sections: `uleb` = unsigned LEB128, `sleb` = signed LEB128, both
  **minimal** (a redundant continuation byte is rejected), at most 10 bytes,
  range-checked against the target type; `f64` = 8 raw IEEE-754 LE bytes;
  `str` = `uleb` length + UTF-8 (no NUL; validated); `hash` = 32 raw
  SHA-256 bytes.
- Every section starts at a multiple of 8 from file start; padding is zero
  and belongs to no section. Section sizes are exact.
- **No file-level string table.** Each section is self-delimiting: own
  magic, own version, own string table. A section can therefore be hashed,
  extracted, copied into another container or appended without touching any
  other section (what #106 payloads and #113 installation need). Cost: the
  module name appears in a few sections.
- Counts are checked against the bytes remaining before anything is
  reserved (`n` elements need at least `n` bytes). Recursion is capped.

### 1.2 Fixed header (64 bytes at offset 0)

| Off | Size | Field | Value |
|---|---|---|---|
| 0 | 8 | `magic` | `89 50 4B 4D 0D 0A 1A 0A` (`\x89PKM\r\n\x1a\n`, PNG-style: catches text-mode damage) |
| 8 | 2 | `format_major` | 1. A reader accepts exactly the majors it knows |
| 10 | 2 | `format_minor` | 0. Additive changes only; a higher minor is accepted |
| 12 | 4 | `header_size` | 64 |
| 16 | 4 | `section_count` | 1..1024 |
| 20 | 4 | `section_entry_size` | 64 |
| 24 | 8 | `section_table_offset` | ≥ `header_size`, 8-aligned (64 today) |
| 32 | 32 | `table_hash` | SHA-256 of the section table bytes |

Nothing else: versions, module name and dependencies are manifest fields,
because they are variable-length, many, and need their own versioning.

### 1.3 Section table entry (64 bytes)

| Off | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | `kind` | §1.4 |
| 4 | 4 | `flags` | bit 0 `REQUIRED` (a reader that does not know `kind` must reject the file); bit 1 `INSTANCED` (the kind may repeat; only `PAYLOAD`); others 0 |
| 8 | 8 | `offset` | from file start; ≥ table end; multiple of 8 |
| 16 | 8 | `size` | exact; `offset + size ≤ file size` (overflow-checked) |
| 24 | 32 | `hash` | SHA-256 of the section bytes |
| 56 | 8 | `reserved` | 0 |

Invariants (writer guarantees, reader checks): entries sorted by
`(kind, offset)`; sections do not overlap and appear in table order with
strictly increasing offsets. Appending a payload is therefore a pure append
plus a table rewrite.

### 1.4 Section kinds

`kind` bits 8–15 are the class: `0x00` header, `0x01` sema, `0x02` binary,
`0x7F` private (never written or required by `paykan`).

| Kind | Name | Class | Count | REQUIRED | Section |
|---|---|---|---|---|---|
| `0x0001` | `MANIFEST` | header | 1 | yes | §2 |
| `0x0100` | `IFACE` | sema | 1 | yes | §3 |
| `0x0101` | `TMPL` | sema | 0..1 | yes when present | §4 |
| `0x0200` | `CODE` | binary | 0..1 | yes when present | §5 |
| `0x0201` | `SYMIDX` | binary | 0..1 | no | §6.1 |
| `0x0202` | `DEBUG` | binary | 0..1 | no | §6.2 |
| `0x0210` | `PAYLOAD` | binary | 0..n | no, `INSTANCED` | §7 |
| `0x7F00`–`0x7FFF` | private | — | any | never | out-of-tree tools |

`paykan` always writes `MANIFEST`, `IFACE`, `CODE`, `SYMIDX`, and `TMPL`
when the module exports a template; `DEBUG` under `-g`/`--debug-lines`;
`PAYLOAD` per §7. `CODE` is optional *in the container* so that an
interface-only module (a C library described for #21, a native-only
distribution) is representable; whether it is usable is a verdict (§8.2),
not a parse error.

### 1.5 Derived hashes (never stored)

| Hash | Definition | Used for |
|---|---|---|
| `iface_hash` | table hash of `IFACE` | importer staleness; `Dep.iface_hash`; payload binding when `CODE` is absent |
| `tmpl_hash` | table hash of `TMPL` (zero when absent) | staleness of modules that value-specialised a template; synthetic-unit cache keys |
| `code_hash` | table hash of `CODE` (zero when absent) | binds a payload to the PIR it was built from |
| `module_hash` | `SHA-256(hash(MANIFEST) ‖ iface_hash ‖ tmpl_hash ‖ code_hash)` | the module's identity across files (what a #23 library archive records). `SYMIDX`, `DEBUG` and payloads are excluded, so adding or stripping a payload does not change what the module *is* |

### 1.6 Limits

File ≤ 2^40 bytes; `section_count` ≤ 1024; `MANIFEST` ≤ 1 MiB; a manifest
string ≤ 64 KiB; a list ≤ 65 536 entries and ≤ remaining bytes; LEB128 ≤ 10
bytes; nesting caps per section (§4: 2048 AST levels; §5: 512 PIR levels).

### 1.7 Reader order

1. `size ≥ 64`; magic; `format_major` known; header fields in range
   (overflow-checked in `uint64_t`).
2. `table_hash` verified.
3. Every entry decoded and checked (alignment, bounds, `reserved == 0`,
   known flag bits, sort order, non-overlap). Exactly one `MANIFEST`; an
   unknown `REQUIRED` kind rejects; a repeated non-`INSTANCED` kind rejects;
   unknown optional kinds are kept in the table (listed by `pkm dump`,
   preserved by a rewrite) and never interpreted.
4. Every section hash verified eagerly, in table order (SHA-256 at hundreds
   of MB/s on kilobytes to a few MB is far below the cost of lowering one
   module). A `LazyHashes` reader option is reserved for #25's per-symbol
   JIT loading of very large modules.
5. `MANIFEST` parsed (§2.5); the compatibility verdict computed (§8.2).
   Nothing else has been interpreted; a rejected file costs one diagnostic.
6. Only then: `IFACE` (+`TMPL`) decoded for Sema; `CODE` decoded and run
   through `pir::verify(Module)`; `SYMIDX`/`DEBUG`/`PAYLOAD` per §6–§7;
   after program assembly `pir::verify(Program)` as today.

Every failure is a `pkm::Error{code, section kind + index, offset, message}`
and becomes one diagnostic:
`error: corrupt module file '.paykan_cache/geometry/shapes.pkm': section IFACE (#2) at offset 0x1a40: hash mismatch`.
In cache mode a corrupt entry is rebuilt from source; a corrupt prebuilt file
is an error.

---

## 2. `MANIFEST` (the header part)

### 2.1 Encoding: flat TLV

```
manifest := "PKMM" u16 manifest_major(=1) u16 manifest_minor(=0) record*
record   := uleb tag, uleb length, value[length]
```

Records sorted by `tag`, each at most once (lists are one record), so the
encoding is canonical. A tag's low bit is its **critical** flag: an unknown
critical tag rejects the file; an unknown non-critical tag is skipped. A
known tag with trailing bytes is an error (additions use new tags, never
longer values). No nesting beyond one list level; no recursion.

**Decision: TLV, not a fixed struct.** The manifest gains fields across
v0.2.0 tickets (#153 libraries, #171 attributes, #21 C exports, #23 init
entry); each must be addable without touching readers of older files.

### 2.2 Records

G = gating (a mismatch yields a verdict), I = informational.

| Tag | Name | Type | G/I | Content |
|---|---|---|---|---|
| `0x01` | `module` | `str` | G | canonical module name; must equal the name the importer resolved |
| `0x03` | `contents` | `u32` bitset | G | bit 0 `HAS_CODE`, 1 `HAS_TEMPLATES`, 2 `HAS_MAIN`, 3 `SYSTEM` (name starts with `::`), 4 `HAS_STATIC_INIT` (#170 lazily initialised storage), 5 `EXPORTS_C` (#21). Must agree with the section table |
| `0x05` | `core` | `{str version; u32 major, minor, patch; str prerelease; str build}` | G | the PaykanLang that produced the file (`include/Version.h.in`). `build` is `""` for a release; for a pre-release it is the configure-time git revision, else a hash of the configure-time source listing (`PAYKAN_BUILD_ID`, new; never an executable size/mtime) |
| `0x07` | `format_versions` | `{u32 iface_major, iface_minor, tmpl_version, pir_version, code_encoding, debug_version}` | G | `pir_version` = `pir::kPIRVersion` (new core constant; `PAYKAN_PIR_TEXT_VERSION` is defined from it so text, binary and manifest agree by construction). `code_encoding`: 0 none, 1 PIR text (development escape hatch only), 2 binary PIR v1 |
| `0x09` | `runtime_abi` | `u32` | G | `PAYKAN_RUNTIME_ABI_VERSION`, moved from `src/Backends/LLVM/PIRToLLVM.cpp:843` (`kPaykanABIVersion = 6`) into `src/Runtime/Runtime.h` with a C++ alias `paykan::kRuntimeABIVersion` (#153 asks for exactly this). Becomes 7 with §9's mangling |
| `0x0B` | `target` | `{u8 pointer_size, slot_size, endianness, int_width, float_width}` | G | the abstract target PIR assumes: `8, 8, 1, 64, 64` (`Runtime.h` LP64 LE asserts). Not a triple: PIR and interface are the same for every LP64 LE triple; the triple belongs to a payload |
| `0x0D` | `deps` | `list<Dep>` | G | direct imports only, sorted by canonical name; §2.3 |
| `0x0F` | `source` | `{hash sha256; u64 size}` | G (cache) / I (prebuilt) | the module's source file exactly as read. Absent for generated modules |
| `0x11` | `libraries` | `list<Lib>` | G (link) | §2.4 |
| `0x20` | `frontend` | `{str name, version, plugin}` | I | which frontend parsed the source. **Decision: recorded, not gating.** Frontends must produce identical ASTs (differential tests enforce it); the record answers "who produced this" when they do not |
| `0x22` | `producer` | `{str tool, version}` | I | `"paykan"`, `kVersion`; a third-party writer names itself |
| `0x24` | `plugin_api` | `u32` | I | `PAYKAN_PLUGIN_API_VERSION` of the producer; gating per payload, not per module |
| `0x26` | `opt_pipeline` | `str` | I | `""` in v0.2.0 (`CODE` is pre-pass PIR, §5.9). Reserved for #97 |
| `0x28` | `attributes` | `list<str>` | I | module-level attribute spellings (#171). Reserved, empty |

Required tags: `module`, `contents`, `core`, `format_versions`,
`runtime_abi`, `target`, `deps`. No paths, no timestamps, no host names.
No interface facts (those belong to `IFACE` so `iface_hash` covers them).

### 2.3 `Dep`

```
Dep := { str  name;          canonical name of the imported module
         u32  flags;         bit 0 SYSTEM; bit 1 INSTANTIATED (this module value-specialised a template of it, §4.5)
         hash iface_hash;    that module's iface_hash when this module was built
         hash tmpl_hash;     its tmpl_hash, or zero when INSTANTIATED is clear
         str  core_version } informational, for messages
```

Only direct imports. The closure is reached through each dependency's own
`deps`; the driver loads each module once per program, keyed by canonical
name.

### 2.4 `Lib` (#153, #21, #23)

```
Lib := { u8 kind;      1 RUNTIME, 2 ALLOCATOR, 3 C_LIBRARY, 4 PAYKAN_LIBRARY
         str name;     link name, no path or prefix ("paykan_runtime", "m")
         str version;  "" when unknown
         u32 abi;      RUNTIME: runtime ABI; ALLOCATOR: allocator ABI (#153 §4); else 0
         u32 flags }   bit 0 REQUIRED_AT_LINK (#21), bit 1 PREFER_STATIC (#171 advisory)
```

v0.2.0 writes one entry, `RUNTIME`; the allocator is a link/run-time choice
and is not recorded by a module; `C_LIBRARY` entries arrive with #21/#171
(`@link(name=…)`); `PAYKAN_LIBRARY` is reserved for #23.

### 2.5 Manifest reader

Magic, `manifest_major` known; records in strictly ascending tag order,
each parsed by a sub-reader bounded to its length and required to consume
it exactly; unknown critical tag rejects, unknown optional tag is skipped;
after the loop: required tags present, `module` well formed, `deps` sorted,
unique, no self-import, `contents` agrees with the table, `libraries` has
exactly one `RUNTIME` entry whose `abi == runtime_abi`; every string valid
UTF-8.

---

## 3. `IFACE` (the Sema part)

### 3.1 Principles

- Exactly what Sema needs to type-check an importer, in Sema's own terms
  (types, declarations, visibility, instantiations). **No PIR, no symbols,
  no bodies.** The lowering derives every extern declaration (PIR
  signature, class layout, vtable, symbol name) from these records through
  one fixed mapping (§9.1), and the loader checks `CODE` against that
  mapping (§8.4), so an interface-only module is possible and the interface
  stays independent of the PIR encoding.
- A structured, indexed type table (not type strings).
- Visibility per #19: private declarations are left out unless an exported
  template body reaches them (then they are *hidden*, §3.6).
- Self-delimiting blob (own string table) so it is byte-identical in any
  container, which is #106's "interface blob" requirement.

### 3.2 Layout

```
IfaceBlob
  "PKMI"  u16 major(=1)  u16 minor(=1)  u32 flags(bit0 has TMPL section; bit1 system)
  u32 n_parts ; parts[n] { u32 tag; u32 flags(bit0 required); u64 offset; u64 size }   offsets blob-relative, 8-aligned
  parts in table order
```

| tag | part | req | content |
|---|---|---|---|
| 1 | `STRS` | yes | `uleb count`, then `str` each; index 0 is `""` |
| 2 | `MODS` | yes | module table (§3.3) |
| 3 | `TYPES` | yes | type table (§3.4) |
| 4 | `DECLS` | yes | declarations (§3.5) |
| 5 | `INST` | yes | instantiations provided and requested (§4.5) |
| 7 | `DIAG` | no | display file name (project-relative) and per-declaration positions for "declared here" notes (§8.6) |

Minor 1 adds parameter modes (`view`, `inout`) to the string-typed records
written today (`include/paykan/pkm/Interface.h`): `uleb n, (u8 mode, Str
name)[n]` after a `FUNC` record's signature, and one such list per method
after a `CLASS` record's methods, written only when a parameter has a mode
(mode 0 by value, 1 `view`, 2 `inout`; `n` is 0 or the number of
parameters).  A 1.0 record reads as having no modes; a 1.0 reader skips
them, but the manifest's `iface_minor` already turns such a reader away.

Every part but `STRS` is `uleb count` then records `uleb tag, uleb len,
payload[len]`. A reader parses the fields it knows and ignores trailing
payload bytes (a later minor may append fields) but never reads past `len`.
Record tags below `0x80` are required (unknown → reject), `0x80` and above
optional (unknown → skip). Index types: `Str`, `Mod`, `Ty`, `Decl`, `Tmpl`
(the last indexes the `TMPL` section, §4).

### 3.3 `MODS`

`MODULE { Str canonical; u8 flags(bit0 system); hash iface_hash; hash tmpl_hash }`.
Index 0 is this module (hashes zero: a blob cannot contain its own hash);
1.. are its direct dependencies, sorted by canonical name: every module a
`T_CLASS`/`T_ENUM`/`T_APPLY`/`T_TRAIT_OBJ` entry or an exported template
body references. A `Mod` of 0 in a reference means "declared here".

### 3.4 `TYPES`

A DAG: entry *i* references only entries `< i`, so the reader resolves it
in one pass without recursion (recursive shapes like `next: Node?` go
through `T_CLASS` by name, not by index).

| tag | entry | payload | meaning |
|---|---|---|---|
| 1–5 | `T_INT`, `T_FLOAT`, `T_BOOL`, `T_CHAR`, `T_VOID` | — | primitives (`void` for returns only) |
| 6 | `T_BUILTIN_CLASS` | `u8 kind` | 0 `Obj`, 1 `Str`, 2 `File`, 3 `Error`, 4 `Int`, 5 `Float`, 6 `Bool`, 7 `Char`: bootstrapped by every `ASTContext`, never described |
| 7 | `T_CLASS` | `Mod, Str name` | a user class; `mod` 0 → described by a `CLASS` record here, else by that module's blob |
| 8 | `T_ENUM` | `Mod, Str name` | likewise |
| 9 | `T_OPAQUE` | `Mod, uleb id` | a module-private class seen only as the type of a private field of a public class (#19): an object reference with no name or members; `id` = position among the module's private classes sorted by name |
| 10 | `T_ARRAY` | `Ty elem` | `T[]` |
| 11 | `T_OPTIONAL` | `Ty inner` | `T?` |
| 12 | `T_TUPLE` | `uleb n (≥2), Ty[n]` | `(T1, …)` |
| 13 | `T_PARAM` | `uleb index` | the *index*-th type parameter of the enclosing template/trait/impl record or `TMPL` body |
| 14 | `T_APPLY` | `Mod, Str tmpl, uleb n, Ty[n]` | a generic application `Box<int>`, `List<Pair<K,V>>`. Concrete iff no `T_PARAM` is reachable; a concrete application's identity is its canonical instantiation name (§4.4) |
| 15 | `T_TRAIT_OBJ` | `Mod, Str trait, uleb n, Ty[n]` | reserved for #147 (bumps the interface minor when first written) |
| 16 | `T_WEAK` | `Ty inner` | reserved for #99 |

### 3.5 `DECLS`

Common pieces:

```
Vis        u8    0 public, 1 protected (`_name`, members only), 2 private (`__name`, members only), 3 hidden (§3.6)
MFlags     u8    1 STATIC (#167), 2 MUT_SELF (#167/#169), 4 FINAL (#18), 8 OVERRIDE (#18), 16 PURE (#18), 32 DEFAULT (trait default; body in TMPL)
Sig        Ty ret, uleb n, params[n] { Str name; Ty type; u8 conv }      conv: 0 owned (only value today), 1 borrowed (#98)
Attrs      uleb n, attrs[n] { Str name; uleb m; params[m] { Str name; Const value } }   #171, exported attributes only
Const      u8 tag: 0 none, 1 int sleb, 2 float f64, 3 bool u8, 4 char u8, 5 enum (Ty, uleb variant), 6 tuple (uleb n, Const[n]), 7 str Str (attribute parameters only)
Bounds     uleb n, traits[n] { Mod; Str trait; uleb k; Ty[k] }           #147; empty today
TypeParams uleb n, params[n] { Str name; Bounds bounds }
```

Records, sorted by `(tag, name)`:

| tag | record | fields |
|---|---|---|
| 1 | `ENUM` | `Str name; Vis; Attrs; Ty self; uleb n; variants[n] { Str name; Attrs }` (variant value = index) |
| 2 | `CLASS` | `Str name; Vis (0 or 3); Attrs; u8 cflags (bit0 FINAL class); Ty self; Ty super;` own fields in declaration order `fields[] { Str name; Vis; Ty; Attrs }`; the **complete vtable** in slot order, inherited slots included, `slots[] { Str name; Vis; MFlags; Sig; Ty owner }` (`owner` = the class whose method fills the slot); non-virtual own methods `direct[] { Str name; Vis; MFlags; Sig }` (private #19 and static #167); `u8 has_init; Sig init`; class statics `statics[] { Str name; Vis; u8 sflags(bit0 LET, bit1 MUT); Ty; Attrs; Const value }` (#170); `impls[] Decl` (#147). Abstractness is **not** stored: importers recompute it from `PURE` slots (#18) |
| 3 | `TRAIT` | `Str name; Vis; Attrs; TypeParams; methods[] { Str name; MFlags; Sig; Tmpl body }` (#147; `body` valid iff `DEFAULT`, a template over the trait's parameters plus implicit `Self` = `T_PARAM n`) |
| 4 | `IMPL` | `trait { Mod; Str; uleb k; Ty[k] }; Ty for; provided[] { Str name; Sig }` (#147; orphan rule enforced by the exporter) |
| 5 | `FUNC` | `Str name; Vis; Attrs; Sig` — only functions declared by this module; imports are never re-exported |
| 6 | `CONST` | `Str name; Vis; Attrs; Ty; Const value` (#170 module-level `let`; `value` tag 0 for object initialisers, reached through the module's accessor in `CODE`) |
| 7 | `TEMPLATE_CLASS` | `Str name; Vis (0/3); Attrs; TypeParams; u8 tflags (bit0 ERASABLE, §4.5); Ty super (concrete); fields[] { Str name; Vis; Ty }; methods[] { Str name; Vis; MFlags; Sig }; u8 has_init; Sig init; Tmpl body` |
| 8 | `TEMPLATE_FUNC` | `Str name; Vis; Attrs; TypeParams; u8 tflags; Sig; Tmpl body` |

`Str`, `Obj`, arrays, tuples, optionals and boxed primitives are never
written: every context bootstraps them. Static fields in generic classes
are rejected at definition (#170), so templates carry none.

**Decision: template bodies are not hashed into `IFACE`.** A `TEMPLATE_*`
record holds only the body's index; the body bytes live in `TMPL` (§4) and
are covered by `tmpl_hash`. So editing a generic's *body* leaves
`iface_hash` unchanged: importers that only use the erased copy are not
rebuilt; importers that value-specialised it are (their `Dep` has
`INSTANTIATED` and records `tmpl_hash`); synthetic units keyed on
`tmpl_hash` are rebuilt. A per-template finer rule is a later refinement
(§13).

### 3.6 Visibility (#19)

| Declaration | In the blob? |
|---|---|
| module-level `__name` function/class/enum/`let` | no, unless *hidden* (next row) |
| a `__name` declaration reached from an exported template body or trait default body (a helper it calls, a class it constructs, an enum it matches, a constant it reads), transitively through signatures | yes, `Vis = 3 hidden`: fully described so the importer can type-check and lower the instantiated body; never bound to any qualifier; naming it from user code is `'__integrate' is private to module 'physics'`. Its link symbol has hidden linkage (§9.3) |
| a public class's `__field` of a private class type | present, type `T_OPAQUE` |
| a public class's `__field`/`_field` of a public type | present with `Vis` |
| `__method` (private, non-virtual) | in `direct[]` with `Vis = 2` (a template of the same hierarchy may call it; dumps show the class whole); importers cannot call it |
| `_method` (protected) | a slot with `Vis = 1` |
| module-level `_name` | public (#19; see open question 12) |

The exporter enforces #19's "no private type in a public signature, base or
generic argument"; the writer only ever sees `T_OPAQUE` in a private field.

### 3.7 Reader invariants

Magic/major; parts inside the blob, aligned, non-overlapping; `STRS` valid;
`MODS[0].canonical` equals the resolved name, dependencies sorted and
unique; `TYPES` references lower indices only, `T_TUPLE` arity ≥ 2,
`T_OPTIONAL` inner not void/optional/enum/tuple (the rules of
`src/Sema/Sema.cpp`), `T_PARAM` only inside template/trait/impl records and
bodies with index in range, `T_APPLY` names a `TEMPLATE_CLASS` of matching
arity once that module is loaded; `DECLS` sorted, names unique per
namespace rule (`Sema::checkDeclNameAvailable`), every index in range,
`CLASS.self == T_CLASS(0, name)`, `super` is a class, a class's slots agree
with its superclass's prefix (the cross-module `reinheritVTable`),
`OVERRIDE` only on an inherited slot, `FINAL`/`PURE` never together, a
`PURE` slot not private, `STATIC` never on a slot, `IMPL.provided` names
trait methods with equal signatures, `CONST.value` kind matches its type;
`INST` records reference existing templates with matching arity, concrete
arguments and a `form` consistent with `ERASABLE`; `flags.has_TMPL` agrees
with the container. A failure is reported once at the import site
(`error: cannot load module 'geometry::shapes' (geometry/shapes.pkm): corrupt interface: <reason>`)
and the module joins `FailedModules`.

---

## 4. `TMPL` and generics across modules (#160, #147, #19)

### 4.1 What a template is exported as

| Option | Verdict |
|---|---|
| A. source text | rejected: the importer would re-parse in the library's name context with *some* frontend; diagnostics would point into text the importer may not have |
| **B. name-resolved AST** (free names resolved at export to canonical references; type annotations resolved to `TYPES` entries with `T_PARAM`; expressions untyped) | **decision**: instantiation reuses `ASTCloner` + Sema unchanged; hygienic; frontend-independent; per-instantiation diagnostics as today |
| C. "template PIR" | rejected: PIR depends on `T` in kind (`i64`/`f64`/`bool`/`char` slot vs `box`), in ARC (retain/release only for boxes), in boxing and `match`; specialising it would need a polymorphic second IR |
| D. fully typed AST | rejected: expression types depend on `T` |

So the module ships, per exported generic: its **signature** (`IFACE`),
its **body as resolved AST** (`TMPL`), and, when the template is erasable,
the **erased copy as verified PIR** in `CODE` (#160's shared copy, compiled
once by the defining module). #57/#160 say "the module ships the generic's
verified PIR"; this design reads that as "ships what importers specialise
*from*", which for value-type arguments cannot be PIR. **Open question 1.**

### 4.2 Two-phase lookup: names resolve at export

| Name in a template body | Resolved at export to |
|---|---|
| a type annotation (`T`, `T[]`, `point::Point`, `Box<T>`, `Node<T>?`) | `Ty` |
| a callee | `Callee`: this module's function/ctor, another module's function `(Mod, name)`, a template, a builtin (`println`, `len`, `open`, conversions), `__super__`, a type parameter used as a value (kept so the importer reports it), or `UNRESOLVED(name)` |
| `Enum::Variant` (qualified or not) | `(Ty, variant index)` |
| a module constant, a class static, a static call (#170/#167) | `(Mod, name)` / a `TYPE` identifier |
| a local, a parameter, `self`; a field or method name | a **string** (resolved per instantiation against the receiver's type, as today) |
| a `match` type arm | `Ty` |

The blob does not carry the library's qualifier table; it carries resolved
references. A name that resolves to nothing is not an export error (a
template may be dead): it is `UNRESOLVED`, and an importer that
instantiates the body gets today's diagnostic with a cross-file note.

### 4.3 Encoding

A separate section so it can be read lazily (only when something is
instantiated) and hashed on its own (`tmpl_hash`). It **shares `IFACE`'s
`STRS` and `TYPES` tables by index** (duplicating a type table per body
would cost more than it saves) and is bound to `IFACE` by the manifest's
`HAS_TEMPLATES` bit and the `Tmpl` indices in `DECLS`.

```
TmplSection
  "PKMT"  u16 major(=1)  u16 minor(=0)  u32 flags(0)
  uleb n_records ; records[n] { uleb tag (1 class, 2 function); uleb len; payload }
    class:    uleb n_methods ; methods[n] { Str name; Node body }      (plus init body under name "__init__")
    function: Node body
Node := u8 kind ; Loc ; fields…           Loc := uleb line, col, line_end, col_end (0s = unknown)
```

Node kinds are the interchange format's (`docs/plugins/ast-format.md`):
statements `block, return, assign, decl, expr, if, while, break, continue,
member-assign, subscript-assign, match, destructure, var`; expressions
`int, float, bool, char, none, string, unary, binary, ternary, ident, call,
method-call, member, subscript, array, tuple, tuple-index, enum-value,
mov`; plus the resolved forms:

```
Ident  := u8 kind: 1 LOCAL Str | 2 CONST Mod, Str | 3 TYPE Ty | 4 UNRESOLVED Str
Callee := u8 kind: 1 FUNC Mod, Str | 2 CTOR Ty | 3 TEMPLATE_FUNC Mod, Str | 4 TEMPLATE_CTOR Mod, Str
          | 5 BUILTIN Str | 6 CONVERSION Str | 7 SUPER_INIT | 8 TYPE_PARAM uleb | 9 UNRESOLVED Str
```

The codec is written next to `src/ASTInterchange/{Writer,Reader}.cpp` as one
`switch` over `ASTNode::NodeKind` with the same cases; the test harness
materialises a decoded body and prints it with `interchange::write` to
compare with `--emit-ast` of the template. Nesting depth ≤ 2048
(`interchange::kMaxDepth`). The text interchange format is **not** reused
(it refuses post-Sema types and is a plugin-API contract, versioned
separately).

Materialisation builds the body in the **importer's `ASTContext`**:
`LOCAL` → `Identifier`; `CONST`/`FUNC`/`TEMPLATE_*` → canonical qualified
names (`geometry::physics::GRAVITY`, `geometry::shapes::Box`) that the
loader registers for every loaded module whether or not the importer
imported it (entries are `IsBuiltin`, never re-exported); `T_PARAM i` → a
`ClassType` stub named by the parameter, exactly what the parser produces
for `T`, so `ASTCloner::cloneType` substitutes it; `T_APPLY` → a
`GenericType` with the canonical template name.

### 4.4 Instantiation identity

**Decision:** the canonical name of an instantiation is
`<template module>::<Template><args>` with every argument spelled
canonically and **no spaces**: builtins bare, user classes and enums
module-qualified, nested applications likewise:
`geometry::shapes::Box<int>`, `geometry::shapes::Box<geometry::point::Point>`,
`::collections::List<geometry::shapes::Box<int>[]>`. One nominal type
program-wide (`match`, `equals`, assignment all agree); `Sema::instantiationName`
produces it once `typeName` spells imported classes canonically. Today's
spelling has a space after commas (`Pair<Str, int>`); **open question 3.**

### 4.5 Hybrid erasure and the `INST` part (#160)

#160: class-type arguments share one **erased** copy compiled by the
defining module (runtime class passed as a hidden argument); value-type
arguments get a **specialised** copy; mixed lists specialise over the value
arguments and erase over the class ones.

```
INST { tmpl { Mod; Str }; uleb n; Ty args[n]; u8 form (1 SPECIALISED, 2 ERASED, 3 MIXED);
       u8 pattern[n] (0 specialised, 1 erased); u8 provided (0 compiled into this module's CODE; 1 requested: a synthetic unit) }
```

Rules:

- **Every exported template ships an erased copy** (owner's decision,
  open question 2): the module writes one `ERASED` record with the canonical
  erased arguments and `provided = 0`, and the erased copy is in its `CODE`,
  compiled once with each parameter at its bound, `Obj` when it has none
  (#147: erased code may only use what the bound promises). Class-type
  arguments always use that copy; value-type arguments are specialised from
  the AST. A body that needs more of an unbounded `T` than `Obj` offers is
  reported at export until it declares a bound; phase D1 settles the exact
  rule.
- **A module compiles into its own `CODE` only instantiations of its own
  templates** (those it uses itself, `provided = 0`). Every instantiation of
  a *foreign* template, whoever requests it, is a **synthetic unit** owned
  by the driver: one `pir::Module` per canonical instantiation name per
  program, lowered once, keyed by **pattern** for `MIXED` forms
  (`Pair<int,$class>`), named by the instantiation. The importer records it
  with `provided = 1`. Consequences: a symbol is defined by exactly one
  module in any program (no COMDAT/`linkonce`, which strict C11 cannot
  express, #62), the provider of any instantiation is unique (the
  template's module or the program), and no module's payload ever contains
  another module's instantiation (§7.6).
- Importers reuse an instantiation a loaded module provides (`provided = 0`
  in that module's `INST`) as an extern instead of requesting a unit.
- Synthetic units are cached like modules (`.paykan_cache/@inst/<mangled>.pkm`),
  keyed by the template module's `tmpl_hash` and the `iface_hash` of every
  module an argument references (phase D follow-up; the format needs
  nothing new).
- `InstantiationStack` spans modules; its limit (16) applies to value
  arguments; class arguments of an erasable template do not grow it
  (`Bad<Bad<T>>` terminates, #160).

### 4.6 Private helpers reached from templates (#19)

The exporter walks every exported template/trait-default body and marks
each `__name` declaration it reaches (transitively through signatures)
`hidden` (§3.6). Importers type-check instantiated bodies against hidden
records like any other and lower calls to them as externs of the library
with hidden linkage (§9.3). Nothing unreachable is described.

### 4.7 Cross-file diagnostics

A failure inside an instantiated body reads as today plus a note:

```
geometry/shapes.pkn:14:12: error: no method 'area' on 'int'
main.pkn:7:9: note: in instantiation of 'geometry::shapes::Box<int>' requested here
```

`DiagEngine` gains a per-diagnostic file (today one `setSourceInfo` per
module); `TMPL` nodes keep source locations; `DIAG` supplies the display
file name and declaration positions, so no source is needed for the message
(the source line excerpt is shown when the file exists). JSON diagnostics
(#144) carry `module`, `file`, `line`, `column` for both frames.

---
## 5. `CODE`: binary PIR

### 5.1 Scope

A one-to-one encoding of `include/paykan/pir/PIR.h` (`Module`, `Function`,
`Class`, `Instr`, statements, globals) with no reinterpretation, so the same
verifier and the same backends consume the decoded module. Text PIR
(`src/PIR/{Printer,Parser}.cpp`) stays the human and plugin-facing form.

**Decision: binary, not embedded text.** The owner asked for binary; the
measured gain is 3.3× over text (§12); decoding needs no lexer; and records
can be sliced per function (§6.1). The `code_encoding = 1` (text) value in
the manifest is a development escape hatch only; a released format has one
encoding.

### 5.2 Layout

```
CODE blob
  u32  magic "PIRB" ; u16 codec major(=1) ; u16 minor(=0)
  u32  pir version   (pir::kPIRVersion)
  u32  flags         bit0 names stripped (§5.8) ; bit1 defines @main ; bits 2-3 pass level (0 = lowering output, §5.9) ; others 0
  --- string table: uleb count ; count × bytes (UTF-8, no NUL; entry 0 = "") ---
  str  module name   (canonical)
  --- item tables, fixed order, each `uleb n` then n records ---
  ExternGlobalRec*  CStrRec*  DataRec*  BytesRec*  SlotRec* (#170; 0 today)  ClassObjRec* (#160; 0 today)  ClassRec*  FunctionRec*
  u32  trailer "BRIP"   (a truncated blob never ends here)
```

**Decision: item order is the lowering's order** (what `pir::print` prints,
which `tests/Lowering/RoundTripTests.cpp` already relies on), so
`print(decode(encode(m))) == print(m)` byte for byte and `--emit-pir` of a
cached module equals `--emit-pir` from source. Sorting by name was rejected
for that reason; the lowering is deterministic already.

### 5.3 Primitive encodings

| Name | Encoding |
|---|---|
| `uleb`/`sleb` | minimal LEB128, ≤ 10 bytes, range-checked |
| `f64` | 8 raw bytes; NaN payload, sign and `-0.0` kept exactly (text drops the NaN payload) |
| `bytes` | `uleb` length + raw bytes |
| `str` | `uleb` index into the blob's string table |
| `type` | one `u8` (§5.4) |
| `sig` | `uleb` param count, that many `type`, then the return `type` |

### 5.4 Type codes (fixed by table, never by the C++ enum value)

| code | type | | code | type |
|---|---|---|---|---|
| 0 | `void` | | 6 | `obj` |
| 1 | `i64` | | 7 | `ptr` |
| 2 | `f64` | | 8–15 | reserved: #96 inline optionals (`opt_i64`, `opt_f64`, `opt_bool`, `opt_char`) |
| 3 | `bool` | | 16 | reserved: #99 `weak` |
| 4 | `char` | | 17–255 | reserved |
| 5 | `box` | | | |

A reader of codec 1.x rejects reserved codes. A `static_assert`-checked
table in `include/paykan/pir/Codes.h` holds both the opcode names the
printer uses and these codes, with a test that every enumerator has both.

### 5.5 Operands and instructions

```
Operand := u8 kind ; payload     0 value (uleb id ≥ 1) | 1 i64 sleb | 2 f64 | 3 bool u8 | 4 char u8 | 5 null box | 6 null obj | 7 symbol str
InstrRec := u8 opcode ; uleb result id (0 = none) ; [type result type ; str result name] ; per-opcode fields ; uleb nArgs ; Operand[nArgs]
```

| op | PIR | per-opcode fields | args |
|---|---|---|---|
| 0–4 | `add sub mul div rem` | — | 2 |
| 5, 6 | `neg not` | — | 1 |
| 7 | `cmp` | `u8` pred (eq ne lt le gt ge) | 2 |
| 8 | `select` | — | 3 |
| 9, 10 | `itof ftoi` | — | 1 |
| 11 | `cast` | `type` | 1 |
| 12 | `call` | `str` callee | n |
| 13 | `vcall` | `str` class (`""` = explicit-signature form); `uleb` slot; `sig` | n |
| 14, 15 | `retain release` | — | 1 |
| 16, 17 | `box unbox` | — | 1 |
| 18 | `new` | `str` class | 0 |
| 19 | `free` | — | 1 |
| 20, 21 | `field.load field.store` | `str` class; `str` field | 1 / 2 |
| 22 | `vtable.load` | — | 1 |
| 23 | `vtable.addr` | `str` class (`""` = operand form) | 0/1 |
| 24, 25 | `load store` | `uleb` local | 0 / 1 |
| 26–29 | reserved #96 (`some none is_some unwrap`) | | |
| 30, 31 | reserved #99 (`weak.load weak.store`) | | |
| 32, 33 | reserved #170 (`global.load global.store`) | | |
| 34 | `local.addr` | `uleb` local | 0 |
| 35 | `field.addr` | `str` class; `str` field | 1 |
| 36, 37 | `ptr.load ptr.store` | — (the loaded type is the result's) | 1 / 2 |
| 38–255 | reserved | | |

Unused `Instr` extras are not written; a decoded `Instr` has the defaults
of `PIR.h`, as the text parser produces, so round-trip equality holds. An
unknown opcode is a hard error; the explicit arg count makes records
self-describing for the dump tool and the fuzzer.

### 5.6 Statements

```
Block := uleb count ; Stmt[count]
Stmt  := u8 tag: 0 instr InstrRec | 1 if Operand, Block then, u8 hasElse, [Block] | 2 while Block cond, Operand, Block body
         | 3 break | 4 continue | 5 ret u8 hasValue, [Operand] | 6 unreachable
```

Nesting depth is capped at 512 (the frontends' source-nesting bound; a
lowering never produces deeper PIR from a bounded parse).

### 5.7 Records

```
FunctionRec
  uleb recordLength      bytes that follow (lets SYMIDX point here and a reader skip or decode one function)
  str  name
  u32  flags             bit0 extern ; bit1 hasModule ; bit2 hasSymbol ; bits3-4 linkage (0 external, 1 hidden, 2 internal; §9.3)
                         bit5 linkonce (reserved, written 0; §4.5 makes it unnecessary) ; bit6 erased (nHiddenParams follows; #160)
                         bit7 noreturn ; bit8 inline ; bit9 noinline ; bit10 cold (#171) ; bit11 cExport (cName follows; #21) ; bit12 hasAttrs
  sig  signature
  [bit1] str module ; [bit2] str symbol ; [bit6] uleb nHiddenParams ; [bit11] str cName
  [bit12] uleb nAttrs ; nAttrs × (str key ; str value)        forward-compatible bag for plugin-namespaced attributes (#171)
  -- definitions only --
  uleb nConventions ; nConventions × u8   per-parameter convention (#98): 0 owned, 1 borrowed; nConventions == nParams or 0
  uleb nParams ; nParams × (uleb id ; type ; str name)
  uleb nLocals ; nLocals × (str name ; type)
  uleb nextValueId
  Block body
ClassRec
  str name ; u32 flags (bit0 extern ; bit1 final #18 ; bit2 abstract #18 ; bit3 singleton #168 ; bit4 hasClassObj #160 ; bits5-6 linkage ; bit7 linkonce reserved)
  str super ("" = root) ; [bit0] str module
  uleb nFields ; nFields × (str name ; type)                layout order, ancestors first
  uleb nSlots  ; nSlots  × (str slot ; str target ("" = null) ; sig)
ExternGlobalRec  str name ; u8 kind (0 obj, 1 vtable)        always $rt.
CStrRec          str name ; bytes                            no NUL stored
DataRec          str name ; uleb n ; n × sleb
BytesRec         str name ; bytes                            tuple slot kinds; each byte ≤ 4, checked
SlotRec (#170)   str name ; u32 flags (bits0-1 linkage ; bit2 constant ; bit3 lazy ; bit4 singletonInstance #168) ; type ; [bit2] Operand init
ClassObjRec (#160) str name ; str class ; uleb nTypeArgs ; nTypeArgs × str
```

The reader checks that a function body ends exactly at `recordLength`
(no slack), so the writer's encoding is the unique one. The object header
kind (`{vtable, backpointer}` today; one allocation after #93) is a
property of the runtime ABI version, not per class.

### 5.8 Determinism

1. String table order is first use in the writer's traversal; a string
   appears once; entry 0 is `""`.
2. Minimal LEB128, reserved bits zero, no padding.
3. `f64` bits verbatim. `encode(parse(print(m)))` may differ from
   `encode(m)` only in NaN payloads, so tests compare text
   (`print(decode(encode(m))) == print(m)`) and binary-of-decoded
   (`encode(decode(b)) == b`), never binary-of-text against binary-of-memory.
4. Value and local names are written (they keep `--emit-pir` of a cached
   module identical and make dumps readable; ~10% of the blob).
   `--strip-names` writes every name empty and sets flag bit 0.
5. The writer walks `pir::Module`'s vectors as they are; it never iterates
   an unordered container.
6. No paths, timestamps or host data.

### 5.9 Which PIR: pass level 0

**Decision: `CODE` stores the verified output of the lowering, before any
optional pass.** #97's pipeline runs in the driver on the *assembled*
program at the requested `-O`, before `Backend::emit`/`run`. One file
serves every `-O` level and pipeline (#113 needs this); `--emit-pir` of a
cached module prints what source prints; the expensive part is cached by
payloads anyway. What is **not** a pass artefact and therefore is in
`CODE`: per-parameter calling conventions (#98 part 2 changes what a
*caller in another module* emits, so they are signature data; a change is
an ABI bump). RC elimination (#98 part 1) and last-use transfer (#145)
rewrite bodies only and run at build time. The `pass level` flag bits are
reserved for a later post-pass variant (PR #104 §5.4 option C); storing
post-pass PIR per `-O` was rejected (N copies per file).

### 5.10 Generics in `CODE` (#160)

| Piece | Representation | Linkage |
|---|---|---|
| erased body of an exported erasable generic (`Pair<$class,$class>.first`) | ordinary `FunctionRec` with `erased` set and `nHiddenParams = k` (the first `k` params after `self` are `ptr` class objects); an erased `T` is a `box` | external |
| the module's own instantiations of its own templates (`Box<int>`) | ordinary records, named by the canonical instantiation spelling (§4.4) | external |
| class object of an erased instantiation (`Box<geometry::point::Point>`: vtable, name, type args) | `ClassObjRec` + the instantiation's `ClassRec` with `hasClassObj`; backends emit a constant global | external |
| the template itself | not in `CODE`: `TMPL` (§4) | — |
| private helpers a template body calls | ordinary functions with `hidden` linkage (§9.3) | hidden |

Synthetic units (§4.5) are ordinary `pir::Module`s named by the
instantiation, assembled into the program after the modules that provide
their dependencies, and cached as their own `.pkm` later.

### 5.11 Static data and attributes (#170, #171)

`SlotRec` is the first mutable module-level storage in PIR: a constant slot
(init follows, backends emit `const` data) or a lazy slot (zero-initialised,
reached through the lowering's `@name.init` guard and accessor, #170 rule 6).
PIR gains `global.load @sym -> T` / `global.store @sym, %v` (opcodes 32/33);
the verifier checks a slot's type like a local's. Exported constants'
values for folding live in `IFACE`; `CODE` holds the storage.
Code-relevant attributes are the function flag bits (`inline`, `noinline`,
`cold`, `noreturn`, `cExport`), class flags (`final`, `abstract`,
`singleton`) and the `(key, value)` bag for attributes the core does not
interpret. **Text PIR gets a syntax for linkage and these attributes in
the same PR as the codec** so `--emit-pir` stays a complete view of `CODE`
(open question 4).

### 5.12 Verifier additions

A `hidden` symbol may not be `extern`; an `erased` function has at least
`nHiddenParams` leading `ptr` params; a `ClassObjRec` names a defined
instantiation class; a constant `SlotRec` has an init of its type;
`nConventions` is 0 or the arity and `borrowed` marks only `box` params; an
`internal` symbol is never named by another module's extern; a vtable slot
target of an `external` class is `external` or `hidden`. The verifier stays
the single rulebook for text and binary.

---

## 6. `SYMIDX` and `DEBUG`

### 6.1 `SYMIDX`

```
"PKSY" u16 major(=1) u16 minor(=0) ; string table
uleb n ; n × { str name ; u8 kind (0 fn, 1 extern fn, 2 class, 3 extern class, 4 cstr, 5 data, 6 bytes, 7 extern global, 8 slot, 9 classobj)
               ; u8 linkage (bit7 linkonce reserved) ; uleb offset into CODE ; uleb length ; hash sha256 of the record bytes }
sorted by (kind, name) ; u32 trailer "YSKP"
```

Uses: lazy per-function decode (`decodeFunction(code, entry)`) for #25's
on-demand JIT loading and `pkm dump --symbol`; per-symbol change detection
(hot reload diffs two indexes); the load-time cross-check with `IFACE`
(§8.4) as a merge of two sorted lists. Optional for a reader (rebuilt when
absent), always written. ~50 B per symbol.

### 6.2 `DEBUG`: statement source positions

PIR carries no locations today. **Decision: locations live beside PIR,
keyed by statement preorder index**, not inside the text format: one new
optional `std::vector<std::pair<uint32_t,uint32_t>> StmtLocs` on
`pir::Function` (filled by `Builder` from a "current location" the lowering
sets per statement; ignored by the printer). A `Loc` inside every `Instr`
was rejected (bloats the hot struct, forces the text format to carry it).

```
"PKDB" u16 major u16 minor ; string table
uleb nFiles ; nFiles × str path                 project-relative, never absolute (#102); one entry today
uleb nFunctions ; nFunctions × { str name ; uleb file, line, col (declaration) ;
                                 uleb nEntries ; nEntries × { uleb stmtIndexDelta ; sleb lineDelta ; uleb column } }
uleb nClasses ; nClasses × { str class ; uleb file, line, col }
u32 trailer "BDKP"
```

Delta coding keeps an entry at 3–4 bytes; straight-line statements from one
source statement cost one entry. ~20% of `CODE`. Consumers: `#line` in
generated C (`--debug-lines`; perturbs the object, so the payload
`configId` includes it), `DILocation` in LLVM IR under `-g`, #144 JSON
notes, #159 "who allocated this". Declaration positions of exported items
are duplicated in `IFACE`'s `DIAG` part on purpose: the Sema part must not
depend on an optional binary section. A failing `DEBUG` section is dropped
with a warning (advisory), never fatal.

---

## 7. `PAYLOAD`: backend-native sections (#106)

### 7.1 Layout

```
"PKPL" u16 envelope major(=1) u16 minor(=0) ; string table
str  backend          registered name: "c", "llvm", a plugin's name
str  backendVersion   what the backend calls its version (c: the toolchain id, §7.5; llvm: "<PAYKAN_PLUGIN_BUILD_VERSION>/LLVM <version>"; plugin: PaykanPlugin.version)
u32  payloadFormat    owned and versioned by the backend (c: 1 = relocatable object; llvm: 1 = bitcode, 2 = object)
str  target           normalised triple ("x86_64-unknown-linux-gnu"); "" not allowed
u32  optLevel         0..3, or 0xFFFFFFFF = independent of -O (bitcode optimised after loading)
u32  runtimeAbi       must equal the manifest's
u32  pluginApi        PAYKAN_PLUGIN_API_VERSION the backend was driven through; 0 for an in-process backend
u32  flags            bit0 PIC ; bit1 sanitizer runtime ; bit2 coverage ; bit3 track-heap ; bit4 has C-ABI exports (#21) ; bit5 static runtime (#153, informational)
str  configId         free-form, backend-owned: everything else the bytes depend on (the C backend: its exact compile flags)
hash codeHash         SHA-256 of this file's CODE section (zero when absent)
hash ifaceHash        iface_hash of this file (binds an interface-only module's payload)
hash payloadHash      SHA-256 of `payload` (the #74 lesson: hash the bytes that get linked)
uleb nSymbols ; nSymbols × (str pirName ; str nativeName)     the exported symbols the payload defines
bytes payload         opaque to the core
```

The core validates every header field and both hashes and never interprets
`payload`.

### 7.2 Contract

1. **Zero payloads is the portable form.** Nothing a backend needs for
   correctness may live only in a payload; nothing Sema needs may live in
   `CODE`.
2. **A payload is a cache of `CODE`.** Reusable for the active backend `X`
   iff `backend == X.name()`, `runtimeAbi` and `pluginApi` match the
   running toolchain, `backendVersion` passes `X`'s own rule (exact for
   in-process backends; `PAYKAN_PLUGIN_COMPATIBLE_VERSIONS` for plugins),
   `codeHash == code_hash` and `ifaceHash == iface_hash` of the file it sits
   in, `payloadHash` verifies, every `pirName` it claims is an
   `external`/`hidden` symbol of `SYMIDX`, and `X.payloadCompatible(stored,
   wanted{target, optLevel, flags, configId})` says yes. Anything else: the
   payload is ignored (`--verbose` says why) and `X` regenerates from
   `CODE`. A stale payload can never be wrong code, only unused bytes.
3. **Additive and droppable.** A file may carry payloads for several
   backends and targets (a stdlib `.pkm` with `c` objects for three triples
   and `llvm` bitcode). Stripping them changes the file but not
   `module_hash`.
4. **Determinism** of payload bytes is the backend's business; the core
   hashes what it gets.

### 7.3 Host-managed; the backend only produces and consumes bytes

**Decision:** a backend never opens, parses or writes a `.pkm`. The host
reads and writes sections, validates and hashes; the backend gets bytes and
a descriptor. This is #106's option C ("thin envelope"), keeps every
container rule in one place and works unchanged for out-of-tree plugins.
Rejected: backend-written sidecar files (#106 B: two files to keep in sync,
integrity has to bind them) and backend-embedded interfaces (#106 A: a
bitcode/ELF parser in the core).

**C plugin API** (append-only; `PAYKAN_PLUGIN_API_VERSION` stays 1 with
`struct_size` gating). To keep `PaykanBackend` small and leave room for
#25's `load_symbol`, the callbacks live in their own descriptor reached
through one appended pointer field:

```c
typedef struct PaykanModuleInput {
  uint32_t struct_size;
  const char *module_name;                       /* canonical */
  const char *pir; size_t pir_size; uint32_t pir_text_version;   /* the whole program's PIR text, as PaykanBackendInput.pir */
  const void *pir_binary; size_t pir_binary_size; uint32_t pir_binary_version;  /* this module's CODE blob when the backend declares PAYKAN_BACKEND_READS_BINARY_PIR (#162); else NULL */
  uint32_t module_index; uint32_t opt_level; const char *target;  /* "" = host */
} PaykanModuleInput;
typedef struct PaykanPayloadInfo {               /* the backend fills, the host stores */
  uint32_t struct_size;
  const char *backend_version; uint32_t payload_format; const char *target; uint32_t opt_level; uint32_t flags; const char *config_id;
  const char *const *pir_names; const char *const *native_names; size_t num_symbols;
} PaykanPayloadInfo;
typedef struct PaykanModuleCodegen {
  uint32_t struct_size;
  int (*emit_module_payload)(void *data, PaykanSession *, const PaykanModuleInput *, void **bytes, size_t *size, PaykanPayloadInfo *info);
  int (*payload_compatible)(void *data, PaykanSession *, const PaykanPayloadInfo *stored, const PaykanModuleInput *wanted);  /* pure */
  int (*load_module_payload)(void *data, PaykanSession *, const PaykanModuleInput *, const void *bytes, size_t size, const PaykanPayloadInfo *);
} PaykanModuleCodegen;
/* appended to PaykanBackend: */ const PaykanModuleCodegen *module_codegen;   /* NULL = no payload support */
```

`emit`/`run` keep their meaning. Per session the host calls
`load_module_payload` for every module whose stored payload is compatible,
then `emit`/`run` with the whole program; the backend translates what it
got no payload for and links; afterwards the host calls
`emit_module_payload` for the modules it translated and updates the files.
A backend without `module_codegen` gets today's behaviour and still reads
`.pkm`-sourced programs, because the driver assembles the PIR.

**C++ `Backend.h`** mirrors it: `Backend::moduleCodegen()` returns an
optional `ModuleCodegen` (`emitModulePayload`, `payloadCompatible`,
`loadModulePayload`) so `BackendAdapter.cpp` is the only translation layer.

### 7.4 Driver flow

```
1. The module graph resolves every import to a fresh .pkm (building when stale); Sema runs; main is lowered.
2. For each imported module M: decode CODE_M → pir::Module; verify (always: a .pkm is untrusted input).
   Among M's PAYLOAD sections with backend == X.name(), take the first that passes §7.2(2).
3. Assemble pir::Program (main, then post-order; synthetic units); run #97 passes on the modules without a payload and on main; verify(Program).
4. X.loadModulePayload(M, bytes) for each module with one; then X.emit / X.run with the whole program.
5. On success, with --write-payloads=on (default) and a writable cache: for each cached module X translated,
   X.emitModulePayload(M) → bytes; rewrite .paykan_cache/<rel>.pkm with the payload added, replacing one with the same
   (backend, target, optLevel, flags, configId); temp file + rename. MANIFEST, IFACE, TMPL, CODE, SYMIDX and DEBUG are byte-identical
   before and after, so iface_hash/code_hash do not move and no importer is invalidated by caching.
```

**Decision: both backend caches under `.paykan_cache` (LLVM `.bc` keyed on
PIR text + executable mtime, `PIRToLLVM.cpp`; C `.c/.o/.key`, `CBuild.cpp`)
are replaced by `PAYLOAD` sections of the cached `.pkm`.** One cache, one
invalidation rule, one directory layout, one atomic write. They stay as
private caches until the providers land (C3/C4), then are deleted.
Concurrency: a file is replaced only by a whole file under a rename; two
builds with different configurations write different sections; a lost
update costs a regeneration, never a mismatched link. Eviction: at most 4
payloads per backend per file, oldest (table order) dropped; no timestamps
in the file. The main module is never cached (as today); `--emit-pkm`
writes it with `CODE` and, with `--with-payload`, a payload, which is what
a library build (#23) uses.

### 7.5 What each backend stores

**C** (`payloadFormat 1`): the relocatable object of the module's
translation unit (`cc -c`, exactly as `CBuild.cpp`); not the generated C,
which is derivable. `backendVersion` = the toolchain id (compiler
`--version` first line, never its path; `Runtime.h` SHA-256; `kVersion`);
`target` = normalised `cc -dumpmachine` or `--target`; `configId` = the
exact compile flags (a sanitizer build never links a plain object);
`payloadCompatible` = equality of all; `loadModulePayload` writes the bytes
to the backend's temp dir and adds them to the link list. Prerequisite: the
module's C must be a function of the module's own PIR, not of the program
(§9.4). Until then the C backend reports no `moduleCodegen`.

**LLVM** (`payloadFormat 1`): bitcode of `translateModule` before
optimisation, `optLevel = 0xFFFFFFFF` (applied after loading, as today's
`.bc`); `payloadFormat 2` = a target object (after #162 the plugin may
prefer it). `backendVersion` = plugin build version + LLVM version; the
executable size/mtime build id is dropped (host state; the manifest's
`core.build` covers pre-release builds); the `paykan.abi.version` module
flag and `paykan.cache.key` metadata go away with the key.
`loadModulePayload` = `parseBitcodeFile` into the session context, kept for
`linkParts`; JIT runs use the same bitcode.

**Out-of-tree plugins:** `print-pir` has no payloads; a plugin that fills
`PaykanModuleCodegen` gets caching for free.

### 7.6 Payloads and generics

A module's payload contains exactly its `external`, `hidden` and `internal`
symbols: its own functions, its own templates' erased copies and its own
instantiations. Synthetic units (§4.5) are never part of any module's
payload; they are translated per build with the main module until they get
their own cached `.pkm` with a payload (phase D follow-up).

### 7.7 Tooling

`paykan pkm strip [--payloads[=<backend>]] in.pkm -o out.pkm`;
`paykan pkm extract --payload=<backend>[:<target>[:<opt>]] in.pkm -o file`
(an object the host's `ar` can archive, #23); `paykan pkm add-payload
--backend=<X> in.pkm` (the batch form of step 5, for building distribution
files).

---

## 8. The module system

### 8.1 One graph, one load path

**Decision.** A new library `paykan_modules` (the *module graph*) sits
between the driver and the existing passes. It owns import resolution,
staleness, building a module (parse → Sema → lower → verify → write),
loading a prebuilt one, synthetic units, and assembling the `pir::Program`
the backends already consume. Sema stops parsing and type-checking imports
itself (today `Sema::processImport` does all of that recursively): it asks
an `ImportResolver` for `pkm::Interface`s. The lowering stops lowering
imports (`ProgramLowering::lowerImport`): it declares externs from the
same interfaces and lowers exactly one module. Deleted: `ModuleInfo`,
the process-static `ModuleCache`, child `SemaContext`s, `ProgramLowering`.

There is **one** load path: a freshly built module is injected into the
importer from its encoded `IFACE` bytes, exactly as a prebuilt one, so the
in-process and on-disk cases cannot drift (the "second in-process compile
never exercises the on-disk cache" problem of `tests/CodeGen/ModuleTests.cpp`).

```
paykan [run|build|--emit-pir|--emit-source|--check-only|--emit-pkm] main.pkn
 1. Driver: plugins, options, DiagEngine; parse main with the selected frontend.
 2. ModuleGraph graph{ProjectRoot, Frontend, ModulePath, CacheDir, WriteCache, ForceRebuild, Backend*, OptLevel}
 3. Sema(main, &graph).run(root)
      processImport(a::b) → graph.resolve("a::b")   [recursive, memoised by canonical name]
        LOCATE  source <root>/a/b.pkn ; prebuilt candidates (§8.3) ; cache <root>/.paykan_cache/a/b.pkm
        DECIDE  source exists → cache fresh? LOAD : BUILD → write → LOAD
                no source     → first prebuilt candidate with verdict Usable → LOAD ; else "module not found (tried …)"
        BUILD   parse → Sema(dep, &graph) (its imports first) → lowerModule → verify → exportInterface → encode → AtomicFile
        LOAD    readFile → verdict → readInterface → ensure deps' hashes match → LoadedModule{Name, Iface, hashes, Code (lazy), Payloads}
      Sema reconstructs the interface into the importer's ASTContext (the typed-table twin of SemaImport.cpp:278-435).
 4. lowerModule(main) → pir::Module; externs declared from interfaces (§9.1).
 5. graph.assembleProgram → pir::Program = [main] + post-order of the import DAG (children in import order, each once) + synthetic units; verify(Program).
 6. --emit-pir prints; --emit-pkm writes main's file and stops; --check-only stops after Sema.
 7. backend emit/run (§7.4).
```

Program order is today's lowering order, so `--emit-pir` output is
byte-identical before and after the switch (the Lowering golden and
round-trip tests compare it).

### 8.2 Verdicts and compatibility

`pkm::checkCompatibility(manifest, host, policy)` with policy `Cache` (a
`.paykan_cache` entry; source available) or `Prebuilt` (distributed; no
source required), in this order:

| Check | Mismatch ⇒ |
|---|---|
| container magic/table/hashes | `Rejected(Corrupt)` |
| `format_major`, `manifest_major`, `iface_major` | `Rejected(Format)` |
| `iface_minor` newer than the reader's | `Rejected` (prebuilt) / `Stale` (cache) |
| `pir_version ≠ pir::kPIRVersion` | `Rejected(PIR)` |
| `runtime_abi ≠ kRuntimeABIVersion` | `Rejected(ABI)` |
| `target` ≠ host abstract target | `Rejected(Target)` |
| `core.version` (+ `build`) | **cache:** ≠ `kVersion`/build id ⇒ `Stale` (a rebuild is free and protects against codegen fixes between versions with unchanged PIR/ABI numbers). **prebuilt:** not on `PAYKAN_PKM_COMPATIBLE_VERSIONS` (new, in `cmake/PluginCompat.cmake`, generated into `PluginCompat.h`, the #103 mechanism #113 and #153 ask to reuse) ⇒ `Rejected(Toolchain)` |
| no `CODE` and no usable payload | `Usable` for `--check-only`; `NoCode` when code is needed |
| `source` hash (when a source is found) | `Stale` |
| each `Dep`: current `iface_hash` (and `tmpl_hash` when `INSTANTIATED`) differs | `Stale`; dependency not found ⇒ `Rejected(MissingDependency)` |

`Stale` with source rebuilds silently (`--verbose` says why). `Stale` or
`Rejected` without source is an error naming what differs:

```
error: module 'a::b' (lib/a/b.pkm) was compiled by paykan 0.1.1 (pkm 1.0, interface 1.0, PIR 1,
       runtime ABI 6); this paykan 0.2.0 needs PIR 2 and runtime ABI 7. Rebuild it from source.
```

Consequences: a body edit rebuilds one module; a signature edit rebuilds its
direct importers, and theirs only if their own interface changed; a generic
body edit rebuilds only value-specialising importers and synthetic units;
diamonds load once; a toolchain bump rebuilds every cache entry on first
use; a corrupt cache entry is rebuilt; a corrupt prebuilt file is an error.
Sema never reads `core`/`frontend` to decide; it decides on the interface
format numbers alone, which is what lets an installed stdlib outlive patch
releases.

### 8.3 Locations and precedence

| Role | Location |
|---|---|
| per-project cache | `<root>/.paykan_cache/<cacheRelativePath(name)>.pkm` (`geometry/shapes.pkm`, `@system/io.pkm`); synthetic units under `@inst/` |
| prebuilt next to source | where the source would be, `.pkm` for `.pkn` |
| module path | each `--module-path=<dir>` (repeatable), then `$PAYKAN_MODULE_PATH` (`:`-separated; never the current directory) |
| standard library (#113) | `$PAYKAN_STDLIB`, else `<prefix>/lib/paykan/stdlib/<version>/` relative to the executable, else `<root>/stdlib` |
| `--emit-pkm` output | `-o <file>` or `<module>.pkm` next to the input |

**Decision:** a source file is authoritative when it exists (the cache is
purely a cache); a prebuilt `.pkm` is used only where no source is found,
in the order above. **System modules invert the rule**: a system `.pkm`
that is `Usable(Prebuilt)` is used even when its source is installed next
to it (#113: user builds never recompile the stdlib). `--prefer-pkm`
extends the inversion to user modules (vendored third-party modules shipped
with sources); default off (open question 7).

Identity is the canonical name: the graph memoises by it, the cycle set is
by it, `ImportedTypeOrigins` records the defining module name, diagnostics
use the path only as a label. Two files resolving to one canonical name is
an error at the second resolution (`ProgramLowering::claimName`'s `.2`
suffix goes away).

### 8.4 Load-time verification (ordered, fail-closed)

1–2. Container and hashes (§1.7). 3. Verdict (§8.2). 4. `IFACE` (+`TMPL`
on demand) per §3.7. 5. `CODE`: `binary::decode` → `pir::verify(Module)`;
a decoded module that fails verification is corrupt or hostile: error,
never a rebuild from the file. 6. `SYMIDX`: entries inside `CODE`, hashes
match, name set equals the decoder's, sorted. 7. **Consistency with
`IFACE`** via the §9.1 mapping: every exported function's derived symbol
names an `external` `fn` with the derived signature; every exported class
names a non-extern `external` class whose fields, vtable (slot names,
signatures, targets), constructor, `__init__` and `destroy` match; every
exported constant names a `slot` of the derived type; every `hidden` symbol
is reachable from an exported template (else it should be `internal`);
nothing `internal` is exported; every `INST` with `provided = 0` names a
defined class/function. 8. `PAYLOAD` per §7.2 (a failing payload is
ignored, not an error). 9. `DEBUG` (advisory). 10. After assembly,
`pir::verify(Program)` as today.

Threat model: a `.pkm` is untrusted (hand-edited, truncated, hostile,
stale). The reader never allocates from an unchecked count, caps recursion,
range-checks every index and code, never links a payload whose hashes do
not verify, and runs the PIR verifier on everything a backend will see, so
backends keep assuming verified PIR. Payload bytes are the one thing the
core cannot validate; they are bound to `CODE` by hash and validated by the
backend (`verifyModule` after `parseBitcodeFile`; the linker for objects).

### 8.5 Driver flags and the `pkm` tool

| Flag | Effect |
|---|---|
| `--emit-pkm [-o f.pkm]` | compile the input module and write its `.pkm`; portable by default |
| `--with-payload[=<backend>]` | with `--emit-pkm`: also append the named (or default) backend's payload |
| `--module-path=<dir>` | repeatable |
| `--write-payloads={on,off}` | default on: after a build/run, add the active backend's payload to cached imports |
| `--prefer-pkm` | §8.3 |
| `--rebuild-modules`, `--no-module-cache` | force rebuild / never read or write the cache |
| `--strip-names`, `--debug-lines` | §5.8, §6.2 |
| `--verbose` | one line per import: file, verdict, why a payload was dropped |

`paykan pkm <verb>`: `dump f.pkm [--section=manifest|iface|tmpl|code|symidx|debug|payloads|all] [--symbol=<name>]`
(stable text; golden-tested; `--section=code` prints exactly `pir::print`
of the decoded module, so it equals `--emit-pir` of the module when it was
built), `check f.pkm` (verdict against this toolchain, exit status),
`strip`, `extract`, `add-payload` (§7.7).

### 8.6 Diagnostics

Every `.pkm` error is an ordinary `DiagEngine` diagnostic at the import
site, so #144's JSON output covers it. Diagnostics inside an imported
module keep `file:line:col`: `DIAG` supplies the project-relative display
file and declaration positions; `TMPL` nodes carry locations; `DEBUG`
supplies statement positions for backends and tools.

---
## 9. Linkage and mangling (both backends, one scheme)

### 9.1 Externs are derived from the interface

The lowering of an importer needs, for each imported declaration, its PIR
symbol, signature and (for classes) layout and vtable. **Decision:** one
fixed core function, `lowering::externsFor(const pkm::Interface &)`, derives
them from `IFACE` records: Sema type → PIR type (`int → i64`, `float →
f64`, `bool`, `char`, every class/array/string/tuple/optional-of-reference
→ `box`, …, the same mapping the lowering uses for its own module); symbol
= the PIR name the lowering gives the declaration (`describe`,
`Shape.area`, `Box<int>`, `Box<int>.get`, the canonical instantiation
spelling of §4.4); field layout = ancestors first, then own fields in
declaration order; vtable = the `slots[]` order. The loader's step 7
(§8.4) checks `CODE` against the same function, so the interface and the
code cannot disagree silently. This mapping is part of the PIR version
contract.

### 9.2 Today's problem

LLVM symbols are `module::name` / `pk.name` (not C identifiers; an `llvm`
object cannot link with a `c` object). C symbols are
`pk_<stem>_<sanitised>` **uniquified with a program-wide counter**, so a
module's C text and object are valid only for the program they were
generated in; that is why the C cache keys on the generated C and why #57
says native objects need stable mangling first.

### 9.3 Linkage classes

| linkage | who | C (strict C11) | LLVM |
|---|---|---|---|
| `external` | every exported function, constructor, `__init__`, destructor, every vtable, exported slots, erased generic bodies, the module's own instantiations | non-`static` | `external` |
| `hidden` | module-private `__name` symbols referenced from an exported template body (callable from a synthetic unit) | non-`static` (ISO C has no visibility; the name carries the `_u_u` escape so collisions are impossible; the cost is one visible symbol, documented) | `hidden` visibility |
| `internal` | every other private symbol; `cstr`/`data`/`bytes`; emitter helpers | `static` | `internal`/`private` |

Decided by the lowering from Sema's visibility (#19) and the exporter's
template-reachability set (§4.6); recorded in `CODE` (§5.7) and `SYMIDX`;
checked by the verifier (§5.12). `linkonce` is reserved and unused (§4.5).

### 9.4 Mangling

**Decision:** one injective, counter-free scheme in the core
(`include/paykan/pir/Mangle.h`: `pir::mangle(module, symbol)`,
`mangleVTable`, `mangleClassObject`), documented in `docs/pir.md` as part
of the runtime ABI, used by **both** backends so `c` and `llvm` objects
link with each other and with hand-written C (#21, #23). Runtime ABI → 7.

```
mangle(module, symbol)      = "pk_"   + enc(module) + "__" + enc(symbol)
mangleVTable(module, class) = "pkvt_" + enc(module) + "__" + enc(class)
mangleClassObject(m, class) = "pkco_" + enc(m)      + "__" + enc(class)
```

`enc` maps bytes to `[A-Za-z0-9_]*` injectively; every `_` in the output
introduces an escape, so `__` can only be the separator:

| input | out | input | out | input | out |
|---|---|---|---|---|---|
| `A–Z a–z 0–9` | itself | `_` | `_u` | `::` | `_m` |
| `.` | `_d` | `<` / `>` | `_l` / `_g` | `,` | `_c` |
| ` ` | `_s` | `?` | `_q` | `[` / `]` | `_a` / `_b` |
| `(` / `)` | `_p` / `_P` | `:` | `_n` | `$` | `_e` |
| other byte | `_x` + 2 hex | | | | |

Examples: (`geometry::shapes`, `describe`) → `pk_geometry_mshapes__describe`;
(`geometry::shapes`, `Box<int>.get`) → `pk_geometry_mshapes__Box_lint_g_dget`;
(`::io`, `__buf`) → `pk__mio___u_ubuf`; `main` stays `main` (the one
unmangled program symbol, as both backends do today). Output is a valid
C11 identifier, never starts with `__` or `_[A-Z]`, never collides with
`Paykan*` runtime symbols or `pkrt_` helpers. No truncation or hashing:
toolchains accept kilobyte symbols; a deep generic is ~90 bytes. Rejected:
Itanium-style length prefixes (less readable, still needs escapes), today's
counters (program-dependent), hashed names (opaque in debuggers).

Generic artefacts use the canonical spelling with `$class` for erased
parameters: `Pair<$class,$class>.first`, `Pair<int,$class>.first`,
`Box<int>`, class object `Box<geometry::point::Point>`.

### 9.5 What changes in the backends

**C** (`CEmitter.cpp`): `collectSymbols` runs over the unit's module only
and calls `pir::mangle`; `UsedSymbols` and the counter are deleted; foreign
symbols are mangled from the extern's `(Module, linkName())` pair without
looking the definition up; `ClassDefs` is keyed by `(module, class)` and
an extern class's struct is emitted from the unit's own `extern class`
item; `static` for `internal`. Then `emitModuleC` is a pure function of
`program.Modules[mi]`, `emitC` (whole program) and `emitModuleC` emit
identical declarations for a symbol, and the per-module object is the
payload of §7.5. A new test links `c` objects of modules compiled in two
different programs into a third.

**LLVM** (`PIRToLLVM.cpp`): names from `pir::mangle*`; linkage and
visibility from `CODE` instead of `ExternalLinkage` for everything; the
payload hooks. Objects then carry C-compatible symbols, so a program may be
linked from `c` and `llvm` payloads together by the system `cc`. After
#162 nothing here changes: the plugin reads PIR (text, later
`pir_binary`), produces payload bytes, and links through the host.

### 9.6 Linking programs and libraries

Executables: today's link line, where the objects are the main unit's fresh
object plus each import's payload object (C) or the single object of the
linked bitcode (LLVM). With #153 the runtime becomes `-lpaykan_runtime`
(versioned SONAME on the runtime ABI) by default, static under
`--static-runtime`. Libraries without `main` (#23): `paykan build --lib`
over a set of modules = every module's payload object (regenerated if
absent), no entry point, archived with `ar` (or `--lib=shared`), plus the
generated C header (#21, from `IFACE`); `export fn` carries `cExport` with
the unmangled `cName`; runtime initialisation without `main` is a runtime
entry the host calls. Cross targets (#23): `--target <triple>` flows to the
backend and into the payload; `CODE` is target-independent by construction
(LP64 LE is a runtime-ABI assumption; a non-LP64 target is an ABI change,
not a `CODE` change).

---

## 10. Libraries and APIs

| Library (CMake) | Headers | Namespace | Depends on |
|---|---|---|---|
| `paykan_support` (new; standard C++) | `include/paykan/support/{Bytes.h, Sha256.h, AtomicFile.h}` | `paykan::support` | — |
| `paykan_pir` (existing) | `pir/Codes.h` (opcode/type codes shared with the printer), `pir/Binary.h` (the `CODE` codec), `pir/Mangle.h`, `pir/Version.h` (`kPIRVersion`) | `paykan::pir`, `::binary` | `paykan_support` |
| `paykan_pkm` (new; format only) | `pkm/File.h` (container, manifest, verdicts), `pkm/Interface.h` (`IFACE`/`TMPL` model + codec), `pkm/Sections.h` (`SymbolIndex`, `DebugInfo`, `Payload`) | `paykan::pkm` | `paykan_pir`, `paykan_support` |
| `paykan_modules` (new) | `modules/ModuleGraph.h`, `modules/ImportResolver.h` | `paykan::modules` | `paykan_pkm`, Sema, Lowering |
| `paykan_backend` (existing) | `Backend.h` + `ModuleCodegen`, `PayloadInfo` (POD; plugins never include the container) | `paykan::backend` | — |

Two libraries rather than one so that plugins and out-of-tree tools can
link the format without Sema (open question 9 confirms).

```cpp
// support
class ByteWriter { void u8/u16/u32/u64/uleb/sleb/f64(...); void bytes(span); size_t size(); std::vector<uint8_t> take(); };
class ByteReader { explicit ByteReader(span); bool u8(uint8_t&) …; bool uleb(uint64_t&, uint64_t max); bool bytes(size_t n, span&);
                   bool count(uint64_t& n, size_t minBytesPerElem); ByteReader sub(size_t len); size_t offset(); const std::string& error(); };
class StringTable;                                   // writer: intern() first-use order; reader: bounds-checked lookups
std::array<uint8_t,32> sha256(span);                 // FIPS 180-4, ~150 lines, standard test vectors
class AtomicFile;                                    // temp + rename, the CBuild.cpp pattern, shared by every writer

// pir::binary
std::vector<uint8_t> encode(const Module&, const EncodeOptions& = {});          // deterministic; never fails for a verified module
StatusOr<Module> decode(span, const DecodeOptions& = {});                      // bounds/versions/reserved codes/nesting/slack/trailer; does NOT verify
StatusOr<BlobInfo> inspect(span);                                               // header + string table only
StatusOr<Function> decodeFunction(span, const BlobInfo&, size_t off, size_t len);
std::vector<pkm::SymbolEntry> index(span);

// pkm
struct File { Header; std::vector<SectionEntry>; Manifest; span section(Kind, size_t instance = 0) const; ... };
StatusOr<File> readFile(span, const ReadOptions& = {});                          // §1.7 steps 1-5
Verdict checkCompatibility(const Manifest&, const HostIdentity&, Policy);
class Writer { void add(Kind, flags, bytes); std::vector<uint8_t> finish(); };   // sorts, aligns, hashes
StatusOr<Interface> readInterface(span iface, span tmpl /*may be empty*/, std::string_view expectedName);
std::vector<uint8_t> writeInterface(const Interface&); std::vector<uint8_t> writeTemplates(const Interface&);
Payload/SymbolIndex/DebugInfo encode/decode; void dump(const File&, std::ostream&, const DumpOptions&);

// modules
class ModuleGraph : public sema::ImportResolver {
  StatusOr<const LoadedModule*> resolve(std::string_view canonical, SourceLoc at);   // locate/decide/build/load, memoised
  StatusOr<pir::Program> assembleProgram(pir::Module main);                         // + synthetic units
  const std::vector<PayloadChoice>& payloads() const; Status writePayloads(...);    // §7.4 steps 2 and 5
};
```

---

## 11. Versioning and compatibility policy

| Change | Bump |
|---|---|
| new optional section kind, new non-critical manifest tag, new optional `IFACE` record tag or appended record field | `format_minor` / `iface_minor` (old readers skip) |
| new critical manifest tag, new required section kind | `format_minor` + the critical/required bit (old readers reject with a clear message) |
| container framing, manifest framing | `format_major` / `manifest_major` |
| interface record semantics (a field means something else) | `iface_major` |
| PIR types/opcodes/semantics (#96, #99, #170), the §9.1 mapping | `pir::kPIRVersion` (the `CODE` codec major stays unless the framing changes) |
| object layout, calling convention, vtable header, mangling (#93, #98, §9.4) | `PAYKAN_RUNTIME_ABI_VERSION` |
| a backend's payload bytes | that backend's `payloadFormat`/`backendVersion` only |

**Decision: v0.2.0 ships one coordinated bump** (format 1.0, interface 1.0,
PIR 2, runtime ABI 7) carrying #93/#96/#98/#99/#170 and the mangling,
rather than four separate bumps. Any PIR change touches `PIR.h`, the
printer, the parser, the verifier, `docs/pir.md` and the binary codec in
one PR.

Reserved now so no later major bump is needed:

| Ticket | Reserved field(s) |
|---|---|
| #19 visibility | `Vis` per declaration; `T_OPAQUE`; linkage bits in `CODE`/`SYMIDX` |
| #170 statics/constants | `CONST` record; `statics[]`; `Const` encoding; `SlotRec`; opcodes 32/33; `HAS_STATIC_INIT` |
| #171 attributes | `Attrs` per declaration; function/class flag bits; `(key, value)` bag; manifest `attributes` |
| #167 explicit self / static methods | `MFlags.STATIC`, `MUT_SELF`; `direct[]` |
| #18 override/final/pure | `MFlags.FINAL/OVERRIDE/PURE`; `cflags.FINAL`; class `abstract` bit |
| #147 traits | `TRAIT`, `IMPL`, `Bounds`, `T_TRAIT_OBJ`, trait default bodies in `TMPL` |
| #160 generics | `TMPL` section, `INST` part, `ERASABLE`, `erased`/`nHiddenParams`, `ClassObjRec`, `$class` spelling |
| #96 inline optionals, #99 weak | type codes 8–16, opcodes 26–31, `T_WEAK` |
| #98 borrowed params | `Sig.conv` per parameter; `nConventions` |
| #93 one allocation | header kind is a property of `runtime_abi`, nothing per class |
| #153 runtime as library | manifest `runtime_abi` + `libraries` (`RUNTIME`, `ALLOCATOR`); payload flag bit 5 |
| #21 C FFI | `cExport`/`cName`; `EXPORTS_C`; `C_LIBRARY` entries |
| #23 libraries / cross targets | payload `target`; `PAYKAN_LIBRARY`; `pkm extract` |
| #25 embedding / hot reload | `SYMIDX` record hashes; `LazyHashes`; `decodeFunction`; `load_symbol` slot in `PaykanModuleCodegen` |
| #113 stdlib | `SYSTEM` bit; stdlib search path; `PAYKAN_PKM_COMPATIBLE_VERSIONS` |
| #162 llvm plugin | `pir_binary` in `PaykanModuleInput`; `PAYKAN_BACKEND_READS_BINARY_PIR` |
| #97 passes | `opt_pipeline`; `CODE` pass-level bits |
| #144 JSON diagnostics | nothing: every error is a `DiagEngine` diagnostic |

---

## 12. Testing

| Suite | Proves |
|---|---|
| `support_tests` | SHA-256 FIPS vectors; LEB128 minimality/overflow/10-byte limit; `ByteReader` on truncation at every offset; `AtomicFile` under a crash |
| `pir_tests` `PIRBinary.RoundTrip` | hand-written PIR with every opcode, NaN payloads, `-0.0`, quoted names, empty blocks, abstract slots, extern classes: both invariants of §5.8, `verify(decode(b))` clean |
| `lowering_tests` `PIRBinary.Corpus` | every corpus program: both invariants; `decodeFunction` for every `SYMIDX` entry equals the whole-module decode |
| `PIRBinary.Determinism` / `pkm_determinism` | same bytes from two processes, after a copy, across platforms (a CI job compares hashes of `--emit-pkm` output between Linux and macOS runners); `strings` finds no absolute path |
| fuzz (ASan/UBSan jobs; the `Rng` of `FuzzSmokeTests.cpp`) | byte flips, truncation at every offset, count inflation, reserved codes, nesting bombs, hostile section tables and manifests: readers fail cleanly or produce a module the verifier accepts; never crash or hang |
| `pkm_tests` | container/manifest/interface codecs; every `pkm::Error` code reachable; payload stale `codeHash`, corrupt `payloadHash`, symbol list naming a missing symbol; `SYMIDX`/`DEBUG` tampering; the `IFACE`–`CODE` cross-check with a lying interface (signature, layout, missing `destroy`, `internal` export) |
| `InterfaceTests` | `exportInterface` → `writeInterface` → `readInterface` → reconstruct on every `samples/imports` program equals today's `ModuleInfo` behaviour; every §3.7 invariant rejects |
| `Mangle.Injective` | property test over random byte strings and every escape: `demangle(mangle(m, s)) == (m, s)`; C11 charset; no `__` except the separator |
| staleness matrix (`driver_tests`) | body edit → one module rebuilt; signature edit → direct importers; generic body edit → only value-specialising importers and units; dependency interface change → transitive stop where unaffected; corrupt cache entry rebuilt; version bump rebuilds all; prebuilt mismatch is the §8.2 error |
| payload cycle (both backends) | cold build writes payloads; warm build loads them (count `cc` invocations with a stub `CC`); body edit regenerates one module's payload; `-O0` vs `-O2` keep separate payloads; tampered payload ignored and regenerated; `pkm strip` leaves `iface_hash`/`code_hash` unchanged; `--write-payloads=off` writes none |
| cross-program / cross-backend link | `--emit-c` of a module compiled in program A and B produces the same C for that module; A's `c` object links into B; an `llvm` object links with a `c` object |
| prebuilt / portable / out-of-tree | a program built from `.pkm` files only (sources deleted) on both backends; a portable `.pkm` built with `c` consumed by `llvm` and by `print-pir`; an out-of-tree payload plugin in `tests/OutOfTree` |
| generics (`GenericsTests`, `samples/imports`) | import a template; value and class instantiations; mixed; nested/transitive; one unit per instantiation across two importers; hidden helper calls; cross-file diagnostics text; `Bad<Bad<T>>` terminates; both backends, identical output, 0 live blocks |
| `SamplesParity` cold and warm through `.pkm`, both backends | nothing changes observably |
| `pkm dump` goldens | the dump format is stable |
| `DEBUG` | a `--debug-lines` C build maps a panic back to the Paykan line; an importer-side error inside a generic names the imported file and line in JSON (#144) |

Measured sizes (`--emit-pir` over 94 corpus programs with a per-record
model of §5): `CODE` ≈ 12–13 bytes per instruction ≈ 30% of PIR text
(3.3× smaller; 12_calc: 106 KB text → ~30 KB); string table ≈ 11% of that;
`SYMIDX` ≈ 50 B/symbol; `DEBUG` ≈ 20% of `CODE`; a C object payload 4–8×
`CODE` at `-O2`; LLVM bitcode 2–3×. A 300-function stdlib module: ~50 KB
`CODE`, ~15 KB `SYMIDX`, ~10 KB `DEBUG`, 200–400 KB per native payload.

---

## 13. Phasing

Each PR is independently mergeable, green on both backends and both
frontends, and leaves `develop` releasable. "Universal first": the
payload-free `.pkm` that both backends and `print-pir` consume lands before
any payload code.

| PR | Lands | Depends on |
|---|---|---|
| **A0** | this document | — |
| **A1** `paykan_support` | SHA-256, `ByteReader`/`ByteWriter`, `StringTable`, `AtomicFile` (CBuild switched to it); `PAYKAN_RUNTIME_ABI_VERSION` into `Runtime.h`; `pir::kPIRVersion`; `PAYKAN_BUILD_ID` | — |
| **A2** binary PIR | `Codes.h`, `Binary.h/.cpp`; linkage/attribute flags in `PIR.h` + text syntax + verifier; corpus round trip; fuzz; `docs/pir.md` "Binary form" | A1 |
| **A3** interface model | `pkm::Interface` + codec (`IFACE` with reserved records, `TMPL` empty); `ModuleInfo` → `pkm::Interface` **in-process** (`ModuleCache` becomes a map of encoded interfaces, so every existing import test exercises the codec); identical diagnostics | A1 |
| **A4** container | header, table, manifest, verdicts, `SYMIDX`, `PAYLOAD` kind recognised and skipped; `--emit-pkm`; `pkm dump/check`; golden dumps | A2, A3 |
| **A5** lowering from interfaces | `lowerModule` beside `lowerProgram`; `externsFor(Interface)`; `verify(Program)` proves interface-derived externs match on every sample; `--emit-pir` unchanged | A3 |
| **A6** module graph | `paykan_modules`; Sema `ImportResolver`; driver switched; `.paykan_cache/**.pkm` with the freshness rule; `ModuleCache`, child contexts, `ProgramLowering` deleted; `--rebuild-modules`, `--no-module-cache`; docs; corpus cold/warm on both backends | A4, A5 |
| **B1** prebuilt | search path, `--module-path`, `$PAYKAN_STDLIB`, "tried …" error, version-mismatch error, `PAYKAN_PKM_COMPATIBLE_VERSIONS`; stdlib installed as `.pkm` (#113 start) | A6 |
| **B2** determinism CI | `scripts/pkm_determinism.py`; cross-platform hash job | A6 |
| **C1** mangling + linkage | `pir::mangle`; both backends on it; `CEmitter` per-module stability; cross-backend link test; runtime ABI 7 | A2 |
| **C2** payload protocol | `Payload`/`DEBUG` codecs; `ModuleCodegen` + `PaykanModuleCodegen` + `BackendAdapter`; driver payload cycle with a fake provider; out-of-tree payload plugin test; `pkm strip/extract/add-payload` | A6, C1 |
| **C3** C provider | object payloads; the `.c/.o/.key` cache deleted; `06-modules.md` cache section rewritten | C2 |
| **C4** LLVM provider | bitcode payloads; the `.bc` cache deleted; lives in the plugin after #162 | C2 |
| **C5** `DEBUG` | `StmtLocs` in `Builder`/lowering; `#line`; `DILocation`; #144 notes | C2 |
| **D1** generics | `TMPL` codec; exporter (two-phase lookup, hidden set); importer materialisation; `INST`; synthetic units; cross-file diagnostics; the `SemaClass.cpp` "cannot be imported yet" rejections removed | A6, #160, #147, #19 |
| **D2** static data / attributes | `CONST`, `statics[]`, `SlotRec`, `global.load/store`, attribute records | A6, #170, #171 |
| **D3** ABI bundle | PIR 2 / runtime ABI 7 with #93/#96/#98/#99 | C1, those tickets |

A1–A3 are mutually independent; A4 and A5 too; A6 is the integration PR
and the only one in phase A that changes user-visible behaviour (the
cache's shape). **#57 closes with A + B** (the universal module, prebuilt
modules, both backends consuming it); #106 closes with C; generics across
modules close under #160 with D1.

---

## 14. Decisions (index) and rejected alternatives

1. Three section *classes*, not three literal sections; per-section SHA-256;
   no file-level string table (§1).
2. TLV manifest with critical-tag rule; abstract target in the manifest,
   triple in the payload; frontend recorded, not gating (§2).
3. Derived hashes, never stored; `module_hash` excludes payloads (§1.5).
4. `IFACE` in Sema's terms with a structured type table and no PIR; externs
   derived by one mapping and cross-checked against `CODE` (§3, §9.1).
5. Visibility per #19 with hidden records for template-reachable privates
   (§3.6).
6. Templates as name-resolved AST in a separate `TMPL` section sharing
   `IFACE`'s tables; `iface_hash` excludes bodies (§3.5, §4).
7. Canonical instantiation names, no spaces; one nominal type program-wide
   (§4.4).
8. Hybrid erasure: erasable iff all parameters bounded; erased copies in
   the defining module's `CODE`; every foreign instantiation a synthetic
   unit; no COMDAT (§4.5).
9. Binary `CODE` mirroring `PIR.h` in lowering order; fixed code tables
   with reserved ranges; length-prefixed function records; pass level 0;
   conventions in the signature (§5).
10. `SYMIDX` for lazy decode and change detection; `DEBUG` as a sidecar
    keyed by statement preorder (§6).
11. Host-managed payloads; three callbacks in an appended
    `PaykanModuleCodegen` descriptor; zero payloads = portable; payloads
    replace both backend caches; synthetic units never in a payload (§7).
12. One module graph, one load path; program order unchanged; identity by
    canonical name; source authoritative except system modules (§8).
13. Three linkages; one injective counter-free mangling in the core for
    both backends; runtime ABI 7 (§9).
14. Four libraries; SHA-256 everywhere; no executable-stamp build ids (§10).
15. One coordinated v0.2.0 bump; reserved fields per ticket (§11).

Rejected (reason in place): fixed-struct header; shared strtab; three fixed
sections; source text or template PIR for generics; typed AST; private
copies or COMDAT for instantiations; "first importer owns it"; type strings
in the interface; embedded text PIR as a permanent encoding; sorted `CODE`
items; post-pass PIR per `-O`; `Loc` inside `Instr`; backend sidecars or
backend-embedded interfaces; one `.pkm` per build configuration; payload
list in the manifest; counters, length-prefixed or hashed mangling; FNV-1a
as a file identity; `core.version` purely informational or exact-match
everywhere.

## 15. Open questions for the owner

1. ~~Generics: the "shipped PIR" reading.~~ **Decided (owner, PR #179
   review):** the erased copy ships as PIR, the template as name-resolved
   AST; value specialisations are re-lowered by the importer's program
   (§4.1).
2. ~~Unbounded templates.~~ **Decided (owner):** the same for unbounded
   templates: every exported template ships both the type-erased PIR copy
   and the resolved AST; class-type arguments use the erased PIR, value-type
   arguments use the AST. So `ERASABLE` is not "all parameters bounded"
   (§4.5): the erased copy is compiled with each parameter at its bound,
   `Obj` when it has none, and a template whose body needs more of an
   unbounded `T` than `Obj` offers is reported at export until it declares a
   bound (#147). Phase D1 settles the exact rule.
3. **Instantiation spelling:** drop the space after commas
   (`Pair<int,Node>`) in `Sema::instantiationName` so PIR names, mangled
   names and `INST` keys share one canonical form? Must be fixed before any
   `.pkm` is distributed.
4. ~~Text PIR syntax~~ **Decided (owner):** yes. This is PIR text only
   (`--emit-pir` and `pkm dump`), never Paykan language syntax.
5. **Defaults:** `--write-payloads=on` for cached modules; `--emit-pkm`
   portable unless `--with-payload`. Confirm.
6. **Pre-release build id:** git revision at configure time, falling back
   to a hash of the source listing; gating only in the cache tier
   (proposed).
7. **`--prefer-pkm`** for vendored user modules shipped with sources:
   flag, default off (proposed), or a project setting?
8. **Backend caches:** delete `.bc` and `.c/.o/.key` once C3/C4 land
   (proposed), or keep them as private caches?
9. **Library split:** `paykan_support` + `paykan_pkm` + `paykan_modules`
   (proposed) or one library?
10. **`--check-only`** builds dependencies fully (parse, Sema, lower, write;
    one code path; proposed) or only their interfaces in memory?
11. **Hidden symbols in strict C11** stay plain external (proposed) or get
    `__attribute__((visibility("hidden")))` under a non-strict flag?
12. **Module-level `_name`** is public (#19's proposal) — confirm, since the
    interface writes `Vis` 0 for it.
13. **Scope:** #57 closes with phases A + B; C under #106; D1 under #160
    (proposed).
14. **Doc location:** this file at `docs/design/pkm.md`, with a user-facing
    `docs/pkm.md` format reference written when A4 lands (proposed).

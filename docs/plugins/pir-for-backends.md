# PIR for backend plugins

A loaded backend receives the program as **PIR text**: the textual form of the
Paykan IR that [`../pir.md`](../pir.md) specifies. It is exactly what
`paykan --emit-pir program.pkn` prints, so you can always look at the input
your backend will get.

## What arrives

`PaykanBackendInput` ([`plugin-api.md`](plugin-api.md#backends)) carries:

| Field | Meaning |
|---|---|
| `pir`, `pir_size` | the whole program: every module, main module first, already lowered and run through the verifier; UTF-8, NUL-terminated at `pir[pir_size]` |
| `pir_text_version` | the version of the text form, `PAYKAN_PIR_TEXT_VERSION` (1) |
| `input_filename`, `project_root` | the main source file and the directory imports were resolved against |
| `opt_level` | `-O<n>`, 0..3; what it means is the backend's choice |

Everything the verifier checks ([`pir.md` §9](../pir.md#9-verifier)) holds:
types match, every value is defined before use in its block structure, every
callee is declared, every class layout and vtable is complete. A backend may
rely on it and needn't re-check.

## Reading it

The text form is line-oriented and made for this:

- A module starts with `module "<name>"`. The items of a module follow, one
  per line or block: `cstr` / `data` / `bytes` globals, `extern fn`,
  `extern obj`, `extern vtable` declarations of runtime symbols, `class`
  layouts, and `fn` definitions whose bodies are indented blocks closed by
  `}`.
- Values are `%name.N` (unique per function), symbols `@name`, constants are
  literal (`42`, `1.5`, `true`, `'a'`, `null box`).
- Control flow is structured (`if`, `while`, `break`, `continue`, `ret`), so
  a body maps directly onto the target's statements; there is no CFG to
  rebuild.

[`pir.md`](../pir.md) specifies every type (§1), value and constant (§2),
function and module-level item (§3, §4), class layout (§5), statement and
instruction (§6), and the runtime ABI, the `$rt.` externs the generated code
calls (§8); [§10](../pir.md#10-text-format-summary) summarises the text
format.
The runtime library and header a native program links against come from the
host: `host->runtime_library(session)` and
`host->runtime_include_dir(session)`.

The example [`src/Backends/PrintPIR`](../../src/Backends/PrintPIR) (C) reads it and writes
it back out unchanged.

## Stability

`PAYKAN_PIR_TEXT_VERSION` is bumped whenever the text form changes
incompatibly: a construct is removed or changes meaning, or a construct a
backend must handle is added. A backend should refuse (report an error
through `host->diagnostic`) a `pir_text_version` it does not know. While
PaykanLang is below 1.0, a minor release may bump it; the CHANGELOG says so,
and the release's plugin compatibility list then drops the versions whose
plugins would misread it.

The printer and parser in the core (`src/PIR/Printer.cpp`,
`src/PIR/Parser.cpp`) define the form; the core's tests round-trip the whole
samples corpus through them (`tests/Lowering/RoundTripTests.cpp`), so what a
backend receives is always parseable PIR. A binary form may be added later
as an option, next to the text.

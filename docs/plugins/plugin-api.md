# The plugin API (`plugin_api.h`)

[`include/paykan/plugin_api.h`](../../include/paykan/plugin_api.h) is the
whole interface between `paykan` and a loaded plugin: pure C11, no other
PaykanLang header, installed as `<prefix>/include/paykan/plugin_api.h`. A
plugin may also copy it into its own tree; it depends on nothing. This page
is its reference; the header's comments say the same, closer to the code.
[`overview.md`](overview.md) explains how plugins are found and checked.

Version: **plugin API 1** (`PAYKAN_PLUGIN_API_VERSION`), PaykanLang
0.1.0-alpha.

## Conventions

These are part of the contract, for every struct and callback below.

- **Strings.** Names, versions, descriptions and paths are NUL-terminated
  UTF-8 (`const char *`). Text buffers (the program, messages, output) are a
  pointer plus a size in bytes, UTF-8, and need not be NUL-terminated; the
  buffers `paykan` passes in happen to be NUL-terminated one byte past the
  size, for convenience.
- **Ownership.** Everything `paykan` passes to a callback is **borrowed for
  that call**: copy what you keep. Everything a plugin's descriptors point
  to must stay valid until the process exits (`paykan` never unloads a
  plugin), so make them static. Memory a plugin returns to `paykan` (no
  callback of API 1 does) is released with the plugin's own
  `PaykanPlugin.free_memory`, never with `paykan`'s allocator.
- **Errors** are `int` status codes: `PAYKAN_OK` (0) is success, anything
  else a failure. Explain a failure with `host->diagnostic(...,
  PAYKAN_DIAG_ERROR, ...)`; `paykan` prints it as the failure message (or
  `backend 'x' failed (status N)` when there is none).
- **No unwinding across the boundary**: no C++ exception, no `longjmp`, no
  Rust panic may leave a callback. A C++ plugin catches everything in each
  callback; a Rust plugin wraps each callback's body in
  `std::panic::catch_unwind` and returns `PAYKAN_ERROR` on a panic. Building
  the cdylib with `panic = "abort"` is the alternative.
- **Versioned structs.** Every struct begins with `uint32_t struct_size`, the
  `sizeof` of the struct as its producer compiled it. Within one plugin API
  version, fields are only ever appended: the reader of a struct never looks
  past `struct_size`, and treats a missing trailing field as absent. So set
  `struct_size = sizeof(<struct>)` and leave the rest to the reader.
- **Threads.** `paykan` calls a plugin from one thread at a time.
- **Calling convention.** The platform's C calling convention; integer
  fields are fixed-width (`uint32_t`, `size_t`), never `enum`.

## The entry point

```c
PAYKAN_PLUGIN_EXPORT const PaykanPlugin *paykan_plugin_init(const PaykanHost *host);
```

The library's one required export (`PAYKAN_PLUGIN_EXPORT` gives it default
visibility). `paykan` calls it once, right after loading the library, and
checks the returned descriptor **before** it calls anything else in the
library. Only record `host` (it stays valid for the process) and return the
static descriptor: no output, no side effects. Returning `NULL` rejects the
plugin.

## `PaykanPlugin`: what the plugin returns

| Field | Type | Meaning |
|---|---|---|
| `struct_size` | `uint32_t` | `sizeof(PaykanPlugin)` |
| `api_version` | `uint32_t` | `PAYKAN_PLUGIN_API_VERSION` of the header built with; must be one `paykan` supports |
| `build_version` | `const char *` | the PaykanLang version the plugin is built with, `PAYKAN_PLUGIN_BUILD_VERSION`; must be on `paykan`'s compatibility list ([#103](https://github.com/parsabee/PaykanLang/issues/103)) |
| `name`, `version` | `const char *` | the plugin's own name and version for `--version`; may be `NULL` |
| `free_memory` | `void (*)(void *)` | releases memory the plugin returned to `paykan`; may be `NULL` |
| `num_backends`, `backends` | `size_t`, `const PaykanBackend *` | the backends it provides (at least one) |

`PAYKAN_PLUGIN_BUILD_VERSION` is `PAYKAN_PLUGIN_HEADER_VERSION`, the
PaykanLang version of the header (generated from the build's version into
`paykan/plugin_api_version.h`, installed next to `plugin_api.h`), unless the build defines it
(`paykan_add_backend_plugin(... BUILT_WITH <version>)` does).

## Backends

```c
typedef struct PaykanBackend {
  uint32_t struct_size;
  const char *name;             /* --backend=<name> */
  const char *description;      /* --list-backends, --version; may be NULL */
  uint32_t capabilities;        /* PAYKAN_BACKEND_* bits */
  const char *source_extension; /* ".c", ".pir"; may be NULL */
  void *data;                   /* passed back to every callback */
  int (*emit)(void *data, PaykanSession *session,
              const PaykanBackendInput *input, uint32_t kind,
              const char *output_path);
  int (*run)(void *data, PaykanSession *session,
             const PaykanBackendInput *input,
             const PaykanRunRequest *request, int *exit_code);
} PaykanBackend;
```

- **`capabilities`**: `PAYKAN_BACKEND_EMIT_SOURCE` (`--emit-source`),
  `PAYKAN_BACKEND_EMIT_OBJECT`, `PAYKAN_BACKEND_EMIT_EXECUTABLE`
  (`paykan build`), `PAYKAN_BACKEND_RUN` (`paykan run`, the default command).
  `paykan` refuses a request the backend doesn't declare before calling it.
- **`emit`** (required): translate the program. `kind` is
  `PAYKAN_EMIT_SOURCE` (write the output with `host->write_output`),
  `PAYKAN_EMIT_OBJECT` or `PAYKAN_EMIT_EXECUTABLE` (write the file
  `output_path`: `-o`, or the input's stem by default).
- **`run`** (required with `PAYKAN_BACKEND_RUN`): execute the program, store
  its exit code in `*exit_code`, and return `PAYKAN_OK`; return an error code
  when it could not be run. A native backend builds an executable in
  `host->temp_dir` and runs it with `host->run_executable`.

The names in one plugin must be distinct; a name already taken by a built-in
or an earlier plugin becomes ambiguous ([`overview.md`](overview.md)).

`PaykanBackendInput`:

| Field | Meaning |
|---|---|
| `struct_size` | as above |
| `input_filename` | the main source file, as given on the command line |
| `project_root` | the directory imports were resolved against (`""` for the current one) |
| `pir`, `pir_size` | the verified program as PIR text ([`pir-for-backends.md`](pir-for-backends.md)) |
| `pir_text_version` | `PAYKAN_PIR_TEXT_VERSION` of that text (1) |
| `opt_level` | `-O<n>`, 0..3 |

`PaykanRunRequest`: `args` / `num_args`, the program's arguments with
`args[0]` the script path (pass them to `run_executable` as they are), and
`track_heap` (`--track-heap`; pass it on too).

## `PaykanHost`: what `paykan` offers

`host` is valid for the life of the process; `session` arguments are the
session of the callback that is running and valid only during it.

| Function | Meaning |
|---|---|
| `api_version`, `toolchain_version` | the running `paykan`'s plugin API version and PaykanLang version (fields) |
| `diagnostic(session, level, file, line, column, message, size)` | report a diagnostic. `level` is `PAYKAN_DIAG_ERROR` / `_WARNING` / `_NOTE`; `file` `NULL` means the input file; `line`/`column` are 1-based, 0 when unknown. Errors become the call's failure message; warnings and notes print at once, as `file:line:col: warning: message` |
| `write_output(session, data, size)` | append bytes to the output: the `--emit-source` stream, or standard output in `run` |
| `allocate(size)`, `deallocate(ptr)` | the C library's `malloc` / `free`, for the plugin's own use |
| `log(level, message, size)` | a line on standard error, `paykan: plugin: <message>`; `PAYKAN_LOG_DEBUG` messages appear only with `PAYKAN_PLUGIN_DEBUG=1` |
| `runtime_library(session)`, `runtime_include_dir(session)` | the Paykan runtime (`libpaykan_runtime.a`, the directory of `Runtime.h`), found as the built-in backends find it (`$PAYKAN_RUNTIME_DIR`, then the installation); `NULL` with a diagnostic if there is none |
| `link_executable(session, inputs, n, output_path)` | link object files, or C sources (compiled with the runtime's headers on the include path), against the runtime into an executable, with the system C compiler (`$CC`, else `cc`) and `-O<opt_level>` |
| `run_executable(session, path, args, n, track_heap, &exit_code)` | run an executable with `argv = args` (`args[0]` is its `argv[0]`; `n == 0` runs it with `argc == 0`), the runtime's heap statistics on when `track_heap`, and wait; `exit_code` is its status, 128 + the signal when it was killed |
| `temp_dir(session)` | a private scratch directory, removed with its contents when the callback returns |

Every host function that can fail returns `PAYKAN_ERROR` (or `NULL`) after
reporting why with a diagnostic, so a callback can just return the error.

## A minimal plugin

```c
#include "paykan/plugin_api.h"

static const PaykanHost *host;

static int emit(void *data, PaykanSession *s, const PaykanBackendInput *in,
                uint32_t kind, const char *output_path) {
  (void)data; (void)kind; (void)output_path;
  return host->write_output(s, in->pir, in->pir_size);
}

static const PaykanBackend backends[] = {{
    .struct_size = sizeof(PaykanBackend), .name = "mine",
    .capabilities = PAYKAN_BACKEND_EMIT_SOURCE, .emit = emit}};

static const PaykanPlugin plugin = {
    .struct_size = sizeof(PaykanPlugin),
    .api_version = PAYKAN_PLUGIN_API_VERSION,
    .build_version = PAYKAN_PLUGIN_BUILD_VERSION,
    .num_backends = 1, .backends = backends};

PAYKAN_PLUGIN_EXPORT const PaykanPlugin *paykan_plugin_init(const PaykanHost *h) {
  host = h;
  return &plugin;
}
```

```sh
cc -std=c11 -shared -fPIC -I<prefix>/include -o libmine.so mine.c
paykan --plugin=./libmine.so --backend=mine --emit-source program.pkn
```

[`src/Backends/PrintPIR`](../../src/Backends/PrintPIR) is this plugin with comments and
a CMake build.

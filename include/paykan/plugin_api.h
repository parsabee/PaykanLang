/* Copyright (c) 2026 Parsa Bagheri
 * SPDX-License-Identifier: MIT
 *
 * The PaykanLang plugin API: the C interface between `paykan` and a plugin
 * loaded at run time (docs/plugins/plugin-api.md is the reference).
 *
 * Pure C11, no other PaykanLang header needed: a plugin can be written in
 * any language that can export a C function and lay out C structs (C, C++
 * with any compiler, Rust, Zig, Go with cgo, ...).  A plugin does not link
 * against PaykanLang at all; everything it needs from paykan comes through
 * the PaykanHost table it is handed.
 *
 * A plugin provides frontends (source text in, the program out as text in
 * the AST interchange format, docs/plugins/ast-format.md) and backends (the
 * program in as PIR text, docs/pir.md, and an artifact out).
 *
 * A plugin is a shared library (.so on Linux, .dylib on macOS) exporting one
 * function:
 *
 *     const PaykanPlugin *paykan_plugin_init(const PaykanHost *host);
 *
 * paykan calls it once, right after loading the library, and checks what it
 * returns (the plugin API version and the PaykanLang version the plugin was
 * built with, #103) BEFORE it calls anything else in the library.  A plugin
 * that fails the check is listed as incompatible and none of its callbacks
 * ever run.  paykan_plugin_init must therefore only return the plugin's
 * description: no output, no allocation the host has to know about, no other
 * side effect.
 *
 * Conventions (all of them part of the contract):
 *   - Strings in descriptors (names, versions, descriptions) and paths are
 *     NUL-terminated UTF-8.  Text buffers (program text, messages, output)
 *     are a pointer plus a size in bytes, UTF-8, and need not be
 *     NUL-terminated; the buffers paykan passes in are NUL-terminated
 *     anyway, one byte past the size.
 *   - Everything paykan passes to a callback is borrowed for the duration of
 *     that call.  Everything the plugin's descriptors point to must stay
 *     valid until the process exits (paykan never unloads a plugin).
 *   - Memory the plugin returns to paykan is released with the plugin's own
 *     PaykanPlugin.free_memory, never by paykan's allocator.
 *   - Errors are int status codes: PAYKAN_OK (0) is success, anything else a
 *     failure, explained through PaykanHost.diagnostic.
 *   - Nothing may unwind across the boundary: no C++ exception, no longjmp,
 *     no Rust panic.  A Rust plugin wraps every callback in
 *     std::panic::catch_unwind; a C++ plugin catches everything.
 *   - Every struct starts with struct_size, the sizeof() of the struct as
 *     its producer compiled it.  New fields are only ever appended, so a
 *     plugin built against an older header keeps working with a newer
 *     paykan of the same API version, and the reader of a struct never
 *     looks past struct_size.
 */

#ifndef PAYKAN_PLUGIN_API_H
#define PAYKAN_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Versions -------------------------------------------------------------- */

/* The version of this interface.  paykan accepts a plugin only if the
 * plugin's PaykanPlugin.api_version is one it supports (today: exactly
 * this one).  It is bumped whenever a struct or callback below changes
 * incompatibly; appending a field is compatible. */
#define PAYKAN_PLUGIN_API_VERSION 1

/* The PaykanLang version this header belongs to, pre-release label
 * included.  The build checks that it equals the toolchain's version. */
#define PAYKAN_PLUGIN_HEADER_VERSION "0.1.0-alpha"

/* The PaykanLang version a plugin declares it is built with (#103): the
 * version of this header unless the build says otherwise
 * (paykan_add_backend_plugin's BUILT_WITH defines it).  paykan accepts the
 * plugin only if this version is on its compatibility list
 * (paykan --version prints the list). */
#ifndef PAYKAN_PLUGIN_BUILD_VERSION
#define PAYKAN_PLUGIN_BUILD_VERSION PAYKAN_PLUGIN_HEADER_VERSION
#endif

/* The version of the PIR text form a backend receives (docs/pir.md). */
#define PAYKAN_PIR_TEXT_VERSION 1

/* The version of the AST interchange format a frontend returns
 * (docs/plugins/ast-format.md): the number after `paykan-ast`. */
#define PAYKAN_AST_FORMAT_VERSION 1

/* The name of the entry point, for hosts and tests that look it up. */
#define PAYKAN_PLUGIN_ENTRY_POINT "paykan_plugin_init"

/* -- Status codes, levels, kinds ------------------------------------------- */

#define PAYKAN_OK 0
#define PAYKAN_ERROR 1

/* PaykanHost.diagnostic levels. */
#define PAYKAN_DIAG_ERROR 0u
#define PAYKAN_DIAG_WARNING 1u
#define PAYKAN_DIAG_NOTE 2u

/* PaykanHost.log levels.  Debug messages are shown only when the
 * environment variable PAYKAN_PLUGIN_DEBUG is set to a non-empty value
 * other than 0. */
#define PAYKAN_LOG_DEBUG 0u
#define PAYKAN_LOG_INFO 1u
#define PAYKAN_LOG_WARNING 2u

/* PaykanBackend.capabilities bits. */
#define PAYKAN_BACKEND_EMIT_SOURCE 0x1u     /* emit(kind = SOURCE) */
#define PAYKAN_BACKEND_EMIT_OBJECT 0x2u     /* emit(kind = OBJECT) */
#define PAYKAN_BACKEND_EMIT_EXECUTABLE 0x4u /* emit(kind = EXECUTABLE) */
#define PAYKAN_BACKEND_RUN 0x8u             /* run() */

/* PaykanBackend.emit kinds. */
#define PAYKAN_EMIT_SOURCE 0u     /* write the output with host->write_output */
#define PAYKAN_EMIT_OBJECT 1u     /* write an object file to output_path */
#define PAYKAN_EMIT_EXECUTABLE 2u /* write an executable to output_path */

/* -- The host: what paykan offers a plugin --------------------------------- */

/* One callback invocation: paykan creates a session for every call into a
 * plugin callback and passes it in; it is valid only during that call.  Its
 * contents are paykan's (opaque to the plugin). */
typedef struct PaykanSession PaykanSession;

typedef struct PaykanHost {
  uint32_t struct_size;
  /* PAYKAN_PLUGIN_API_VERSION of the running paykan. */
  uint32_t api_version;
  /* The running paykan's version ("0.1.0-alpha"). */
  const char *toolchain_version;

  /* Report a diagnostic.  file may be NULL for the input file of the
   * session; line and column are 1-based, 0 when unknown.  An error makes
   * the callback's failure message; warnings and notes are printed at
   * once. */
  void (*diagnostic)(PaykanSession *session, uint32_t level, const char *file,
                     uint32_t line, uint32_t column, const char *message,
                     size_t message_size);

  /* Append bytes to the session's output: the stream emit(SOURCE) writes
   * to, paykan's standard output in run(), the token listing in a
   * frontend's dump_tokens(), and standard error in a frontend's parse()
   * (for debug traces).  Returns PAYKAN_OK. */
  int (*write_output)(PaykanSession *session, const void *data, size_t size);

  /* An allocator for the plugin's own use (the C library's malloc/free).
   * Memory the plugin RETURNS to paykan is not allocated here; it is
   * released with PaykanPlugin.free_memory. */
  void *(*allocate)(size_t size);
  void (*deallocate)(void *ptr);

  /* A message on paykan's standard error, prefixed with "paykan: plugin: ".
   * PAYKAN_LOG_DEBUG messages are dropped unless PAYKAN_PLUGIN_DEBUG is
   * set. */
  void (*log)(uint32_t level, const char *message, size_t message_size);

  /* The Paykan runtime a native program links against: the path of
   * libpaykan_runtime.a and the directory holding Runtime.h, found the way
   * the built-in backends find them.  NULL (with a diagnostic) when there is
   * none.  Valid for the session. */
  const char *(*runtime_library)(PaykanSession *session);
  const char *(*runtime_include_dir)(PaykanSession *session);

  /* Link inputs (object files, or C sources: they are compiled with the
   * runtime's include directory on the path) against the runtime into the
   * executable output_path, with the system C compiler ($CC, else cc) as
   * the linker driver.  Returns PAYKAN_OK, or PAYKAN_ERROR with a
   * diagnostic. */
  int (*link_executable)(PaykanSession *session, const char *const *inputs,
                         size_t num_inputs, const char *output_path);

  /* Run the executable path with argv = args[0..num_args) (args[0] becomes
   * its argv[0]; num_args == 0 runs it with argc == 0) and wait for it.
   * track_heap != 0 switches on the runtime's heap statistics
   * (--track-heap).  Stores the exit status (128 + the signal number when
   * it was killed) in *exit_code.  Returns PAYKAN_OK, or PAYKAN_ERROR with a
   * diagnostic when it could not be run. */
  int (*run_executable)(PaykanSession *session, const char *path,
                        const char *const *args, size_t num_args,
                        uint32_t track_heap, int *exit_code);

  /* A fresh private directory for the session's scratch files, removed with
   * its contents when the callback returns.  NULL (with a diagnostic) when
   * it cannot be created. */
  const char *(*temp_dir)(PaykanSession *session);
} PaykanHost;

/* -- Backends -------------------------------------------------------------- */

/* The program a backend works on. */
typedef struct PaykanBackendInput {
  uint32_t struct_size;
  /* The main source file, as given on the command line. */
  const char *input_filename;
  /* The directory imports were resolved against ("" for the current one). */
  const char *project_root;
  /* The verified program as PIR text (docs/pir.md, version
   * pir_text_version), main module first; NUL-terminated at pir[pir_size]. */
  const char *pir;
  size_t pir_size;
  uint32_t pir_text_version;
  /* -O<n>, 0..3. */
  uint32_t opt_level;
} PaykanBackendInput;

/* What run() is asked to do. */
typedef struct PaykanRunRequest {
  uint32_t struct_size;
  /* The program's arguments; args[0] is the script path. */
  const char *const *args;
  size_t num_args;
  /* --track-heap: pass it on to run_executable. */
  uint32_t track_heap;
} PaykanRunRequest;

typedef struct PaykanBackend {
  uint32_t struct_size;
  /* The name users select it by (--backend=<name>). */
  const char *name;
  /* One line for --list-backends and --version; may be NULL. */
  const char *description;
  /* PAYKAN_BACKEND_* bits.  paykan refuses a request the backend does not
   * support before calling it. */
  uint32_t capabilities;
  /* The extension of the source output (".pir", ".c"); may be NULL. */
  const char *source_extension;
  /* Passed back as the first argument of every callback. */
  void *data;
  /* Translate the program.  kind is a PAYKAN_EMIT_* value: SOURCE output
   * goes through host->write_output, OBJECT and EXECUTABLE output to
   * output_path.  Returns PAYKAN_OK or an error code. */
  int (*emit)(void *data, PaykanSession *session,
              const PaykanBackendInput *input, uint32_t kind,
              const char *output_path);
  /* Execute the program and store its exit code in *exit_code.  Only called
   * when PAYKAN_BACKEND_RUN is set; may be NULL otherwise.  Returns
   * PAYKAN_OK, or an error code when the program could not be run. */
  int (*run)(void *data, PaykanSession *session,
             const PaykanBackendInput *input, const PaykanRunRequest *request,
             int *exit_code);
} PaykanBackend;

/* -- Frontends ------------------------------------------------------------- */

/* The file a frontend parses. */
typedef struct PaykanFrontendInput {
  uint32_t struct_size;
  /* The file's name, for locations and diagnostics; the frontend does not
   * read the file. */
  const char *filename;
  /* The file's text; NUL-terminated at source[source_size]. */
  const char *source;
  size_t source_size;
  /* PAYKAN_AST_FORMAT_VERSION of the AST paykan reads. */
  uint32_t ast_format_version;
  /* The deepest nesting a frontend accepts: input nested deeper is rejected
   * with "nesting too deep (more than <max_nesting> levels)"
   * (docs/grammar.md section 9). */
  uint32_t max_nesting;
  /* --trace-parser / --trace-scanner, for a frontend with debug traces
   * (written with host->write_output). */
  uint32_t trace_parsing;
  uint32_t trace_scanning;
} PaykanFrontendInput;

/* What parse() returns.  paykan allocates it and sets struct_size (the
 * plugin writes only the fields it knows); the plugin fills the rest. */
typedef struct PaykanFrontendOutput {
  uint32_t struct_size;
  /* The program in the AST interchange format: allocated by the plugin,
   * released by paykan with PaykanPlugin.free_memory once it has read it.
   * May be NULL when error_count > 0. */
  char *ast;
  size_t ast_size;
  /* The number of syntax errors, each reported with host->diagnostic.  The
   * parse succeeded iff it returns PAYKAN_OK with error_count == 0 (and then
   * ast is required); after errors the AST is not used. */
  uint32_t error_count;
} PaykanFrontendOutput;

typedef struct PaykanFrontend {
  uint32_t struct_size;
  /* The name users select it by (--frontend=<name>). */
  const char *name;
  /* One line for --list-frontends and --version; may be NULL. */
  const char *description;
  /* Passed back as the first argument of every callback. */
  void *data;
  /* Parse input->source into output (see PaykanFrontendOutput).  Returns
   * PAYKAN_OK, or an error code when the frontend failed (with the reason
   * reported as a diagnostic). */
  int (*parse)(void *data, PaykanSession *session,
               const PaykanFrontendInput *input, PaykanFrontendOutput *output);
  /* --dump-tokens: write the token stream of input->source with
   * host->write_output, one token per line.  May be NULL (not supported).
   * Returns PAYKAN_OK or an error code. */
  int (*dump_tokens)(void *data, PaykanSession *session,
                     const PaykanFrontendInput *input);
} PaykanFrontend;

/* -- The plugin: what paykan_plugin_init returns --------------------------- */

typedef struct PaykanPlugin {
  uint32_t struct_size;
  /* PAYKAN_PLUGIN_API_VERSION of the header the plugin was built with. */
  uint32_t api_version;
  /* PAYKAN_PLUGIN_BUILD_VERSION: the PaykanLang version the plugin is built
   * with, checked against paykan's compatibility list (#103). */
  const char *build_version;
  /* The plugin's own name and version, for listings; may be NULL. */
  const char *name;
  const char *version;
  /* Releases memory the plugin returned to paykan (a frontend's AST);
   * required with frontends, may be NULL otherwise. */
  void (*free_memory)(void *ptr);
  /* The backends it provides: an array of num_backends PaykanBackend, all
   * with the same struct_size, which paykan steps through the array by. */
  size_t num_backends;
  const PaykanBackend *backends;
  /* The frontends it provides, likewise.  These two fields were appended
   * after the backends: a descriptor whose struct_size ends before them has
   * no frontends.  A plugin provides at least one backend or frontend. */
  size_t num_frontends;
  const PaykanFrontend *frontends;
} PaykanPlugin;

/* The entry point's type. */
typedef const PaykanPlugin *(*PaykanPluginInitFn)(const PaykanHost *host);

/* Marks the entry point for export from the shared library. */
#if defined(_WIN32)
#define PAYKAN_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define PAYKAN_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define PAYKAN_PLUGIN_EXPORT
#endif

/* The entry point every plugin defines (with PAYKAN_PLUGIN_EXPORT). */
PAYKAN_PLUGIN_EXPORT const PaykanPlugin *
paykan_plugin_init(const PaykanHost *host);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PAYKAN_PLUGIN_API_H */

/* Copyright (c) 2026 Parsa Bagheri
 * SPDX-License-Identifier: MIT
 *
 * A configurable test plugin for the plugin loader tests
 * (tests/Plugin/LoaderTests.cpp), built once per variant by
 * tests/CMakeLists.txt with these definitions:
 *
 *   TP_NAME            the backend's name (default "test-good")
 *   TP_BUILD_VERSION   the build version it declares (default: this header's)
 *   TP_API_VERSION     the plugin API version it declares (default: this one)
 *   TP_NULL_DESCRIPTOR paykan_plugin_init returns NULL
 *   TP_NO_ENTRY_POINT  no paykan_plugin_init at all
 *   TP_NO_EMIT         the backend has no emit callback (invalid)
 *   TP_NO_RUN          it can run programs but has no run callback (invalid)
 *   TP_NO_NAME         the backend has no name (invalid)
 *   TP_TWICE           it lists the backend twice (invalid)
 *   TP_PLUGIN_SIZE, TP_BACKEND_SIZE, TP_NUM_BACKENDS
 *                      override the descriptor's struct sizes / count
 *   TP_FUTURE          two backends ("test-future-1", "-2") in structs a
 *                      newer header of the same API would have: a field
 *                      appended, so the array's stride is larger
 *
 * Every variant has a global constructor that creates the file
 * <$PAYKAN_TEST_MARKER_DIR>/<TP_NAME>.ctor, and every callback creates
 * <...>/<TP_NAME>.called before doing anything: the tests check which plugin
 * code ran.  A plugin paykan rejects must never get its callbacks called.
 *
 * The full backend (the default variant) exercises the whole host table:
 * emit(SOURCE) writes the PIR; emit(EXECUTABLE) and run() write a small C
 * program that prints its arguments and $PAYKAN_TRACK_HEAP and exits with
 * argc, and link it with the host's link_executable (run() then runs it with
 * run_executable).  $PAYKAN_TEST_PLUGIN_FAIL makes emit fail: "diag" with an
 * error diagnostic (and a warning first), anything else silently (status 5).
 */

#include "paykan/plugin_api.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TP_NAME
#define TP_NAME "test-good"
#endif
#ifndef TP_BUILD_VERSION
#define TP_BUILD_VERSION PAYKAN_PLUGIN_BUILD_VERSION
#endif
#ifndef TP_API_VERSION
#define TP_API_VERSION PAYKAN_PLUGIN_API_VERSION
#endif

static const PaykanHost *host;

/* Create <$PAYKAN_TEST_MARKER_DIR>/<TP_NAME><suffix>, if the variable is
 * set. */
static void mark(const char *suffix) {
  const char *dir = getenv("PAYKAN_TEST_MARKER_DIR");
  char path[4096];
  FILE *f;
  if (!dir || !*dir)
    return;
  snprintf(path, sizeof(path), "%s/%s%s", dir, TP_NAME, suffix);
  f = fopen(path, "w");
  if (f)
    fclose(f);
}

__attribute__((constructor)) static void test_plugin_constructor(void) {
  mark(".ctor");
}

static void say(PaykanSession *s, uint32_t level, const char *file,
                uint32_t line, uint32_t column, const char *msg) {
  host->diagnostic(s, level, file, line, column, msg, strlen(msg));
}

/* Write the test program's C source into the session's scratch directory;
 * returns its path in buf, or NULL. */
static const char *write_program(PaykanSession *s, char *buf, size_t size) {
  static const char program[] =
      "#include <stdio.h>\n"
      "#include <stdlib.h>\n"
      "#include \"Runtime.h\"\n"
      "int main(int argc, char **argv) {\n"
      "  const char *track = getenv(\"PAYKAN_TRACK_HEAP\");\n"
      "  for (int i = 0; i < argc; ++i)\n"
      "    printf(\"arg %d: %s\\n\", i, argv[i]);\n"
      "  printf(\"track-heap: %s\\n\", track ? track : \"off\");\n"
      "  return argc;\n"
      "}\n";
  const char *dir = host->temp_dir(s);
  FILE *f;
  if (!dir)
    return NULL;
  snprintf(buf, size, "%s/program.c", dir);
  f = fopen(buf, "w");
  if (!f) {
    say(s, PAYKAN_DIAG_ERROR, NULL, 0, 0, "cannot write the test program");
    return NULL;
  }
  fputs(program, f);
  fclose(f);
  return buf;
}

__attribute__((unused)) static int test_emit(void *data, PaykanSession *s,
                                             const PaykanBackendInput *in,
                                             uint32_t kind,
                                             const char *output_path) {
  const char *fail = getenv("PAYKAN_TEST_PLUGIN_FAIL");
  char source[4096];
  const char *inputs[1];
  mark(".called");
  if (data != (void *)&host)
    abort(); /* the descriptor's data must come back */
  if (fail && *fail) {
    if (strcmp(fail, "diag") == 0) {
      say(s, PAYKAN_DIAG_WARNING, NULL, 2, 0, "a warning first");
      say(s, PAYKAN_DIAG_ERROR, NULL, 3, 7, "test failure requested");
      say(s, PAYKAN_DIAG_ERROR, "other.pkn", 0, 0, "and a second error");
      return PAYKAN_ERROR;
    }
    return 5;
  }
  host->log(PAYKAN_LOG_DEBUG, "debug from the test plugin", 26);
  if (kind == PAYKAN_EMIT_SOURCE) {
    void *copy = host->allocate(in->pir_size + 1);
    int rc;
    if (!copy)
      return PAYKAN_ERROR;
    memcpy(copy, in->pir, in->pir_size + 1); /* NUL-terminated */
    rc = host->write_output(s, copy, in->pir_size);
    host->deallocate(copy);
    return rc;
  }
  if (kind != PAYKAN_EMIT_EXECUTABLE || !host->runtime_library(s) ||
      !host->runtime_include_dir(s))
    return PAYKAN_ERROR;
  inputs[0] = write_program(s, source, sizeof(source));
  if (!inputs[0])
    return PAYKAN_ERROR;
  return host->link_executable(s, inputs, 1, output_path);
}

__attribute__((unused)) static int test_run(void *data, PaykanSession *s,
                                            const PaykanBackendInput *in,
                                            const PaykanRunRequest *req,
                                            int *exit_code) {
  char exe[4096];
  const char *dir;
  (void)data;
  mark(".called");
  dir = host->temp_dir(s);
  if (!dir)
    return PAYKAN_ERROR;
  snprintf(exe, sizeof(exe), "%s/program", dir);
  if (test_emit(&host, s, in, PAYKAN_EMIT_EXECUTABLE, exe) != PAYKAN_OK)
    return PAYKAN_ERROR;
  return host->run_executable(s, exe, req->args, req->num_args, req->track_heap,
                              exit_code);
}

#ifndef TP_PLUGIN_SIZE
#define TP_PLUGIN_SIZE sizeof(PaykanPlugin)
#endif
#ifndef TP_BACKEND_SIZE
#define TP_BACKEND_SIZE sizeof(PaykanBackend)
#endif
#ifdef TP_NO_NAME
#define TP_BACKEND_NAME NULL
#else
#define TP_BACKEND_NAME TP_NAME
#endif
#ifdef TP_NO_RUN
#define TP_RUN NULL
#else
#define TP_RUN test_run
#endif

#define TP_BACKEND                                                             \
  {                                                                            \
    .struct_size = TP_BACKEND_SIZE, .name = TP_BACKEND_NAME,                   \
    .description = "the loader tests' backend",                                \
    .capabilities = PAYKAN_BACKEND_EMIT_SOURCE |                               \
                    PAYKAN_BACKEND_EMIT_EXECUTABLE | PAYKAN_BACKEND_RUN,       \
    .source_extension = ".pir", .data = (void *)&host, .emit = TP_EMIT,        \
    .run = TP_RUN,                                                             \
  }
#ifdef TP_NO_EMIT
#define TP_EMIT NULL
#else
#define TP_EMIT test_emit
#endif

__attribute__((unused)) static const PaykanBackend backends[] = {
    TP_BACKEND,
#ifdef TP_TWICE
    TP_BACKEND,
#endif
};

#ifdef TP_FUTURE
typedef struct FutureBackend {
  PaykanBackend Base;
  uint64_t Appended; /* a field a newer header appends */
} FutureBackend;
#define TP_FUTURE_BACKEND(n)                                                   \
  {                                                                            \
    {.struct_size = sizeof(FutureBackend),                                     \
     .name = (n),                                                              \
     .description = "a newer header's backend",                                \
     .capabilities = PAYKAN_BACKEND_EMIT_SOURCE,                               \
     .data = (void *)&host,                                                    \
     .emit = test_emit},                                                       \
        42                                                                     \
  }
static const FutureBackend future_backends[] = {
    TP_FUTURE_BACKEND("test-future-1"), TP_FUTURE_BACKEND("test-future-2")};
#define TP_BACKENDS (&future_backends[0].Base)
#define TP_NUM_BACKENDS 2
#else
#define TP_BACKENDS backends
#endif

#ifndef TP_NUM_BACKENDS
#define TP_NUM_BACKENDS (sizeof(backends) / sizeof(backends[0]))
#endif

static const PaykanPlugin plugin = {
    .struct_size = TP_PLUGIN_SIZE,
    .api_version = TP_API_VERSION,
    .build_version = TP_BUILD_VERSION,
    .name = TP_NAME "-plugin",
    .version = "0.1",
    .free_memory = NULL,
    .num_backends = TP_NUM_BACKENDS,
    .backends = TP_BACKENDS,
};

#ifndef TP_NO_ENTRY_POINT
PAYKAN_PLUGIN_EXPORT const PaykanPlugin *
paykan_plugin_init(const PaykanHost *h) {
  host = h;
#ifdef TP_NULL_DESCRIPTOR
  (void)plugin;
  return NULL;
#else
  return &plugin;
#endif
}
#else
/* Something exported, so it is a valid library without the entry point. */
PAYKAN_PLUGIN_EXPORT int not_a_paykan_plugin(void) {
  return plugin.num_backends == 1 ? 0 : 1;
}
#endif

/* Copyright (c) 2026 Parsa Bagheri
 * SPDX-License-Identifier: MIT
 *
 * A configurable test frontend plugin for tests/Plugin/LoaderTests.cpp,
 * built once per variant by tests/CMakeLists.txt:
 *
 *   TP_NAME            the frontend's name (default "test-fe")
 *   TP_BUILD_VERSION   the build version it declares (default: this header's)
 *   TP_NO_FREE         no free_memory (invalid with frontends)
 *   TP_NO_PARSE        no parse callback (invalid)
 *
 * Its source language is the AST interchange format: parse returns the
 * source as the AST (like utils/ast-text-frontend), so the tests feed it
 * `paykan --emit-ast` output.  $PAYKAN_TEST_FE_MODE changes what parse does:
 *   errors      two syntax errors, a warning and a note (error_count 2)
 *   other-file  an error about another file
 *   bad-ast     returns text that is not a valid AST
 *   noast       returns PAYKAN_OK with neither an AST nor an error
 *   fail        returns 7 without a diagnostic
 *   trace       writes a trace line (to standard error) first
 * dump_tokens writes one line naming the file and its size, or with
 * PAYKAN_TEST_FE_MODE=fail returns 3.  Every callback creates
 * <$PAYKAN_TEST_MARKER_DIR>/<TP_NAME>.called, the global constructor
 * <...>/<TP_NAME>.ctor.
 */

#include "paykan/plugin_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TP_NAME
#define TP_NAME "test-fe"
#endif
#ifndef TP_BUILD_VERSION
#define TP_BUILD_VERSION PAYKAN_PLUGIN_BUILD_VERSION
#endif

static const PaykanHost *host;

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

__attribute__((constructor)) static void test_frontend_constructor(void) {
  mark(".ctor");
}

static int mode(const char *m) {
  const char *env = getenv("PAYKAN_TEST_FE_MODE");
  return env && strcmp(env, m) == 0;
}

static void say(PaykanSession *s, uint32_t level, const char *file,
                uint32_t line, uint32_t column, const char *msg) {
  host->diagnostic(s, level, file, line, column, msg, strlen(msg));
}

static char *copy(const char *text, size_t size) {
  char *buf = malloc(size + 1);
  if (buf) {
    memcpy(buf, text, size);
    buf[size] = '\0';
  }
  return buf;
}

__attribute__((unused)) static int test_parse(void *data, PaykanSession *s,
                                              const PaykanFrontendInput *in,
                                              PaykanFrontendOutput *out) {
  static const char bad[] = "(paykan-ast 1 (unit (frob)))";
  mark(".called");
  if (data != (void *)&host || out->struct_size < sizeof(*out) ||
      in->max_nesting != 512 ||
      in->ast_format_version != PAYKAN_AST_FORMAT_VERSION)
    abort();
  if (mode("errors")) {
    say(s, PAYKAN_DIAG_ERROR, NULL, 1, 5, "unexpected thing");
    say(s, PAYKAN_DIAG_WARNING, NULL, 1, 1, "a warning");
    say(s, PAYKAN_DIAG_NOTE, NULL, 1, 1, "a note");
    say(s, PAYKAN_DIAG_ERROR, in->filename, 2, 0, "and another");
    out->error_count = 2;
    out->ast = copy(bad, strlen(bad)); /* released, never read */
    out->ast_size = strlen(bad);
    return PAYKAN_OK;
  }
  if (mode("other-file")) {
    say(s, PAYKAN_DIAG_ERROR, "other.pkn", 3, 4, "in another file");
    out->error_count = 1;
    return PAYKAN_ERROR;
  }
  if (mode("fail"))
    return 7;
  if (mode("noast"))
    return PAYKAN_OK;
  if (mode("trace"))
    host->write_output(s, "trace: parsing\n", 15);
  if (mode("bad-ast")) {
    out->ast = copy(bad, strlen(bad));
    out->ast_size = strlen(bad);
    return PAYKAN_OK;
  }
  out->ast = copy(in->source, in->source_size);
  out->ast_size = in->source_size;
  out->error_count = 0;
  return out->ast ? PAYKAN_OK : PAYKAN_ERROR;
}

static int test_dump_tokens(void *data, PaykanSession *s,
                            const PaykanFrontendInput *in) {
  char line[4200];
  int n;
  (void)data;
  mark(".called");
  if (mode("fail"))
    return 3;
  n = snprintf(line, sizeof(line), "tokens of %s: %lu bytes\n", in->filename,
               (unsigned long)in->source_size);
  return host->write_output(s, line, (size_t)n);
}

__attribute__((unused)) static void test_free(void *ptr) { free(ptr); }

static const PaykanFrontend frontends[] = {{
    .struct_size = sizeof(PaykanFrontend),
    .name = TP_NAME,
    .description = "the loader tests' frontend",
    .data = (void *)&host,
#ifdef TP_NO_PARSE
    .parse = NULL,
#else
    .parse = test_parse,
#endif
    .dump_tokens = test_dump_tokens,
}};

static const PaykanPlugin plugin = {
    .struct_size = sizeof(PaykanPlugin),
    .api_version = PAYKAN_PLUGIN_API_VERSION,
    .build_version = TP_BUILD_VERSION,
    .name = TP_NAME "-plugin",
    .version = "0.1",
#ifdef TP_NO_FREE
    .free_memory = NULL,
#else
    .free_memory = test_free,
#endif
    .num_backends = 0,
    .backends = NULL,
    .num_frontends = 1,
    .frontends = frontends,
};

PAYKAN_PLUGIN_EXPORT const PaykanPlugin *
paykan_plugin_init(const PaykanHost *h) {
  host = h;
  return &plugin;
}

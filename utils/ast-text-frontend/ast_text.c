/* Copyright (c) 2026 Parsa Bagheri
 * SPDX-License-Identifier: MIT
 *
 * The `ast-text` frontend: the smallest possible frontend, as a template
 * for writing your own (docs/writing-a-frontend-plugin.md).  Plain C11
 * against paykan/plugin_api.h only.  Its source language is the AST
 * interchange format itself (docs/plugins/ast-format.md): it hands the file
 * to paykan unchanged, and paykan reads and checks it like any frontend's
 * output.  So it runs a program another tool wrote as an AST:
 *
 *   paykan --emit-ast program.pkn > program.ast
 *   paykan --plugin=libpaykan_frontend_ast_text.so --frontend=ast-text \
 *       program.ast
 *
 * and it checks a frontend's output by hand.  A real frontend parses
 * input->source, reports each syntax error with host->diagnostic, and
 * writes the AST it built in the same format.
 */

#include "paykan/plugin_api.h"

#include <stdlib.h>
#include <string.h>

static const PaykanHost *host;

static void error(PaykanSession *session, const char *msg) {
  host->diagnostic(session, PAYKAN_DIAG_ERROR, NULL, 1, 1, msg, strlen(msg));
}

/* parse: the AST is the source, copied into memory paykan releases with
 * free_memory (below) once it has read it. */
static int ast_text_parse(void *data, PaykanSession *session,
                          const PaykanFrontendInput *input,
                          PaykanFrontendOutput *output) {
  char *copy;
  (void)data;
  if (input->ast_format_version != PAYKAN_AST_FORMAT_VERSION) {
    error(session, "ast-text reads AST format 1 only");
    output->error_count = 1;
    return PAYKAN_OK;
  }
  copy = malloc(input->source_size + 1);
  if (!copy) {
    error(session, "out of memory");
    output->error_count = 1;
    return PAYKAN_ERROR;
  }
  memcpy(copy, input->source, input->source_size + 1); /* NUL-terminated */
  output->ast = copy;
  output->ast_size = input->source_size;
  output->error_count = 0;
  return PAYKAN_OK;
}

/* Releases what parse returned. */
static void ast_text_free(void *ptr) { free(ptr); }

static const PaykanFrontend frontends[] = {{
    .struct_size = sizeof(PaykanFrontend),
    .name = "ast-text", /* --frontend=ast-text */
    .description = "reads the AST interchange format",
    .data = NULL,
    .parse = ast_text_parse,
    .dump_tokens = NULL, /* --dump-tokens: not supported */
}};

static const PaykanPlugin plugin = {
    .struct_size = sizeof(PaykanPlugin),
    .api_version = PAYKAN_PLUGIN_API_VERSION,
    .build_version = PAYKAN_PLUGIN_BUILD_VERSION,
    .name = "ast-text",
    .version = "1.0",
    .free_memory = ast_text_free,
    .num_backends = 0,
    .backends = NULL,
    .num_frontends = sizeof(frontends) / sizeof(frontends[0]),
    .frontends = frontends,
};

PAYKAN_PLUGIN_EXPORT const PaykanPlugin *
paykan_plugin_init(const PaykanHost *h) {
  host = h;
  return &plugin;
}

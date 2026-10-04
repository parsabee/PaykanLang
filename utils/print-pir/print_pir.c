/* Copyright (c) 2026 Parsa Bagheri
 * SPDX-License-Identifier: MIT
 *
 * The `print-pir` backend: the smallest possible backend, as a template for
 * writing your own (docs/writing-a-backend.md).  Plain C11 against
 * paykan/plugin_api.h only: it links nothing of PaykanLang, and the
 * installed `paykan` loads it at run time.  Its "source output" is the PIR
 * text of the program, so
 *
 *   paykan --plugin=libpaykan_backend_print_pir.so --backend=print-pir \
 *       --emit-source program.pkn
 *
 * prints what every backend receives after the lowering and the verifier
 * (the same text as `paykan --emit-pir`).
 */

#include "paykan/plugin_api.h"

#include <string.h>

/* The host table paykan_plugin_init was given: how the backend talks back
 * to paykan (output, diagnostics, the toolchain helpers). */
static const PaykanHost *host;

/* The program arrives as PIR text (docs/pir.md): every module, main module
 * first, lowered and verified.  A real backend parses it and translates
 * each instruction one-to-one; this one writes it out unchanged. */
static int print_pir_emit(void *data, PaykanSession *session,
                          const PaykanBackendInput *input, uint32_t kind,
                          const char *output_path) {
  static const char only_source[] = "print-pir only emits source";
  (void)data;
  (void)output_path;
  if (kind != PAYKAN_EMIT_SOURCE) {
    /* paykan refuses requests outside the capabilities before calling, so
     * this is a safety net. */
    host->diagnostic(session, PAYKAN_DIAG_ERROR, NULL, 0, 0, only_source,
                     strlen(only_source));
    return PAYKAN_ERROR;
  }
  return host->write_output(session, input->pir, input->pir_size);
}

/* What paykan lists and may ask for: only "source"; `paykan build` and
 * `paykan run` are refused before reaching it, so run is NULL. */
static const PaykanBackend backends[] = {{
    .struct_size = sizeof(PaykanBackend),
    .name = "print-pir",                   /* --backend=print-pir */
    .description = "prints the Paykan IR", /* --list-backends, --version */
    .capabilities = PAYKAN_BACKEND_EMIT_SOURCE,
    .source_extension = ".pir",
    .data = NULL, /* passed back to the callbacks */
    .emit = print_pir_emit,
    .run = NULL,
}};

static const PaykanPlugin plugin = {
    .struct_size = sizeof(PaykanPlugin),
    .api_version = PAYKAN_PLUGIN_API_VERSION,
    /* Checked against paykan's compatibility list (#103). */
    .build_version = PAYKAN_PLUGIN_BUILD_VERSION,
    .name = "print-pir",
    .version = "1.0",
    .free_memory = NULL, /* it returns no memory to paykan */
    .num_backends = sizeof(backends) / sizeof(backends[0]),
    .backends = backends,
};

/* The one entry point.  paykan checks the descriptor before calling
 * anything else, so this only records the host table and returns it. */
PAYKAN_PLUGIN_EXPORT const PaykanPlugin *
paykan_plugin_init(const PaykanHost *h) {
  host = h;
  return &plugin;
}

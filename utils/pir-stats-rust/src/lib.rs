// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// `pir-stats`: a PaykanLang backend written in Rust, as a `cdylib` against
// the C plugin interface (include/paykan/plugin_api.h,
// docs/plugins/plugin-api.md).  Its "source output" is a summary of the
// program's PIR: how many modules, functions, extern functions and
// instructions it has.
//
//     paykan --plugin=libpaykan_backend_pir_stats.so --backend=pir-stats \
//         --emit-source program.pkn
//
// The structs below mirror plugin_api.h field for field (#[repr(C)]); no
// crate is needed.  Two rules of the interface matter most in Rust:
//   - nothing may unwind into paykan: every callback runs its body in
//     std::panic::catch_unwind and turns a panic into PAYKAN_ERROR;
//   - everything paykan passes in is borrowed for the call only.

use std::ffi::{c_char, c_int, c_void, CStr};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicPtr, Ordering};

// -- plugin_api.h -------------------------------------------------------------

const PAYKAN_PLUGIN_API_VERSION: u32 = 1;
const PAYKAN_OK: c_int = 0;
const PAYKAN_ERROR: c_int = 1;
const PAYKAN_DIAG_ERROR: u32 = 0;
const PAYKAN_BACKEND_EMIT_SOURCE: u32 = 0x1;
const PAYKAN_EMIT_SOURCE: u32 = 0;

/// Opaque: one callback invocation (PaykanSession).
#[repr(C)]
pub struct PaykanSession {
    _private: [u8; 0],
}

#[repr(C)]
pub struct PaykanHost {
    struct_size: u32,
    api_version: u32,
    toolchain_version: *const c_char,
    diagnostic: unsafe extern "C" fn(
        *mut PaykanSession,
        u32,
        *const c_char,
        u32,
        u32,
        *const c_char,
        usize,
    ),
    write_output: unsafe extern "C" fn(*mut PaykanSession, *const c_void, usize) -> c_int,
    allocate: unsafe extern "C" fn(usize) -> *mut c_void,
    deallocate: unsafe extern "C" fn(*mut c_void),
    log: unsafe extern "C" fn(u32, *const c_char, usize),
    runtime_library: unsafe extern "C" fn(*mut PaykanSession) -> *const c_char,
    runtime_include_dir: unsafe extern "C" fn(*mut PaykanSession) -> *const c_char,
    link_executable: unsafe extern "C" fn(
        *mut PaykanSession,
        *const *const c_char,
        usize,
        *const c_char,
    ) -> c_int,
    run_executable: unsafe extern "C" fn(
        *mut PaykanSession,
        *const c_char,
        *const *const c_char,
        usize,
        u32,
        *mut c_int,
    ) -> c_int,
    temp_dir: unsafe extern "C" fn(*mut PaykanSession) -> *const c_char,
}

#[repr(C)]
pub struct PaykanBackendInput {
    struct_size: u32,
    input_filename: *const c_char,
    project_root: *const c_char,
    pir: *const c_char,
    pir_size: usize,
    pir_text_version: u32,
    opt_level: u32,
}

#[repr(C)]
pub struct PaykanRunRequest {
    struct_size: u32,
    args: *const *const c_char,
    num_args: usize,
    track_heap: u32,
}

type EmitFn = unsafe extern "C" fn(
    *mut c_void,
    *mut PaykanSession,
    *const PaykanBackendInput,
    u32,
    *const c_char,
) -> c_int;
type RunFn = unsafe extern "C" fn(
    *mut c_void,
    *mut PaykanSession,
    *const PaykanBackendInput,
    *const PaykanRunRequest,
    *mut c_int,
) -> c_int;

#[repr(C)]
pub struct PaykanBackend {
    struct_size: u32,
    name: *const c_char,
    description: *const c_char,
    capabilities: u32,
    source_extension: *const c_char,
    data: *mut c_void,
    emit: Option<EmitFn>,
    run: Option<RunFn>,
}

#[repr(C)]
pub struct PaykanPlugin {
    struct_size: u32,
    api_version: u32,
    build_version: *const c_char,
    name: *const c_char,
    version: *const c_char,
    free_memory: Option<unsafe extern "C" fn(*mut c_void)>,
    num_backends: usize,
    backends: *const PaykanBackend,
}

// The descriptors are immutable statics that hold raw pointers; paykan only
// reads them.
unsafe impl Sync for PaykanBackend {}
unsafe impl Sync for PaykanPlugin {}

// -- The backend --------------------------------------------------------------

/// The PaykanLang version this plugin is built with (#103): set by the build
/// (utils/pir-stats-rust/CMakeLists.txt), else the version it was written
/// for.
const BUILD_VERSION_STR: &str = match option_env!("PAYKAN_PLUGIN_BUILD_VERSION") {
    Some(v) => v,
    None => "0.1.0-alpha",
};

/// @p s as a NUL-terminated C string, at compile time.
const fn c_string<const N: usize>(s: &str) -> [u8; N] {
    let bytes = s.as_bytes();
    assert!(bytes.len() < N, "the build version is too long");
    let mut out = [0u8; N];
    let mut i = 0;
    while i < bytes.len() {
        out[i] = bytes[i];
        i += 1;
    }
    out
}
static BUILD_VERSION: [u8; 64] = c_string(BUILD_VERSION_STR);

static HOST: AtomicPtr<PaykanHost> = AtomicPtr::new(std::ptr::null_mut());

fn host() -> &'static PaykanHost {
    // Set by paykan_plugin_init before any callback can run.
    unsafe { &*HOST.load(Ordering::Acquire) }
}

/// The summary of a PIR text (docs/pir.md): top-level lines start with their
/// keyword; a function body is the indented lines up to the closing "}".
fn stats(pir: &str) -> String {
    let (mut modules, mut functions, mut externs, mut instructions) = (0, 0, 0, 0);
    let mut in_body = false;
    for line in pir.lines() {
        if in_body {
            if line == "}" {
                in_body = false;
            } else if !line.trim().is_empty() && !line.trim_start().starts_with('}') {
                instructions += 1;
            }
            continue;
        }
        if line.starts_with("module ") {
            modules += 1;
        } else if line.starts_with("extern fn ") {
            externs += 1;
        } else if line.starts_with("fn ") {
            functions += 1;
            in_body = line.ends_with('{');
        }
    }
    format!(
        "modules: {modules}\nfunctions: {functions}\nextern functions: {externs}\n\
         instructions: {instructions}\n"
    )
}

fn error(session: *mut PaykanSession, msg: &str) {
    unsafe {
        (host().diagnostic)(
            session,
            PAYKAN_DIAG_ERROR,
            std::ptr::null(),
            0,
            0,
            msg.as_ptr() as *const c_char,
            msg.len(),
        )
    };
}

unsafe extern "C" fn emit(
    _data: *mut c_void,
    session: *mut PaykanSession,
    input: *const PaykanBackendInput,
    kind: u32,
    _output_path: *const c_char,
) -> c_int {
    // No panic may cross into paykan.
    let result = catch_unwind(AssertUnwindSafe(|| {
        if kind != PAYKAN_EMIT_SOURCE {
            error(session, "pir-stats only emits source");
            return PAYKAN_ERROR;
        }
        let input = unsafe { &*input };
        let bytes = unsafe { std::slice::from_raw_parts(input.pir as *const u8, input.pir_size) };
        let Ok(pir) = std::str::from_utf8(bytes) else {
            error(session, "the PIR text is not UTF-8");
            return PAYKAN_ERROR;
        };
        let name = unsafe { CStr::from_ptr(input.input_filename) }.to_string_lossy();
        let out = format!("pir-stats for {name}\n{}", stats(pir));
        let rc = unsafe { (host().write_output)(session, out.as_ptr() as *const c_void, out.len()) };
        if rc == PAYKAN_OK {
            PAYKAN_OK
        } else {
            PAYKAN_ERROR
        }
    }));
    result.unwrap_or(PAYKAN_ERROR)
}

static BACKENDS: [PaykanBackend; 1] = [PaykanBackend {
    struct_size: std::mem::size_of::<PaykanBackend>() as u32,
    name: c"pir-stats".as_ptr(),
    description: c"counts the functions and instructions of the Paykan IR (Rust)".as_ptr(),
    capabilities: PAYKAN_BACKEND_EMIT_SOURCE,
    source_extension: c".txt".as_ptr(),
    data: std::ptr::null_mut(),
    emit: Some(emit),
    run: None,
}];

static PLUGIN: PaykanPlugin = PaykanPlugin {
    struct_size: std::mem::size_of::<PaykanPlugin>() as u32,
    api_version: PAYKAN_PLUGIN_API_VERSION,
    build_version: BUILD_VERSION.as_ptr() as *const c_char,
    name: c"pir-stats".as_ptr(),
    version: c"1.0".as_ptr(),
    free_memory: None,
    num_backends: 1,
    backends: BACKENDS.as_ptr(),
};

/// The entry point: record the host table and return the descriptor.
///
/// # Safety
/// Called by paykan with a host table that outlives the process's use of
/// the plugin.
#[no_mangle]
pub unsafe extern "C" fn paykan_plugin_init(host: *const PaykanHost) -> *const PaykanPlugin {
    HOST.store(host as *mut PaykanHost, Ordering::Release);
    &PLUGIN
}

#[cfg(test)]
mod tests {
    #[test]
    fn counts() {
        let pir = "module \"m\"\nextern fn @f() -> void\n\nfn @main() -> i64 {\n  ret 0\n}\n";
        assert_eq!(
            super::stats(pir),
            "modules: 1\nfunctions: 1\nextern functions: 1\ninstructions: 1\n"
        );
    }
}

# Plugins: overview

A **plugin** adds a backend to an installed `paykan` without rebuilding
PaykanLang. You have PaykanLang x.y.z installed; you write or download a
backend built for x.y.z; your system's `paykan` loads it at startup and
`--backend=<name>` selects it like a built-in one
([#141](https://github.com/parsabee/PaykanLang/issues/141)).

A plugin is a shared library (`.so` on Linux, `.dylib` on macOS) with a
**C interface**, [`include/paykan/plugin_api.h`](../../include/paykan/plugin_api.h).
It links nothing of PaykanLang: everything it needs comes through a table of
host functions that `paykan` hands it. So it can be written in any language
that can export a C function and lay out C structs: C, C++ (with any
compiler), Rust, Zig, Go with cgo, and so on.

| Page | For |
|---|---|
| this page | using plugins, and how `paykan` finds and checks them |
| [`plugin-api.md`](plugin-api.md) | the C interface, field by field |
| [`pir-for-backends.md`](pir-for-backends.md) | the program a backend receives (PIR text) |
| [`../writing-a-backend.md`](../writing-a-backend.md) | writing a backend, step by step |

The built-in plugins (the `recursive-descent` frontend, the `c` backend and
the opt-in in-tree `llvm` backend) are linked into `paykan` and keep their
in-process C++ interface. Frontends are not loadable yet.

## Using a plugin

```sh
# Try it: load one file.
paykan --plugin=./libpaykan_backend_print_pir.so --backend=print-pir --emit-source hello.pkn

# Install it for everyone using this paykan (the plugin's own build does this
# with `cmake --install`):
cp libpaykan_backend_print_pir.so "$(dirname "$(command -v paykan)")/../lib/paykan/plugins/0.1.0-alpha/"
paykan --backend=print-pir --emit-source hello.pkn

# Or just for you:
mkdir -p ~/.paykan/plugins/0.1.0-alpha
cp libpaykan_backend_print_pir.so ~/.paykan/plugins/0.1.0-alpha/
```

`paykan --list-backends` shows each loaded backend with its file, and
`paykan --version` prints the plugin directories searched and every plugin
file with what it provides. Include `paykan --version` in bug reports.

```text
$ paykan --list-backends
c (default)
print-pir: prints the Paykan IR [/usr/local/lib/paykan/plugins/0.1.0-alpha/libpaykan_backend_print_pir.so]
```

## Where `paykan` looks

At startup, in this order:

1. every `--plugin=<file>`, in command-line order (repeatable);
2. each directory in `$PAYKAN_PLUGIN_PATH` (`:`-separated);
3. the user directory `~/.paykan/plugins/<version>/`;
4. the system directory `<prefix>/lib/paykan/plugins/<version>/`, found
   relative to the running `paykan` executable (an installation can be moved).

`<version>` is the toolchain's version with its pre-release label
(`0.1.0-alpha`), so plugins for different installed versions never mix. In a
directory, every file whose name ends in the platform's suffix (`.so`,
`.dylib`) is loaded, in name order; subdirectories are not searched. A file
reached twice (the same directory listed twice, a symbolic link) is loaded
once.

**`--no-plugins`** or **`PAYKAN_NO_PLUGINS=1`** turns off steps 2 to 4, for
reproducible builds and debugging. Files named with `--plugin` are still
loaded, so `paykan --no-plugins --plugin=a.so` uses exactly the built-in
plugins plus `a.so`.

**Security.** Loading a plugin runs its native code with your privileges,
exactly like installing any program. `paykan` looks only in the four places
above: never in the current directory or the project being compiled (an
empty `$PAYKAN_PLUGIN_PATH` entry is skipped, it does not mean `.`).

## The check: nothing of a plugin runs before it passes

For every candidate file, `paykan`

1. loads the library with the platform's dynamic loader (`dlopen` with
   `RTLD_NOW | RTLD_LOCAL`);
2. looks up the one entry point, `paykan_plugin_init`, and calls it with the
   host table. The plugin returns its **descriptor**: the plugin API version
   it was built for, the PaykanLang version it was **built with**, and its
   backends;
3. checks the descriptor before calling anything else in the library:
   - the plugin API version must be one this `paykan` supports
     (`PAYKAN_PLUGIN_API_VERSION`; `paykan --version` prints it);
   - the descriptor must be well formed (sizes, names, callbacks);
   - the build version must be on this release's compatibility list
     ([#103](https://github.com/parsabee/PaykanLang/issues/103); exact match,
     pre-release label included);
4. registers each backend under its name, with the file it came from.

**The guarantee.** Before a plugin passes the check, `paykan` runs exactly
two things of it: what the platform's loader runs when it loads a library
(its global constructors, and those of the libraries it depends on), and
`paykan_plugin_init`. Neither may have side effects: the entry point only
returns the static descriptor, and a plugin must not have side-effecting
global constructors (in C++, no global object whose constructor does more
than initialise memory; in Rust, none of the `ctor`-style crates). No
backend callback of a plugin that fails the check is ever called. The tests
(`tests/Plugin/LoaderTests.cpp`) check this with a plugin whose constructor
leaves a marker file and whose callbacks would leave another: for a
rejected plugin, the first exists and the second never does.

### What happens to a plugin that fails

- **Wrong build version.** Its backends are registered as incompatible:
  listed with the reason, never callable. Selecting one exits with status 2:

  ```text
  $ paykan --list-backends
  c (default)
  mine (incompatible: built with PaykanLang 0.0.9; this paykan 0.1.0-alpha accepts 0.1.0-alpha) [/home/me/.paykan/plugins/0.1.0-alpha/libmine.so]
  $ paykan --backend=mine program.pkn
  paykan: cannot use backend 'mine' (incompatible: built with PaykanLang 0.0.9; this paykan 0.1.0-alpha accepts 0.1.0-alpha) [/home/me/.paykan/plugins/0.1.0-alpha/libmine.so]
  $ echo $?
  2
  ```

- **Rejected file** (it can't be loaded, has no `paykan_plugin_init`, returns
  no descriptor, targets another plugin API version, or its descriptor is
  malformed). Nothing of it is registered; the listings and `--version` end
  with one line per rejected file:

  ```text
  rejected plugin /home/me/.paykan/plugins/0.1.0-alpha/libold.so: built for plugin API 2; this paykan supports plugin API 1
  ```

  A rejected file named with `--plugin` stops a compile with status 2
  (`paykan: cannot load plugin '<file>': <why>`). One found in a directory
  doesn't; naming a backend it would have provided then fails as
  `unknown backend 'x' (see --list-backends; 1 plugin file was rejected)`.

- **Duplicate name.** If a name is already taken, by a built-in plugin or an
  earlier file, the name becomes **ambiguous**: it is listed with both files
  and selecting it exits with status 2 until one of them is removed (or
  `--no-plugins` is used):

  ```text
  $ paykan --backend=mine program.pkn
  paykan: cannot use backend 'mine' (ambiguous: provided by both /a/libmine.so and /b/libmine.so)
  ```

  A plugin that takes a built-in name (`c`) makes that name ambiguous too,
  including as the default backend.

## Compatibility and stability

- **The plugin API** (`plugin_api.h`, `PAYKAN_PLUGIN_API_VERSION`) is a
  versioned C contract. Within one API version, fields are only ever appended
  to the structs; every struct starts with `struct_size`, and the reader never
  looks past it. So a plugin built against an older header keeps loading into
  a newer `paykan` of the same API version. An incompatible change bumps the
  version.
- **The program text** a backend receives is PIR text, versioned separately
  (`PAYKAN_PIR_TEXT_VERSION`, see [`pir-for-backends.md`](pir-for-backends.md)).
- **The build version** list (#103) is the release's statement of which
  plugin builds it accepts; see
  [`writing-a-backend.md` §7](../writing-a-backend.md#7-plugin-compatibility).

While PaykanLang is below 1.0, any of these may change between minor
releases; the CHANGELOG says when.

## Platforms

Linux and macOS (`dlopen`; the dynamic loader is part of the C library on
both, so `paykan` still needs nothing beyond standard C++ and the platform's
loader). On macOS a plugin is a `.dylib` (`paykan_add_backend_plugin` names
it so; a CMake `MODULE` would otherwise be called `.so`) and needs no
`-undefined dynamic_lookup`: it references no symbol of `paykan`. Windows is
not supported yet: the portability layer
(`src/Backends/Toolchain/Platform.cpp`) has a stub that rejects every plugin
with a message; `LoadLibraryW` / `GetProcAddress` and `.dll` are the planned
implementation.

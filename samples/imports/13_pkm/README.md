# 13_pkm: a module served through a `.pkm` file

`main.pkn` imports `geometry::shapes`, a small library (a class hierarchy, an
enum and two functions).  Nothing in the two source files is about `.pkm`;
what this sample shows is how the import reaches the program.  Run every
command below from this directory.  The transcripts are those of a
development build: `<prefix>` is where `paykan` is installed and
`<version>` its version.

## 1. Run it: the cache is written

```text
$ paykan --verbose main.pkn
module ::error: prebuilt <prefix>/lib/paykan/stdlib/<version>/error.pkm usable
module ::result: prebuilt <prefix>/lib/paykan/stdlib/<version>/result.pkm usable
module ::io: prebuilt <prefix>/lib/paykan/stdlib/<version>/io.pkm usable
module geometry::shapes: no cache entry; building from source
module geometry::shapes: wrote .paykan_cache/geometry/shapes.pkm
rect of area 12
rect of area 25
37
flat
solid
```

The first run compiles `geometry/shapes.pkn` and writes its compiled form to
`.paykan_cache/geometry/shapes.pkm`: the module's interface (what the type
checker needs to check an importer), its code compiled by the backend
(LLVM bitcode, for the mlir backend) with the PIR that declares it, and a
manifest naming the toolchain that produced it and the source it was built
from.  The standard library modules `::io`, which `main.pkn` imports, and
`::error` and `::result`, which `::io` imports, are prebuilt `.pkm` files
installed with `paykan`, so they are never compiled here.  The second run
reuses the file:

```text
$ paykan --verbose main.pkn
module ::error: prebuilt <prefix>/lib/paykan/stdlib/<version>/error.pkm usable
module ::result: prebuilt <prefix>/lib/paykan/stdlib/<version>/result.pkm usable
module ::io: prebuilt <prefix>/lib/paykan/stdlib/<version>/io.pkm usable
module geometry::shapes: cache .paykan_cache/geometry/shapes.pkm usable
...
```

Edit `geometry/shapes.pkn` and the entry is rebuilt on the next run (an edit
to a function body rebuilds that module only; an edit to a signature also
rebuilds the modules that import it).  `--rebuild-modules` ignores the
entries, `--no-module-cache` neither reads nor writes them, and deleting
`.paykan_cache/` is always safe.

## 2. Look inside

```text
$ paykan pkm dump .paykan_cache/geometry/shapes.pkm
pkm 0.3.0, 5 sections, ...
...
sections:
  #0 MANIFEST flags REQUIRED ...
  #1 IFACE flags REQUIRED ...
  #2 CODE flags REQUIRED ...
  #3 SYMIDX flags - ...
  #4 OBJ flags REQUIRED ...
manifest:
  module geometry::shapes
  contents 0x41 (HAS_CODE,HAS_OBJECT)
  core "0.3.0" 0.3.0 prerelease "" build "..."
  format_versions iface 0.3.0 tmpl 0 pir 0.3.0 code_encoding 2 debug 0
  runtime_abi 0.3.0
  ...
iface:
  module geometry::shapes
  display_file "geometry/shapes.pkn"
  ...
  class Rect super "Shape" [local from "geometry::shapes"]
  ...
obj: ...
  backend "mlir"
  format "llvm-bc"
  ...
pir:
module "geometry::shapes"

extern class Shape module "geometry::shapes" {
...
```

The `core` line names the `paykan` that wrote the file: a release has no
pre-release label and no build ID (`build ""`).  `OBJ` holds the module's
code, compiled to LLVM bitcode when the file was written; its PIR only
declares that code (`extern`), since the module has no copies of generic
instantiations, which would stay PIR.
`--section=manifest|iface|code|...` prints one part; `--section=code` is
exactly the module's PIR text.  `paykan pkm check <file>` prints whether
this `paykan` can use the file and exits with 0 when it can.

## 3. Write the module file yourself

```text
$ paykan --emit-pkm geometry/shapes.pkn -o geometry/shapes.pkm
```

`--emit-pkm` compiles one module and writes its `.pkm`; the file is the
same bytes the cache holds (the writer is deterministic).  A module given
by a relative path inside the project is named by that path
(`geometry::shapes`), so the file is what an importer of it expects.

## 4. Run from the prebuilt module, without the source

```text
$ mv geometry/shapes.pkn /tmp/
$ rm -rf .paykan_cache
$ paykan --verbose main.pkn
module ::error: prebuilt <prefix>/lib/paykan/stdlib/<version>/error.pkm usable
module ::result: prebuilt <prefix>/lib/paykan/stdlib/<version>/result.pkm usable
module ::io: prebuilt <prefix>/lib/paykan/stdlib/<version>/io.pkm usable
module geometry::shapes: prebuilt geometry/shapes.pkm usable
rect of area 12
...
$ paykan build -o shapes main.pkn && ./shapes
rect of area 12
...
$ mv /tmp/shapes.pkn geometry/
```

Where no source is found, `paykan` looks for `<module>.pkm` where the source
would be, then under each `--module-path=<dir>` and `$PAYKAN_MODULE_PATH`
directory.  The file's code was compiled by the backend that wrote it (the
mlir backend's LLVM bitcode), which links it for `run` and `build` alike,
so only that backend can use the file; its interface and its PIR are
backend-neutral.  A prebuilt file written by another `paykan` release, or
by another development build, is rejected with a message naming what
differs, never used silently.

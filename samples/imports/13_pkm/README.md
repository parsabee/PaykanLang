# 13_pkm: a module served through a `.pkm` file

`main.pkn` imports `geometry::shapes`, a small library (a class hierarchy, an
enum and two functions).  Nothing in the two source files is about `.pkm`;
what this sample shows is how the import reaches the program.  Run every
command below from this directory.

## 1. Run it: the cache is written

```text
$ paykan --verbose main.pkn
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
checker needs to check an importer), its PIR (what either backend generates
code from) and a manifest naming the toolchain that produced it and the
source it was built from.  The second run reuses the file:

```text
$ paykan --verbose main.pkn
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
pkm 1.0, 4 sections, ...
sections:
  #0 MANIFEST ...
  #1 IFACE ...
  #2 CODE ...
  #3 SYMIDX ...
manifest:
  module geometry::shapes
  contents 0x1 (HAS_CODE)
  core "0.1.1" ...
  format_versions iface 1.0 tmpl 0 pir 1 code_encoding 2 debug 0
  runtime_abi 6
  ...
iface:
  module geometry::shapes
  display_file "geometry/shapes.pkn"
  ...
  class Rect super "Shape" [local from "geometry::shapes"]
  ...
pir:
module "geometry::shapes"
...
```

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
$ paykan --verbose --backend=c main.pkn
module geometry::shapes: prebuilt geometry/shapes.pkm usable
rect of area 12
...
$ paykan --verbose --backend=llvm main.pkn
module geometry::shapes: prebuilt geometry/shapes.pkm usable
rect of area 12
...
$ paykan build --backend=llvm -o shapes main.pkn && ./shapes
rect of area 12
...
$ mv /tmp/shapes.pkn geometry/
```

Where no source is found, `paykan` looks for `<module>.pkm` where the source
would be, then under each `--module-path=<dir>` and `$PAYKAN_MODULE_PATH`
directory.  The file carries no backend-specific code: the c backend and the
llvm backend both generate their output from the PIR inside it, which is
why one file serves both.  A prebuilt file written by a `paykan` with a
different PIR or runtime ABI version is rejected with a message naming the
versions, never used silently.

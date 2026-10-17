# Instructions for Claude

## Version numbers stay fixed until 0.2.0

Do not bump the runtime ABI (`PAYKAN_RUNTIME_ABI_VERSION` in `src/Runtime/Runtime.h`), the
PIR version (`kPIRVersion` in `include/paykan/pir/Version.h`), the `.pkm` format versions
(`include/paykan/pkm/Format.h`), the plugin API version or the AST interchange format
version. They stay at their current values until the 0.2.0 release, which bumps them
once (`docs/design/pkm.md`).

## Clear the compilation caches after a breaking change

Because the versions are not bumped, a compiler with a breaking change still trusts the
`.paykan_cache` directories an older build wrote, and running those cached modules and
objects can crash. A breaking change is one to the generated code, the runtime ABI, PIR,
the `.pkm` format or the layout of values. After making or merging one, and before running
any program or test, delete every cache:

```sh
find . -name .paykan_cache -type d -prune -exec rm -rf {} +
```

A test failure that disappears after this was a stale cache, not a bug.

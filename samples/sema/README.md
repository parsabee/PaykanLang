# Sema Samples

Sample `.pkn` files that exercise the semantic analysis pass for class-related
checks. Each file is self-contained and can be run with the `--check-only` flag
to stop after type-checking (no codegen or JIT execution).

## Running

```bash
# Type-check a single file — exits 0 on success, 1 on error
paykan --check-only <file.pkn>

# Type-check all samples and print results
for f in samples/sema/*.pkn; do
  paykan --check-only "$f" 2>&1 && echo "OK: $f" || echo "ERR: $f"
done
```

## Naming convention

| Prefix | Meaning |
|--------|---------|
| `ok_`  | Valid program — `--check-only` must exit **0** |
| `err_` | Invalid program — `--check-only` must exit **1** with a diagnostic |
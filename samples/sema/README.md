# Sema Samples

Sample `.pkn` files that exercise the semantic analysis pass. Each file is
self-contained and can be run with the `--check-only` flag to stop after
type-checking (no codegen or JIT execution).

## Directory layout

| Directory  | Feature area                                      |
|------------|---------------------------------------------------|
| `arrays/`  | Array types, literals, subscript, `len()`, match  |
| `classes/` | Class declarations, fields, methods, inheritance, `__super__` |
| `functions/` | Free-function declarations and top-level name collisions |
| `match/`   | `match` statement — arms, bindings, wildcards     |
| `enums/`   | `enum` declarations, variant access, equality, enum match |

## Running

```bash
# Type-check a single file — exits 0 on success, 1 on error
paykan --check-only <file.pkn>

# Type-check all samples in a feature directory
for f in samples/sema/arrays/*.pkn; do
  paykan --check-only "$f" 2>&1 && echo "OK: $f" || echo "ERR: $f"
done

# Type-check every sample across all feature directories
for f in samples/sema/arrays/*.pkn samples/sema/classes/*.pkn \
         samples/sema/functions/*.pkn samples/sema/match/*.pkn \
         samples/sema/enums/*.pkn; do
  paykan --check-only "$f" 2>&1 && echo "OK: $f" || echo "ERR: $f"
done
```

## Naming convention

| Prefix | Meaning |
|--------|---------|
| `ok_`  | Valid program — `--check-only` must exit **0** |
| `err_` | Invalid program — `--check-only` must exit **1** with a diagnostic |
# Sema Samples

Sample `.pkn` files that exercise the semantic analysis pass. Each file is
self-contained and can be run with the `--check-only` flag to stop after
type-checking (no codegen or JIT execution).

## Directory layout

| Directory  | Feature area                                      |
|------------|---------------------------------------------------|
| `arrays/`  | Array types `T[n]`, slice parameters, literals, subscript, `len()` |
| `classes/` | Class declarations, fields, methods, inheritance, `__super__` |
| `functions/` | Free-function declarations and top-level name collisions |
| `match/`   | `match` statement — arms, bindings, wildcards     |
| `enums/`   | `enum` declarations, variant access, equality, enum match |
| `generics/` | Generic classes / functions: instantiation, inference, diagnostics |
| `optionals/` | Optional types `T?` — `None`, widening, unwrapping with `match`, optional fields and arrays of optionals |
| `tuples/`  | Tuple types, literals, `.N` access, destructuring, immutability |
| `structs/` | `struct` declarations, fields assigned in place, `impl` blocks, nominal typing, the `let` / `view` / `inout` rules, generic structs, what is not supported yet |
| `lambdas/` | Function types `(T) -> R`, lambdas and their captures, named functions as values, calls through function values |
| `visibility/` | Private members: `__` fields and methods reachable only from their type's methods, `_` functions and types private to their module and kept out of public signatures |
| `conversions/` | Conversion constructors `Target<Source>(value)`: the supported pairs, result types |
| `sized/` | Sized numeric types `i8` … `unsigned`, `f16`, `f32`: literals taking the expected type, the one-type rule for operators, conversions between every numeric pair |

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
         samples/sema/enums/*.pkn samples/sema/generics/*.pkn \
         samples/sema/optionals/*.pkn samples/sema/tuples/*.pkn \
         samples/sema/structs/*.pkn samples/sema/lambdas/*.pkn \
         samples/sema/visibility/*.pkn \
         samples/sema/conversions/*.pkn samples/sema/sized/*.pkn; do
  paykan --check-only "$f" 2>&1 && echo "OK: $f" || echo "ERR: $f"
done
```

## Naming convention

| Prefix | Meaning |
|--------|---------|
| `ok_`  | Valid program — `--check-only` must exit **0** |
| `err_` | Invalid program — `--check-only` must exit **1** with a diagnostic |
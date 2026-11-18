# Samples

Paykan programs that show every feature of the language, in two trees:

- **`valid/`**: programs the compiler accepts.  Each one compiles and runs,
  and ends with no live heap blocks.
- **`invalid/`**: programs the compiler rejects.  Each one shows a
  diagnostic, from the parser or from Sema.

Both trees use the same categories.  Each file in a category shows one
topic, and its name says what the topic is.

| Category           | What it covers |
|--------------------|----------------|
| `basics/`          | literals, variables, scopes, strings and chars, identifiers |
| `arithmetic/`      | arithmetic, relational, logical and bitwise operators, compound assignment, sized numeric types, float edge cases |
| `conversions/`     | conversion constructors `T(x)` / `Str<T>(x)`, parsing with `toInt()` and the other `to...()` methods |
| `control-flow/`    | `if` / `else`, the ternary, `while`, `for`, `break` / `continue`, `match` |
| `functions/`       | functions, parameter modes (`view`, `inout`), expression bodies, untyped parameters, function types and lambdas, `native fn` |
| `classes/`         | classes, inheritance, `__init__` / `__super__` / `__deinit__`, `view fn`, pure virtual methods, `final`, private members |
| `data-structures/` | arrays and fixed-size arrays, tuples, structs, optionals, enums and tagged unions |
| `generics/`        | generic classes, functions, structs, enums and methods; inference, bounds and instantiation |
| `memory/`          | ownership and reference counting: moves, borrows, weak references, and the `heap_*` leak checks ([`valid/memory/README.md`](valid/memory/README.md)) |
| `io/`              | `::io`: files, standard input, command-line arguments, the print family |
| `concurrency/`     | the concurrency prototype: `async` / `await`, `parallel for`, `atomic` and `locked` cells, the ownership qualifiers ([`valid/concurrency/README.md`](valid/concurrency/README.md)) |
| `modules/`         | `import`: each sample is a directory, a program `main.pkn` and the modules it imports |

`invalid/` has only the categories that have a rejected program.

## Running

```bash
# Run a valid sample
paykan run samples/valid/control-flow/match.pkn

# Run a modules sample (its main.pkn, from its directory)
paykan run samples/valid/modules/calculator/main.pkn

# Type-check an invalid sample: exits 1 with a diagnostic
paykan --check-only samples/invalid/data-structures/tuple_immutable.pkn
```

## How the tests use the samples

- `scripts/samples_parity.py` (ctest `SamplesParity*`) runs every program of
  `valid/` on each backend.  The backends must give the same output and
  exit code, and the run must end with zero live heap blocks.  A sample
  can pin its output with `// expect-stdout: <line>` lines, its arguments
  with an `// args: <word>...` line, and expected leaks with an
  `// expect-leaks: <why>` line.
- `scripts/c_strict.py` (ctest `CStrictC11`) compiles the same programs
  as strict C11.
- `scripts/let_check.py` (ctest `LetDecls`) requires each program of
  `valid/` to compile, and checks the programs of `invalid/` only where
  Sema reaches them.
- The frontend, AST-interchange and PIR round-trip tests read every `.pkn`
  file in both trees.

To add a sample, put it in the category it shows, in `valid/` or
`invalid/`, and name it after its topic.  A modules sample is a
directory with a `main.pkn`.

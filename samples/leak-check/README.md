# Leak-check samples

Each file here isolates **one** heap-allocating construct so that a single
allocation site can be exercised and checked for leaks in isolation.

Run any sample under the tracking allocator:

```sh
./build/bin/paykan --track-heap samples/leak-check/<file>.pkn
```

A clean sample dumps `live blocks : 0` at exit. A non-zero live-block count
points the finger at the specific runtime allocation site that file isolates.

Lines starting with `// expect-stdout:` give a sample's expected output, one
line each; the samples parity check (`scripts/samples_parity.py`, ctest
`SamplesParity` / `SamplesParityBuild`) enforces it on every backend.

| File                       | Isolates                                              | Runtime site                         |
|----------------------------|-------------------------------------------------------|--------------------------------------|
| `01_string_literal.pkn`    | A single string literal                               | `PaykanString_new` (`String.c`)      |
| `02_string_concat.pkn`     | `Str + Str` concatenation                             | `PaykanString_concat` (`String.c`)   |
| `03_str_int.pkn`           | `Str<int>` int→`Str` conversion                       | `PaykanString_new` via `IO.c`        |
| `04_array_literal.pkn`     | An `int[]` array literal                              | `PaykanArray_*` (`Array.c`)          |
| `05_array_push.pkn`        | `push` growth (realloc) on an empty array             | `PaykanArray_push` (`Array.c`)       |
| `06_array_obj.pkn`         | An `Obj[]` holding a boxed element                    | `PaykanShared` (`Shared.c`)          |
| `07_class_empty.pkn`       | A fieldless class instance                            | object struct + `PaykanShared`       |
| `08_class_str_field.pkn`   | A class instance holding a `Str` field                | object struct + `PaykanString_new`   |
| `09_class_inherit.pkn`     | A subclass instance via `__super__`                   | object struct + `PaykanShared`       |
| `10_error_open.pkn`        | `Error` from a failed `open`                          | `PaykanError_new` (`Error.c`)        |
| `11_file_write.pkn`        | `File` from a successful `open` + `write`             | `PaykanFile_new` (`File.c`)          |
| `12_file_readln.pkn`       | `readln` line buffer                                  | `PaykanFile_readln` (`File.c`)       |
| `13_ownership_transfer.pkn` | shared hand-off + re-assigned source                | `PaykanString_new` + `PaykanShared`  |
| `14_self_return_chain.pkn` | `self` returned (chained) out of a method             | unique-box acquire (`Shared.c`)      |
| `15_match_binding_ownership.pkn` | match-arm binding consumed as arg/var/field     | unique-box acquire (`Shared.c`)      |
| `16_call_rooted_member_chain.pkn` | `makeH().a` chains + borrowed field acquisition | receiver-box teardown + field retain |
| `17_tuple.pkn`             | Tuple literals, destructuring, nesting, `mk().0`, `==`, copies | `PaykanTuple_new` (`Tuple.c`) + `PaykanShared` |
| `19_empty_array_literal_sinks.pkn` | `[]` into a field, argument, `push`, return, subscript, nested literal | `PaykanArray_new_obj` vs `PaykanArray_new` (`Array.c`) |
| `20_match_binding_reassign_some_paths.pkn` | match-arm binding reassigned on some paths only | binding release at the join (`Shared.c`) |
| `21_match_binding_reassign_loop.pkn` | match-arm binding reassigned in a loop          | per-iteration release (`Shared.c`)   |
| `22_match_subject_reassign.pkn` | match subject reassigned inside the arm          | binding keeps the subject alive (`Shared.c`) |
| `23_match_binding_return_mix.pkn` | binding reassigned in a loop + returned, `a = a` | owned binding returned (`Shared.c`)  |
| `24_optional_primitives.pkn` | `int?` / `float?` / `bool?` / `char?` boxes in every sink, rebinding, copies, `T?` with T = int | `Paykan{Int,Float,Bool,Char}_new` (`Basic.c`) + `PaykanShared` |
| `25_conversions.pkn` | `Str<...>` temporaries, `int<Str>` / `float<Str>` boxes (present and None), numeric conversions | `PaykanString_from_*` (`String.c`), `Paykan{Int,Float}_from_str` (`Basic.c`) |
| `26_deep_chain_drop.pkn` | dropping a 1,000,000-node list, a deep wide tree, and 200,000-deep array / tuple chains (#118) | iterative destroy in `Paykan_release` (`Shared.c`) |

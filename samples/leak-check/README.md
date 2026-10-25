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
| `03_str_int.pkn`           | `Str<int>` int→`Str` conversion                       | `PaykanString_from_int` (`String.c`) |
| `04_array_literal.pkn`     | An `int[3]` array literal (held inline)               | none: nothing is allocated           |
| `06_array_obj.pkn`         | An `Obj[1]` holding a reference element               | `PaykanString_new` (`String.c`)      |
| `07_class_empty.pkn`       | A fieldless class instance                            | object struct (`Paykan_malloc`)      |
| `08_class_str_field.pkn`   | A class instance holding a `Str` field                | object struct + `PaykanString_new`   |
| `09_class_inherit.pkn`     | A subclass instance via `__super__`                   | object struct (`Paykan_malloc`)      |
| `10_error_open.pkn`        | The `err::Error` of a failed `io::open`               | `Paykan_error_text` (`Error.c`)      |
| `11_file_write.pkn`        | `io::File` from a successful `io::open` + `write`     | `PaykanFile_new` (`File.c`) + the `io::File` object |
| `12_file_readln.pkn`       | `readln` line buffer                                  | `PaykanFile_readln` (`File.c`)       |
| `14_self_return_chain.pkn` | `self` returned (chained) out of a method             | `Paykan_retain` of the object (`Shared.c`) |
| `15_match_binding_ownership.pkn` | match-arm binding consumed as arg/var/field     | `Paykan_retain` of the subject (`Shared.c`) |
| `16_call_rooted_member_chain.pkn` | `makeH().a` chains + borrowed field acquisition | receiver teardown + field retain     |
| `17_tuple.pkn`             | Tuple literals, destructuring, nesting, `mk().0`, `==`, copies (held in place: only their `Str` elements allocate) | `PaykanString_new` (`String.c`) + the `Holder` instance |
| `18_optional.pkn`          | Optional references (`T?`): locals, fields, rebinding, copies, printing, `match`, `T?[n]` slots | the `Node` instances and strings |
| `20_match_binding_reassign_some_paths.pkn` | match-arm binding reassigned on some paths only | binding release at the join (`Shared.c`) |
| `21_match_binding_reassign_loop.pkn` | match-arm binding reassigned in a loop          | per-iteration release (`Shared.c`)   |
| `22_match_subject_reassign.pkn` | match subject reassigned inside the arm          | binding keeps the subject alive (`Shared.c`) |
| `23_match_binding_return_mix.pkn` | binding reassigned in a loop + returned, `a = a` | owned binding returned (`Shared.c`)  |
| `24_optional_primitives.pkn` | `int?` / `float?` / `bool?` / `char?` values in every sink (inline, no allocation), rebinding, copies, `T?` with T = int | the instances and strings only |
| `25_conversions.pkn` | `Str<...>` temporaries, `int<Str>` / `float<Str>` optionals (present and None), numeric conversions | `PaykanString_from_*` (`String.c`) |
| `26_deep_chain_drop.pkn` | dropping a 1,000,000-node list, a deep wide tree, a 200,000-link chain through array fields and a 200,000-link chain of objects through tuple fields (#118) | iterative destroy in `Paykan_release` (`Shared.c`) |
| `27_borrowed_parameters.pkn` | a box parameter lent and handed over: kept in a field, handed on to the field that keeps it, only read, returned (#98) | the retain at a lent parameter's keep (`Paykan_retain_inline`, `Runtime.h`) |
| `28_buffer_out_of_memory.pkn` | a `memory::Buffer` reserve and grow of 2^61 - 1 ints, which leave no room for the tracking allocator's header: `OutOfMemory`, the buffer unchanged | `buffer_try_realloc` (`Buffer.c`) through `Paykan_tryRealloc` (`Heap.c`) |
| `29_file_update_modes.pkn` | files opened with `"w+"`, `"r+"` and `"a+"` whose reads (`readbytes`, `readln`, `read`) and writes alternate: every write kept, each starting where the other stopped | `file_start_read` and `file_write` (`File.c`), the read buffers and their `Str`s |

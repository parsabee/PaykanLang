# Memory samples

The ownership and reference-counting samples: moves at a value's last use,
borrowed parameters, weak references, reference-count elimination.

## Leak checks (`heap_*.pkn`)

Each `heap_*.pkn` file isolates **one** heap-allocating construct so that a single
allocation site can be exercised and checked for leaks in isolation.

Run any sample under the tracking allocator:

```sh
./build/bin/paykan --track-heap samples/valid/memory/<file>.pkn
```

A clean sample dumps `live blocks : 0` at exit. A non-zero live-block count
points the finger at the specific runtime allocation site that file isolates.

Lines starting with `// expect-stdout:` give a sample's expected output, one
line each; the samples parity check (`scripts/samples_parity.py`, ctest
`SamplesParity` / `SamplesParityBuild`) enforces it on every backend.

| File                       | Isolates                                              | Runtime site                         |
|----------------------------|-------------------------------------------------------|--------------------------------------|
| `heap_string_literal.pkn`    | A single string literal                               | `PaykanString_new` (`String.c`)      |
| `heap_string_concat.pkn`     | `Str + Str` concatenation                             | `PaykanString_concat` (`String.c`)   |
| `heap_str_of_int.pkn`           | `Str<int>` int→`Str` conversion                       | `PaykanString_from_int` (`String.c`) |
| `heap_array_literal.pkn`     | An `int[3]` array literal (held inline)               | none: nothing is allocated           |
| `heap_object_array.pkn`         | An `Obj[1]` holding a reference element               | `PaykanString_new` (`String.c`)      |
| `heap_empty_class.pkn`       | A fieldless class instance                            | object struct (`Paykan_malloc`)      |
| `heap_class_str_field.pkn`   | A class instance holding a `Str` field                | object struct + `PaykanString_new`   |
| `heap_class_inheritance.pkn`     | A subclass instance via `__super__`                   | object struct (`Paykan_malloc`)      |
| `heap_failed_open.pkn`        | The `err::Error` of a failed `io::open`               | `Paykan_error_text` (`Error.c`)      |
| `heap_file_write.pkn`        | `io::File` from a successful `io::open` + `write`     | `file_new` (`File.c`), closed by the `io::File`'s `__deinit__` |
| `heap_file_readln.pkn`       | `readln` line buffer                                  | `file_readln` (`File.c`)             |
| `heap_self_return_chain.pkn` | `self` returned (chained) out of a `shared fn`        | `Paykan_retain` of the object (`Shared.c`) |
| `heap_match_binding_ownership.pkn` | match-arm binding consumed as arg/var/field     | `Paykan_retain` of the subject (`Shared.c`) |
| `heap_call_rooted_member_chain.pkn` | `makeH().a` chains + borrowed field clones | receiver teardown + field clone      |
| `heap_tuples.pkn`             | Tuple literals, destructuring, nesting, `mk().0`, `==`, clones and a move (held in place: only their `Str` elements allocate) | `PaykanString_new` (`String.c`) + the `Holder` instance |
| `heap_optionals.pkn`          | Optional references (`T?`): locals, fields, rebinding, moves, printing, `match`, `T?[n]` slots | the `Node` instances and strings |
| `heap_match_binding_reassign_some_paths.pkn` | match-arm binding reassigned on some paths only | binding release at the join (`Shared.c`) |
| `heap_match_binding_reassign_loop.pkn` | match-arm binding reassigned in a loop          | per-iteration release (`Shared.c`)   |
| `heap_match_subject_reassign.pkn` | match subject reassigned inside the arm          | binding keeps the subject alive (`Shared.c`) |
| `heap_match_binding_return_mix.pkn` | binding reassigned in a loop + returned, `a = a` | owned binding returned (`Shared.c`)  |
| `heap_optional_primitives.pkn` | `int?` / `float?` / `bool?` / `char?` values in every sink (inline, no allocation), rebinding, copies, `T?` with T = int | the instances and strings only |
| `heap_conversions.pkn` | `Str<...>` temporaries, `s.toInt()` / `s.toFloat()` optionals (present and None), numeric conversions | `PaykanString_from_*` (`String.c`) |
| `heap_deep_chain_drop.pkn` | dropping a 1,000,000-node list, a deep wide tree, a 200,000-link chain through array fields and a 200,000-link chain of objects through tuple fields (#118) | iterative destroy in `Paykan_release` (`Shared.c`) |
| `heap_borrowed_parameters.pkn` | a box parameter lent and handed over: kept in a field, handed on to the field that keeps it, only read, returned (#98) | the retain at a lent parameter's keep (`Paykan_retain_inline`, `Runtime.h`) |
| `heap_buffer_out_of_memory.pkn` | a `memory::Buffer` reserve and grow of 2^61 - 1 ints, which leave no room for the tracking allocator's header: `OutOfMemory`, the buffer unchanged | `buffer_try_realloc` (`Buffer.c`) through `Paykan_tryRealloc` (`Heap.c`) |
| `heap_file_update_modes.pkn` | files opened with `"w+"`, `"r+"` and `"a+"` whose reads (`readbytes`, `readln`, `read`) and writes alternate: every write kept, each starting where the other stopped | `file_start_read` and `file_write` (`File.c`), the read buffers and their `Str`s |
| `heap_buffer_unset_values.pkn` | `memory::Buffer` slots never set of a tagged union whose first variant holds a `Str`, a function value, a struct holding one, a nested and a generic tagged union: `copy` and `swap` give `NotFound`; slots whose zero bytes are a value (an `int`, an optional function value, `Maybe<int>`) read as it | `PaykanType_isUnset` (`Type.c`) through `PaykanBuffer_isSet` (`Buffer.c`), the `Str`s and closures the slots hold |
| `heap_conditional_receivers.pkn` | fields of a conditional `(if c then a else b)` of class type, on both branches: read (a `Str`, an `int`, a `weak` field, a tuple field's element, a tuple and a struct field whole), assigned, passed `inout` and matched | the box the conditional gives, released after the statement, and the new object in one branch |
| `heap_str_of_value.pkn` | `Str(x)` of every value with a text (an optional, a tuple, a struct, an enum, an object, `None`), of a call, a field, a list's element and a generic value, kept and dropped; and of an object whose `toString()` replaces the only field holding it (`Str(h.c)`, `Str(a.b.c)`, `Str(self.c)`), alive until it returns | the box each `toString()` gives (`PaykanString_new`, `PaykanTuple_toString`) and the object a `toString()` replaces |
| `heap_drop_by_tag.pkn` | values held in memory with more than four references (a struct holding structs and a tagged union) released at each way out of a function in place until its ways out have written 512 such releases, and after that by their types' drop functions, a union's by its tag (a variant with fields, a later one, one with none); a parameter whose references were handed on before its drop; values released in place at the end of a scope, in a class's field and as an assignment's old value | the releases in place and the drop functions' of the Strs each variant and struct holds |

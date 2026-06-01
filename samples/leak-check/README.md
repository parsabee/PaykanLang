# Leak-check samples

Each file here isolates **one** heap-allocating construct so that a single
allocation site can be exercised and checked for leaks in isolation.

Run any sample under the tracking allocator:

```sh
./build/bin/paykan --track-heap samples/leak-check/<file>.pkn
```

A clean sample dumps `live blocks : 0` at exit. A non-zero live-block count
points the finger at the specific runtime allocation site that file isolates.

| File                       | Isolates                                              | Runtime site                         |
|----------------------------|-------------------------------------------------------|--------------------------------------|
| `01_string_literal.pkn`    | A single string literal                               | `PaykanString_new` (`String.c`)      |
| `02_string_concat.pkn`     | `Str + Str` concatenation                             | `PaykanString_concat` (`String.c`)   |
| `03_str_int.pkn`           | `StrInt` int→`Str` conversion                         | `PaykanString_new` via `IO.c`        |
| `04_array_literal.pkn`     | An `int[]` array literal                              | `PaykanArray_*` (`Array.c`)          |
| `05_array_push.pkn`        | `push` growth (realloc) on an empty array             | `PaykanArray_push` (`Array.c`)       |
| `06_array_obj.pkn`         | An `Obj[]` holding a boxed element                    | `PaykanShared` (`Shared.c`)          |
| `07_class_empty.pkn`       | A fieldless class instance                            | object struct + `PaykanShared`       |
| `08_class_str_field.pkn`   | A class instance holding a `Str` field                | object struct + `PaykanString_new`   |
| `09_class_inherit.pkn`     | A subclass instance via `__super__`                   | object struct + `PaykanShared`       |
| `10_error_open.pkn`        | `Error` from a failed `open`                          | `PaykanError_new` (`Error.c`)        |
| `11_file_write.pkn`        | `File` from a successful `open` + `write`             | `PaykanFile_new` (`File.c`)          |
| `12_file_readln.pkn`       | `readln` line buffer                                  | `PaykanFile_readln` (`File.c`)       |

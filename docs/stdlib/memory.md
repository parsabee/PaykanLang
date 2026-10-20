# `::memory`

Storage that a program manages itself: `Buffer<T>`, a growable run of slots whose memory
is the runtime's (the [containers](collections.md) keep their elements in buffers), and
`Box<T>`, one value of a value type held in a shared object. A program imports the module
with `import ::memory;`. A buffer reports what goes wrong as a [`::result`](result.md)
value, so a program that looks at the errors imports that module too
(`import ::result as res;`).

```pkn
import ::io;
import ::memory;
import ::result as res;

fn main() -> int {
  b = memory::Buffer<Str>();
  b.push("a");
  b.push("b");
  b.grow(1);                          // slot 2: unset
  alias = b;                          // a copy shares the slots
  alias.set(0, "A");
  io::println(Str(b.len()) + " " + b.get(0) + b.get(1));
  io::println(res::name(b.set(5, "x").code));
  io::println(res::name(b.copy(2).error.code));
  io::println(res::name(b.swap(0, 2).code));
  io::println(res::name(b.shrink(9).code));
  match b.copy(1).value {
    s: Str { io::println("copy " + s); }
    None   { io::println("none"); }
  }
  return 0;
}
```

Output:

```
3 Ab
OutOfRange
NotFound
NotFound
InvalidArgument
copy b
```

---

## `Buffer<T>`

```pkn
struct Buffer<T> { ... }
```

`Buffer<T>()` is an empty run of slots of `T`, with nothing allocated, whose storage is the
runtime's, reached through [generic native
functions](../language/03-functions.md#generic-native-functions). `Buffer` is a
[struct](../language/11-structs.md) holding a reference to that storage, a runtime object:
copying a buffer copies the reference, so every copy shares the same slots, as copies of a
slice header do, and the storage goes with the last copy.

A buffer has `len()` slots, and grows and shrinks only when asked to. A new slot holds zero
bytes: `0`, `0.0`, `False`, char `0` or `None`. A slot of a reference type that cannot be
`None` (a class, `Str`, a tuple) is **unset** until it is stored. The buffer holds its own
reference to each value it stores, and drops it when the slot is overwritten or shrunk
away, or the buffer goes. Every index and count is an `unsigned`.

What can go wrong is reported as a `::result` value, not a panic: the methods that can fail
return a `res::Error` (or a `res::Result<T>`) and leave the buffer unchanged when they do.
The errors are:

| Code | When |
|------|------|
| `OutOfRange` | an index `>= len()` |
| `NotFound` | reading or swapping an unset slot |
| `InvalidArgument` | shrinking by more than `len()` |
| `OutOfMemory` | the memory for new slots is not there (its message is C's, `Cannot allocate memory`) |

Only `get(i)` and `copyUnchecked(i)` panic on a bad index or an unset slot, for code that
has checked the index already, as reading a fixed-size array out of bounds does.

| Method | Does | When it fails |
|--------|------|---------------|
| `Buffer<T>()` | An empty buffer: no slots, nothing allocated | |
| `view fn len() -> unsigned` | The number of slots | |
| `view fn get(i: unsigned) -> view T` | Slot `i` itself, read in place, nothing copied | panics `buffer index <i> out of bounds (len=<n>)`, or `buffer value <i> was never set` for an unset slot |
| `fn copyUnchecked(i: unsigned) -> T` | A copy of slot `i`, to keep | panics as `get` does |
| `fn copy(i: unsigned) -> res::Result<T>` | A copy of slot `i`, to keep | `OutOfRange`, `NotFound` |
| `fn set(i: unsigned, v: view T) -> res::Error` | Stores `v` in slot `i`, dropping what it held | `OutOfRange` |
| `fn push(v: view T) -> res::Error` | Adds a slot at the end holding `v` | `OutOfMemory` |
| `fn grow(by: unsigned) -> res::Error` | Adds `by` new slots at the end | `OutOfMemory` |
| `fn reserve(n: unsigned) -> res::Error` | Makes the buffer at least `n` slots long, adding new slots at the end (a buffer already that long is unchanged) | `OutOfMemory` |
| `fn shrink(by: unsigned) -> res::Error` | Drops the last `by` slots | `InvalidArgument` |
| `fn swap(i: unsigned, j: unsigned) -> res::Error` | Exchanges slots `i` and `j` | `OutOfRange`, then `NotFound` |

`reserve(n)` changes `len()`: it is `grow(n - len())` when `n > len()`, not a request for
capacity behind the slots. Growing one slot at a time is amortised O(1): the allocation at
least doubles when it grows, and is halved once at most a quarter of it is in use.

The methods that change the buffer are not `view fn`s, and neither are `copy` and
`copyUnchecked`: a copy of an object shares it with the buffer, so it could be changed
through the copy. A `view` parameter of type `memory::Buffer<T>` is read with `len` and
`get`, which lends the slot as a `view`, and nothing changes through it (`'b' is a 'view'
parameter; 'push' is not a 'view fn'`, and the same for `copyUnchecked`).

---

## `Box<T: ValueType>`

```pkn
class Box<T: ValueType> { ... }
```

`Box<T>(v)`, or `Box(v)` with `T` inferred, holds one value of a
[`ValueType`](../language/06-traits.md#valuetype) (`int`, `float`, `bool`, `char`, a sized
number, an enum, a tuple, a struct or a function value) in an object, so the value can be
shared: `Box` is a class, so assigning a box shares it, and a change through one name shows
through every other. A `Str` or a class is not a `ValueType` (`'Str' does not implement
trait 'ValueType', which 'memory::Box' requires of 'T'`): it is shared by reference already.

| Method | Does |
|--------|------|
| `Box<T>(v: T)` | A box holding `v` |
| `fn get() -> inout T` | The value's storage |

`get()` returns `inout T`, the value's storage: read it (`pair.get().1`), pass it to an
`inout` parameter, assign to one of its fields (`pt.get().x = 5` for a struct), or assign
through an `inout` local, which borrows the box until its last use. The call itself is not
a place to assign to (`b.get() = 5` is `left-hand side of '=' must be an identifier, field
access, or subscript`). `get` is not a `view fn`, so a
`view` box cannot be read through it.

```pkn
import ::io;

import ::memory;

fn bump(n: inout int) { n = n + 1; }

fn main() -> int {
  hits = memory::Box(0);
  alias = hits;
  bump(hits.get());
  n: inout int = alias.get();
  n = n * 10;
  pair = memory::Box((1, "one"));
  p: inout (int, Str) = pair.get();
  p = (2, "two");
  io::println(Str(hits.get()) + " " + pair.get().1);
  return 0;
}
```

Output:

```
10 two
```

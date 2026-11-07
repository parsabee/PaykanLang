# `::memory`

Storage that a program manages itself: `Buffer<T>`, a growable run of slots whose memory
is the runtime's (the [containers](collections.md) keep their elements in buffers),
`Box<T>`, one value of a value type held in an object, and [`swap`, `replace` and
`take`](#moving-values-out-of-places-swap-replace-and-take), which move a value out of a
field or another place that `move` does not take. A program imports the module with
`import ::memory;` (or `import ::memory as mem;`). A buffer reports what goes wrong as an [`::error`](error.md)
`Error` (in a [`::result`](result.md) `Result` when the operation makes a value), so a
program that looks at the errors imports `::error` too (`import ::error as err;`).

```pkn
import ::io;
import ::error as err;
import ::memory;

fn main() -> int {
  b = memory::Buffer<Str>();
  b.push("a");
  b.push("b");
  b.grow(1);                          // slot 2: unset
  b.set(0, "A");                      // slot 0 holds "A" now
  io::println(Str(b.len()) + " " + b.at(0) + b.at(1));
  io::println(err::name(b.set(5, "x").code));
  match b.copy(2) {
    s: Ok  { io::println("copy " + s.value); }
    e: Err { io::println(err::name(e.error.code)); }
  }
  io::println(err::name(b.swap(0, 2).code));
  io::println(err::name(b.shrink(9).code));
  match b.copy(1) {
    s: Ok  { io::println("copy " + s.value); }
    e: Err { io::println(err::name(e.error.code)); }
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
[struct](../language/11-structs.md) holding a reference to that storage, a runtime object
that the buffer owns: a buffer is move-only, of numbers too, so it is moved (`c = move b;`)
and never copied (`c = b;` is an error that says to write `move b`), and the storage goes
with it ([Ownership transfers](../language/16-memory.md#ownership-transfers)). `b.clone()`
is a new buffer of as many slots, each holding a clone of `b`'s value (an unset slot stays
unset), when the values are [Cloneable](../language/06-traits.md#cloneable); a buffer of
values that are not is not Cloneable either (`'Buffer<Node>' is not Cloneable: ...`).

A buffer has `len()` slots, and grows and shrinks only when asked to. A new slot holds zero
bytes: `0`, `0.0`, `False`, char `0`, `None`, or an enum's first variant. Where zero bytes
are no value of `T`, a slot is **unset** until it is stored:

- a reference that cannot be `None`: a class, `Str`, `Obj`;
- a function value, `() -> int` (zero bytes are only its optional's `None`);
- a tuple or a struct with an element or a field of such a type, `(int, Str)`;
- a tagged union whose first variant has a field of such a type, `enum Shape { Named(s:
  Str), Empty }`, a generic one's instantiation too (`Maybe<Str>` for `enum Maybe<T> {
  Just(v: T), Nada }`).

The rest have no unset slot: a new slot of `enum Count { Tally(n: int, s: Str?), Label(s:
Str) }` is `Tally(n: 0, s: None)`, of `Maybe<int>` `Just(v: 0)`, of an optional `None`.

```pkn
import ::io;
import ::error as err;
import ::memory;

enum Shape { Named(s: Str), Empty }

fn main() -> int {
  shapes = memory::Buffer<Shape>();
  shapes.grow(2);                     // two unset slots
  shapes.set(1, Shape::Empty);
  io::println(err::name(shapes.swap(0, 1).code));
  shapes.set(0, Shape::Named("n"));
  io::println(err::name(shapes.swap(0, 1).code));
  io::println(shapes.at(1));
  return 0;
}
```

Output:

```
NotFound
Ok
Named(s: n)
```

A buffer owns the values in its slots. A value goes in by a move: `set` and `push` take
the one they are given (`b.push(move s)`, or `b.push(s.clone())` to keep `s`; a temporary
or a Copyable value goes in as it is). It comes out lent, by `at` (the slot itself, a
`view`, nothing copied); moved, by `pop`; or as a clone the caller keeps, by `get` and
`copy`, which only a buffer of Cloneable values has: they are in an `impl Buffer<T:
Cloneable>` block, so a `Buffer<int>` or a `Buffer<Str>` has them and a `Buffer<Node>` of
a class that is not Cloneable has neither (`no method 'get' on type 'Buffer<Node>'`). A
value is dropped when its slot is overwritten or shrunk away, or the buffer goes. Every
index and count is an `unsigned`.

```pkn
import ::io;
import ::memory as mem;

class Node {
  name: Str;
}

fn main() -> int {
  let s = "a";
  b = mem::Buffer<Str>();
  b.push(s.clone());                  // a clone goes in, and s stays
  b.push("b");
  let c = b.clone();                  // a new buffer: Str is Cloneable
  let last = b.pop();                 // moved out of the last slot
  nodes = mem::Buffer<Node>();
  nodes.push(Node("n"));
  io::println(s + " " + b.get(0) + " " + last + " " + Str(c.len()) + " " + nodes.at(0).name);
  return 0;
}
```

Output:

```
a a b 2 n
```

What can go wrong is reported as an `::error` value, not a panic: the methods that can fail
return an `err::Error` (or a `res::Result<T, err::Error>`) and leave the buffer unchanged
when they do.
The errors are:

| Code | When |
|------|------|
| `OutOfRange` | an index `>= len()` |
| `NotFound` | reading or swapping an unset slot |
| `InvalidArgument` | shrinking by more than `len()` |
| `OutOfMemory` | the memory for new slots is not there (its message is C's, `Cannot allocate memory`) |

Only `at(i)`, `get(i)` and `pop()` panic on a bad index or an unset slot, for code that has
checked the index already, as reading a fixed-size array out of bounds does. `at` lends the
slot as a `view` and `get` hands over a clone, as a [container's](collections.md) `at` and
`get` do; `copy(i)` is the checked read.

| Method | Does | When it fails |
|--------|------|---------------|
| `Buffer<T>()` | An empty buffer: no slots, nothing allocated | |
| `view fn len() -> unsigned` | The number of slots | |
| `view fn at(i: unsigned) -> view T` | Slot `i` itself, read in place, nothing copied | panics `buffer index <i> out of bounds (len=<n>)`, or `buffer value <i> was never set` for an unset slot |
| `view fn get(i: unsigned) -> T` | A clone of slot `i`'s value, to keep (Cloneable `T` only) | panics as `at` does |
| `view fn copy(i: unsigned) -> res::Result<T, err::Error>` | A clone of slot `i`'s value, to keep (Cloneable `T` only) | `OutOfRange`, `NotFound` |
| `fn set(i: unsigned, v: T) -> err::Error` | Stores `v`, which moves in, in slot `i`, dropping what it held | `OutOfRange` (and `v` is dropped) |
| `fn push(v: T) -> err::Error` | Adds a slot at the end holding `v`, which moves in | `OutOfMemory` (and `v` is dropped) |
| `fn pop() -> T` | Removes the last slot and returns its value, which moves out | panics on an empty buffer (`buffer index 18446744073709551615 out of bounds (len=0)`: the index `len() - 1` wraps) or an unset last slot (`buffer value <i> was never set`) |
| `fn grow(by: unsigned) -> err::Error` | Adds `by` new slots at the end | `OutOfMemory` |
| `fn reserve(n: unsigned) -> err::Error` | Makes the buffer at least `n` slots long, adding new slots at the end (a buffer already that long is unchanged) | `OutOfMemory` |
| `fn shrink(by: unsigned) -> err::Error` | Drops the last `by` slots | `InvalidArgument` |
| `fn swap(i: unsigned, j: unsigned) -> err::Error` | Exchanges slots `i` and `j`: each value moves to the other's slot | `OutOfRange`, then `NotFound` |
| `view fn clone() -> Buffer<T>` | A new buffer of clones of the values (Cloneable `T` only) | panics `out of memory: ...` when the slots cannot be allocated |

`reserve(n)` changes `len()`: it is `grow(n - len())` when `n > len()`, not a request for
capacity behind the slots. Growing one slot at a time is amortised O(1): the allocation at
least doubles when it grows, and is halved once at most a quarter of it is in use.

The methods that change the buffer are not `view fn`s; the reads are, the copying ones
too, since a clone is a value of its own. A `view` parameter of type `memory::Buffer<T>` is
read with `len`, `at`, `get` and `copy`, and nothing changes through it (`'b' is a 'view'
parameter; 'push' is not a 'view fn'`).

---

## `Box<T: ValueType>`

```pkn
class Box<T: ValueType> { ... }
```

`Box<T>(v)`, or `Box(v)` with `T` inferred, holds one value of a
[`ValueType`](../language/06-traits.md#valuetype) (`int`, `float`, `bool`, `char`, a sized
number, an enum, a tuple, a struct or a function value) in an object, so the value can be
shared: `Box` is a class, so a `shared` box is held by several names, and a change through
a `shared mut` one shows through every other. A box with no qualifier has one owner, as
every object does, and moves. A `Str` or a class is not a `ValueType` (`'Str' does not implement
trait 'ValueType', which 'memory::Box' requires of 'T'`): it is shared by reference already.

| Method | Does |
|--------|------|
| `Box<T>(v: T)` | A box holding `v` |
| `fn get() -> inout T` | The value's storage |
| `view fn value() -> view T` | The value, read only: the read of any box, a plain, `view` or frozen (`atomic shared`) one |

`get()` returns `inout T`, the value's storage: read it (`pair.get().1`), pass it to an
`inout` parameter, assign to one of its fields (`pt.get().x = 5` for a struct), or assign
through an `inout` local, which borrows the box until its last use. The call itself is not
a place to assign to (`b.get() = 5` is `left-hand side of '=' must be an identifier, field
access, or subscript`). `get` is not a `view fn`, so it needs a `mut` box: a plain one is
not read through it (`'b' is immutable (a 'Box<int>'; write 'mut Box<int>' to change it);
'get' is not a 'view fn'` for `let b = mem::Box(0); b.get()`), nor is a `view` of one or a
frozen `atomic shared` one. `value()` is a `view fn`, and reads any box: a `view T`, which
cannot be assigned
([Concurrency](../language/19-concurrency.md#values-take-no-qualifier-membox) reads a plain
box through it, and [`atomic shared`](../language/19-concurrency.md#atomic-shared-frozen-objects)
a frozen one).

```pkn
import ::io;

import ::memory;

fn bump(n: inout int) { n = n + 1; }

fn main() -> int {
  let hits: shared mut memory::Box<int> = memory::Box(0);
  let alias = hits;                     // the same box, read through value()
  bump(hits.get());
  n: inout int = hits.get();
  n = n * 10;
  let pair: mut = memory::Box((1, "one"));
  p: inout (int, Str) = pair.get();
  p = (2, "two");
  io::println(Str(alias.value()) + " " + pair.get().1);
  return 0;
}
```

Output:

```
10 two
```

---

## Moving values out of places: `swap`, `replace` and `take`

```pkn
native fn swap<T>(a: inout T, b: inout T)
fn replace<T>(slot: inout T, v: T) -> T
fn take<T>(slot: inout T?) -> T?
```

`move x` takes a variable. A move-only value in a field (a unique object, a `Str`, a value
holding one) leaves it in exchange for another, which these functions make
([Ownership transfers](../language/16-memory.md#fields-and-elements-memreplace-memtake-and-memswap)):

| Function | Does |
|----------|------|
| `swap(a, b)` | Exchanges the values of `a` and `b`, each with what it owns: nothing is copied |
| `replace(slot, v)` | Stores `v` in `slot` and returns what `slot` held |
| `take(slot)` | Returns what the optional `slot` held and leaves `None` in it |

```pkn
import ::io;
import ::memory as mem;

class Node {
  name: Str;
}

class Shelf {
  top: Node;
  spare: Node?;
}

fn main() -> int {
  let s: mut = Shelf(Node("a"), Node("b"));
  let old = mem::replace(s.top, Node("c"));   // s.top's node; s.top now holds c
  let spare = mem::take(s.spare);             // s.spare's node; s.spare is None
  x: mut = "x";
  y: mut = "y";
  mem::swap(x, y);
  io::println(old.name + " " + s.top.name + " " + Str(s.spare == None) + " " + x + y);
  match spare {
    n: Node { io::println(n.name); }
    None { }
  }
  return 0;
}
```

Output:

```
a c True yx
b
```

Their places are `inout` parameters, so each is a place an `inout` argument can be: a
variable that is not `let` (`mut` when it holds an object or a string), or a field of a
`mut` object (`'s' is immutable (a 'Shelf'; write 'mut Shelf' to change it); it cannot be
passed to 'inout' parameter 'slot'`), or an element of a fixed-size array that is one
(`mem::take(xs[i])`), and two arguments of one call share no storage: `mem::swap(xs[0],
xs[1])` swaps two elements, but two at indexes that are not literals may be one element
(`'xs' is passed to two 'inout' parameters ('a' and 'b')`), so one moves to another by
`xs[j] = mem::replace(xs[i], v)`. A slice's element is not a place yet (`an element of a
slice cannot be passed to 'inout' parameter 'slot' yet`). `replace` and `take` are written with `swap`, which the runtime does with no
count: the two values trade their bytes.

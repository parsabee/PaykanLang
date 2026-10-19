# PaykanLang — The Standard Library

The standard library is written in Paykan and ships with the compiler. A program imports
its modules with a system import (`import ::collections;`, see
[Modules](06-modules.md#system-imports)) and names what they declare with the module's
qualifier: `collections::List<int>()`.

---

## Quick Summary

- `::collections`: `List<T>`, `LinkedList<T>`, `Stack<T>`, `Queue<T>`, `HashMap<K, V>`,
  `HashSet<K>`, `MinPriorityQueue<T>` and `MaxPriorityQueue<T>`, and the functions
  `listOf` and `listToString`.
- `::memory`: `Buffer<T>`, the growable storage the containers are built on, and
  `Box<T: ValueType>`, one value in a shared object.
- `::math`: the C library's math functions on `float` (`sqrt`, `pow`, `exp`, `log`,
  `sin`, `atan2`, `floor`, `round`, `gamma`, …), bound directly to `<math.h>`, with
  `pi()`, `e()`, `inf()`, `nan()` and `isNaN`, `isInf`, `isFinite`.
- Every container is an object, so assigning one shares it. A container holds its own
  reference to each element. A method that hands an element out returns a copy, which
  shares an object element with the container.
- Elements are compared with `==`, so a class compares by its `equals`.
- Every index and size is an `unsigned`: `len()` returns one, `get(i)`, `set(i, v)`,
  `insert(i, v)`, `removeAt(i)` and `swap(i, j)` take one, and `indexOf(v)` returns an
  `unsigned?` that is `None` when there is no such element. A literal index takes the
  type (`xs.get(0)`); a counter is declared `i: unsigned = 0`.
- The keys of a `HashMap` or `HashSet` implement the builtin trait
  [`Hashable`](12-traits.md#hashable): `int`, `bool`, `char`, `Str`, a sized integer type
  (`u32`, `unsigned`, …), or a class with an `impl Hashable for` block.
- A broken precondition panics: `pop` on an empty container, an index out of range,
  `get` of a key a map does not have.
- The library's `_` and `__` names are [private](06-modules.md#private-to-the-module): a
  container's `__` fields and methods are reachable only from its own methods, and the
  modules' `_` functions and classes (`collections::_Entry`, `memory::_new`) only from
  inside the module.  A program that names one is rejected (`'__size' is private to
  'HashMap<int, int>': it is reachable only from its own methods`).
- The [sized numeric types](01-language-basics.md#sized-numeric-types) are elements and
  keys like `int` and `float`: a `List<u8>`, a `HashMap<u32, Str>`, a `Box<f32>`; a
  literal pushed or put takes the element or key type.
- The build compiles the library with the compiler and installs it in
  `<prefix>/lib/paykan/stdlib/<version>/`, where `paykan` finds it (see
  [Where the library is](#where-the-library-is)).

---

## An Example

```pkn
import ::collections;

fn main() -> int {
  counts = collections::HashMap<Str, int>();
  words: Str[5] = ["to", "be", "or", "not", "to"];
  i: int = 0;
  while (i < words.len()) {
    counts.put(words[i], counts.getOr(words[i], 0) + 1);
    i = i + 1;
  }
  println("to: " + Str(counts.get("to")) + ", keys: " + Str(counts.len()));

  todo = collections::MinPriorityQueue<Str>();
  todo.push("write", 2);
  todo.push("plan", 1);
  todo.push("ship", 3);
  out = collections::List<Str>();
  while (!todo.isEmpty()) { out.push(todo.pop()); }
  println(collections::listToString(out));
  return 0;
}
```

Output:

```
to: 2, keys: 4
[plan, write, ship]
```

---

## `::collections`

### `List<T>`

A growable array. `push` and `pop` at the end are amortised O(1); `insert` and `removeAt`
move the elements after the index.

| Method | Description |
|--------|-------------|
| `List<T>()` | An empty list |
| `len()`, `isEmpty()` | The number of elements |
| `get(i)`, `set(i, v)` | Element `i`, `0 <= i < len()` |
| `push(v)`, `pop()`, `last()` | At the end |
| `insert(i, v)`, `removeAt(i)` | Anywhere, `i <= len()` for `insert` |
| `indexOf(v)`, `contains(v)`, `remove(v)` | Search with `==`; `indexOf` is an `unsigned?`, `None` when there is none, `remove` drops the first match |
| `swap(i, j)`, `reverse()`, `clear()` | In place |
| `copy()` | A new list of the same elements |

`listOf(xs)` makes a list of an array's or a slice's elements, and `listToString(xs)` prints
a list of `Show` elements as `[a, b, c]`.

### `LinkedList<T>`

A doubly linked list: both ends are O(1) to reach, add to and remove from. `contains`
and `remove` walk the list.

| Method | Description |
|--------|-------------|
| `LinkedList<T>()` | An empty list |
| `len()`, `isEmpty()` | The number of elements |
| `pushFront(v)`, `pushBack(v)` | Add at an end |
| `popFront()`, `popBack()` | Remove and return an end |
| `front()`, `back()` | The element at an end |
| `contains(v)`, `remove(v)` | Search with `==`; `remove` drops the first match |
| `clear()` | Remove every element |
| `toList()` | The elements from first to last, in a new `List<T>` |

Each node owns the next one and holds the previous one as a
[`weak` reference](08-memory-model.md#weak-references), so two neighbours form no
reference cycle and dropping the list frees every node. `pushFront` and `pushBack` take
the element by value (an object is then shared with the list), like `HashMap.put`.

### `Stack<T>` and `Queue<T>`

Both are kept in a `LinkedList<T>`, and every operation is O(1).

| `Stack<T>` | `Queue<T>` | Description |
|------------|------------|-------------|
| `push(v)` | `enqueue(v)` | Add: on top of the stack, at the back of the queue |
| `pop()` | `dequeue()` | Remove and return: the top, the front |
| `peek()` | `peek()` | The element `pop` or `dequeue` would return |
| `len()`, `isEmpty()`, `clear()`, `toList()` | the same | `toList` is top to bottom, front to back |

### `HashMap<K: Hashable, V>` and `HashSet<K: Hashable>`

Hash tables with separate chaining. Two keys are the same key when they are `==`, and keys
that are `==` must have the same `hash()`. A class key overrides `equals` and implements
`Hashable` to match. `put`, `get`, `containsKey` and `remove` are O(1) on average. The
table keeps at most three entries for every four buckets, and doubles when it would hold
more. The order of `keys()` is the table's, not the insertion order.

| `HashMap<K, V>` | Description |
|-----------------|-------------|
| `put(k, v)` | Map `k` to `v`; `True` if `k` is a new key |
| `get(k)`, `getOr(k, otherwise)` | The value of `k`; `get` panics if `k` is not a key |
| `containsKey(k)`, `remove(k)` | `remove` is `True` if `k` was a key |
| `len()`, `isEmpty()`, `clear()` | |
| `keys()`, `values()` | New lists, the values in the order of the keys |

| `HashSet<K>` | Description |
|--------------|-------------|
| `add(k)` | `True` if `k` was not in the set |
| `contains(k)`, `remove(k)` | `remove` is `True` if `k` was in the set |
| `len()`, `isEmpty()`, `clear()`, `toList()` | |

### `MinPriorityQueue<T>` and `MaxPriorityQueue<T>`

Binary heaps of elements, each pushed with an `int` priority. A `MinPriorityQueue` pops the
element of lowest priority first, a `MaxPriorityQueue` the highest. Elements of equal
priority pop in the order they were pushed. `push` and `pop` are O(log n).

| Method | Description |
|--------|-------------|
| `push(v, priority)` | Add `v` |
| `pop()` | Remove and return the next element |
| `peek()`, `peekPriority()` | The next element and its priority |
| `len()`, `isEmpty()`, `clear()` | |

The priority is a separate `int` because `<` is defined only for `int`, `float` and `char`.
To order numbers by their own value, push each one with itself as its priority:
`q.push(n, n)`.

---

### Sized numeric types in the containers

Every container takes the [sized numeric types](01-language-basics.md#sized-numeric-types)
as elements, and the hash tables take the sized integer types as keys (they are `Hashable`).
A literal flowing into a container's method takes the element type, so `bytes.push(250)`
pushes a `u8`, and the elements keep their arithmetic: adding two `u8` elements wraps at 256.

```pkn
import ::collections;

fn main() -> int {
  bytes = collections::List<u8>();
  bytes.push(250);
  bytes.push(bytes.get(0) + 10);          // 4: u8 arithmetic wraps
  codes = collections::HashMap<u32, Str>();
  codes.put(4000000000, "big");           // a key above the i32 range
  println(collections::listToString<u8>(bytes) + " " + codes.get(4000000000));
  return 0;
}
```

Output:

```
[250, 4] big
```

## `::memory`

`Buffer<T>(n)` is `n` slots of `T` whose storage is the runtime's, reached through generic
native functions (see [Native functions](02-functions-and-calling.md#native-functions)). A
new slot holds zero bytes: `0`, `0.0`, `False`, char 0 or `None`. A slot of a reference type
that cannot be `None` (a class, `Str`, a tuple) is unset until it is stored, and reading
it panics.

| Method | Description |
|--------|-------------|
| `len()` | The number of slots, an `unsigned` |
| `get(i)`, `copy(i)`, `set(i, v)` | Slot `i` (an `unsigned`): a `view` of it, a copy to keep, or a new value |
| `push(v)`, `grow(by)`, `reserve(n)`, `shrink(by)` | Change the number of slots; growing one at a time is amortised O(1) |
| `swap(i, j)` | Exchange two slots |

`Box<T: ValueType>(v)` holds one value of a [`ValueType`](12-traits.md#valuetype) (`int`,
`float`, `bool`, `char`, an enum or a tuple) in an object, so the value can be shared:
assigning a box shares it, and a change through one name shows through every other.
`get()` returns `inout T`, the value's storage. Read it, pass it to an `inout`
parameter, or assign through an `inout` local:

```pkn
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
  println(Str(hits.get()) + " " + pair.get().1);
  return 0;
}
```

Output:

```
10 two
```

## `::math`

The C library's math functions on `float`. Each one is a [native
function](02-functions-and-calling.md#native-functions) bound to the C99 `<math.h>`
function of the same name, so it computes exactly what C's does: a domain error
(`sqrt(-1.0)`, `log(-1.0)`) is NaN, a result out of range is an infinity, and nothing
panics. The trigonometric functions work in radians.

| Functions | Description |
|-----------|-------------|
| `sqrt`, `cbrt`, `pow(x, y)`, `hypot(x, y)` | Roots and powers |
| `exp`, `exp2`, `expm1`, `log`, `log2`, `log10`, `log1p` | Exponentials and logarithms; `log` is the natural one, `expm1(x)` is `e^x - 1` and `log1p(x)` is `log(1 + x)`, both accurate near 0 |
| `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y, x)` | Trigonometric functions |
| `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh` | Hyperbolic functions |
| `floor`, `ceil`, `trunc`, `round` | Rounding to a whole `float`; `round` takes halves away from zero |
| `fmod(x, y)`, `remainder(x, y)` | `fmod` has the sign of `x`, like `%`; `remainder` rounds the quotient to the nearest |
| `abs`, `min(x, y)`, `max(x, y)`, `copysign(x, y)` | `abs` is C's `fabs`, `min` and `max` are `fmin` and `fmax`, which ignore a NaN operand |
| `erf`, `erfc`, `gamma`, `lgamma` | `gamma` is C's `tgamma`; `lgamma` is the logarithm of the absolute value of `gamma` |
| `pi()`, `e()`, `inf()`, `nan()` | Constants (the language has no global constants) |
| `isNaN(x)`, `isInf(x)`, `isFinite(x)` | Classification |

The functions take and return `float`; a sized float (`f32`, `f16`) converts with
`float(x)` first.

```pkn
import ::math;

fn main() -> int {
  r = 2.0;
  println("area " + Str(math::pi() * math::pow(r, 2.0)));
  println("hypot " + Str(math::hypot(3.0, 4.0)) + ", 45 degrees is "
          + Str(math::atan2(1.0, 1.0)) + " radians");
  println(Str(math::floor(-2.5)) + " " + Str(math::round(-2.5)) + " "
          + Str(math::gamma(5.0)));
  println(Str(math::isNaN(math::sqrt(-1.0))) + " " + Str(math::log(0.0)));
  return 0;
}
```

Output:

```
area 12.5664
hypot 5, 45 degrees is 0.785398 radians
-3 -3 24
True -inf
```

---

## Where the Library Is

A system import looks for `<name>.pkn` (or a prebuilt `<name>.pkm`) in the first of:

1. `$PAYKAN_STDLIB`, when it is set;
2. the installed library, `<prefix>/lib/paykan/stdlib/<version>/` next to the `paykan`
   executable's `<prefix>/bin/`, when that directory exists;
3. `stdlib/` under the source root.

The sources are in [`stdlib/`](../../stdlib) in the repository. The build stages them, with
a `.pkm` that the built `paykan` compiles from each, in `lib/paykan/stdlib/<version>/` of
the build tree, next to its `bin/paykan`. `cmake --install` installs both, and the release
packages include them. Configure with `-DPAYKAN_BUILD_STDLIB=OFF` to leave the library out.
It is off by default when cross-compiling, because the built `paykan` cannot run on the
build machine.

Every module of the library has generic declarations, and a `.pkm` does not hold templates
yet ([Generics](11-generics.md#modules)). So a program builds the library's modules from
the sources installed next to the `.pkm` files.

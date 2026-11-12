# `::collections`

The standard containers: `List<T>`, a growable array; `LinkedList<T>`, a doubly linked
list; `Stack<T>` and `Queue<T>`; the hash tables `HashMap<K, V>` and `HashSet<K>`; and the
priority queues `MinPriorityQueue<T>` and `MaxPriorityQueue<T>`. They are written in Paykan
on [`::memory`](memory.md)'s buffers: `LinkedList` is a generic class, and the others are
generic [structs](../language/11-structs.md), each holding the buffer or the one object that
keeps its state. A container owns its elements: they go in by a move and come out lent, moved
or cloned ([Rules for every container](#rules-for-every-container)). A program imports the module with
`import ::collections;` (or an alias, `import ::collections as c;`) and names a container
with its qualifier: `collections::List<int>()`. The module also has
[functions on strings](#strings): `split`, `join`, `lines`, `words`, `indexOf`, `contains`,
`startsWith`, `endsWith`, `substring`, `trim`, `replace` and `repeat`.

```pkn
import ::io;
import ::collections;

fn main() -> int {
  xs = collections::List<Str>();
  xs.push("b");
  xs.push("c");
  xs.insert(0, "a");
  i: unsigned = 0;
  while i < xs.len() {
    io::print(xs.at(i) + " ");
    i = i + 1;
  }
  io::println(xs.indexOf("c"));
  io::println(xs.indexOf("z"));
  same = move xs;                  // the same list, now same's: xs is gone
  same.pop();
  io::println(collections::listToString(same) + " " + Str(same.len()));
  return 0;
}
```

Output:

```
a b c 2
None
[a, b] 2
```

---

## Rules for every container

- **A container is move-only.** `List`, `Stack`, `Queue`, `HashMap`,
  `HashSet` and the priority queues are structs whose one field is what holds every
  element and count (a `Buffer`, a `LinkedList`, the hash table of a `HashMap` or a
  `HashSet`, or the heap), and a `LinkedList` is that object itself. A container is never
  copied ([Ownership transfers](../language/16-memory.md#ownership-transfers)): assigning
  one, passing it to a plain parameter or storing it in a field moves it (`ys = move xs;`),
  and `ys = xs;` is an error; a `view` or `inout` parameter borrows it.
- **`xs.clone()` copies a container** whose elements are
  [Cloneable](../language/06-traits.md#cloneable) (a number, a `Str`, a struct or a tuple
  of Cloneable parts, a class with an `impl Cloneable for` block): a new container of clones
  of the elements, as a struct's clone is of its fields. A container of elements that are not
  Cloneable is not Cloneable either (`'List<Node>' is not Cloneable: its field '__items'
  ('Buffer<Node>') is not Cloneable`). A `HashMap` clones when its keys and its values are
  Cloneable. (`List`'s `copy()` is gone: `clone()` replaces it.)
- **A container is a struct**, so the rules of [structs](../language/11-structs.md) apply:
  through a `let` variable only its `view fn`s can be called (`'xs' is declared with 'let';
  its value cannot change, and 'push' is not a 'view fn'`), and an `inout` parameter
  changes the caller's in place; a `view` of one calls only its `view fn`s; and it is not
  an `Obj`: where one is needed, put the container in a
  [`Box`](memory.md) (with `import ::memory as mem;`, `let b: mut = mem::Box(move xs);`), an
  object whose `get()`, through a `mut` box, is the container itself (`b.get().push(1);`),
  and its own `toString()` is a struct's, which shows its private field. `listToString`
  prints a list's elements. A `LinkedList` is a class, with a class's rules.
- **An element goes in by a move.** Every method that stores one takes it by value, and
  the element moves in: `push(v: T)`, `set`, `insert`, `pushFront`, `pushBack`, `enqueue`,
  `put(k: K, v: V)` (the key and the value both), `add`, and the priority queues'
  `push(v: T, priority: int)`. A temporary or a Copyable value is passed as it is; a
  move-only one read from a variable is moved (`xs.push(move s)`) or cloned to keep it
  (`xs.push(s.clone())`), and `xs.push(s)` is `cannot copy 's' (a unique 'Str'); write
  'move s' to transfer it, 's.clone()' to copy it, or declare it 'shared Str'`. The lookups
  (`contains`, `indexOf`, `remove`, `containsKey`, and a map's `at`, `get` and their kin)
  borrow their argument, a `view`.
- **An element comes out three ways.**
  - Lent, as a `view` that stays the container's (`-> view T`): `List`'s `at` and `last`,
    `LinkedList`'s `front` and `back`, the `peek` of `Stack`, `Queue` and the priority
    queues, and `HashMap`'s `at` and `atOr`. The element is read in place, passed to a
    `view` parameter or bound to a `view` local (`f: view = fs.at(i);`), and a Copyable
    element is copied out (`n = xs.at(0);`), but a move-only one is neither stored nor
    changed through it (`the result of 'at' is a 'view'; cannot assign to its field 'n'`).
  - Moved out, by the methods that remove it: `pop`, `removeAt`, `popFront`, `popBack`,
    `dequeue` and the priority queues' `pop` return the element, the caller's now.
  - As a clone the caller keeps (`-> T`): the `get` of `List`, `HashMap`, `Stack`, `Queue`
    and the priority queues, `HashMap`'s `getOr`, `keys()` and `values()`, `LinkedList`'s
    `getFront` and `getBack`, and the `toList()` of `LinkedList`, `Stack`, `Queue` and
    `HashSet`. These copying methods are in `impl X<T: Cloneable>` blocks, so they exist
    only where the elements are Cloneable (a Copyable type is): a `List<int>` or a
    `List<Str>` has `get`, and a `List<Node>` of a class `Node` that is not Cloneable has
    none (`no method 'get' on type 'List<Node>'`). It lends with `at` and moves out with
    `pop`, or the class opts in with `impl Cloneable for Node {}` where cloning fits.
- **Elements are compared with `==`**, so a class compares by its `equals` (the
  identity, unless it overrides `equals`).
- **Every index and size is an `unsigned`**: `len()` returns one, `at(i)`, `get(i)`, `set(i, v)`,
  `insert(i, v)`, `removeAt(i)` and `swap(i, j)` take one, and `indexOf(v)` returns an
  `unsigned?` that is `None` when there is no such element. A literal index takes the
  type (`xs.at(0)`), and a counter is declared `i: unsigned = 0`: an `int` is not an index
  (`argument 1 of 'at' has type 'int', expected 'unsigned'`), and a negative literal does
  not fit (`the literal -1 does not fit in 'unsigned' ...`).
- **A broken precondition panics** with a message that names it: an index out of range
  (`List index 3 out of range (len=1)`), taking from an empty container (`pop from an
  empty List`), `at` or `get` of a key a map does not have. The tables below give each message.
  There is no other error: a container that cannot get memory for a new element panics
  with `out of memory: ...`.
- **The methods that only read are `view fn`s**, the copying ones too, so they can be
  called through a `view` parameter (`fn total(xs: view collections::List<int>)` may call
  `len()`, `at(i)` and `get(i)`, not `push`): a `view` of a container changes nothing in
  it.
- The keys of a `HashMap` or `HashSet` implement the builtin trait
  [`Hashable`](../language/06-traits.md#hashable): `int`, `bool`, `char`, `Str`, a sized
  integer type (`u32`, `unsigned`, ...), or a class or a struct with an `impl Hashable for`
  block in its own module, also when it is imported. A `float` key is rejected (`'float'
  does not implement trait 'Hashable', which 'collections::HashMap' requires of 'K'`).

```pkn
import ::io;
import ::collections as cl;

class Node { name: Str; }

fn main() -> int {
  let s = "kept";
  names = cl::List<Str>();
  names.push(s.clone());               // a clone goes in, and s stays
  names.push("new");                   // a temporary goes in as it is
  let copy = names.clone();            // a Str is Cloneable, so the list is
  nodes = cl::List<Node>();
  nodes.push(Node("a"));
  nodes.push(Node("b"));
  let last = nodes.pop();              // moved out: the list keeps one node
  io::println(s + " " + names.get(1) + " " + Str(copy.len()) + " " + nodes.at(0).name
      + " " + last.name + " " + Str(nodes.len()));
  return 0;
}
```

Output:

```
kept new 2 a b 1
```

```pkn
import ::collections as cl;

class Node { name: Str; }

fn main() -> int {
  nodes = cl::List<Node>();
  nodes.push(Node("a"));
  let n = nodes.get(0);
  let ns = nodes.clone();
  return 0;
}
```

Error:

```
no method 'get' on type 'List<Node>'
'List<Node>' is not Cloneable: its field '__items' ('Buffer<Node>') is not Cloneable
```

---

## `List<T>`

A growable array, kept in a [`memory::Buffer<T>`](memory.md). `push` and `pop` at
the end are amortised O(1); `insert` and `removeAt` move the elements after the index, and
`indexOf`, `contains` and `remove` walk the list, comparing each element with `==`. An
element's `equals` may change the list: the walk goes on over the elements the list then
has, so one that empties it makes the search find nothing. `indexOf` is the index the
element had when its `==` was `True`, and `remove` removes the element at that index. When
the `equals` that found it changed the list's length, that index may name another element
or none, so `remove` then removes nothing and is `False`. A change that keeps the length (a
`swap` or a `set`) goes unseen: `remove` then removes the element at that index, which may
be another one, and is `True`.

| Method | Does | When it fails |
|--------|------|---------------|
| `List<T>()` | An empty list | |
| `view fn len() -> unsigned` | The number of elements | |
| `view fn isEmpty() -> bool` | Whether there are none | |
| `view fn at(i: unsigned) -> view T` | Element `i`, as a `view` | panics `List index <i> out of range (len=<n>)` unless `i < len()` |
| `view fn get(i: unsigned) -> T` | A clone of element `i`, to keep (Cloneable `T` only) | panics as `at` |
| `fn set(i: unsigned, v: T)` | Replaces element `i` with `v`, which moves in; the element there is dropped | panics as `at` |
| `fn push(v: T)` | Appends `v`, which moves in | |
| `fn pop() -> T` | Removes the last element and returns it: it moves out | panics `pop from an empty List` |
| `view fn last() -> view T` | The last element, as a `view` | panics `last of an empty List` |
| `fn insert(i: unsigned, v: T)` | Inserts `v`, which moves in, before element `i`; `i == len()` appends | panics as `at` unless `i <= len()` |
| `fn removeAt(i: unsigned) -> T` | Removes element `i` and returns it: it moves out, and the elements after it move up one | panics as `at` |
| `view fn indexOf(v: view T) -> unsigned?` | The index of the first element `== v` | `None` when there is none |
| `view fn contains(v: view T) -> bool` | Whether some element is `== v` | |
| `fn remove(v: view T) -> bool` | Removes the first element `== v`; `True` if there was one | `False`, removing nothing, when the `equals` that found it changed the list's length; the element then at its index when that `equals` changed the list but not its length |
| `fn swap(i: unsigned, j: unsigned)` | Exchanges elements `i` and `j` | panics as `at` |
| `fn reverse()` | Reverses the order of the elements | |
| `fn clear()` | Removes every element | |
| `view fn clone() -> List<T>` | A new list of clones of the elements (Cloneable `T` only) | |

### `listOf`

```pkn
fn listOf<T: Cloneable>(xs: view T[]) -> List<T>
```

A new list of clones of the elements of an array or a slice, in order:
`collections::listOf(arr)` for `arr: int[4]` is a `List<int>`. The elements are
Cloneable (`'Node' does not implement trait 'Cloneable', which 'collections::listOf'
requires of 'T'`).

### `listToString`

```pkn
fn listToString<T: Showable>(xs: view List<T>) -> Str
```

The elements' `toString()`, in order, as `[a, b, c]`; `[]` for an empty list. The result is
a string of its own, which each element's text is appended to with
[`concat`](../language/04-strings-and-conversions.md#string-type), so the time is about
linear in its length: a list of 400,000 ints takes a tenth of a second.

## `LinkedList<T>`

A doubly linked list: both ends are O(1) to reach, add to and remove from. `contains` and
`remove` walk the list. Its nodes are objects of its own, each owning the next one and
reaching the one before it by a `weak mut` reference, as the list reaches its last node; a
node is `shared mut`, so it is changed through either link. So a `LinkedList` has `shared`
and `weak` parts, and is not one of the values that cross to another task
([Concurrency](../language/19-concurrency.md)).

| Method | Does | When it fails |
|--------|------|---------------|
| `LinkedList<T>()` | An empty list | |
| `view fn len() -> unsigned` | The number of elements | |
| `view fn isEmpty() -> bool` | Whether there are none | |
| `fn pushFront(v: T)` | Adds `v`, which moves in, before the first element | |
| `fn pushBack(v: T)` | Adds `v`, which moves in, after the last element | |
| `fn popFront() -> T` | Removes the first element and returns it: it moves out | panics `popFront on an empty LinkedList` |
| `fn popBack() -> T` | Removes the last element and returns it: it moves out | panics `popBack on an empty LinkedList` |
| `view fn front() -> view T` | The first element, as a `view` | panics `front on an empty LinkedList` |
| `view fn back() -> view T` | The last element, as a `view` | panics `back on an empty LinkedList` |
| `view fn getFront() -> T` | A clone of the first element, to keep (Cloneable `T` only) | panics `getFront on an empty LinkedList` |
| `view fn getBack() -> T` | A clone of the last element, to keep (Cloneable `T` only) | panics `getBack on an empty LinkedList` |
| `view fn contains(v: view T) -> bool` | Whether some element is `== v` | |
| `fn remove(v: view T) -> bool` | Removes the first element `== v`; `True` if there was one | |
| `fn clear()` | Removes every element | |
| `view fn toList() -> List<T>` | Clones of the elements from first to last, in a new `List<T>` (Cloneable `T` only) | |
| `view fn clone() -> LinkedList<T>` | A new list of clones of the elements (Cloneable `T` only) | |

Every node and element is the list's alone: the list owns the first node and each node the
next, and only `weak mut` references, which own nothing, lead back, so two neighbours form
no reference cycle and dropping the list drops every node and element. A `weak mut`
reference names a `shared mut` node and changes it ([Weak
references](../language/16-memory.md#weak-references)), so the list adds and removes at its
back through its link to the last node as it does at its front. Taking an element out, at
an end or by `remove`, unlinks its node from both of its neighbours and moves the element
out.

## `Stack<T>` and `Queue<T>`

A `Stack` is last in, first out; a `Queue` first in, first out. Both are kept in a
`LinkedList<T>`, and every operation is O(1), as the list's ends are.

| `Stack<T>` | `Queue<T>` | Does | When it fails |
|------------|------------|------|---------------|
| `Stack<T>()` | `Queue<T>()` | An empty stack, an empty queue | |
| `fn push(v: T)` | `fn enqueue(v: T)` | Adds `v`, which moves in: on top of the stack, at the back of the queue | |
| `fn pop() -> T` | `fn dequeue() -> T` | Removes the top, the front, and returns it: it moves out | panics `pop from an empty Stack`, `dequeue from an empty Queue` |
| `view fn peek() -> view T` | `view fn peek() -> view T` | The element `pop` or `dequeue` would return, as a `view` | panics `peek at an empty Stack`, `peek at an empty Queue` |
| `view fn get() -> T` | `view fn get() -> T` | A clone of that element, to keep (Cloneable `T` only) | panics `get from an empty Stack`, `get from an empty Queue` |
| `view fn len() -> unsigned` | the same | The number of elements | |
| `view fn isEmpty() -> bool` | the same | Whether there are none | |
| `fn clear()` | the same | Removes every element | |
| `view fn toList() -> List<T>` | the same | Clones of the elements in a new list, top to bottom, front to back (Cloneable `T` only) | |
| `view fn clone()` | the same | A new stack, a new queue, of clones of the elements (Cloneable `T` only) | |

## `HashMap<K: Hashable, V>` and `HashSet<K: Hashable>`

Hash tables with separate chaining. Two keys are the same key when they are `==`, and keys
that are `==` must have the same `hash()`. A class key overrides `equals` and implements
`Hashable` to match. `put`, `at`, `get`, `containsKey` and `remove` are O(1) on average. The
table keeps at most three entries for every four buckets, and doubles when it would hold
more. The order of `keys()` is the table's, not the insertion order.

The table chains its entries: each bucket holds the first entry of its chain, and each
entry its key, its value, its key's hash and the next entry. The entries are `shared mut`
objects of the table's own, each owning the next as a `LinkedList`'s nodes do (with no link
back), so a lookup changes the entry it reaches; every key and value is still the table's
alone. A rehash links the
entries into new buckets with no key or value moved. A `HashMap` or a `HashSet` has `shared`
parts, so it is not one of the values that cross to another task.

A map keeps each key's hash from when it was put, so an object key must not change in a
way that changes its `hash()` or `equals` while it is in the map: it would no longer be
found.

| `HashMap<K, V>` | Does | When it fails |
|-----------------|------|---------------|
| `HashMap<K, V>()` | An empty map | |
| `view fn len() -> unsigned` | The number of keys | |
| `view fn isEmpty() -> bool` | Whether there are none | |
| `fn put(k: K, v: V) -> bool` | Maps `k` to `v`, both moving in, replacing the value of a key it has (and dropping `k`); `True` if `k` is a new key | |
| `view fn at(k: view K) -> view V` | The value of `k`, as a `view` | panics `HashMap.at of a key it does not have` |
| `view fn atOr(k: view K, otherwise: view V) -> view V` | The value of `k`, or `otherwise` if it is not a key, as a `view` | |
| `view fn get(k: view K) -> V` | A clone of the value of `k`, to keep (Cloneable `V` only) | panics `HashMap.get of a key it does not have` |
| `view fn getOr(k: view K, otherwise: V) -> V` | A clone of the value of `k`, or `otherwise` if it is not a key (Cloneable `V` only) | |
| `view fn containsKey(k: view K) -> bool` | Whether `k` is a key | |
| `fn remove(k: view K) -> bool` | Removes `k` and its value; `True` if it was a key | |
| `fn clear()` | Removes every key | |
| `view fn keys() -> List<K>` | Clones of the keys, in a new list (Cloneable `K` only) | |
| `view fn values() -> List<V>` | Clones of the values, in a new list, in the order of `keys()` (Cloneable `V` only) | |
| `view fn clone() -> HashMap<K, V>` | A new map of clones of the keys and the values (Cloneable `K` and `V` only) | |

| `HashSet<K>` | Does |
|--------------|------|
| `HashSet<K>()` | An empty set |
| `view fn len() -> unsigned` | The number of keys |
| `view fn isEmpty() -> bool` | Whether there are none |
| `fn add(k: K) -> bool` | Adds `k`, which moves in (or is dropped, if it is in the set already); `True` if it was not in the set |
| `view fn contains(k: view K) -> bool` | Whether `k` is in the set |
| `fn remove(k: view K) -> bool` | Removes `k`; `True` if it was in the set |
| `fn clear()` | Removes every key |
| `view fn toList() -> List<K>` | Clones of the keys, in a new list, in the table's order (Cloneable `K` only) |
| `view fn clone() -> HashSet<K>` | A new set of clones of the keys (Cloneable `K` only) |

## `MinPriorityQueue<T>` and `MaxPriorityQueue<T>`

Binary heaps of elements, each pushed with an `int` priority. A `MinPriorityQueue` pops the
element of lowest priority first, a `MaxPriorityQueue` the highest. Elements of equal
priority pop in the order they were pushed. `push` and `pop` are O(log n). Both have the
same methods:

| Method | Does | When it fails |
|--------|------|---------------|
| `MinPriorityQueue<T>()`, `MaxPriorityQueue<T>()` | An empty queue | |
| `view fn len() -> unsigned` | The number of elements | |
| `view fn isEmpty() -> bool` | Whether there are none | |
| `fn push(v: T, priority: int)` | Adds `v`, which moves in, with `priority` | |
| `fn pop() -> T` | Removes the next element and returns it: it moves out | panics `pop on an empty priority queue` |
| `view fn peek() -> view T` | The next element, as a `view` | panics `peek on an empty priority queue` |
| `view fn get() -> T` | A clone of the next element, to keep (Cloneable `T` only) | panics `get on an empty priority queue` |
| `view fn peekPriority() -> int` | The next element's priority | panics `peekPriority on an empty priority queue` |
| `fn clear()` | Removes every element | |
| `view fn clone()` | A new queue of clones of the elements, in the same order (Cloneable `T` only) | |

The priority is a separate `int` because `<` is defined only for the numeric types and `char`.
To order numbers by their own value, push each one with itself as its priority:
`q.push(n, n)`.

---

## Sized numeric types in the containers

Every container takes the [sized numeric types](../language/02-basics.md#sized-numeric-types)
as elements, and the hash tables take the sized integer types as keys (they are `Hashable`).
A literal flowing into a container's method takes the element type, so `bytes.push(250)`
pushes a `u8`, and the elements keep their arithmetic: adding two `u8` elements wraps at 256.

```pkn
import ::io;

import ::collections;

fn main() -> int {
  bytes = collections::List<u8>();
  bytes.push(250);
  bytes.push(bytes.get(0) + 10);          // 4: u8 arithmetic wraps
  codes = collections::HashMap<u32, Str>();
  codes.put(4000000000, "big");           // a key above the i32 range
  io::println(collections::listToString<u8>(bytes) + " " + codes.get(4000000000));
  return 0;
}
```

Output:

```
[250, 4] big
```

---

## Strings

Functions on `Str`, named with the module's qualifier like its containers:
`collections::split(s, ",")`, or `c::split(s, ",")` after `import ::collections as c;`.
Like `s.len()` and `s[i]`, they count **bytes**: an index or a length is a number of bytes,
an `unsigned`, and a character outside ASCII is more than one (`"é"` is two).
**Whitespace** is ASCII's: a space, `\t`, `\n`, `\r`, and the vertical tab and the form
feed (`char(11)`, `char(12)`). Each takes its strings as `view`s, so a variable, a field
or an element read with `at` is passed as it is, and returns new ones: a result is a
string of its own, or a `List<Str>` of them, never one of the arguments, so a `concat` on
it (held in a `mut Str`) changes nothing else.

```pkn
import ::io;
import ::collections as c;

fn main() -> int {
  let fields = c::split("name,qty,,price", ",");
  io::println(Str(fields.len()) + " fields: " + c::join(fields, " | "));
  let ls = c::lines("first\r\nsecond\n\nlast\n");
  for i in 0..ls.len() {
    io::println(Str(i) + ": [" + ls.at(i) + "]");
  }
  io::println(c::join(c::words("  to  be\tor not\n"), "_"));
  return 0;
}
```

Output:

```
4 fields: name | qty |  | price
0: [first]
1: [second]
2: []
3: [last]
to_be_or_not
```

| Function | Does | When it fails |
|----------|------|---------------|
| `fn split(s: view Str, delim: view Str) -> List<Str>` | The fields of `s` between the occurrences of `delim`, in order, empty ones kept: `split("a,,b", ",")` is `["a", "", "b"]`, `split("", ",")` is `[""]`, and a string without `delim` is the one field `[s]`. `join(split(s, d), d) == s` | panics `split: the delimiter is empty` when `delim` is `""` |
| `fn join(parts: view List<Str>, sep: view Str) -> Str` | The elements of `parts`, in order, with `sep` between each two: `""` for no elements | |
| `fn lines(s: view Str) -> List<Str>` | The lines of `s`: the text before each `\n`, and after the last one, each without one `\r` it ends with. A `\n` at the end of `s` ends its last line and starts no other: `lines("a\r\nb\n")` is `["a", "b"]`, `lines("a\n\n")` is `["a", ""]`, and `lines("")` is `[]` | |
| `fn words(s: view Str) -> List<Str>` | The runs of bytes that are not whitespace, in order: `words(" a  b\tc\n")` is `["a", "b", "c"]`. No word is empty, and a string of whitespace has none | |
| `fn indexOf(s: view Str, needle: view Str) -> unsigned?` | The index of the first occurrence of `needle` in `s`: `indexOf("hello", "lo")` is `3`, and an empty `needle` is at `0` | `None` when `needle` does not occur |
| `fn contains(s: view Str, needle: view Str) -> bool` | Whether `needle` occurs in `s`; an empty `needle` does in every string | |
| `fn startsWith(s: view Str, prefix: view Str) -> bool` | Whether `s` starts with `prefix` (`""` starts every string) | |
| `fn endsWith(s: view Str, suffix: view Str) -> bool` | Whether `s` ends with `suffix` (`""` ends every string) | |
| `fn substring(s: view Str, from: unsigned, to: unsigned) -> Str` | The bytes of `s` from index `from` up to, not including, `to`: `substring("hello", 1, 4)` is `"ell"`, and `from == to` gives `""` | panics `string slice <from>..<to> out of bounds (len=<n>)` unless `from <= to <= s.len()` |
| `fn trim(s: view Str) -> Str` | `s` without the whitespace at its start and at its end | |
| `fn replace(s: view Str, from: view Str, to: view Str) -> Str` | `s` with every occurrence of `from` replaced by `to`. The occurrences are found from the start, each after the one before it ends, and a `to` put in is not searched again: `replace("aaa", "aa", "b")` is `"ba"` | panics `replace: the text to replace is empty` when `from` is `""` |
| `fn repeat(s: view Str, n: unsigned) -> Str` | `s`, `n` times over: `repeat("ab", 3)` is `"ababab"`, and `n == 0` gives `""` | |

```pkn
import ::io;
import ::collections as c;

fn main() -> int {
  let p = c::trim("  /usr/local/bin/paykan \n");
  io::println("[" + p + "]");
  io::println(Str(c::startsWith(p, "/usr")) + " " + Str(c::endsWith(p, ".pkn")) + " "
      + Str(c::contains(p, "local")));
  match c::indexOf(p, "/bin") {
    i: unsigned {
      io::println(c::substring(p, 0, i) + " then " + c::substring(p, i + 1, unsigned(p.len())));
    }
    None { io::println("no /bin"); }
  }
  io::println(c::replace(p, "/", "\\"));
  io::println(c::repeat("=-", 5) + "=");
  return 0;
}
```

Output:

```
[/usr/local/bin/paykan]
True False True
/usr/local then bin/paykan
\usr\local\bin\paykan
=-=-=-=-=-=
```

A search is the runtime's: C's `memchr` finds each place the first byte of the delimiter
or the needle is, and `memcmp` compares the rest of it there. Each part is copied out once,
never built a byte at a time, so `split`, `lines`, `words`, `indexOf` and `contains` take
time linear in the length of `s` for a delimiter or a needle of a few bytes. A long one
costs more where its first byte recurs in `s`: at worst, the time is proportional to the
length of `s` times the needle's. A 10 MB string of a million lines splits in well under a
second. `startsWith` and `endsWith` compare the bytes of the prefix or the suffix only.

`replace` finds the occurrences as `split` does. It and `join` put their pieces together in
pairs, as a binary counter carries (two strings of one piece each make one of two, two of
two make one of four, and so on), so each byte of a result of k pieces is copied about
log2(k) times, whatever C's `realloc` does. Appending each piece to the result so far with
`+` would copy that result once per piece. Replacing each of the million line feeds of a
10 MB string takes about a second. `repeat` doubles its string, so its time is linear in the
length of the result.

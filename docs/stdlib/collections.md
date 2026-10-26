# `::collections`

The standard containers: `List<T>`, a growable array; `LinkedList<T>`, a doubly linked
list; `Stack<T>` and `Queue<T>`; the hash tables `HashMap<K, V>` and `HashSet<K>`; and the
priority queues `MinPriorityQueue<T>` and `MaxPriorityQueue<T>`. They are written in Paykan
on [`::memory`](memory.md)'s buffers: `LinkedList` is a generic class, and the others are
generic [structs](../language/11-structs.md), each holding the one object that keeps its
state. A program imports the module with
`import ::collections;` (or an alias, `import ::collections as c;`) and names a container
with its qualifier: `collections::List<int>()`.

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
  same = xs;                       // the same list: copies share its state
  same.pop();
  io::println(collections::listToString(xs) + " " + Str(xs.len()));
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

- **Copies of a container are the same container.** `List`, `Stack`, `Queue`, `HashMap`,
  `HashSet` and the priority queues are structs whose one field is the object that holds
  every element and count (a `Buffer`, a `LinkedList`, the hash table of a `HashMap` or a `HashSet`, or the heap), and a
  `LinkedList` is that object itself. So assigning a container, passing it or storing it
  in a field names the same container: a change through any of the names is seen through
  all of them, and `==` is True only for the same container. `copy()` (on a `List`) or
  `toList()` makes a new one.
- **A container is a struct**, so the rules of [structs](../language/11-structs.md) apply:
  through a `let` variable only its `view fn`s can be called (`'xs' is declared with 'let';
  its value cannot change, and 'push' is not a 'view fn'`), though a copy of it, or a plain
  parameter it is passed to, is the same container and can change it (`ys = xs;
  ys.push(2);` adds to `xs` too); a `view` of one calls only its `view fn`s; and it is not
  an `Obj`: where one is needed, put the container in a
  [`Box`](memory.md) (with `import ::memory as mem;`, `b = mem::Box(xs);`), an object
  whose `get()` is the container itself (`b.get().push(1);`), and its own
  `toString()` is a struct's, which shows its private field. `listToString` prints a list's
  elements. A `LinkedList` is a class, with a class's rules.
- **A container holds its own reference to each element**, whether the method that stores
  it takes the element by value (`pushFront(v: T)`, `put(k: K, v: V)`) or lent
  (`push(v: view T)`): an object element is shared with the container, and a value element
  is copied into it.
- **An element is lent as a `view` or handed over as a copy.** A method that reads an
  element and leaves it in the container does one of two things:
  - It lends the element as a `view` (`-> view T`), which stays the container's: `List`'s
    `at` and `last`, `LinkedList`'s `front` and `back`, the `peek` of `Stack`, `Queue` and
    the priority queues, and `HashMap`'s `at` and `atOr`. These are `view fn`s. The
    element is read in place, passed to a `view` parameter or bound to a `view` local
    (`f: view = fs.at(i);`), and a value element is copied out (`n = xs.at(0);`), but an
    object element is neither stored nor changed through it (`the result of 'at' is a
    'view'; cannot assign to its field 'n'`).
  - It hands over a copy the caller keeps (`-> T`): the `get` of `List`, `HashMap`,
    `Stack`, `Queue` and the priority queues, `HashMap`'s `getOr`, and `LinkedList`'s
    `getFront` and `getBack`. The copy is stored, returned and changed like any value of
    `T`: a value element (an `int`, a tuple, a struct) is the caller's alone, and an object
    element is shared with the container, so a change through the copy shows in the
    container, and the object stays the caller's after the container lets it go. Since a
    copy of an object could change an element, these are not `view fn`s, and a `view` of a
    container cannot call them (`'xs' is a 'view' parameter; 'get' is not a 'view fn'`):
    a `view` of a container changes nothing in it.

  A method that takes an element out (`pop`, `removeAt`, `dequeue`, `popFront`, ...)
  returns it, and a new list (`copy()`, `toList()`, `keys()`) holds its own references.
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
- **The methods that only read are `view fn`s**, the copying ones aside, so they can be
  called through a `view` parameter (`fn total(xs: view collections::List<int>)` may call
  `len()` and `at(i)`, not `push` or `get(i)`).
- The keys of a `HashMap` or `HashSet` implement the builtin trait
  [`Hashable`](../language/06-traits.md#hashable): `int`, `bool`, `char`, `Str`, a sized
  integer type (`u32`, `unsigned`, ...), or a class or a struct with an `impl Hashable for`
  block. A `float` key is rejected (`'float' does not implement trait 'Hashable', which
  'collections::HashMap' requires of 'K'`).

---

## `List<T>`

A growable array, kept in a [`memory::Buffer<T>`](memory.md). `push` and `pop` at
the end are amortised O(1); `insert` and `removeAt` move the elements after the index, and
`indexOf`, `contains` and `remove` walk the list.

| Method | Does | When it fails |
|--------|------|---------------|
| `List<T>()` | An empty list | |
| `view fn len() -> unsigned` | The number of elements | |
| `view fn isEmpty() -> bool` | Whether there are none | |
| `view fn at(i: unsigned) -> view T` | Element `i`, as a `view` | panics `List index <i> out of range (len=<n>)` unless `i < len()` |
| `fn get(i: unsigned) -> T` | A copy of element `i`, to keep | panics as `at` |
| `fn set(i: unsigned, v: view T)` | Replaces element `i` | panics as `at` |
| `fn push(v: view T)` | Appends `v` | |
| `fn pop() -> T` | Removes and returns the last element | panics `pop from an empty List` |
| `view fn last() -> view T` | The last element, as a `view` | panics `last of an empty List` |
| `fn insert(i: unsigned, v: view T)` | Inserts `v` before element `i`; `i == len()` appends | panics as `at` unless `i <= len()` |
| `fn removeAt(i: unsigned) -> T` | Removes and returns element `i` | panics as `at` |
| `view fn indexOf(v: view T) -> unsigned?` | The index of the first element `== v` | `None` when there is none |
| `view fn contains(v: view T) -> bool` | Whether some element is `== v` | |
| `fn remove(v: view T) -> bool` | Removes the first element `== v`; `True` if there was one | |
| `fn swap(i: unsigned, j: unsigned)` | Exchanges elements `i` and `j` | panics as `at` |
| `fn reverse()` | Reverses the order of the elements | |
| `fn clear()` | Removes every element | |
| `view fn copy() -> List<T>` | A new list of the same elements (shared, if they are objects) | |

### `listOf`

```pkn
fn listOf<T>(xs: view T[]) -> List<T>
```

A new list of the elements of an array or a slice, in order:
`collections::listOf(arr)` for `arr: int[4]` is a `List<int>`.

### `listToString`

```pkn
fn listToString<T: Show>(xs: view List<T>) -> Str
```

The elements' `toString()`, in order, as `[a, b, c]`; `[]` for an empty list.

## `LinkedList<T>`

A doubly linked list: both ends are O(1) to reach, add to and remove from. `contains`
and `remove` walk the list.

| Method | Does | When it fails |
|--------|------|---------------|
| `LinkedList<T>()` | An empty list | |
| `view fn len() -> unsigned` | The number of elements | |
| `view fn isEmpty() -> bool` | Whether there are none | |
| `fn pushFront(v: T)` | Adds `v` before the first element | |
| `fn pushBack(v: T)` | Adds `v` after the last element | |
| `fn popFront() -> T` | Removes and returns the first element | panics `popFront on an empty LinkedList` |
| `fn popBack() -> T` | Removes and returns the last element | panics `popBack on an empty LinkedList` |
| `view fn front() -> view T` | The first element, as a `view` | panics `front on an empty LinkedList` |
| `view fn back() -> view T` | The last element, as a `view` | panics `back on an empty LinkedList` |
| `fn getFront() -> T` | A copy of the first element, to keep | panics `getFront on an empty LinkedList` |
| `fn getBack() -> T` | A copy of the last element, to keep | panics `getBack on an empty LinkedList` |
| `view fn contains(v: view T) -> bool` | Whether some element is `== v` | |
| `fn remove(v: view T) -> bool` | Removes the first element `== v`; `True` if there was one | |
| `fn clear()` | Removes every element | |
| `view fn toList() -> List<T>` | The elements from first to last, in a new `List<T>` | |

Each node owns the next one and holds the previous one as a
[`weak` reference](../language/16-memory.md#weak-references), so two neighbours form no
reference cycle and dropping the list frees every node. `pushFront` and `pushBack` take
the element by value (an object is then shared with the list), like `HashMap.put`.

## `Stack<T>` and `Queue<T>`

A `Stack` is last in, first out; a `Queue` first in, first out. Both are kept in a
`LinkedList<T>`, and every operation is O(1).

| `Stack<T>` | `Queue<T>` | Does | When it fails |
|------------|------------|------|---------------|
| `Stack<T>()` | `Queue<T>()` | An empty stack, an empty queue | |
| `fn push(v: T)` | `fn enqueue(v: T)` | Adds `v`: on top of the stack, at the back of the queue | |
| `fn pop() -> T` | `fn dequeue() -> T` | Removes and returns the top, the front | panics `pop from an empty Stack`, `dequeue from an empty Queue` |
| `view fn peek() -> view T` | `view fn peek() -> view T` | The element `pop` or `dequeue` would return, as a `view` | panics `peek at an empty Stack`, `peek at an empty Queue` |
| `fn get() -> T` | `fn get() -> T` | A copy of that element, to keep | panics `get from an empty Stack`, `get from an empty Queue` |
| `view fn len() -> unsigned` | the same | The number of elements | |
| `view fn isEmpty() -> bool` | the same | Whether there are none | |
| `fn clear()` | the same | Removes every element | |
| `view fn toList() -> List<T>` | the same | The elements in a new list, top to bottom, front to back | |

## `HashMap<K: Hashable, V>` and `HashSet<K: Hashable>`

Hash tables with separate chaining. Two keys are the same key when they are `==`, and keys
that are `==` must have the same `hash()`. A class key overrides `equals` and implements
`Hashable` to match. `put`, `at`, `get`, `containsKey` and `remove` are O(1) on average. The
table keeps at most three entries for every four buckets, and doubles when it would hold
more. The order of `keys()` is the table's, not the insertion order.

A map keeps each key's hash from when it was put, so an object key must not change in a
way that changes its `hash()` or `equals` while it is in the map: it would no longer be
found.

| `HashMap<K, V>` | Does | When it fails |
|-----------------|------|---------------|
| `HashMap<K, V>()` | An empty map | |
| `view fn len() -> unsigned` | The number of keys | |
| `view fn isEmpty() -> bool` | Whether there are none | |
| `fn put(k: K, v: V) -> bool` | Maps `k` to `v`, replacing the value of a key it has; `True` if `k` is a new key | |
| `view fn at(k: view K) -> view V` | The value of `k`, as a `view` | panics `HashMap.at of a key it does not have` |
| `view fn atOr(k: view K, otherwise: view V) -> view V` | The value of `k`, or `otherwise` if it is not a key, as a `view` | |
| `fn get(k: view K) -> V` | A copy of the value of `k`, to keep | panics `HashMap.get of a key it does not have` |
| `fn getOr(k: view K, otherwise: V) -> V` | A copy of the value of `k`, or `otherwise` if it is not a key | |
| `view fn containsKey(k: view K) -> bool` | Whether `k` is a key | |
| `fn remove(k: view K) -> bool` | Removes `k` and its value; `True` if it was a key | |
| `fn clear()` | Removes every key | |
| `view fn keys() -> List<K>` | The keys, in a new list | |
| `view fn values() -> List<V>` | The values, in a new list, in the order of `keys()` | |

| `HashSet<K>` | Does |
|--------------|------|
| `HashSet<K>()` | An empty set |
| `view fn len() -> unsigned` | The number of keys |
| `view fn isEmpty() -> bool` | Whether there are none |
| `fn add(k: K) -> bool` | Adds `k`; `True` if it was not in the set |
| `view fn contains(k: view K) -> bool` | Whether `k` is in the set |
| `fn remove(k: view K) -> bool` | Removes `k`; `True` if it was in the set |
| `fn clear()` | Removes every key |
| `view fn toList() -> List<K>` | The keys, in a new list, in the table's order |

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
| `fn push(v: view T, priority: int)` | Adds `v` with `priority` | |
| `fn pop() -> T` | Removes and returns the next element | panics `pop on an empty priority queue` |
| `view fn peek() -> view T` | The next element, as a `view` | panics `peek on an empty priority queue` |
| `fn get() -> T` | A copy of the next element, to keep | panics `get on an empty priority queue` |
| `view fn peekPriority() -> int` | The next element's priority | panics `peekPriority on an empty priority queue` |
| `fn clear()` | Removes every element | |

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

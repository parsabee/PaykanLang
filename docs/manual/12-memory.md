# Memory

PaykanLang has no garbage collector and no `free`. Memory is managed by **automatic
reference counting** (ARC): the compiler counts the references to each object, and frees
the object the moment the count drops to zero. This chapter explains what that means for
your programs, how to check them for leaks, the one kind of leak ARC cannot prevent, and
the `mov` operator.

## Values and references

Types come in two kinds:

- **Value types**: `int`, `float`, `bool`, `char` and enums. They are copied on assignment
  and never allocated, so there is nothing to manage.
- **Reference types**: class instances, `Str`, arrays, tuples, `File`, `Error` and optional
  values. They live on the heap, and variables, fields and array elements hold references
  to them.

Each heap object carries a **reference count**. Storing a reference (in a variable, a field,
an array or a tuple, or by passing it to a function) increments the count; when a reference
goes away (a variable goes out of scope, a field or element is overwritten, an object holding
it is destroyed) the count is decremented. When it reaches zero, the object is destroyed:
its own references are released, which may destroy further objects in turn, and its memory
is freed. You never write any of this; the compiler inserts the counting.

The consequences you can rely on:

- An object lives exactly as long as something refers to it, and is freed **immediately**
  after: there are no collection pauses, and destruction happens at a predictable point.
- You cannot free something too early or twice, or use an object after it was freed.
- Resources are released deterministically: a `File` is closed as soon as its last reference
  goes away.
- Destruction does not recurse on the call stack, so dropping a linked list of a million
  nodes is safe.

## Checking for leaks: `--track-heap`

`paykan --track-heap` runs a program with a counting allocator and, when it exits, prints
statistics to standard error:

```text
$ paykan --track-heap hello.pkn
Hello, world!
paykan heap stats:
  total allocations : 5
  total frees       : 5
  total reallocs    : 0
  live blocks       : 0
  live bytes        : 0
  peak bytes        : 108
```

`live blocks` is the line to read: it counts the allocations that were never freed, and is
`0` for a program that does not leak. (The other numbers depend on the program and the
compiler version.) The test suite runs every example in this manual, and every sample
program in the repository, this way.

## Reference cycles

Reference counting has one blind spot. If two objects refer to each other, each keeps the
other's count above zero, so neither is ever freed, even after the program has dropped every
reference to both. Such a **cycle** leaks:

```text
class Person {
  name: Str;
  friend: Person?;
  fn __init__(name: Str) { self.name = name; }
}

fn main() -> int {
  a = Person("Ada");
  b = Person("Grace");
  a.friend = b;
  b.friend = a;          // a -> b -> a: a cycle
  println(a.name + " and " + b.name + " are friends");
  return 0;
}
```

```text
$ paykan --track-heap friends.pkn
Ada and Grace are friends
paykan heap stats:
  ...
  live blocks       : 10
  live bytes        : 202
  ...
  ** LEAK: 10 block(s) / 202 byte(s) not freed **
```

PaykanLang currently has no cycle collector and no weak references (both are on the
roadmap). Until then, avoid cycles among long-lived objects, or break a cycle yourself
before the objects become unreachable by setting one of the links to `None`:

```pkn
class Person {
  name: Str;
  friend: Person?;
  fn __init__(name: Str) { self.name = name; }
}

fn main() -> int {
  a = Person("Ada");
  b = Person("Grace");
  a.friend = b;
  b.friend = a;
  println(a.name + " and " + b.name + " are friends");
  b.friend = None;       // break the cycle: now a and b can be freed
  return 0;
}
```

Output:

```
Ada and Grace are friends
```

This version ends with `live blocks : 0`. Structures where links only point one way, such
as a list where each node refers to the next, or a tree where parents refer to children but
not back, never form cycles and need no care.

## Moving with `mov`

Earlier releases had a `mov` keyword that handed a reference on without touching its count.
It is being retired: `mov s` now means the same as `s`, and the compiler will infer
ownership transfers on its own. New code should not use it.

## Threads

The reference counting in this release is not atomic: it assumes a single thread, which is
all the language offers today. Thread-safe reference counting is part of the language's
plans for concurrency.

The reference chapter, [Memory model](../language/08-memory-model.md), describes the
mechanism in detail. Next: [The paykan command](13-the-paykan-command.md).

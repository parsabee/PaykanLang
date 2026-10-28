# Arrays

An array `T[n]` holds exactly `n` values of one type `T`. Its length is part of its type,
and it is a **value**: it is stored inline, in its variable, its field or the array that
holds it, and assigning it copies its elements. A function takes an array of any length
through a slice parameter, `view T[]` or `inout T[]`. This chapter shows how to create,
read and change arrays, how to collect a varying number of values in one, and gives the
`tasks` project a `TaskList` that holds all its tasks; then it gives the precise rules.

Storage that grows is the standard library's: see `List<T>` in
[Collections](../stdlib/collections.md) and `Buffer<T>` in [Memory](../stdlib/memory.md).

## Creating arrays

The type of an array of `n` values of type `T` is written `T[n]`. An array literal lists its
elements in square brackets; its element type is inferred from them, and its length is the
number of elements:

```pkn
import ::io;

fn main() -> int {
  let primes = [2, 3, 5, 7];           // int[4]
  let names: Str[2] = ["Ada", "Grace"];
  let sizes: float[_] = [1.5, 2, 4];   // float[3]: _ takes the literal's length
  let zeros = [0; 6];                  // int[6]: six copies of 0
  io::println(Str(primes.len()) + " primes, " + Str(names.len()) + " names, "
          + Str(sizes.len()) + " sizes, " + Str(zeros.len()) + " zeros");
  io::println("the first prime is " + Str(primes[0]));
  return 0;
}
```

Output:

```
4 primes, 2 names, 3 sizes, 6 zeros
the first prime is 2
```

The length is part of the type: an `int[4]` is not an `int[3]`, and a literal for a `T[n]`
has exactly `n` elements (`names: Str[2] = ["Ada"]` is an error). Write `_` for a length to
take it from the initializer, as `sizes` does. An array has at least one element, so `[]` is
an error. The elements of a literal must have a common type: `int`s and `float`s mix into
`float`s, and objects of different classes give their nearest common ancestor.

`[v; n]` is an array of `n` copies of `v`, where `n` is a number written out, at least 1
(`[0; n]` with a variable `n` is an error). It is how you make an array that the program
fills in afterwards, as the next section does: `[0; 6]` is six zeros, `[""; 3]` three empty
strings, and `flags: bool[4] = [False; 4]` four `False`s. With a declared type, `v` takes
its element type, so `ratios: float[3] = [0; 3]` holds `0.0`s.

## Reading and writing

| Operation      | Meaning                                                    |
|----------------|------------------------------------------------------------|
| `a.len()`      | the number of elements                                     |
| `a[i]`         | the element at index `i`, counting from 0                  |
| `a[i] = v`     | replaces the element at index `i`                          |

An array's length never changes. Every index is checked: a constant one when compiling
(`primes[4]` is an error), any other when the program runs, where an index outside `0` to
`len() - 1` is a panic: the program stops with a message such as `paykan: array index 5 out
of bounds (len=3)`.

You walk an array with a [`for` loop](02-basics.md#for) over its indices:

```pkn
import ::io;

fn main() -> int {
  squares = [0; 5];
  for i in 0..squares.len() {
    squares[i] = (i + 1) * (i + 1);
  }
  squares[0] = 100;

  sum = 0;
  for i in 0..squares.len() {
    sum = sum + squares[i];
  }
  io::println(Str(squares.len()) + " squares, sum " + Str(sum));
  return 0;
}
```

Output:

```
5 squares, sum 154
```

There are no other built-in array operations (no `insert`, `remove` or sorting), but they
are a few lines each, as the project below shows.

## Arrays are values

An array is stored in its variable, like an `int`, and assigning it copies every element.
A plain parameter, such as `xs: int[3]`, gets a copy too.

To let a function work on an array of any length, give it a **slice** parameter, written
`view T[]` or `inout T[]`. A slice takes an array of any length with elements of type `T`,
and `len()` tells how long it is. A `view` slice reads the caller's array; an `inout` one
changes its elements in place:

```pkn
import ::io;

fn doubleAll(xs: inout int[]) {
  i = 0;
  while i < xs.len() {
    xs[i] = xs[i] * 2;
    i = i + 1;
  }
}

fn join(xs: view int[], sep: Str) -> Str {
  out = "";
  i = 0;
  while i < xs.len() {
    if i > 0 { out = out + sep; }
    out = out + Str(xs[i]);
    i = i + 1;
  }
  return out;
}

fn main() -> int {
  a = [1, 2, 3];
  let b = a;             // a copy
  doubleAll(a);
  io::println(join(a, ", ") + " / " + join(b, ", "));
  io::println(join([7, 8, 9, 10], "-"));
  return 0;
}
```

Output:

```
2, 4, 6 / 1, 2, 3
7-8-9-10
```

Two things to note:

- `==` is not defined on arrays. Write a loop to compare elements.
- `io::println` does not take an array, so a helper such as `join` is the way to show its
  elements.

## Keeping a count

Since an array's length is fixed when it is created, a program that collects a number of
values it does not know in advance gives the array a **capacity**, the most it will hold,
and keeps a **count** of the slots in use beside it. Adding a value writes the slot at the
count and increments the count; removing the last value decrements it:

```pkn
import ::io;

fn main() -> int {
  stack = [0; 4];            // room for 4
  count = 0;
  n = 1;
  while n <= 3 {
    stack[count] = n * 10;
    count = count + 1;
    n = n + 1;
  }
  count = count - 1;         // drop the top value
  io::println(Str(count) + " left, top " + Str(stack[count - 1]) + ", room for "
          + Str(stack.len() - count) + " more");
  return 0;
}
```

Output:

```
2 left, top 20, room for 2 more
```

Before adding, check that `count` is below `len()`: the program decides what to do when the
array is full, rather than letting the index panic.

When there is no sensible capacity, use a list from the standard library instead of an
array: `collections::List<T>` keeps its own count and grows as values are pushed (see
[Collections](../stdlib/collections.md)).

## Arrays of arrays and of objects

The element type can be any type, including another array type or a class. `int[2][3]` is
two rows of three `int`s, indexed `grid[row][column]`:

```pkn
import ::io;

class City {
  name: Str;
  people: int;
}

impl City {
  fn __init__(name: Str, people: int) { self.name = name; self.people = people; }
}

fn main() -> int {
  grid: int[2][3] = [[1, 2, 3], [4, 5, 6]];
  grid[1][2] = 60;
  io::println("row 1 has " + Str(grid[1].len()) + " cells, the last is " + Str(grid[1][2]));

  let cities = [City("Tehran", 9000000), City("Lyon", 520000), City("Oslo", 700000)];
  biggest = cities[0];
  i = 1;
  while i < cities.len() {
    if cities[i].people > biggest.people { biggest = cities[i]; }
    i = i + 1;
  }
  io::println("largest: " + biggest.name);
  return 0;
}
```

Output:

```
row 1 has 3 cells, the last is 60
largest: Tehran
```

Every row of an array of arrays has the same length. An array of objects holds references
to them, so copying the array copies the references: both arrays share the same objects.

Arrays are **invariant**: if `Dog` extends `Animal`, a `Dog[2]` is still not an
`Animal[2]`. To keep different subclasses in one array, give it the base type, as in
`pets: Animal[2] = [Dog("Rex"), Cat("Tom")]`.

## Project: the task list

`TaskList` keeps the tasks in an array with room for eight, and counts the ones in use.
Its constructor fills the array with a blank task, which the slots past the count keep until
a task is added there. Tasks are numbered from 1 for the user, so the methods convert
between those numbers and array indices, and they check the number before using it rather
than letting a bad one panic:

```pkn
import ::io;

class Task {
  title: Str;
  done: bool;
  view fn toString() -> Str {
    return (if self.done then "[x] " else "[ ] ") + self.title;
  }
}

impl Task {
  fn __init__(title: Str) { self.title = title; self.done = False; }
}

class TaskList {
  tasks: Task[8];   // room for eight tasks
  count: int;       // tasks[0] to tasks[count - 1] are in use

  // Adds a task; False if the list is full.
  fn add(title: Str) -> bool {
    if self.count == self.tasks.len() { return False; }
    self.tasks[self.count] = Task(title);
    self.count = self.count + 1;
    return True;
  }

  // Marks task number `n` (counting from 1) done; False if there is none.
  fn complete(n: int) -> bool {
    if n < 1 || n > self.count { return False; }
    self.tasks[n - 1].done = True;
    return True;
  }

  view fn pending() -> int {
    count = 0;
    i = 0;
    while i < self.count {
      if !self.tasks[i].done { count = count + 1; }
      i = i + 1;
    }
    return count;
  }

  // Drops the finished tasks, moving the others down, and returns how many there were.
  fn clearDone() -> int {
    kept = 0;
    i = 0;
    while i < self.count {
      if !self.tasks[i].done {
        self.tasks[kept] = self.tasks[i];
        kept = kept + 1;
      }
      i = i + 1;
    }
    let removed = self.count - kept;
    self.count = kept;
    return removed;
  }

  view fn show() {
    i = 0;
    while i < self.count {
      io::println(Str(i + 1) + ". " + self.tasks[i].toString());
      i = i + 1;
    }
    io::println(Str(self.pending()) + " of " + Str(self.count) + " to do");
  }
}

impl TaskList {
  fn __init__() {
    self.tasks = [Task(""); 8];
    self.count = 0;
  }
}

fn main() -> int {
  let list = TaskList();
  list.add("buy milk");
  list.add("fix the leak");
  list.add("call mum");
  list.complete(2);
  if !list.complete(7) { io::println("there is no task 7"); }
  list.show();
  io::println("cleared " + Str(list.clearDone()));
  list.show();
  return 0;
}
```

Output:

```
there is no task 7
1. [ ] buy milk
2. [x] fix the leak
3. [ ] call mum
2 of 3 to do
cleared 1
1. [ ] buy milk
2. [ ] call mum
2 of 2 to do
```

`[Task(""); 8]` calls `Task("")` once, and a class value is a reference to its object, so
the eight slots hold the same blank task. That is fine here: `add` puts a new task in a slot
before anything changes it, and `complete` only reaches the slots in use. An array whose
elements each need their own object from the start is filled in a loop.

---

## Quick Summary

- Type notation: `T[n]`, `n` an integer literal of at least 1 (`int[3]`, `Str[2]`,
  `Point[4]`).
- Arrays of arrays: `int[2][3]` is two `int[3]`s, indexed `grid[i][j]`.
- Literals: `[e1, e2, e3]` is an array of its own length; `[1, 2, 3]` is an `int[3]`.
- `[v; n]` is `n` copies of `v`, `n` an integer literal of at least 1: `[0; 8]` is an
  `int[8]` of zeros. `v` is evaluated once, so `[Node(); 3]` holds one object three times.
- A length written `_` (`x: int[_] = …`) is the initializer's.
- An array is a value: assigning it, or passing it to a plain parameter, copies its
  elements. It is not an `Obj`.
- Its one method is `.len()`. `xs[i]` reads an element and `xs[i] = v` assigns one; every
  index is bounds-checked.
- A `view T[]` or `inout T[]` parameter is a **slice**: it takes an array of any length.
- An array holds at most 16384 elements.

---

## Creation

### Literals

```pkn
import ::io;

fn main() -> int {
  let nums: int[3] = [10, 20, 30];
  let words = ["hello", "world"];  // a Str[2]
  let mixed = [1, 2.5];            // a float[2]: the int promotes
  io::println(Str(nums[2]) + " " + words[1] + " " + Str(mixed[0]));
  return 0;
}
```

Output:

```
30 world 1
```

On its own, a literal is an array of its elements' common type: `int` and `float` give
`float`, and objects of different classes give their nearest common ancestor
(`[Dog(), Cat()]` is an `Animal[2]`). Elements without one are an error: `array literal has
inconsistent element types: 'int' and 'Str'`.

A literal that flows into a `T[n]` (a declaration's type, an assignment, an argument, a
return, a field) has exactly `n` elements, and each takes the element type `T` where it
needs it: `xs: float[2] = [1, 2]`, `ns: Node?[2] = [None, n]`.

- `xs: int[3] = [1, 2]` is an error: `array literal has 2 element(s), but 'int[3]' has 3`.
- An array has at least one element, so `[]` has no type: `an empty array literal has no
  type: an array has at least one element`.

### Inferred lengths

In a variable declaration with an initializer, a length can be `_`: it is the
initializer's. An array literal gives its element count, and its first element the lengths
of a nested `_`. Anything else gives the lengths of the array it is.

```pkn
import ::io;

fn fill<T>(v: T) -> T[3] { return [v; 3]; }

fn main() -> int {
  let x: int[_][_] = [[1, 2, 3], [4, 5, 6]];   // an int[2][3]
  let z: float[_] = fill<float>(1.5);      // a float[3]
  io::println(Str(x.len()) + " " + Str(x[0].len()) + " " + Str(z.len()));
  return 0;
}
```

Output:

```
2 3 3
```

A `_` anywhere else is an error (`a length can be '_' only in a variable declaration with an
initializer, which gives it`), and so is an initializer that gives no length: `x: int[_] =
3` (`'int[_]' takes its lengths from a fixed-size array of its shape, not from 'int'`).

### Repeated values

The count `n` of `[v; n]` is an integer literal of at least 1, and `[v; n]` types as the
literal `[v, ..., v]` of `n` elements would: on its own `[0; 3]` is an `int[3]`; flowing
into a `T[n]`, `v` takes the element type `T` (`[0; 4]` into a `float[4]` is four `0.0`s,
`[None; 8]` into an `int?[8]` eight `None`s); and it gives a `_` length its count.

```pkn
import ::io;

fn main() -> int {
  let zeros: float[4] = [0; 4];      // the int 0 becomes 0.0
  let slots: int?[8] = [None; 8];
  grid: int[_][_] = [[0; 3]; 2];     // an int[2][3]
  grid[1][2] = 7;
  io::println(Str(zeros.len()) + " " + Str(slots[5] == None) + " " + Str(grid[0][2]) + " "
          + Str(grid[1][2]));
  return 0;
}
```

Output:

```
4 True 0 7
```

`v` is evaluated once, and each element is a copy of its value. A value of a reference type
(a class, `Str`) is a reference, so the elements all name one object: `[Counter(0);
3]` holds a single counter three times, and a change made through one element shows through
the others until an element is assigned another object. A tuple is copied into each element,
and the copies share the objects it holds: `[(0, Counter(0)); 3]` is three tuples naming one
counter.

```pkn
import ::io;

class Counter { n: int; }

fn main() -> int {
  cs = [Counter(0); 3];    // one Counter, three times
  cs[0].n = 5;
  cs[1] = Counter(1);      // cs[1] now holds a second Counter
  io::println(Str(cs[2].n) + " " + Str(cs[1].n));
  return 0;
}
```

Output:

```
5 1
```

- The count is a literal: `[0; n]` is an error (`unexpected identifier 'n'; expected the
  number of copies, an integer literal`), and so are `[0; -1]` and `[0; 3.0]`.
- `[0; 0]` is an error: `an array has at least one element: the count must be 1 or more`.
- The count is the literal's length: `xs: int[3] = [0; 4]` is an error (`array literal has
  4 element(s), but 'int[3]' has 3`), and so is `[0; 20000]` (`array literal of type
  'int[20000]' is too big: a fixed-size array holds at most 16384 elements`).
- `v` must fit the element type: `xs: Str[2] = [1; 2]` is an error (`array literal element
  of type 'int' does not match the element type 'Str' of 'Str[2]'`).

### Filling an array

An array starts with the elements of its initializer, and a loop can set them afterwards.
A program that collects a varying number of elements uses a capacity and a count, as in
[Keeping a count](#keeping-a-count): `[v; n]` fills the capacity with a placeholder that the
count has not reached yet.

```pkn
import ::io;

fn main() -> int {
  squares: int[5] = [0; 5];
  i = 0;
  while i < squares.len() {
    squares[i] = i * i;
    i = i + 1;
  }
  io::println(Str(squares[3]) + " " + Str(squares[4]));
  return 0;
}
```

Output:

```
9 16
```

```pkn
import ::io;

fn main() -> int {
  divisors: int[16] = [0; 16];   // room for 16
  count = 0;                     // divisors[0] to divisors[count - 1] are in use
  d = 1;
  while d <= 36 && count < divisors.len() {
    if 36 % d == 0 {
      divisors[count] = d;
      count = count + 1;
    }
    d = d + 1;
  }
  io::println(Str(count) + " divisors, the largest " + Str(divisors[count - 1]));
  return 0;
}
```

Output:

```
9 divisors, the largest 36
```

For an array of objects the placeholder is one blank object (`tasks: Task[8] =
[Task(""); 8]`, as in [the task list](#project-the-task-list)), which the unused slots
share: a slot gets its own object when it is assigned one, before the program changes it.
An optional element type (`Task?[8]`) can start as `[None; 8]` instead.

---

## Length and Indexing

`xs.len()` is the array's length, an `int`. Indices are zero-based `int`s:

```pkn
import ::io;

fn main() -> int {
  a = [10, 20, 30];
  a[1] = 99;
  io::println(Str(a[1]) + " " + Str(a.len()));
  return 0;
}
```

Output:

```
99 3
```

Every index is checked. A constant one is checked when compiling (`index 3 is out of bounds
for 'int[3]' (its indexes are 0 to 2)`), any other when running: an index out of bounds
aborts the program with `paykan: array index 3 out of bounds (len=3)`.

---

## Copying

An array is a value: assigning one copies its elements, so the copy and the original change
independently.

```pkn
import ::io;

fn main() -> int {
  xs: int[3] = [1, 2, 3];
  ys = xs;                 // a copy
  ys[0] = 10;
  xs = [xs[2], xs[0], xs[1]];
  io::println(Str(xs[0]) + " " + Str(ys[0]) + " " + Str(xs.len()));
  return 0;
}
```

Output:

```
3 10 3
```

The elements are part of the value, so a `let` array's elements cannot be assigned, and
neither can a `view`'s:

```pkn
fn main() -> int {
  let xs: int[2] = [1, 2];
  xs[0] = 5;
  return 0;
}
```

Error:

```
'xs' is declared with 'let'; its elements are part of its value and cannot be assigned
```

An `inout` local names the array itself (`r: inout = xs;`).

### Arrays of references

An element can be a scalar (`int`, `float`, `bool`, `char`, a sized numeric type such as
`u8`, which takes an 8-byte slot like `int`), an optional of one (`int?`,
held inline, `None` when zeroed), an enum, a reference (a class, `Str`, an optional of one), a
tuple or a struct (held inline, see [Tuples](10-tuples.md) and [Structs](11-structs.md)), a
function value or another array. An element of a reference type is
a reference, as a variable of that type is: copying the array copies the references, so the
copy and the original share their elements' objects, and an element of a `view` array is a
`view` too.

```pkn
import ::io;

class Counter { n: int; }

fn main() -> int {
  let cs: Counter[2] = [Counter(1), Counter(2)];
  let copy = cs;           // the same two counters
  copy[0].n = 10;
  names: Str[2] = ["ann", "bob"];
  names[1] = names[0] + "!";
  io::println(Str(cs[0].n) + " " + names[1]);
  return 0;
}
```

Output:

```
10 ann!
```

An array's element type is exact: a `Dog[2]` is not an `Animal[2]`.

---

## Iteration

Use `while` with an index:

```pkn
import ::io;

class Point {
  x: int;
  y: int;
  view fn toString() -> Str { return "(" + Str(self.x) + "," + Str(self.y) + ")"; }
}

fn main() -> int {
  let pts = [Point(0, 0), Point(1, 2), Point(-3, 4)];
  i = 0;
  while i < pts.len() {
    io::println(pts[i].toString());
    i = i + 1;
  }
  return 0;
}
```

Output:

```
(0,0)
(1,2)
(-3,4)
```

---

## Arrays of Arrays

An array of arrays puts its first length on its first index, as in C: `int[2][3]` is two
`int[3]`s, indexed `grid[i][j]` with `i < 2` and `j < 3`. Every row has the same length, and
a row is a value too: `row = grid[1];` copies it.

```pkn
import ::io;

fn main() -> int {
  grid: int[2][3] = [[1, 2, 3], [4, 5, 6]];
  row = grid[1];
  row[0] = 40;
  grid[0][2] = 30;
  io::println(Str(grid[1][0]) + " " + Str(row[0]) + " " + Str(grid[0][2]));
  return 0;
}
```

Output:

```
4 40 30
```

An array holds at most 16384 elements, counted across its rows (`int[2][3]` holds 6):
`xs: int[20000]` is an error (`which is too big: a fixed-size array holds at most 16384
elements`).

---

## Fields

A class's field can be an array: it is held inside its object, and `__init__` assigns it like
any other field (a struct's field cannot be one yet). Its elements change only where the
object can (`self.cells[0][1] = 5;` is not allowed in a `view fn`).

```pkn
import ::io;

class Board { cells: char[3][3]; }

fn main() -> int {
  let b = Board([['.'; 3]; 3]);
  b.cells[1][1] = 'x';
  io::println(Str(b.cells[1][1]) + " " + Str(b.cells.len()));
  return 0;
}
```

Output:

```
x 3
```

---

## Parameters and Results

A parameter can be an array in every mode (see [Parameter modes](03-functions.md#parameter-modes-view-and-inout)).
A plain one is the function's own copy, a `view` one reads the caller's array, and an
`inout` one changes it. A result is returned as a copy (`-> int[3]`; a `view` or `inout`
result of an array is not supported yet).

```pkn
import ::io;

fn bumped(xs: int[3]) -> int[3] {
  xs[0] = xs[0] + 100;     // the function's own copy
  return xs;
}

fn clear(xs: inout int[3]) {
  xs = [0; 3];             // the caller's array
}

fn main() -> int {
  a: int[3] = [1, 2, 3];
  let b = bumped(a);
  clear(a);
  io::println(Str(a[0]) + " " + Str(b[0]));
  return 0;
}
```

Output:

```
0 101
```

### Slices

A `view T[]` or `inout T[]` parameter is a **slice**. It takes an array of any length with
element type `T`, and reaches the caller's elements in place; `xs.len()` is that array's
length. A `view` slice only reads. An `inout` one assigns elements (`xs[i] = v;`), but not
the whole slice (`'xs' is a slice; assign its elements ('xs[i] = ...')`).

```pkn
import ::io;

fn sum(xs: view int[]) -> int {
  t = 0;
  i = 0;
  while i < xs.len() {
    t = t + xs[i];
    i = i + 1;
  }
  return t;
}

fn scale(xs: inout int[], by: int) {
  i = 0;
  while i < xs.len() {
    xs[i] = xs[i] * by;
    i = i + 1;
  }
}

fn main() -> int {
  a: int[3] = [1, 2, 3];
  scale(a, 10);
  let grid: int[2][2] = [[1, 2], [3, 4]];
  io::println(Str(sum(a)) + " " + Str(sum([5, 6])) + " " + Str(sum(grid[1])));
  return 0;
}
```

Output:

```
60 11 7
```

- An argument is an array (a variable, a row such as `grid[1]`, a field), a literal, which
  is an array of its own length, or a slice passed on.
- The array's element type must be the slice's exactly: an `int[3]` is not a `float[]`.
- `int[3][]` is a slice of `int[3]`s: it takes an `int[n][3]` of any `n`.
- A slice can only be indexed, measured with `len()` and passed on. It cannot be stored,
  copied, returned or printed (`a slice ('int[]') can only be indexed, measured with 'len()'
  or passed to a 'view' or 'inout' array parameter`).
- `T[]` is a slice only there: a variable, a field, a result or any other type written
  `int[]` is an error (`variable 'x' has type 'int[]': a slice can only be a 'view' or
  'inout' parameter, and an array has a length ('int[3]')`). The length is suggested only
  where a fixed-size array would do; a struct's or a variant's field and a tuple element
  say instead that they cannot be an array yet (`..., and a struct field cannot be an array
  yet`).
- A generic `xs: view T[]` takes `T` from the array's elements.

The program's entry point takes its command-line arguments as a slice,
`fn main(args: view Str[]) -> int` (see [Functions](03-functions.md) and
[Input and output](../stdlib/io.md)).

---

## Limits for now

An array is only ever a local variable, a parameter, a result or a class's field. It cannot
be a struct's field (`a struct field cannot be a fixed-size array yet`), a tuple element, an
optional (`int[3]?`), a type argument (`Box<int[3]>`) or a `match` arm, and
it does not convert to `Obj`; each is an error such as `tuple element 0: a fixed-size array
('int[2]') can only be a local variable, a parameter, a result or a class field for now`.

- Arrays have no `==` (`operator '==' is not defined for types 'int[2]' and 'int[2]'`).
- An array is not printed and has no `toString`; print its elements.
- `len()` is its only method (`'int[2]' has no method 'toString' with these arguments (a
  fixed-size array has 'len()')`).
- An element cannot be named by an `inout` local or passed to an `inout` parameter yet (`an
  array element cannot be passed to 'inout' parameter 'n' yet`).

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Inconsistent element types | `[1, "two"]` |
| Empty literal | `[]` anywhere |
| Literal of the wrong length | `xs: int[3] = [1, 2]`, `xs: int[3] = [0; 4]` |
| Element that does not fit the element type | `xs: Str[2] = [1, 2]`, `xs: Str[2] = [1; 2]` |
| Length that is not a positive literal | `int[0]` (at parse time) |
| Count that is not a positive literal | `[0; 0]`, `[0; n]`, `[0; -1]`, `[0; 3.0]` (at parse time) |
| `_` length with nothing to give it | `fn f(x: int[_])`, `x: int[_] = 3` |
| Too many elements | `int[20000]`, `int[200][100]`, `[0; 20000]` |
| Index out of bounds | `xs[3]` for an `int[3]`: a constant index when compiling, any other at runtime (`paykan: array index 3 out of bounds (len=3)`) |
| Subscript on a non-array | `x[0]` where `x` is not an array, a slice or a `Str` |
| Element assigned through a `let` or a `view` | `xs[0] = 5` with `let xs` or `xs: view int[]` |
| Slice anywhere but a parameter | `x: int[]`, a field or a result of type `int[]` |
| Slice element type mismatch | an `int[3]` passed to a `view float[]` |
| Array where it cannot be | a struct field, a tuple element, an optional, a type argument, a `match` arm, an `Obj` |

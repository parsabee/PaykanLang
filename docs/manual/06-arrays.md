# Arrays

An array holds a fixed number of values of one type. In this chapter `tasks` gets a
`TaskList` that holds all the tasks.

## Creating arrays

The type of an array of `n` values of type `T` is written `T[n]`. An array literal lists its
elements in square brackets; its element type is inferred from them, and its length is the
number of elements:

```pkn
fn main() -> int {
  primes = [2, 3, 5, 7];               // int[4]
  names: Str[2] = ["Ada", "Grace"];
  sizes: float[_] = [1.5, 2, 4];       // float[3]: _ takes the literal's length
  println(Str(primes.len()) + " primes, " + Str(names.len()) + " names, "
          + Str(sizes.len()) + " sizes");
  println("the first prime is " + Str(primes[0]));
  return 0;
}
```

Output:

```
4 primes, 2 names, 3 sizes
the first prime is 2
```

The length is part of the type: an `int[4]` is not an `int[3]`, and a literal for a `T[n]`
has exactly `n` elements (`names: Str[2] = ["Ada"]` is an error). Write `_` for a length to
take it from the initializer, as `sizes` does. An array has at least one element, so `[]` is
an error. All elements of a literal must have the same type, except that `int`s and `float`s
mix into `float`s.

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

There is no `for` loop, so you walk an array with `while` and an index:

```pkn
fn main() -> int {
  squares = [0, 0, 0, 0, 0];
  i = 0;
  while (i < squares.len()) {
    squares[i] = (i + 1) * (i + 1);
    i = i + 1;
  }
  squares[0] = 100;

  sum = 0;
  i = 0;
  while (i < squares.len()) {
    sum = sum + squares[i];
    i = i + 1;
  }
  println(Str(squares.len()) + " squares, sum " + Str(sum));
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
fn doubleAll(xs: inout int[]) {
  i = 0;
  while (i < xs.len()) {
    xs[i] = xs[i] * 2;
    i = i + 1;
  }
}

fn join(xs: view int[], sep: Str) -> Str {
  out = "";
  i = 0;
  while (i < xs.len()) {
    if (i > 0) { out = out + sep; }
    out = out + Str(xs[i]);
    i = i + 1;
  }
  return out;
}

fn main() -> int {
  a = [1, 2, 3];
  b = a;                 // a copy
  doubleAll(a);
  println(join(a, ", ") + " / " + join(b, ", "));
  println(join([7, 8, 9, 10], "-"));
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
- `println` does not take an array, so a helper such as `join` is the way to show its
  elements.

## Keeping a count

Since an array's length is fixed when it is created, a program that collects a number of
values it does not know in advance gives the array a **capacity**, the most it will hold,
and keeps a **count** of the slots in use beside it. Adding a value writes the slot at the
count and increments the count; removing the last value decrements it:

```pkn
fn main() -> int {
  stack = [0, 0, 0, 0];      // room for 4
  count = 0;
  n = 1;
  while (n <= 3) {
    stack[count] = n * 10;
    count = count + 1;
    n = n + 1;
  }
  count = count - 1;         // drop the top value
  println(Str(count) + " left, top " + Str(stack[count - 1]) + ", room for "
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

## Arrays of arrays and of objects

The element type can be any type, including another array type or a class. `int[2][3]` is
two rows of three `int`s, indexed `grid[row][column]`:

```pkn
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
  println("row 1 has " + Str(grid[1].len()) + " cells, the last is " + Str(grid[1][2]));

  cities = [City("Tehran", 9000000), City("Lyon", 520000), City("Oslo", 700000)];
  biggest = cities[0];
  i = 1;
  while (i < cities.len()) {
    if (cities[i].people > biggest.people) { biggest = cities[i]; }
    i = i + 1;
  }
  println("largest: " + biggest.name);
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
    if (self.count == self.tasks.len()) { return False; }
    self.tasks[self.count] = Task(title);
    self.count = self.count + 1;
    return True;
  }

  // Marks task number `n` (counting from 1) done; False if there is none.
  fn complete(n: int) -> bool {
    if (n < 1 || n > self.count) { return False; }
    self.tasks[n - 1].done = True;
    return True;
  }

  view fn pending() -> int {
    count = 0;
    i = 0;
    while (i < self.count) {
      if (!self.tasks[i].done) { count = count + 1; }
      i = i + 1;
    }
    return count;
  }

  // Drops the finished tasks, moving the others down, and returns how many there were.
  fn clearDone() -> int {
    kept = 0;
    i = 0;
    while (i < self.count) {
      if (!self.tasks[i].done) {
        self.tasks[kept] = self.tasks[i];
        kept = kept + 1;
      }
      i = i + 1;
    }
    removed = self.count - kept;
    self.count = kept;
    return removed;
  }

  view fn show() {
    i = 0;
    while (i < self.count) {
      println(Str(i + 1) + ". " + self.tasks[i].toString());
      i = i + 1;
    }
    println(Str(self.pending()) + " of " + Str(self.count) + " to do");
  }
}

impl TaskList {
  fn __init__() {
    blank = Task("");
    self.tasks = [blank, blank, blank, blank, blank, blank, blank, blank];
    self.count = 0;
  }
}

fn main() -> int {
  list = TaskList();
  list.add("buy milk");
  list.add("fix the leak");
  list.add("call mum");
  list.complete(2);
  if (!list.complete(7)) { println("there is no task 7"); }
  list.show();
  println("cleared " + Str(list.clearDone()));
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

The reference chapter is [Arrays](../language/05-arrays.md). Next:
[Tuples](07-tuples.md).

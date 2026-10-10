# Arrays

An array is an ordered, growable sequence of values of one type. In this chapter `tasks`
gets a `TaskList` that holds all the tasks.

## Creating arrays

The type of an array of `T` is written `T[]`. An array literal lists its elements in square
brackets, and the element type is inferred from them:

```pkn
fn main() -> int {
  primes = [2, 3, 5, 7];               // int[]
  names: Str[] = ["Ada", "Grace"];
  empty: float[] = [];
  println(Str(primes.len()) + " primes, " + Str(names.len()) + " names, "
          + Str(empty.len()) + " floats");
  println("the first prime is " + Str(primes[0]));
  return 0;
}
```

Output:

```
4 primes, 2 names, 0 floats
the first prime is 2
```

An empty literal `[]` has nothing to infer the element type from, so it needs a type
annotation: `empty = [];` is an error (`cannot infer element type of empty array literal`).
All elements of a literal must have the same type.

## Reading, writing, growing and shrinking

| Operation      | Meaning                                                    |
|----------------|------------------------------------------------------------|
| `a.len()`      | the number of elements                                     |
| `a[i]`         | the element at index `i`, counting from 0                  |
| `a[i] = v`     | replaces the element at index `i`                          |
| `a.push(v)`    | appends `v` at the end                                     |
| `a.pop()`      | removes the last element and returns it                    |

Every index is checked when the program runs. An index outside `0` to `len() - 1`, or a
`pop()` on an empty array, is a panic: the program stops with a message such as
`paykan: array index 5 out of bounds (len=3)`.

There is no `for` loop, so you walk an array with `while` and an index:

```pkn
fn main() -> int {
  squares: int[] = [];
  i = 1;
  while (i <= 5) {
    squares.push(i * i);
    i = i + 1;
  }
  squares[0] = 100;
  last = squares.pop();

  sum = 0;
  i = 0;
  while (i < squares.len()) {
    sum = sum + squares[i];
    i = i + 1;
  }
  println("popped " + Str(last) + ", " + Str(squares.len()) + " left, sum " + Str(sum));
  return 0;
}
```

Output:

```
popped 25, 4 left, sum 129
```

There are no other built-in array operations yet (no `insert`, `remove`, slicing or
sorting), but they are a few lines each. To remove elements, build a new array from the
ones you keep, as the project below does.

## Arrays are shared

Like objects, arrays live on the heap and variables hold references to them. Assigning an
array or passing it to a function shares it, so changes made through one name are visible
through the other:

```pkn
fn doubleAll(xs: int[]) {
  i = 0;
  while (i < xs.len()) {
    xs[i] = xs[i] * 2;
    i = i + 1;
  }
}

fn join(xs: int[], sep: Str) -> Str {
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
  b = a;
  b.push(4);
  doubleAll(a);
  println(join(a, ", "));
  println(Str(a == b) + " " + Str(a == [2, 4, 6, 8]));
  return 0;
}
```

Output:

```
2, 4, 6, 8
True False
```

Two things to note:

- `==` on arrays compares identity, not contents: `a == b` is `True` because they are the
  same array, but an equal-looking literal is a different array. Write a loop to compare
  elements.
- Printing an array with `println` shows only an address and the length
  (`Array@0x...[len=4]`), so a helper such as `join` is the way to show its elements.

## Arrays of arrays and of objects

The element type can be any type, including another array type (`int[][]`) or a class.
Each inner array is its own array, with its own length:

```pkn
class City {
  name: Str;
  people: int;
  fn __init__(name: Str, people: int) { self.name = name; self.people = people; }
}

fn main() -> int {
  grid: int[][] = [[1, 2, 3], [4, 5], [6]];
  grid[1].push(50);
  println("row 1 has " + Str(grid[1].len()) + " cells, the last is " + Str(grid[1][2]));

  cities = [City("Tehran", 9000000), City("Lyon", 520000)];
  cities.push(City("Oslo", 700000));
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
row 1 has 3 cells, the last is 50
largest: Tehran
```

Arrays are **invariant**: if `Dog` extends `Animal`, a `Dog[]` is still not an `Animal[]`.
To keep different subclasses in one array, give it the base type, as in
`pets: Animal[] = [Dog("Rex"), Cat("Tom")]`.

## Project: the task list

`TaskList` keeps the tasks in an array. Tasks are numbered from 1 for the user, so the
methods convert between those numbers and array indices, and they check the number before
using it rather than letting a bad one panic:

```pkn
class Task {
  title: Str;
  done: bool;
  fn __init__(title: Str) { self.title = title; self.done = False; }
  view fn toString() -> Str {
    return (if self.done then "[x] " else "[ ] ") + self.title;
  }
}

class TaskList {
  tasks: Task[];

  fn __init__() { self.tasks = []; }

  fn add(title: Str) { self.tasks.push(Task(title)); }

  // Marks task number `n` (counting from 1) done; False if there is none.
  fn complete(n: int) -> bool {
    if (n < 1 || n > self.tasks.len()) { return False; }
    self.tasks[n - 1].done = True;
    return True;
  }

  view fn pending() -> int {
    count = 0;
    i = 0;
    while (i < self.tasks.len()) {
      if (!self.tasks[i].done) { count = count + 1; }
      i = i + 1;
    }
    return count;
  }

  // Drops the finished tasks and returns how many there were.
  fn clearDone() -> int {
    kept: Task[] = [];
    i = 0;
    while (i < self.tasks.len()) {
      if (!self.tasks[i].done) { kept.push(self.tasks[i]); }
      i = i + 1;
    }
    removed = self.tasks.len() - kept.len();
    self.tasks = kept;
    return removed;
  }

  view fn show() {
    i = 0;
    while (i < self.tasks.len()) {
      println(Str(i + 1) + ". " + self.tasks[i].toString());
      i = i + 1;
    }
    println(Str(self.pending()) + " of " + Str(self.tasks.len()) + " to do");
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

# Tuples and structs

A tuple groups a fixed number of values, which may have different types, into one value.
Their main use is returning more than one value from a function. In this chapter `tasks`
uses them to report statistics and to split lines of text. A struct is a tuple whose
elements have names and can be changed; the chapter ends with one.

## Tuple types and values

A tuple type lists its element types in parentheses, `(int, Str)`, and a tuple value lists
its elements the same way. Elements are read with `.0`, `.1` and so on:

```pkn
import ::io;

fn main() -> int {
  pair: (int, Str) = (7, "seven");
  point = (2.5, -1.0, True);          // (float, float, bool)
  nested = ("a", (1, 2));             // (Str, (int, int))
  io::println(pair.1 + " is " + Str(pair.0));
  io::println(Str(point.0 + point.1));
  io::println(Str(nested.1.0 + nested.1.1));
  io::println(nested);
  return 0;
}
```

Output:

```
seven is 7
1.5
3
(a, (1, 2))
```

- A tuple has at least two elements; `(x)` is just `x` in parentheses.
- The index after the dot must be a number written in the source, and the compiler checks
  it: `pair.2` on a pair is an error.
- `io::println` prints a tuple as its elements in parentheses, strings without quotes.
- There is no implicit `int` to `float` promotion inside a tuple literal:
  `t: (float, int) = (1, 2)` is an error, so write `(1.0, 2)`.

Tuples are **immutable**: `pair.0 = 8` is an error. To change one, build a new tuple.
(Objects inside a tuple can still change, as objects always can.)

## Returning several values

A function returns a tuple by listing the values, and the caller takes it apart with
**destructuring**: several names on the left of `=`, separated by commas.

```pkn
import ::io;

fn divmod(a: int, b: int) -> (int, int) {
  return (a / b, a % b);
}

fn minMax(xs: view int[]) -> (int, int) {
  lo = xs[0];
  hi = xs[0];
  i = 1;
  while i < xs.len() {
    if xs[i] < lo { lo = xs[i]; }
    if xs[i] > hi { hi = xs[i]; }
    i = i + 1;
  }
  return (lo, hi);
}

fn main() -> int {
  q, r = divmod(17, 5);
  io::println("17 = 5 * " + Str(q) + " + " + Str(r));

  lo, hi = minMax([4, -2, 9, 3]);
  io::println("range " + Str(lo) + ".." + Str(hi));

  _, rem = divmod(100, 7);          // _ skips an element
  io::println("100 % 7 = " + Str(rem));

  q, r = divmod(9, 2);              // assigns the existing q and r
  a: float, b: int = (1, 2);        // annotated targets; 1 becomes a float
  io::println(Str(q) + " " + Str(r) + " " + Str(a) + " " + Str(b));
  return 0;
}
```

Output:

```
17 = 5 * 3 + 2
range -2..9
100 % 7 = 2
4 1 1 2
```

Destructuring follows the usual rules for each name: a new name is declared with the
element's type, an existing one is assigned, and `name: Type` always declares a new
variable. The number of names must match the tuple's size, and a name can appear only once.
Nested patterns such as `a, (b, c) = ...` are not supported; destructure in two steps.

## Tuples as values

A tuple is a value, like an `int` or an array: it can be stored in a variable, a field or an
array, and passed to and returned from functions, and each of these copies it. Its elements
are held right where the tuple is, so a tuple of numbers costs no allocation at all. A tuple
is not an `Obj`, though: `o: Obj = (1, 2)` is an error. `==` and `!=` compare tuples element
by element, with each element compared the way `==` compares it on its own (strings by
content):

```pkn
import ::io;

fn main() -> int {
  scores: (Str, int)[3] = [("ada", 92), ("alan", 85), ("grace", 97)];
  i = 0;
  while i < scores.len() {
    name, score = scores[i];
    io::println(name + ": " + Str(score));
    i = i + 1;
  }
  io::println(Str(scores[0] == ("ada", 92)) + " " + Str(scores[0] == ("ada", 91)));
  return 0;
}
```

Output:

```
ada: 92
alan: 85
grace: 97
True False
```

Tuples have no ordering operators, and `match` cannot take a tuple apart: destructure it
first.

## Project: statistics and splitting lines

`tasks` will soon store each task as one line of text, the title and the priority separated
by a `|`, such as `fix the leak|high`. A `splitOnce` function returns the two parts as a
tuple, and a `stats` function counts done and pending tasks in one pass:

```pkn
import ::io;

class Task {
  title: Str;
  done: bool;
}

impl Task {
  fn __init__(title: Str, done: bool) { self.title = title; self.done = done; }
}

// Splits `s` at the first `sep`: ("a", "b") for "a|b", (s, "") when there is none.
fn splitOnce(s: Str, sep: char) -> (Str, Str) {
  left = "";
  i = 0;
  while i < s.len() && s[i] != sep {
    left = left + Str(s[i]);
    i = i + 1;
  }
  right = "";
  i = i + 1;
  while i < s.len() {
    right = right + Str(s[i]);
    i = i + 1;
  }
  return (left, right);
}

// (done, pending)
fn stats(tasks: view Task[]) -> (int, int) {
  done = 0;
  i = 0;
  while i < tasks.len() {
    if tasks[i].done { done = done + 1; }
    i = i + 1;
  }
  return (done, tasks.len() - done);
}

fn main() -> int {
  title, priority = splitOnce("fix the leak|high", '|');
  io::println("title '" + title + "', priority '" + priority + "'");
  io::println(splitOnce("no separator", '|'));

  tasks = [Task("a", True), Task("b", False), Task("c", True)];
  done, pending = stats(tasks);
  io::println(Str(done) + " done, " + Str(pending) + " pending");
  return 0;
}
```

Output:

```
title 'fix the leak', priority 'high'
(no separator, )
2 done, 1 pending
```

## Structs: named fields

When a group of values has a shape worth naming, declare a **struct**: a value with named
fields. `tasks` keeps its statistics in one:

```pkn
import ::io;

struct Stats {
  done: int;
  pending: int;
}

impl Stats {
  view fn total() -> int { return self.done + self.pending; }
  fn count(isDone: bool) {
    if isDone { self.done = self.done + 1; } else { self.pending = self.pending + 1; }
  }
}

fn main() -> int {
  s = Stats(0, 0);                  // the fields, in order
  s.count(True);
  s.count(False);
  s.count(True);
  s.pending = s.pending + 10;       // a field is assigned in place
  t = s;                            // a copy
  t.done = 0;
  io::println(s);
  io::println(Str(s.total()) + " " + Str(t.total()) + " " + Str(s == t));
  return 0;
}
```

Output:

```
Stats(done: 2, pending: 11)
13 11 False
```

- `Stats(0, 0)` builds a struct from its fields in order; an `impl` block can declare an
  `__init__` instead, and holds the struct's methods, as a class's does.
- A struct is a value like a tuple: `t = s` copies it, and `t.done = 0` leaves `s` alone.
  Unlike a tuple, its fields have names and can be assigned, and in a method `self` is the
  variable the method was called on, so `s.count(True)` changes `s`.
- A struct has no superclass and no `vtable`: it is not an object, and `==` compares two
  structs field by field. `io::println` prints the struct's name with each field.

Use a class when values must share one object that several names refer to, or when
subclasses and `match` are needed; use a struct for a small group of fields that is
copied around like a number. A struct can be generic, like a class (chapter 9):
`struct Pair<A, B> { a: A; b: B; }`, used as `Pair<int, Str>` or inferred from
`Pair(1, "one")`.

The reference chapters are [Tuples](../language/09-tuples.md) and
[Structs](../language/14-structs.md). Next: [Optionals and None](08-optionals.md).

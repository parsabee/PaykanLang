# Enums and match

An **enum** is a type with a fixed set of named values, such as the days of the week or
the priority of a task. `match` is the statement that branches on them, and on much more.
This chapter covers both and gives tasks in `tasks` a priority.

## Declaring an enum

```pkn
enum Direction { North, East, South, West }

fn turnRight(d: Direction) -> Direction {
  match d {
    North { return Direction::East; }
    East  { return Direction::South; }
    South { return Direction::West; }
    West  { return Direction::North; }
  }
}

fn name(d: Direction) -> Str {
  match d {
    North { return "north"; }
    East  { return "east"; }
    South { return "south"; }
    West  { return "west"; }
  }
}

fn main() -> int {
  d = Direction::North;
  i = 0;
  route = name(d);
  while (i < 4) {
    d = turnRight(d);
    route = route + " -> " + name(d);
    i = i + 1;
  }
  println(route);
  println(Str(d == Direction::North));
  return 0;
}
```

Output:

```
north -> east -> south -> west -> north
True
```

The values, called **variants**, are written `Enum::Variant` in expressions. An enum is a
type of its own:

- It is not an `int`: there is no conversion between an enum and a number in either
  direction, and no arithmetic on enums.
- The only operators are `==` and `!=`, between values of the same enum. There is no
  ordering (`<`).
- It cannot be printed directly (`println(d)` is an error), so a function such as `name`
  above turns it into a string.
- It is a plain value like an `int`: copied on assignment, never allocated. Enums can be
  stored in variables, fields and arrays, and passed to and returned from functions.

A trailing comma after the last variant is allowed, which suits enums written one variant
per line.

## match

`match` takes a value and runs the first **arm** whose pattern fits it. What a pattern can
be depends on the type of the value; there are four modes.

### Matching enum variants

On an enum, each arm names a variant, **without** the `Enum::` prefix, as in `turnRight`
above. A `match` that names every variant is **exhaustive**, which the compiler knows: that
is why `turnRight` needs no `return` after its `match`. If you add a fifth direction, the
compiler reports that these functions no longer always return a value, pointing you at
every place that needs updating. To handle only some variants, end with the wildcard arm
`_`, which matches anything:

```pkn
enum Day { Mon, Tue, Wed, Thu, Fri, Sat, Sun }

fn isWeekend(d: Day) -> bool {
  match d {
    Sat { return True; }
    Sun { return True; }
    _   { return False; }
  }
}

fn main() -> int {
  println(Str(isWeekend(Day::Sun)) + " " + Str(isWeekend(Day::Wed)));
  return 0;
}
```

Output:

```
True False
```

### Matching values

On an `int`, `float`, `char`, `bool` or `Str`, the arms are literals, compared by value
(strings by content):

```pkn
fn command(word: Str) -> Str {
  match word {
    "add"  { return "adding"; }
    "done" { return "completing"; }
    "list" { return "listing"; }
    _      { return "unknown command '" + word + "'"; }
  }
}

fn main() -> int {
  println(command("add"));
  println(command("list"));
  println(command("frobnicate"));
  n = 2;
  match n * 3 {
    0 { println("zero"); }
    6 { println("six"); }
    _ { println("something else"); }
  }
  return 0;
}
```

Output:

```
adding
listing
unknown command 'frobnicate'
six
```

A `bool` match with both `True` and `False` arms is exhaustive. On numbers and strings
there are always values you did not list, so a `match` that must produce a result needs a
`_` arm.

### Matching classes

On an object, the arms name classes and match the object's exact runtime class, as you saw
in [Classes](04-classes.md#checking-the-class-at-run-time). An arm can bind the object to a
name with the narrower type: `c: Circle { ... }`.

### Matching optionals

On an optional value (`int?`, `Str?`, `Task?`) the arms tell a present value from `None`.
You met this form when parsing numbers in [the last chapter](03-strings-and-conversions.md);
[Optionals](08-optionals.md) covers it.

### Rules common to every mode

- `match` is a statement, not an expression: arms do their work by assigning, calling
  functions or returning. Each arm's body is a block with its own scope.
- The first arm that matches runs, and only that one. There is no fallthrough.
- `_` must be the last arm.
- If no arm matches, nothing happens and execution continues after the `match`. A
  wildcard or full coverage is only required where the compiler needs every path to
  produce a result, such as at the end of a function that returns a value.

## Project: task priorities

Each task gets a priority. The enum has three variants; `priorityName` turns one into text,
and `parsePriority` reads the words a user would type. Since an unknown word must still
give a priority, it falls back to `Normal`:

```pkn
enum Priority { Low, Normal, High }

fn priorityName(p: Priority) -> Str {
  match p {
    Low    { return "low"; }
    Normal { return "normal"; }
    High   { return "high"; }
  }
}

fn parsePriority(word: Str) -> Priority {
  match word {
    "low"  { return Priority::Low; }
    "high" { return Priority::High; }
    "!"    { return Priority::High; }
    _      { return Priority::Normal; }
  }
}

class Task {
  title: Str;
  done: bool;
  priority: Priority;

  fn __init__(title: Str, priority: Priority) {
    self.title = title;
    self.done = False;
    self.priority = priority;
  }

  fn complete() { self.done = True; }

  view fn toString() -> Str {
    box = if self.done then "[x]" else "[ ]";
    marker = if self.priority == Priority::High then " !" else "";
    return box + " " + self.title + marker;
  }
}

fn main() -> int {
  a = Task("water the plants", parsePriority("low"));
  b = Task("fix the leak", parsePriority("!"));
  c = Task("read a book", parsePriority("whenever"));
  b.complete();
  println(a);
  println(b);
  println(c.title + " has priority " + priorityName(c.priority));
  return 0;
}
```

Output:

```
[ ] water the plants
[x] fix the leak !
read a book has priority normal
```

The reference has the full rules in [Enums](../language/03-enums.md) and
[Match statements](../language/07-match-statements.md). Next: [Arrays](06-arrays.md).

# Modules and imports

So far every program has been one file. Real programs are split into **modules**, and in
PaykanLang every `.pkn` file is a module. This chapter explains how imports work, then
splits `tasks` into a small multi-file project.

## Files are modules

A module's name is its path relative to the **source root**, the directory that holds the
main file (the one you pass to `paykan`), with `/` written as `::` and the `.pkn` dropped.
In this project:

```text
project/
├── main.pkn            the main file; project/ is the source root
└── geometry/
    ├── shapes.pkn      module geometry::shapes
    └── units.pkn       module geometry::units
```

`main.pkn` imports a module with `import geometry::shapes;` and then refers to what the
module declares through its last name segment: `shapes::Circle`, `shapes::area(...)`.
Here are the two files. The comment on the first line of each is not part of the language;
it only tells you (and the test suite) where the file lives:

```pkn
// file: geometry/shapes.pkn
class Circle {
  r: float;
}

impl Circle {
  fn __init__(r: float) { self.r = r; }
}

enum Kind { Round, Square }

fn area(c: Circle) -> float {
  return 3.14159 * c.r * c.r;
}
```

```pkn
// file: main.pkn
import geometry::shapes;

fn main() -> int {
  c: shapes::Circle = shapes::Circle(2.0);
  k = shapes::Kind::Round;
  println("area " + Str(shapes::area(c)));
  println(Str(k == shapes::Kind::Round));
  return 0;
}
```

Output:

```
area 12.5664
True
```

- Everything declared at the top level of a module (functions, classes and enums) can be
  imported, except the names starting with `_`, which are the module's own. There is no
  `export` keyword.
- Imported names are always qualified: a type is `shapes::Circle`, a variant is
  `shapes::Kind::Round`. Your own module's names stay unqualified.
- Import paths always start from the source root, also inside the modules in
  subdirectories: `geometry/units.pkn` imports its sibling as `import geometry::shapes;`.
- Imports are resolved when the program is compiled; there is no package registry, and
  nothing outside the source root is searched (except system imports, below).

## Import forms

```text
import geometry::shapes;              // qualifier: shapes
import geometry::shapes as geo;       // qualifier: geo
import geometry::{shapes, units};     // two modules of one directory
import geometry::{shapes as s, units};
```

An **alias** (`as geo`) replaces the qualifier. It is useful for long names and required
when two modules would otherwise share one: `import a::util; import b::util;` is an error,
because `util::` must name exactly one module. Write `import b::util as butil;`.

The **brace form** imports several modules from one directory, each with an optional alias.
It is shorthand for one `import` per module; the braces list *modules*, not the names inside
them.

## Rules worth knowing

**Imports are not transitive.** If `main` imports `list`, and `list` imports `task`, then
`main` cannot write `task::Task` without importing the `task` module itself. It can still use
the `Task` objects that `list` hands it: call their methods, read their fields and pass them
back. It just cannot *name* the type, in an annotation for instance, until it imports the
module that declares it.

**Type names are global, function names are not.** Within one program (the main file and
everything it imports, directly or not), two modules cannot declare different classes or
enums with the same name. Functions are per module: `a::helper()`, `b::helper()` and your own
`helper()` are three different functions.

**No cycles.** If `a` imports `b`, `b` cannot import `a`, directly or through other
modules. Move what both need into a third module.

**Generics.** A module's generic classes and functions are imported with the rest of it:
`shapes::Box<int>(7)`, `b: shapes::Box<Str>`, `shapes::first(xs)`. Their bodies use what
`shapes` declares and imports, whatever your module calls those names. A `Box<int>` is one
class however many modules use it, so the values pass freely between them (see
[Generics](09-generics.md#how-generic-code-is-checked)).

**System imports.** A leading `::`, as in `import ::collections;`, looks for the module in
the standard library instead of the source root. The standard library ships with
PaykanLang: `::collections` has lists, linked lists, stacks, queues, hash maps, hash sets
and priority queues (see [the standard library](../language/13-standard-library.md)). Set
`$PAYKAN_STDLIB` to use another directory of system modules. The built-in functions and
types (`println`, `Str`, `open`, `File`, `Stdin` and the rest) need no import.

## Project: `tasks` as a multi-file program

`tasks` has outgrown a single file. It now has this layout:

```text
tasks/
├── main.pkn          command handling
└── tasks/
    ├── task.pkn      the Priority enum and the Task class
    ├── text.pkn      string helpers
    └── list.pkn      TaskList
```

`tasks/task.pkn` holds the data model:

```pkn
// file: tasks/task.pkn
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
    _      { return Priority::Normal; }
  }
}

class Task {
  title: Str;
  done: bool;
  priority: Priority;

  view fn toString() -> Str {
    box = if self.done then "[x] " else "[ ] ";
    marker = if self.priority == Priority::High then " !" else "";
    return box + self.title + marker;
  }
}

impl Task {
  fn __init__(title: Str, priority: Priority) {
    self.title = title;
    self.done = False;
    self.priority = priority;
  }
}
```

`tasks/text.pkn` collects the string helpers from earlier chapters:

```pkn
// file: tasks/text.pkn
fn isSpace(c: char) -> bool {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

fn slice(s: Str, from: int, to: int) -> Str {
  out = "";
  i = from;
  while i < to {
    out = out + Str(s[i]);
    i = i + 1;
  }
  return out;
}

fn trim(s: Str) -> Str {
  start = 0;
  end = s.len();
  while start < end && isSpace(s[start]) { start = start + 1; }
  while end > start && isSpace(s[end - 1]) { end = end - 1; }
  return slice(s, start, end);
}

// Splits at the first `sep`: (s, "") when there is none.
fn splitOnce(s: Str, sep: char) -> (Str, Str) {
  i = 0;
  while i < s.len() && s[i] != sep { i = i + 1; }
  if i == s.len() { return (s, ""); }
  return (slice(s, 0, i), slice(s, i + 1, s.len()));
}
```

`tasks/list.pkn` imports `tasks::task`, so it names the class `task::Task`:

```pkn
// file: tasks/list.pkn
import tasks::task;

class TaskList {
  tasks: task::Task[8];   // room for eight tasks
  count: int;             // tasks[0] to tasks[count - 1] are in use

  // Adds a task; False if the list is full.
  fn add(title: Str, priority: task::Priority) -> bool {
    if self.count == self.tasks.len() { return False; }
    self.tasks[self.count] = task::Task(title, priority);
    self.count = self.count + 1;
    return True;
  }

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

  view fn show() {
    i = 0;
    while i < self.count {
      println(Str(i + 1) + ". " + self.tasks[i].toString());
      i = i + 1;
    }
    println(Str(self.pending()) + " of " + Str(self.count) + " to do");
  }
}

impl TaskList {
  fn __init__() {
    self.tasks = [task::Task("", task::Priority::Normal); 8];
    self.count = 0;
  }
}
```

Finally `main.pkn` imports all three, the list module under the alias `tl`. It runs a few
commands of the form `add <title>|<priority>` and `done <number>`; in the next chapter they
will come from the command line and standard input instead:

```pkn
// file: main.pkn
import tasks::{task, text, list as tl};

fn run(tasks: tl::TaskList, line: Str) {
  command, rest = text::splitOnce(text::trim(line), ' ');
  match command {
    "add" {
      title, priority = text::splitOnce(rest, '|');
      if !tasks.add(title, task::parsePriority(priority)) { println("the list is full"); }
    }
    "done" {
      match int(rest) {
        n: int { if !tasks.complete(n) { println("no task " + rest); } }
        None   { println("not a task number: " + rest); }
      }
    }
    "list" { tasks.show(); }
    _      { println("unknown command: " + command); }
  }
}

fn main() -> int {
  tasks = tl::TaskList();
  script = ["add buy milk", "add fix the leak|high", "add call mum|low",
            "done 1", "done 9", "list"];
  i = 0;
  while i < script.len() {
    run(tasks, script[i]);
    i = i + 1;
  }
  return 0;
}
```

Output:

```
no task 9
1. [x] buy milk
2. [ ] fix the leak !
3. [ ] call mum
2 of 3 to do
```

Run it from anywhere by naming the main file: `paykan tasks/main.pkn`. The source root is
always the main file's directory, and the compiled modules are cached in
`tasks/.paykan_cache/`, so after a change only the modules affected by it are recompiled.

The reference chapter is [Modules](../language/06-modules.md). Next:
[Files and I/O](11-files-and-io.md).

# Files and I/O

Programs talk to the world through files, command-line arguments, standard input and
standard error. This chapter covers each, and finishes `tasks`: it now keeps its tasks in a
file and takes commands from the command line.

## Opening files

Files, like printing, come from the standard library's `::io` module. `io::open(path, mode)`
opens a file. The mode is `"r"` to read, `"w"` to write (creating the file, or emptying an
existing one) or `"a"` to append. Opening can fail, so `open` returns a **result**: a
`Result<io::File>` from the `::result` module, holding an `error` and a `value`. The value is
an `io::File?`, the file when the open succeeded and `None` when it did not, and you tell the
two apart with `match`:

```pkn
import ::io;
import ::result as res;

fn main() -> int {
  w = io::open("notes.txt", "w");
  match w.value {
    f: io::File {
      f.write("first line\n");
      f.write("second line\n");
    }
    None { io::printerrln(w.error.message()); }
  }

  r = io::open("no/such/dir/notes.txt", "r");
  match r.value {
    f: io::File { io::println("opened?"); }
    None { io::println("error: " + res::name(r.error.code) + ": " + r.error.message()); }
  }
  return 0;
}
```

Output:

```
error: NotFound: No such file or directory
```

A result's `error` is a `res::Error`: a `code`, one of the standard `res::ErrorCode`s
(`Ok`, `NotFound`, `PermissionDenied`, `EndOfFile` and the rest), and a message.
`error.message()` is the message, here the operating system's, and `res::name(code)` is the
code's name. The error's code is `Ok` exactly when the value is there; `r.isOk()` tests it.
Relative paths are relative to the directory the program runs in, not to the source file.

`f.write(s)` reports failure the same way: it returns a `res::Error`, whose code is `Ok` when
it wrote. The example ignores it; `if !f.write(s).isOk() { ... }` checks it.

There is no `close`: a file is closed automatically when the last reference to it goes
away. The result holds one, so above `notes.txt` stays open until `w` goes away at the end
of `main`. Everything written is on disk once the file is closed; to read back a file you
wrote, write it in a function of its own, which closes it when it returns (the next
example does).

## Reading

An `io::File` has three reading methods. Each returns a `Result<Str>`: its `value` is a
`Str?`, the data read, or `None` when there is nothing more to read (its error is then
`EndOfFile`).

| Method          | Reads                                                         |
|-----------------|---------------------------------------------------------------|
| `readln()`      | the next line, **including** its `\n` if it has one           |
| `readbytes(n)`  | the next `n` bytes (fewer at the end of the file)             |
| `read()`        | everything that is left                                       |

A line-by-line loop matches each read's `value`:

```pkn
import ::io;

// Writes `text` to the file at `path`; False if it could not.
fn writeFile(path: Str, text: Str) -> bool {
  w = io::open(path, "w");
  match w.value {
    f: io::File { return f.write(text).isOk(); }
    None {
      io::printerrln(w.error.message());
      return False;
    }
  }
}   // the file is closed here, when `w` and `f` go away

fn main() -> int {
  if !writeFile("shopping.txt", "eggs\nflour\nsugar\n") { return 1; }

  r = io::open("shopping.txt", "r");
  match r.value {
    f: io::File {
      n = 0;
      while True {
        match f.readln().value {
          line: Str {
            n = n + 1;
            io::print(Str(n) + ": " + line);   // line ends in \n already
          }
          None { break; }                   // end of file
        }
      }
    }
    None { io::printerrln(r.error.message()); return 1; }
  }
  return 0;
}
```

Output:

```
1: eggs
2: flour
3: sugar
```

Note `return 1;` in `main` when the file cannot be written or opened: `main` can return
from anywhere, and a non-zero exit status tells the caller something failed.

## Standard input

`io::stdin()` is an `io::File` connected to the program's standard input. It is always open,
and has the same reading methods:

```pkn
import ::io;

fn main() -> int {
  total = 0;
  bad = 0;
  while True {
    match io::stdin().readln().value {
      line: Str {
        // drop the trailing newline before parsing
        digits = "";
        i = 0;
        while i < line.len() && line[i] != '\n' {
          digits = digits + Str(line[i]);
          i = i + 1;
        }
        match int(digits) {
          n: int { total = total + n; }
          None   { bad = bad + 1; }
        }
      }
      None { break; }
    }
  }
  io::println("total " + Str(total) + ", " + Str(bad) + " bad line(s)");
  return 0;
}
```

Input:

```
10
20
oops
12
```

Output:

```
total 42, 1 bad line(s)
```

Run it as `printf '10\n20\n' | paykan sum.pkn`, or type lines and end with Ctrl-D.

## Standard error

`io::printerr(x)` and `io::printerrln(x)` work like `io::print` and `io::println` but write
to standard error. Use them for diagnostics, so that they do not mix with the program's real
output when it is redirected to a file or a pipe.

## Command-line arguments

To receive its command-line arguments, `main` takes a `view Str[]`, a slice of the
arguments (see [Arrays](06-arrays.md#arrays-are-values)):

```pkn
import ::io;

fn main(args: view Str[]) -> int {
  if args.len() < 2 {
    io::printerrln("usage: greet <name>...");
    return 2;
  }
  i = 1;
  while i < args.len() {
    io::println("Hello, " + args[i] + "!");
    i = i + 1;
  }
  return 0;
}
```

Output (arguments: Ada Grace):

```
Hello, Ada!
Hello, Grace!
```

`args[0]` is the program itself: the path of the source file under `paykan run`, and the
path of the executable for a program made with `paykan build`. The real arguments start at
`args[1]`. With `paykan run`, everything after the source file belongs to the program, so
`paykan greet.pkn --help` passes `--help` to `greet`, not to `paykan`.

The arguments are a `view`, so `main` reads them but does not keep them: an argument can be
printed, compared, parsed (`int(args[1])`), passed to a `view` parameter or joined into a new
string, but `name = args[1];` is an error (`'args' is a 'view' parameter; it cannot be
stored, only read or passed to a 'view' parameter`). To keep one, take a string of the
program's own: `name = args[1].toString();`.

## Project: `tasks` remembers

The last piece of `tasks` is a new module, `tasks/store.pkn`, that saves the list to a file
and loads it back. Each task is one line, `<done>|<priority>|<title>`:

```text
0|high|fix the leak
1|normal|buy milk
```

`main.pkn` now reads its command from the command line: `add <title> [high|low]`,
`done <number>` or `list`. It loads `tasks.txt`, runs the command, and saves the list when
the command changed it. The modules `task`, `text` and `list` are the ones from the
[last chapter](10-modules.md#project-tasks-as-a-multi-file-program), with one addition to
`TaskList`: `addTask`, for adding an existing `Task` while loading.

<details markdown="block">
<summary>The modules <code>task</code>, <code>text</code> and <code>list</code></summary>

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

```pkn
// file: tasks/list.pkn
import ::io;
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

  fn addTask(t: task::Task) -> bool {
    if self.count == self.tasks.len() { return False; }
    self.tasks[self.count] = t;
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
      io::println(Str(i + 1) + ". " + self.tasks[i].toString());
      i = i + 1;
    }
    io::println(Str(self.pending()) + " of " + Str(self.count) + " to do");
  }
}

impl TaskList {
  fn __init__() {
    self.tasks = [task::Task("", task::Priority::Normal); 8];
    self.count = 0;
  }
}
```

</details>

The new module. A missing file is not an error for `load`: it just means no tasks yet. A
line it cannot read is reported on standard error and skipped:

```pkn
// file: tasks/store.pkn
import ::io;
import tasks::{task, text, list};

fn load(path: Str) -> list::TaskList {
  tasks = list::TaskList();
  match io::open(path, "r").value {
    f: io::File {
      while True {
        match f.readln().value {
          line: Str { addLine(tasks, text::trim(line)); }
          None { break; }
        }
      }
    }
    None { }   // no file yet: an empty list
  }
  return tasks;
}

fn addLine(tasks: list::TaskList, line: Str) {
  done, rest = text::splitOnce(line, '|');
  priority, title = text::splitOnce(rest, '|');
  if title == "" || (done != "0" && done != "1") {
    io::printerrln("skipping bad line: " + line);
    return;
  }
  t = task::Task(title, task::parsePriority(priority));
  t.done = done == "1";
  if !tasks.addTask(t) { io::printerrln("the list is full, skipping: " + line); }
}

fn save(tasks: list::TaskList, path: Str) -> bool {
  r = io::open(path, "w");
  match r.value {
    f: io::File {
      i = 0;
      while i < tasks.count {
        t = tasks.tasks[i];
        f.write((if t.done then "1" else "0") + "|"
                + task::priorityName(t.priority) + "|" + t.title + "\n");
        i = i + 1;
      }
      return True;
    }
    None {
      io::printerrln("cannot save: " + r.error.message());
      return False;
    }
  }
}
```

`save` needs no `return` after its `match`. The subject `r.value` is an `io::File?`, and an
`io::File` arm and a `None` arm together cover every case, so the compiler knows each path
returns. A `match` on an object of a class (an `Obj`, say) is different: an arm for each
subclass you know of does not cover a class you don't, so a function returning from inside
such arms needs a `return` after the `match` too (or a `_` arm), or it is rejected with
"does not always return a value".

And the new `main.pkn`:

```pkn
// file: main.pkn
import ::io;
import tasks::{task, store, list};

fn usage() -> int {
  io::printerrln("usage: tasks add <title> [high|low] | done <number> | list");
  return 2;
}

// Joins args[from..] with spaces, leaving out a trailing priority word.
fn titleFrom(args: view Str[], from: int) -> (Str, task::Priority) {
  last = args.len();
  priority = task::Priority::Normal;
  if last - from > 1 && (args[last - 1] == "high" || args[last - 1] == "low") {
    priority = if args[last - 1] == "high" then task::Priority::High else task::Priority::Low;
    last = last - 1;
  }
  title = "";
  i = from;
  while i < last {
    title = title + (if i > from then " " else "") + args[i];
    i = i + 1;
  }
  return (title, priority);
}

fn main(args: view Str[]) -> int {
  if args.len() < 2 { return usage(); }
  path = "tasks.txt";
  tasks: list::TaskList = store::load(path);
  changed = False;
  match args[1] {
    "add" {
      if args.len() < 3 { return usage(); }
      title, priority = titleFrom(args, 2);
      if !tasks.add(title, priority) {
        io::printerrln("the list is full");
        return 1;
      }
      changed = True;
    }
    "done" {
      if args.len() != 3 { return usage(); }
      match int(args[2]) {
        n: int {
          if !tasks.complete(n) {
            io::printerrln("no task " + args[2]);
            return 1;
          }
        }
        None { return usage(); }
      }
      changed = True;
    }
    "list" { }
    _ { return usage(); }
  }
  if changed && !store::save(tasks, path) { return 1; }
  tasks.show();
  return 0;
}
```

Output (arguments: add fix the leak high):

```
1. [ ] fix the leak !
1 of 1 to do
```

That was the first run, with no `tasks.txt` yet. Later runs pick up where it left off:

```text
$ paykan main.pkn add buy milk
1. [ ] fix the leak !
2. [ ] buy milk
2 of 2 to do
$ paykan main.pkn done 2
1. [ ] fix the leak !
2. [x] buy milk
1 of 2 to do
$ paykan main.pkn done 5
no task 5
$ cat tasks.txt
0|high|fix the leak
1|normal|buy milk
```

Build it once with `paykan build main.pkn -o tasks` and you have a `tasks` command to put on
your `PATH`. (It saves `tasks.txt` in the directory you run it from.)

The reference covers `::io` and `::result` in [the standard
library](../language/13-standard-library.md). Next: [Memory](12-memory.md),
which looks at what happens to all these objects.

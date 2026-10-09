# Files and I/O

Programs talk to the world through files, command-line arguments, standard input and
standard error. This chapter covers each, and finishes `tasks`: it now keeps its tasks in a
file and takes commands from the command line.

## Opening files

`open(path, mode)` opens a file. The mode is `"r"` to read, `"w"` to write (creating the
file, or emptying an existing one) or `"a"` to append. Opening can fail, so `open` returns
an `Obj` that is either a `File` or an `Error`, and you tell them apart with `match`:

```pkn
fn main() -> int {
  match open("notes.txt", "w") {
    f: File {
      f.write("first line\n");
      f.write("second line\n");
    }
    err: Error { printerrln(err.toString()); }
  }

  match open("no/such/dir/notes.txt", "r") {
    f: File    { println("opened?"); }
    err: Error { println("error: " + err.toString()); }
  }
  return 0;
}
```

Output:

```
error: open("no/such/dir/notes.txt", "r"): No such file or directory
```

An `Error`'s `toString()` is its message. Relative paths are relative to the directory the
program runs in, not to the source file.

There is no `close`: a `File` is closed automatically when the last reference to it goes
away, which for the `f` bound in a `match` arm is the end of the `match`. Everything written
is on disk by then, so the file can be opened again right after.

## Reading

A `File` has three reading methods. Each returns an `Obj`: a `Str` with the data, or `None`
when there is nothing more to read.

| Method          | Returns                                                       |
|-----------------|---------------------------------------------------------------|
| `readln()`      | the next line, **including** its `\n` if it has one           |
| `readbytes(n)`  | the next `n` bytes (fewer at the end of the file)             |
| `read()`        | everything that is left                                       |

Because they return `Obj` rather than `Str?`, the arm for the absent case is `_` (a `None`
arm is only for optional types). A line-by-line loop looks like this:

```pkn
fn main() -> int {
  match open("shopping.txt", "w") {
    f: File    { f.write("eggs\nflour\nsugar\n"); }
    err: Error { printerrln(err.toString()); return 1; }
  }

  match open("shopping.txt", "r") {
    f: File {
      n = 0;
      while (True) {
        match f.readln() {
          line: Str {
            n = n + 1;
            print(Str(n) + ": " + line);   // line ends in \n already
          }
          _ { break; }                     // None: end of file
        }
      }
    }
    err: Error { printerrln(err.toString()); return 1; }
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

Note `return 1;` inside the `Error` arms: `main` can return from anywhere, and a non-zero
exit status tells the caller something failed.

## Standard input

`Stdin` is a built-in `File` connected to the program's standard input. It is always open,
and has the same reading methods:

```pkn
fn main() -> int {
  total = 0;
  bad = 0;
  while (True) {
    match Stdin.readln() {
      line: Str {
        // drop the trailing newline before parsing
        digits = "";
        i = 0;
        while (i < line.len() && line[i] != '\n') {
          digits = digits + Str(line[i]);
          i = i + 1;
        }
        match int(digits) {
          n: int { total = total + n; }
          None   { bad = bad + 1; }
        }
      }
      _ { break; }
    }
  }
  println("total " + Str(total) + ", " + Str(bad) + " bad line(s)");
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

`printerr(x)` and `printerrln(x)` work like `print` and `println` but write to standard
error. Use them for diagnostics, so that they do not mix with the program's real output when
it is redirected to a file or a pipe.

## Command-line arguments

To receive its command-line arguments, `main` takes a `Str[]`:

```pkn
fn main(args: Str[]) -> int {
  if (args.len() < 2) {
    printerrln("usage: greet <name>...");
    return 2;
  }
  i = 1;
  while (i < args.len()) {
    println("Hello, " + args[i] + "!");
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

  fn __init__(title: Str, priority: Priority) {
    self.title = title;
    self.done = False;
    self.priority = priority;
  }

  view fn toString() -> Str {
    box = if self.done then "[x] " else "[ ] ";
    marker = if self.priority == Priority::High then " !" else "";
    return box + self.title + marker;
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
  while (i < to) {
    out = out + Str(s[i]);
    i = i + 1;
  }
  return out;
}

fn trim(s: Str) -> Str {
  start = 0;
  end = s.len();
  while (start < end && isSpace(s[start])) { start = start + 1; }
  while (end > start && isSpace(s[end - 1])) { end = end - 1; }
  return slice(s, start, end);
}

// Splits at the first `sep`: (s, "") when there is none.
fn splitOnce(s: Str, sep: char) -> (Str, Str) {
  i = 0;
  while (i < s.len() && s[i] != sep) { i = i + 1; }
  if (i == s.len()) { return (s, ""); }
  return (slice(s, 0, i), slice(s, i + 1, s.len()));
}
```

```pkn
// file: tasks/list.pkn
import tasks::task;

class TaskList {
  tasks: task::Task[];

  fn __init__() { self.tasks = []; }

  fn add(title: Str, priority: task::Priority) {
    self.tasks.push(task::Task(title, priority));
  }

  fn addTask(t: task::Task) { self.tasks.push(t); }

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

  view fn show() {
    i = 0;
    while (i < self.tasks.len()) {
      println(Str(i + 1) + ". " + self.tasks[i].toString());
      i = i + 1;
    }
    println(Str(self.pending()) + " of " + Str(self.tasks.len()) + " to do");
  }
}
```

</details>

The new module. A missing file is not an error for `load`: it just means no tasks yet. A
line it cannot read is reported on standard error and skipped:

```pkn
// file: tasks/store.pkn
import tasks::{task, text, list};

fn load(path: Str) -> list::TaskList {
  tasks = list::TaskList();
  match open(path, "r") {
    f: File {
      while (True) {
        match f.readln() {
          line: Str { addLine(tasks, text::trim(line)); }
          _ { break; }
        }
      }
    }
    _ { }   // no file yet: an empty list
  }
  return tasks;
}

fn addLine(tasks: list::TaskList, line: Str) {
  done, rest = text::splitOnce(line, '|');
  priority, title = text::splitOnce(rest, '|');
  if (title == "" || (done != "0" && done != "1")) {
    printerrln("skipping bad line: " + line);
    return;
  }
  t = task::Task(title, task::parsePriority(priority));
  t.done = done == "1";
  tasks.addTask(t);
}

fn save(tasks: list::TaskList, path: Str) -> bool {
  match open(path, "w") {
    f: File {
      i = 0;
      while (i < tasks.tasks.len()) {
        t = tasks.tasks[i];
        f.write((if t.done then "1" else "0") + "|"
                + task::priorityName(t.priority) + "|" + t.title + "\n");
        i = i + 1;
      }
      return True;
    }
    err: Error { printerrln("cannot save: " + err.toString()); }
  }
  return False;
}
```

Note the `return False;` after the `match` in `save`. To the compiler, the result of `open`
is just an `Obj`, which could be of any class, so a `match` with a `File` and an `Error` arm
does not cover every case. A function that returns from inside the arms needs a `return`
after the `match` too (or a `_` arm), or it is rejected with "does not always return a
value".

And the new `main.pkn`:

```pkn
// file: main.pkn
import tasks::{task, store, list};

fn usage() -> int {
  printerrln("usage: tasks add <title> [high|low] | done <number> | list");
  return 2;
}

// Joins args[from..] with spaces, leaving out a trailing priority word.
fn titleFrom(args: Str[], from: int) -> (Str, Str) {
  last = args.len();
  priority = "normal";
  if (last - from > 1 && (args[last - 1] == "high" || args[last - 1] == "low")) {
    priority = args[last - 1];
    last = last - 1;
  }
  title = "";
  i = from;
  while (i < last) {
    title = title + (if i > from then " " else "") + args[i];
    i = i + 1;
  }
  return (title, priority);
}

fn main(args: Str[]) -> int {
  if (args.len() < 2) { return usage(); }
  path = "tasks.txt";
  tasks: list::TaskList = store::load(path);
  changed = False;
  match args[1] {
    "add" {
      if (args.len() < 3) { return usage(); }
      title, priority = titleFrom(args, 2);
      tasks.add(title, task::parsePriority(priority));
      changed = True;
    }
    "done" {
      if (args.len() != 3) { return usage(); }
      match int(args[2]) {
        n: int {
          if (!tasks.complete(n)) {
            printerrln("no task " + args[2]);
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
  if (changed && !store::save(tasks, path)) { return 1; }
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

The reference covers the built-in I/O functions in
[Language basics](../language/01-language-basics.md#file-io). Next: [Memory](12-memory.md),
which looks at what happens to all these objects.

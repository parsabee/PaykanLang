# `::io`

Printing, files, the standard input and standard error. Printing is not built into the
language: a program that prints imports the module (`import ::io;`, by convention without
an alias) and calls `io::println(...)`. Opening and reading a file can fail, and `::io`
reports that as a [`::result`](result.md) `Result`, which holds an [`::error`](error.md)
`Error` when it failed, so a program that looks at the errors imports `::error` too (`import
::error as err;`). This page teaches files, the standard
streams and the command-line arguments, finishes the `tasks` project, and then documents
[every function and method](#api) of the module.

```pkn
import ::io;
import ::error as err;

struct Point { x: int; y: int; }

fn main() -> int {
  io::println(42);
  io::println((1, "one"));
  io::println(Point(3, 4));
  n: int? = None;
  io::println(n);
  io::print("no newline, ");
  io::println(True);
  match io::open("/no/such/dir/file.txt", "r") {
    f: Ok { io::println("opened?"); }
    e: Err { io::println(err::name(e.error.code) + ": " + e.error.message()); }
  }
  return 0;
}
```

Output:

```
42
(1, one)
Point(x: 3, y: 4)
None
no newline, True
NotFound: No such file or directory
```

---

## Opening files

`io::open(path, mode)` opens a file. The mode is `"r"` to read, `"w"` to write (creating the
file, or emptying an existing one) or `"a"` to append. Opening can fail, so `open` returns a
**result**: a `Result<io::File, Error>` from the `::result` module. It is a [tagged
union](../language/07-enums.md#variants-with-fields), either `Ok`, whose `value` is the file,
or `Err`, whose `error` says why it could not be opened, and you tell the two apart with
`match`:

```pkn
import ::io;
import ::error as err;

fn main() -> int {
  match io::open("notes.txt", "w") {
    w: Ok {
      f = w.value;
      f.write("first line\n");
      f.write("second line\n");
    }
    e: Err { io::printerrln(e.error.message()); }
  }

  match io::open("no/such/dir/notes.txt", "r") {
    r: Ok { io::println("opened?"); }
    e: Err { io::println("error: " + err::name(e.error.code) + ": " + e.error.message()); }
  }
  return 0;
}
```

Output:

```
error: NotFound: No such file or directory
```

An `Err`'s `error` is an `err::Error` ([`::error`](error.md)): a `code`, one of the standard
`err::ErrorCode`s (`NotFound`, `PermissionDenied`, `EndOfFile` and the rest), and a message.
`error.message()` is the message, here the operating system's, and `err::name(code)` is the
code's name. Relative paths are relative to the directory the program runs in, not to the
source file.

`f.write(s)` makes no value, so it reports failure with an `err::Error` alone, whose code is
`Ok` when it wrote. The example ignores it; `if !f.write(s).isOk() { ... }` checks it.

There is no `close`: a file is closed automatically when the last reference to it goes
away. Above, the result `match` took apart holds the only one, so `notes.txt` is closed when
that `match` ends. Everything written is on disk once the file is closed, so a later `open`
reads it back (the next example writes the file in a function of its own, which closes it
when it returns). A file passed to a function where its variable is last used is handed on
to it ([Ownership transfers](../language/16-memory.md#ownership-transfers)): it is closed when
that function returns, unless the function keeps it.

## Reading

An `io::File` has three reading methods. Each returns a `Result<Str, Error>`: `Ok` with the
data read as its `value`, or `Err` when there is nothing more to read (the error's code is
then `EndOfFile`) or the read failed.

| Method          | Reads                                                         |
|-----------------|---------------------------------------------------------------|
| `readln()`      | the next line, **including** its `\n` if it has one           |
| `readbytes(n)`  | the next `n` bytes (fewer at the end of the file)             |
| `read()`        | everything that is left                                       |

A line-by-line loop matches each read:

```pkn
import ::io;

// Writes `text` to the file at `path`; False if it could not.
fn writeFile(path: Str, text: Str) -> bool {
  match io::open(path, "w") {
    w: Ok { return w.value.write(text).isOk(); }
    e: Err {
      io::printerrln(e.error.message());
      return False;
    }
  }
}   // the file is closed here, when the result goes away

fn main() -> int {
  if !writeFile("shopping.txt", "eggs\nflour\nsugar\n") { return 1; }

  match io::open("shopping.txt", "r") {
    r: Ok {
      f = r.value;
      n = 0;
      while True {
        match f.readln() {
          line: Ok {
            n = n + 1;
            io::print(Str(n) + ": " + line.value);   // the line ends in \n already
          }
          _ { break; }                            // the end of the file
        }
      }
    }
    e: Err { io::printerrln(e.error.message()); return 1; }
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
    match io::stdin().readln() {
      read: Ok {
        line = read.value;
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
      _ { break; }
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
arguments (see [Arrays](../language/09-arrays.md#arrays-are-values)):

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
the command changed it. The modules `task`, `text` and `list` are the ones from
[Modules](../language/15-modules.md#project-tasks-as-a-multi-file-program), with one
addition to `TaskList`: `addTask`, for adding an existing `Task` while loading.

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
  match io::open(path, "r") {
    r: Ok {
      while True {
        match r.value.readln() {
          line: Ok { addLine(tasks, text::trim(line.value)); }
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
  if title == "" || (done != "0" && done != "1") {
    io::printerrln("skipping bad line: " + line);
    return;
  }
  t = task::Task(title, task::parsePriority(priority));
  t.done = done == "1";
  if !tasks.addTask(t) { io::printerrln("the list is full, skipping: " + line); }
}

fn save(tasks: list::TaskList, path: Str) -> bool {
  match io::open(path, "w") {
    w: Ok {
      f = w.value;
      i = 0;
      while i < tasks.count {
        t = tasks.tasks[i];
        f.write((if t.done then "1" else "0") + "|"
                + task::priorityName(t.priority) + "|" + t.title + "\n");
        i = i + 1;
      }
      return True;
    }
    e: Err {
      io::printerrln("cannot save: " + e.error.message());
      return False;
    }
  }
}
```

`save` needs no `return` after its `match`. The subject is a `Result`, and an arm for each
of its variants, `Ok` and `Err`, covers every case, so the compiler knows each path
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
[Memory](../language/16-memory.md) looks at what happens to all these objects.

---

## API

The module declares four print functions, two functions that make a `File`, and the class
`File`. Every other name in `stdlib/io.pkn` starts with `_` and is the module's own.

### Printing

#### `print`

```pkn
fn print<T: Show>(value: view T)
```

Writes `value.toString()` to standard output, with no newline after it.

#### `println`

```pkn
fn println<T: Show>(value: view T)
```

Writes `value.toString()` and a newline to standard output.

#### `printerr`

```pkn
fn printerr<T: Show>(value: view T)
```

Writes `value.toString()` to standard error, with no newline after it.

#### `printerrln`

```pkn
fn printerrln<T: Show>(value: view T)
```

Writes `value.toString()` and a newline to standard error.

Each print function takes exactly one value of any type that implements
[`Show`](../language/06-traits.md): an object (every class, `Str`), a number of any size, a
`bool`, a `char`, a tuple, a struct, or an optional of one, which prints `None` when it is
absent. A function value is not `Show` (`'() -> int' does not implement trait 'Show',
which 'io::println' requires of 'T'`). Paykan has no variadic functions or overloading, so
several pieces are joined into one `Str` with `+` first (`io::println("n = " + Str(n))`).
The value is a `view`: a temporary, or a borrowed `view` parameter, prints without being
copied.

The print functions report no errors. Standard output is flushed before a
[panic](index.md#quick-summary), so output printed before one is never lost, and comes
ahead of the panic's message when both go to the same file or pipe.

### Files

#### `open`

```pkn
fn open(path: view Str, mode: view Str) -> res::Result<File, err::Error>
```

Opens the file at `path` with C's `fopen` and the mode `mode`: `"r"` to read, `"w"` to
write (creating the file, or emptying it), `"a"` to append, and C's other modes (`"r+"`,
`"w+"`, `"a+"`, ...). A file opened for update (a mode with `+`) is read and written in
any order: a read starts where the write before it ended, and a write where the read before
it stopped, except in `"a+"`, which writes at the end of the file like `"a"` (where a read
before the first write starts is the C library's: the start of the file on Linux, the end
on macOS). A relative `path` is relative to the current
directory of the process. The result is `Ok` with the `File` as its `value`, or `Err` when
the file could not be opened; then its `error` has the code of C's `errno` and C's
`strerror` text as its message: `NotFound` (`No such file or directory`),
`PermissionDenied`, `IsADirectory` (a directory opened to write), `InvalidArgument` (a mode
`fopen` does not take, or a path or a mode holding a NUL byte, `"\0"`, which C would read
only up to it), and the rest of [the mapping](error.md#errorcode). Opening a directory to read succeeds on Linux; its
reads then fail with `IsADirectory`.

#### `stdin`

```pkn
fn stdin() -> File
```

The process's standard input. It cannot fail, and it is never closed: every call returns a
`File` for the same stream, and it stays open when the `File`s go. Writing to it fails with
`InvalidArgument` (`Bad file descriptor`).

#### `File`

```pkn
class File { ... }
```

An open file, made by `open` or `stdin`. A program does not construct one: its initializer
is `___init__`, private to `::io` (`io::File(...)` is `'io::File' cannot be constructed
here: its initializer '___init__' is private to module '::io'`). A `File` is an object, so
assigning one shares it, and the file is closed when the last reference to it goes; there
is no `close`. Its `toString()` is `File@0x...`, and two `File`s are `==` when they are the
same object (`io::stdin() == io::stdin()` is `False`: each call makes a new `File`). Every
method is a `view fn`, so it can be called through a `view` parameter, and each reports
failure with an [`::error`](error.md) `Error`: a write's alone, a read's in its `Result`.

#### `File.write`

```pkn
view fn write(s: view Str) -> err::Error
```

Writes `s` to the file. The error is `Ok` when it wrote, `InvalidArgument` (`Bad file
descriptor`) when the file is not open for writing (`stdin()`, a file opened with `"r"`).

#### `File.readln`

```pkn
view fn readln() -> res::Result<Str, err::Error>
```

The next line, with its `\n` when it has one (the last line of a file may not).

#### `File.readbytes`

```pkn
view fn readbytes(n: int) -> res::Result<Str, err::Error>
```

The next `n` bytes, or fewer at the end of the file. `InvalidArgument` when `n` is not
positive.

#### `File.read`

```pkn
view fn read() -> res::Result<Str, err::Error>
```

Everything that is left of the file.

#### End of file, failed reads and buffering

A read that finds nothing left fails with `EndOfFile` (it is an `Err`), so a read never
returns an empty `Str`: `read()` of an empty file, or of one already read to the end,
is `EndOfFile` too. A read that the operating system fails has the code of its `errno`
(`IsADirectory` for a directory, `Io`, ...). Reading a file opened only to write fails with
`InvalidArgument` (`Bad file descriptor`), as writing to one opened only to read does.

Writes are buffered by the C library: what `write` reports is whether the data was taken,
and the data is on disk once the file is closed, or once a read of the same `File` follows
the write. An error the operating system reports only when the buffer is written out (a full
disk, say) is the error of that read (`NoSpace`, ...), which then reads nothing; when the
file is closed, it is not reported to the program.

#### Errors at a glance

| Situation | Code |
|-----------|------|
| `open` of a path that does not exist | `NotFound` |
| `open` without the permission | `PermissionDenied` |
| `open` of a directory to write | `IsADirectory` |
| `open` with a mode `fopen` rejects | `InvalidArgument` |
| `open` of a path, or with a mode, holding a NUL byte (`"\0"`) | `InvalidArgument` |
| a read with nothing left | `EndOfFile` |
| `readbytes(n)` with `n <= 0` | `InvalidArgument` |
| `write` to a file not open for writing (`stdin()`, a `"r"` file) | `InvalidArgument` |
| a read of a directory opened with `"r"` | `IsADirectory` |
| a read after a write whose data cannot be written out (a full disk) | the code of its `errno` (`NoSpace`, ...) |
| any other failure | the code of its `errno` ([`ErrorCode`](error.md#errorcode)) |

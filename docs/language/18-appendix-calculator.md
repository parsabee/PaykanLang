# Appendix: a calculator

This appendix walks through a complete program from the repository,
[`samples/imports/12_calc`](../../samples/imports/12_calc/main.pkn): an arithmetic calculator
with variables, a command history and an interactive mode. At about 890 lines in four
modules it uses most of what this reference covers: classes, an enum, `match` in three of its
four modes (all but matching classes), arrays, tuples, optionals, a generic class, modules,
standard input and command-line arguments. The test suite runs it on every backend, and the
excerpts below are checked against the sample's source, so they are always current.

## Using it

Copy the directory (or clone the repository) and run `main.pkn`. Expressions given as
arguments are evaluated in order, sharing their variables. The sample normally starts by
replaying a scripted demonstration session; `--no-demo` skips it:

```text
$ cd samples/imports/12_calc
$ paykan main.pkn --no-demo "3 + 4 * (2 - 1)" "10 / 4"
3 + 4 * (2 - 1) = 7
10 / 4 = 2.5
$ paykan main.pkn --no-demo -e "x = 6" -e "x * 7"
x = 6
x * 7 = 42
```

Without expressions it reads one statement per line from standard input, so it works both
interactively and in a pipe. Errors go to standard error:

```text
$ printf 'r = 2\npi * r * r\n:vars\nfoo\n' | paykan main.pkn --no-demo
calc — arithmetic with variables.  :help for commands, :quit to exit.
r = 2
pi * r * r = 12.5664
  e = 2.71828
  pi = 3.14159
  ans = 12.5664
  r = 2
error: undefined variable 'foo'
```

## The modules

```text
12_calc/
├── main.pkn          command line, REPL, history
└── calc/
    ├── parser.pkn    a recursive-descent expression parser
    ├── hashmap.pkn   a hash map from Str to float, for the variables
    └── utils.pkn     string helpers
```

`main.pkn` imports the three library modules, and `parser.pkn` imports `hashmap.pkn`
because the parser looks variables up while it evaluates:

```pkn
// from: samples/imports/12_calc/main.pkn
import calc::parser;
import calc::hashmap;
import calc::utils;
```

## The variable store: `calc/hashmap.pkn`

Variables live in a hash table with separate chaining. The table is a fixed array of eight
buckets, `Entry[8]`, each the head of a linked chain: a head holds no key, and its `next`
field is the bucket's first entry, or `None` while the bucket is empty; each entry's `next`
links the one after it. Every entry has one owner, the entry before it (or the head), so
the table owns its chains, and a chain is changed in place through its head, by methods of
`Entry` that work on the entries after the one they are called on. `append` hands the new
entry on until it reaches the entry that has no `next`:

```pkn
// from: samples/imports/12_calc/calc/hashmap.pkn
final class Entry {
  key: Str;
  value: float;
  next: Entry?;   // the next entry in the same bucket, or None

  // Link `e` at the end of the chain that starts here.
  fn append(e: Entry) {
    match self.next {
      n: inout Entry { n.append(move e); }
      None { self.next = move e; }
    }
  }
// ...

impl Entry {
  fn __init__(k: Str, v: float) { self.key = move k; self.value = v; }
}
// ...

// Eight empty buckets: a head each, with no key.
fn heads() -> Entry[8] {
  return [Entry("", 0.0); 8];
}

class HashMap {
  buckets: Entry[8];
  size: int;
// ...

impl HashMap {
  fn __init__() { self.buckets = heads(); self.size = 0; }
}
```

`next` is optional, so it starts as `None` and `Entry`'s `__init__` leaves it alone. `heads`
makes a head for each of the eight buckets: an `Entry` is not Copyable, so `[Entry("", 0.0);
8]` evaluates `Entry("", 0.0)` once per element ([Arrays](09-arrays.md#repeated-values)), and
`clear` assigns them again. The hash is
folded into `self.buckets.len()`, so the lookups follow the field's type rather than
repeating the number of buckets. `append` walks the chain through an `inout` binding, which
reaches each entry in place and changes the last one
([Match](08-match.md#borrowing-the-subject)), and hands the new entry on with `move` at
each step, as a move-only value goes from one owner to the next
([Ownership transfers](16-memory.md#ownership-transfers)).

Looking a key up returns an **optional**, a `float?`, so a missing variable is `None` rather
than a special value. `find` walks the chain by recursion through plain bindings, each a
borrow of the entry it names: it only reads, so it and `lookup` are `view fn`s:

```pkn
// from: samples/imports/12_calc/calc/hashmap.pkn
  view fn find(key: view Str) -> float? {
    match self.next {
      n: Entry {
        if n.key.equals(key) {
          return n.value;
        }
        return n.find(key);
      }
      None { return None; }
    }
  }
// ...
  view fn lookup(key: view Str) -> float? {
    return self.buckets[self.indexFor(key)].find(key);
  }
```

`insert` changes the value of a key the table has in place, through `update`, an `inout`
walk like `append`'s, and otherwise appends a new entry, whose key is a copy of the one it
was given, `key.clone()`, since `key` is a `view` and the entry keeps its key. A new key goes
at the end of its bucket's chain, so a bucket keeps its keys in the order they were added:

```pkn
// from: samples/imports/12_calc/calc/hashmap.pkn
  fn update(key: view Str, value: float) -> bool {
    match self.next {
      n: inout Entry {
        if n.key.equals(key) {
          n.value = value; // update in place
          return True;
        }
        return n.update(key, value);
      }
      None { return False; }
    }
  }
// ...
  fn insert(key: view Str, value: float) -> bool {
    let b = self.indexFor(key);
    if self.buckets[b].update(key, value) {
      return False;
    }
    self.buckets[b].append(Entry(key.clone(), value));
    return True;
  }
```

Keys are compared with `n.key.equals(key)`, which compares contents just like `n.key == key`
would.

Removing a key unlinks its entry. `unlink` walks the chain through `inout` bindings to the
entry before the one with the key; there the binding's last use has passed, and `mem::take`
moves the entry with the key out of `self.next`, leaving `None`. The arm owns that entry, as
the binding of a temporary, and `move n.next` hands the rest of the chain back to
`self.next` ([Fields and elements](16-memory.md#fields-and-elements-memreplace-memtake-and-memswap)).
`Entry` is a `final class` for that move: a field leaves an object only when no
`__deinit__` can run on it, and a class that another module could derive from might get one
([Final classes](05-classes.md#final-classes)):

```pkn
// from: samples/imports/12_calc/calc/hashmap.pkn
  fn unlink(key: view Str) -> bool {
    match self.next {
      n: inout Entry {
        if !n.key.equals(key) {
          return n.unlink(key);
        }
      }
      None { return False; }
    }
    match mem::take(self.next) {
      n: Entry { self.next = move n.next; }
      None { }
    }
    return True;
  }
// ...
  fn remove(key: view Str) -> bool {
    if !self.buckets[self.indexFor(key)].unlink(key) {
      return False;
    }
    self.size = self.size - 1;
    return True;
  }
```

For the `:vars` command, the map gives out the number of buckets and a `view` of the first
entry of each, `bucket(b)`. `listVars` in `main.pkn` takes the map as a `view`, and
`listChain` follows the `next` links from there, recursively, through `view` parameters:
nothing is copied out of the table, and nothing in it changes.

```pkn
// from: samples/imports/12_calc/main.pkn
// List the variables of the chain of entries that starts at `e`.
fn listChain(e: view hashmap::Entry) {
  io::println("  " + e.key + " = " + show(e.value));
  match e.next {
    n: hashmap::Entry { listChain(n); }
    None { }
  }
}

// List every stored variable, walking each bucket's chain of entries.
fn listVars(env: view hashmap::HashMap) {
  if env.count() == 0 {
    io::println("(no variables)");
    return;
  }
  b = 0;
  while b < env.bucketCount() {
    match env.bucket(b) {
      e: hashmap::Entry { listChain(e); }
      None { }
    }
    b = b + 1;
  }
}
```

## The parser: `calc/parser.pkn`

The four operators and modulo form an enum, and `applyOp` is an exhaustive variant-mode
`match`:

```pkn
// from: samples/imports/12_calc/calc/parser.pkn
enum Op { Add, Sub, Mul, Div, Mod }

fn applyOp(op: Op, a: float, b: float) -> float {
  match op {
    Add { return a + b; }
    Sub { return a - b; }
    Mul { return a * b; }
    Div { return a / b; }
    Mod { return a % b; }
  }
}
```

The `Parser` class holds the input and a position in it. Its `env` field is the variable
store, a `shared mut hashmap::HashMap`: `main` and the parser hold the one map, and both
change it ([Concurrency](19-concurrency.md#who-holds-an-object-the-qualifiers)). Its
`error` field is a `Str?`: optional fields start as `None`, so `__init__` leaves it alone,
and `fail` keeps only the first error, which it takes by `move`:

```pkn
// from: samples/imports/12_calc/calc/parser.pkn

class Parser {
  input: Str;
  pos: int;
  env: shared mut hashmap::HashMap;
  error: Str?;   // the first error, or None

  // Record the first error only; later errors are usually cascades.
  fn fail(msg: Str)      { if self.error == None { self.error = move msg; } }
// ...

impl Parser {
  fn __init__(s: Str, env: shared mut hashmap::HashMap) {
    self.input = move s;
    self.pos = 0;
    self.env = env;
  }
}
```

The grammar has one method per precedence level, and each level calls the next one up. A
`term` is a chain of factors joined by `*`, `/` or `%`; the operator is picked with a
chained conditional expression:

```pkn
// from: samples/imports/12_calc/calc/parser.pkn

  fn parseTerm() -> float {
    result = self.parseFactor();
    self.skipWs();
    while !self.done() && (self.atIs('*') || self.atIs('/') || self.atIs('%')) {
      let op = if self.atIs('*') then Op::Mul else if self.atIs('/')
          then Op::Div else Op::Mod;
      self.advance();
      let rhs = self.parseFactor();
      result = applyOp(op, result, rhs);
      self.skipWs();
    }
    return result;
  }
```

A factor that is a name is looked up in the variable store, and the `float?` that `lookup`
returns is unwrapped by an optional-mode `match`:

```pkn
// from: samples/imports/12_calc/calc/parser.pkn
    if self.atIsIdentStart() {
      let name = self.parseIdent();
      match self.env.lookup(name) {
        v: float {
          return v;
        }
        None {
          self.fail("undefined variable '" + name + "'");
          return 0.0;
        }
      }
    }
```

Finally, a whole line is either an assignment, `name = expr`, or an expression.
`parseStatement` returns both answers at once as a tuple `(Str?, float)`: the name assigned
to, or `None`, and the value. The `target` and `noName` variables give the `Str?` element
its type, since a bare `None` in a tuple literal with no declared destination would be an
`Obj`; `target` takes the name by `move`, and the tuple each variable by `move` in turn:

```pkn
// from: samples/imports/12_calc/calc/parser.pkn
  fn parseStatement() -> (Str?, float) {
    self.skipWs();
    let start = self.pos;
    if self.atIsIdentStart() {
      let name = self.parseIdent();
      self.skipWs();
      if self.atAssign() {
        self.advance(); // consume '='
        let v = self.parseExpr();
        self.skipWs();
        let target: Str? = move name;
        return (move target, v);
      }
      self.pos = start; // not an assignment — rewind and parse as an expression
    }
    let v = self.parseExpr();
    self.skipWs();
    let noName: Str? = None;
    return (move noName, v);
  }
```

## The program: `main.pkn`

### A generic ring buffer

The `:history` command shows the last 16 statements. They are kept in a generic ring buffer,
`Ring<T>`, declared in `main.pkn` itself (a generic class could equally live in a module of
its own and be imported, as [Modules](15-modules.md#what-can-be-imported) shows). It holds an
array of 16 slots, a count of the slots in use, and the index of the oldest item once all of
them are. Until the buffer is full, `add` fills the
next free slot; after that it overwrites the oldest item and moves `start` on:

```pkn
// from: samples/imports/12_calc/main.pkn
class Ring<T> {
  items: T[16];
  count: int; // how many slots are in use: all of them once the buffer is full
  start: int; // index of the oldest item once the buffer is full

  // Append `x`, overwriting the oldest item when the buffer is full.
  fn add(x: T) {
    if self.count < self.items.len() {
      self.items[self.count] = move x;
      self.count = self.count + 1;
    } else {
      self.items[self.start] = move x;
      self.start = (self.start + 1) % self.items.len();
    }
  }
// ...

impl Ring<T> {
  // Every slot starts as a clone of `blank`, until an item is added there.
  fn __init__(blank: T) {
    self.items = [blank.clone(), blank.clone(), blank.clone(), blank.clone(),
        blank.clone(), blank.clone(), blank.clone(), blank.clone(),
        blank.clone(), blank.clone(), blank.clone(), blank.clone(),
        blank.clone(), blank.clone(), blank.clone(), move blank];
    self.count = 0;
    self.start = 0;
  }
}
```

An array always holds all of its elements, so `__init__` takes a `blank` item and fills the
slots with it until they are used, as the task list in
[Arrays](09-arrays.md#project-the-task-list) does: fifteen clones and `blank` itself, since
`[blank; 16]` would copy it, which a tuple holding a `Str` does not allow
([Cloneable](06-traits.md#cloneable)). `add` stores its item with `move`, as a move-only
value goes into a slot only by `move`, as the stack in
[Generics](13-generics.md#project-undo-with-a-generic-stack) does. The history holds
`(Str, float)` tuples, the statement and its value, so its type is `Ring<(Str, float)>`, and
its blank is an empty statement. It is a variable that is not `let`, since the functions
that add to it take it as an `inout` parameter:

```pkn
// from: samples/imports/12_calc/main.pkn
  history: mut = Ring(("", 0.0));
```

### Evaluating a statement

`evalStatement` ties the pieces together. It destructures the parser's tuple, checks the
optional `error` field with a `match`, and then matches on the optional target name to decide
between printing an assignment and printing an expression's value. It changes the
environment, which it shares with the parser, so it takes it as a `shared mut` reference,
and the history in place, an `inout` parameter. The parser it makes is `mut`, since parsing
moves it along its input, and is given a copy of the statement, `src.clone()`, as is the
history, since `evalStatement` prints `src` too:

```pkn
// from: samples/imports/12_calc/main.pkn
fn evalStatement(src: Str, env: shared mut hashmap::HashMap,
                 history: inout Ring<(Str, float)>, quiet: bool) {
  let p: mut = parser::Parser(src.clone(), env);
  target, value = p.parseStatement();

  match p.error {
    msg: Str {
      io::printerrln("error: " + msg);
      return;
    }
    None { }
  }
  if !p.done() {
    io::printerrln("error: unexpected '" + p.rest() + "'");
    return;
  }

  env.put("ans", value);
  history.add((src.clone(), value));
  match target {
    name: Str {
      env.put(name, value);
      if quiet {
        io::println(show(value));
      } else {
        io::println(name + " = " + show(value));
      }
    }
    None {
      if quiet {
        io::println(show(value));
      } else {
        io::println(src + " = " + show(value));
      }
    }
  }
}
```

### The REPL

The interactive loop is the standard-input idiom from [Files and I/O](../stdlib/io.md).
`readln` returns a `Result` that is `Ok` with the line, or `Err` at the end of the input,
which the `_` arm catches; `handle` returns `False` for `:quit`:

```pkn
// from: samples/imports/12_calc/main.pkn
  while True {
    match io::stdin().readln() {
      line: Ok {
        if !handle(line.value, env, history, quiet) {
          return 0;
        }
      }
      _ { break; } // Err — the end of the input
    }
  }
```

### The command line

`main` takes `args: view Str[]`, a slice of the arguments, and walks it with an index, so
that `-e` can consume the argument after it. Exit status `2` signals a usage error, a common
convention:

```pkn
// from: samples/imports/12_calc/main.pkn

fn main(args: view Str[]) -> int {
  quiet = False;
  runDemo = True;
  nexprs = 0;

  // -- command-line parsing: flags, repeatable -e, positional expressions --
  i = 1;
  while i < args.len() {
    a: view = args[i];
    if a.equals("-h") || a.equals("--help") {
      printUsage();
      return 0;
    } elif a.equals("-q") || a.equals("--quiet") {
      quiet = True;
    } elif a.equals("--no-demo") {
      runDemo = False;
    } elif a.equals("-e") || a.equals("--eval") {
      if i + 1 < args.len() {
        i = i + 1;
        nexprs = nexprs + 1;
      } else {
        io::printerrln("error: " + a + " requires an argument");
        return 2;
      }
```

The arguments are a `view`, which `main` reads but does not keep, and `a` is a `view` local
of one. So this first pass only checks the options and counts the expressions. After the
demo, a second pass walks the arguments again and evaluates the expressions in order. Each
one is passed as its `toString()`, a string of the program's own that the parser and the
history can keep:

```pkn
// from: samples/imports/12_calc/main.pkn
  if nexprs > 0 {
    j = 1;
    while j < args.len() {
      if args[j].equals("-e") || args[j].equals("--eval") {
        j = j + 1;
        evalStatement(args[j].toString(), env, history, quiet);
      } elif !isOption(args[j]) {
        evalStatement(args[j].toString(), env, history, quiet);
      }
      j = j + 1;
    }
    return 0;
  }
```

## Ideas for extending it

The calculator is a good playground. Some changes to try, each exercising a part of the
language:

- Add a `^` operator: a new `Op` variant, a new precedence level in the parser, and a call
  to `math::pow` (from `import ::math;`) in `applyOp`. The compiler points out the `match`
  that no longer covers every variant.
- Add functions such as `abs(x)` and `sqrt(x)`: parse a name followed by `(`, dispatch on
  the name with a value-mode `match` on the `Str`, and call `math::abs` or `math::sqrt`.
- Add `:save <file>` and `:load <file>` commands that write the variables with `io::open` and
  read them back with `readln` and `toFloat()`.
- Run the result with `paykan --track-heap` to check that it still frees everything.

Back to the [contents](index.md).

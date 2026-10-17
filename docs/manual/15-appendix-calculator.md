# Appendix: a calculator

This appendix walks through a complete program from the repository,
[`samples/imports/12_calc`](../../samples/imports/12_calc/main.pkn): an arithmetic calculator
with variables, a command history and an interactive mode. At about 870 lines in four
modules it uses most of what this manual covers: classes, an enum, `match` in all its modes,
arrays, tuples, optionals, a generic class, modules, standard input and command-line
arguments. The test suite runs it on every backend, and the excerpts below are checked
against the sample's source, so they are always current.

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
buckets, `Entry?[8]`: each bucket is the first `Entry` of a linked chain, or `None` while it
is empty, and each entry's `next` field links the one after it. `append` walks a chain by
handing the new entry on until it reaches the entry that has no `next`:

```pkn
// from: samples/imports/12_calc/calc/hashmap.pkn
class Entry {
  key: Str;
  value: float;
  next: Entry?;   // the next entry in the same bucket, or None

  // Link `e` at the end of the chain that starts here.
  fn append(e: Entry) {
    match self.next {
      n: Entry { n.append(e); }
      None     { self.next = e; }
    }
  }
// ...
impl Entry {
  fn __init__(k: Str, v: float) {
    self.key = k;
    self.value = v;
  }
}
// ...
class HashMap {
  buckets: Entry?[8];
  size: int;
// ...
impl HashMap {
  fn __init__() {
    self.buckets = [None, None, None, None, None, None, None, None];
    self.size = 0;
  }
}
```

`next` is optional, so it starts as `None` and `Entry`'s `__init__` leaves it alone. The
hash is folded into `self.buckets.len()`, so the number of buckets is written only once, in
the field's type.

Looking a key up returns an **optional**, `Entry?`, so a missing variable is `None` rather
than a special value. The loop is the linked-list walk from [Optionals](08-optionals.md).
`lookup` changes nothing, but it is not a `view fn`: the entry it returns is the one stored
in the table, and `insert` changes it.

```pkn
// from: samples/imports/12_calc/calc/hashmap.pkn
  fn lookup(key: Str) -> Entry? {
    cur: Entry? = self.buckets[self.indexFor(key)];
    while (cur != None) {
      match cur {
        e: Entry {
          if (e.key.equals(key)) { return e; }
          cur = e.next;
        }
        None { }
      }
    }
    return None;
  }
```

and `insert` unwraps it with an optional-mode `match`. Because `Entry` is a class, `e` in the
first arm is a reference to the entry stored in the table, so assigning `e.value` updates the
table in place. A new key goes into its bucket, which it starts when the bucket is `None`;
otherwise it goes at the end of the chain, so a bucket keeps its keys in the order they were
added:

```pkn
// from: samples/imports/12_calc/calc/hashmap.pkn
  fn insert(key: Str, value: float) -> bool {
    match self.lookup(key) {
      e: Entry {
        e.value = value; // update in place (Entry is a reference type)
        return False;
      }
      None {
        b: int = self.indexFor(key);
        match self.buckets[b] {
          first: Entry { first.append(Entry(key, value)); }
          None         { self.buckets[b] = Entry(key, value); }
        }
        return True;
      }
    }
  }
```

The `match` covers both cases and returns from each arm, so the function needs no `return`
after it. Keys are compared with `e.key.equals(key)`, which compares contents just like
`e.key == key` would. (A comment at the top of the module predates `==` comparing strings by
content.)

Removing a key unlinks its entry: the first one of a bucket by storing its `next` in the
bucket, any other by `Entry.unlink`, which links the entry before it to the one after it.

```pkn
// from: samples/imports/12_calc/calc/hashmap.pkn
  fn remove(key: Str) -> bool {
    b: int = self.indexFor(key);
    match self.buckets[b] {
      first: Entry {
        if (first.key.equals(key)) {
          self.buckets[b] = first.next; // the chain now starts at the second
        } else if (!first.unlink(key)) {
          return False;
        }
        self.size = self.size - 1;
        return True;
      }
      None { return False; }
    }
  }
```

For the `:vars` command, the map gives out the number of buckets and the first entry of
each, `bucket(b)`; `listVars` in `main.pkn` follows the `next` links from there. `bucket` is
not a `view fn` either, since the entries it reaches can be changed:

```pkn
// from: samples/imports/12_calc/main.pkn
fn listVars(env: hashmap::HashMap) {
  if (env.count() == 0) {
    println("(no variables)");
    return;
  }
  b: int = 0;
  while (b < env.bucketCount()) {
    cur: hashmap::Entry? = env.bucket(b);
    while (cur != None) {
      match cur {
        e: hashmap::Entry {
          println("  " + e.key + " = " + show(e.value));
          cur = e.next;
        }
        None { }
      }
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

The `Parser` class holds the input and a position in it. Its `error` field is a `Str?`:
optional fields start as `None`, so `__init__` leaves it alone, and `fail` keeps only the
first error:

```pkn
// from: samples/imports/12_calc/calc/parser.pkn
class Parser {
  input: Str;
  pos: int;
  env: hashmap::HashMap;
  error: Str?;   // the first error, or None

  // Record the first error only; later errors are usually cascades.
  fn fail(msg: Str) {
    if (self.error == None) { self.error = msg; }
  }
// ...
impl Parser {
  fn __init__(s: Str, env: hashmap::HashMap) {
    self.input = s;
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
    result: float = self.parseFactor();
    self.skipWs();
    while (!self.done() && (self.atIs('*') || self.atIs('/') || self.atIs('%'))) {
      op: Op = if self.atIs('*') then Op::Mul
               else if self.atIs('/') then Op::Div else Op::Mod;
      self.advance();
      rhs: float = self.parseFactor();
      result = applyOp(op, result, rhs);
      self.skipWs();
    }
    return result;
  }
```

A factor that is a name is looked up in the variable store. The `match` names the type as
`hashmap::Entry`, since it comes from another module:

```pkn
// from: samples/imports/12_calc/calc/parser.pkn
    if (self.atIsIdentStart()) {
      name: Str = self.parseIdent();
      match self.env.lookup(name) {
        e: hashmap::Entry { return e.value; }
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
`Obj`:

```pkn
// from: samples/imports/12_calc/calc/parser.pkn
  fn parseStatement() -> (Str?, float) {
    self.skipWs();
    start: int = self.pos;
    if (self.atIsIdentStart()) {
      name: Str = self.parseIdent();
      self.skipWs();
      if (self.atAssign()) {
        self.advance(); // consume '='
        v: float = self.parseExpr();
        self.skipWs();
        target: Str? = name;
        return (target, v);
      }
      self.pos = start; // not an assignment — rewind and parse as an expression
    }
    v: float = self.parseExpr();
    self.skipWs();
    noName: Str? = None;
    return (noName, v);
  }
```

## The program: `main.pkn`

### A generic ring buffer

The `:history` command shows the last 16 statements. They are kept in a generic ring buffer,
`Ring<T>`, declared in `main.pkn` itself because generic classes cannot be imported from
another module yet. It holds an array of 16 slots, a count of the slots in use, and the
index of the oldest item once all of them are. Until the buffer is full, `add` fills the
next free slot; after that it overwrites the oldest item and moves `start` on:

```pkn
// from: samples/imports/12_calc/main.pkn
class Ring<T> {
  items: T[16];
  count: int; // how many slots are in use: all of them once the buffer is full
  start: int; // index of the oldest item once the buffer is full

  // Append `x`, overwriting the oldest item when the buffer is full.
  fn add(x: T) {
    if (self.count < self.items.len()) {
      self.items[self.count] = x;
      self.count = self.count + 1;
    } else {
      self.items[self.start] = x;
      self.start = (self.start + 1) % self.items.len();
    }
  }
// ...
impl Ring<T> {
  // Every slot starts as `blank`, until an item is added there.
  fn __init__(blank: T) {
    self.items = [blank, blank, blank, blank, blank, blank, blank, blank,
                  blank, blank, blank, blank, blank, blank, blank, blank];
    self.count = 0;
    self.start = 0;
  }
}
```

An array always holds all of its elements, so `__init__` takes a `blank` item to fill the
slots with until they are used, as the task list in [Arrays](06-arrays.md#project-the-task-list)
does. The history holds `(Str, float)` tuples, the statement and its value, so its type is
`Ring<(Str, float)>`, and its blank is an empty statement:

```pkn
// from: samples/imports/12_calc/main.pkn
  history: Ring<(Str, float)> = Ring<(Str, float)>(("", 0.0));
```

### Evaluating a statement

`evalStatement` ties the pieces together. It destructures the parser's tuple, checks the
optional `error` field with a `match`, and then matches on the optional target name to decide
between printing an assignment and printing an expression's value:

```pkn
// from: samples/imports/12_calc/main.pkn
fn evalStatement(src: Str, env: hashmap::HashMap,
                 history: Ring<(Str, float)>, quiet: bool) {
  p: parser::Parser = parser::Parser(src, env);
  target, value = p.parseStatement();

  match p.error {
    msg: Str {
      printerrln("error: " + msg);
      return;
    }
    None { }
  }
  if (!p.done()) {
    printerrln("error: unexpected '" + p.rest() + "'");
    return;
  }

  env.put("ans", value);
  history.add((src, value));
  match target {
    name: Str {
      env.put(name, value);
      if (quiet) { println(show(value)); }
      else { println(name + " = " + show(value)); }
    }
    None {
      if (quiet) { println(show(value)); }
      else { println(src + " = " + show(value)); }
    }
  }
}
```

### The REPL

The interactive loop is the standard-input idiom from [Files and I/O](11-files-and-io.md).
`readln` returns an `Obj`, so the end of input is caught by the `_` arm; `handle` returns
`False` for `:quit`:

```pkn
// from: samples/imports/12_calc/main.pkn
  while (True) {
    match Stdin.readln() {
      line: Str { if (!handle(line, env, history, quiet)) { return 0; } }
      _ { break; } // None — end of input
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
  quiet: bool = False;
  runDemo: bool = True;
  nexprs: int = 0;

  // -- command-line parsing: flags, repeatable -e, positional expressions --
  i: int = 1;
  while (i < args.len()) {
    a: view = args[i];
    if (a.equals("-h") || a.equals("--help")) {
      printUsage();
      return 0;
    } else if (a.equals("-q") || a.equals("--quiet")) {
      quiet = True;
    } else if (a.equals("--no-demo")) {
      runDemo = False;
    } else if (a.equals("-e") || a.equals("--eval")) {
      if (i + 1 < args.len()) {
        i = i + 1;
        nexprs = nexprs + 1;
      } else {
        printerrln("error: " + a + " requires an argument");
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
  if (nexprs > 0) {
    j: int = 1;
    while (j < args.len()) {
      if (args[j].equals("-e") || args[j].equals("--eval")) {
        j = j + 1;
        evalStatement(args[j].toString(), env, history, quiet);
      } else if (!isOption(args[j])) {
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

- Add a `^` operator: a new `Op` variant, a new precedence level in the parser, and a loop
  in `applyOp`. The compiler points out the `match` that no longer covers every variant.
- Add functions such as `abs(x)` and `sqrt(x)`: parse a name followed by `(`, and dispatch on
  the name with a value-mode `match` on the `Str`.
- Add `:save <file>` and `:load <file>` commands that write the variables with `open` and read
  them back with `readln` and `float(...)`.
- Run the result with `paykan --track-heap` to check that it still frees everything.

Back to the [manual's contents](index.md).

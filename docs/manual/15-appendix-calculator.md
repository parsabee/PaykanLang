# Appendix: a calculator

This appendix walks through a complete program from the repository,
[`samples/imports/12_calc`](../../samples/imports/12_calc/main.pkn): an arithmetic calculator
with variables, a command history and an interactive mode. At about 860 lines in four
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

Variables live in a hash table with separate chaining: an array of buckets, each an array of
`Entry` objects. Note the type `Entry[][]` and the loop that fills the buckets with empty
chains:

```pkn
// from: samples/imports/12_calc/calc/hashmap.pkn
class Entry {
  key: Str;
  value: float;
  fn __init__(k: Str, v: float) {
    self.key = k;
    self.value = v;
  }
}
// ...
class HashMap {
  buckets: Entry[][];
  size: int;
  nbuckets: int;

  fn __init__() {
    self.nbuckets = 8;
    self.size = 0;
    self.buckets = [];
    self.fill();
  }

  // Append `nbuckets` empty chains to a freshly-reset bucket array.
  fn fill() {
    i: int = 0;
    while (i < self.nbuckets) {
      chain: Entry[] = [];
      self.buckets.push(chain);
      i = i + 1;
    }
  }
```

`__init__` may call methods, like `fill` here. It does so after every field has a value,
which is the safe order: the compiler does not stop a method called earlier from reading a
field that is not set yet.

Looking a key up returns an **optional**, `Entry?`, so a missing variable is `None` rather
than a special value:

```pkn
// from: samples/imports/12_calc/calc/hashmap.pkn
  fn lookup(key: Str) -> Entry? {
    chain: Entry[] = self.buckets[self.indexFor(key)];
    i: int = 0;
    while (i < chain.len()) {
      e: Entry = chain[i];
      if (e.key.equals(key)) { return e; }
      i = i + 1;
    }
    return None;
  }
```

and `insert` unwraps it with an optional-mode `match`. Because `Entry` is a class, `e` in the
first arm is a reference to the entry stored in the table, so assigning `e.value` updates the
table in place:

```pkn
// from: samples/imports/12_calc/calc/hashmap.pkn
  fn insert(key: Str, value: float) -> bool {
    match self.lookup(key) {
      e: Entry {
        e.value = value; // update in place (Entry is a reference type)
        return False;
      }
      None {
        self.buckets[self.indexFor(key)].push(Entry(key, value));
        return True;
      }
    }
  }
```

The `match` covers both cases and returns from each arm, so the function needs no `return`
after it. Keys are compared with `e.key.equals(key)`, which compares contents just like
`e.key == key` would. (A comment at the top of the module predates `==` comparing strings by
content.)

`entries` returns every variable as an array of tuples, `(Str, float)[]`, for the `:vars`
command:

```pkn
// from: samples/imports/12_calc/calc/hashmap.pkn
  fn entries() -> (Str, float)[] {
    out: (Str, float)[] = [];
    bi: int = 0;
    while (bi < self.buckets.len()) {
      chain: Entry[] = self.buckets[bi];
      j: int = 0;
      while (j < chain.len()) {
        e: Entry = chain[j];
        out.push((e.key, e.value));
        j = j + 1;
      }
      bi = bi + 1;
    }
    return out;
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

  fn __init__(s: Str, env: hashmap::HashMap) {
    self.input = s;
    self.pos = 0;
    self.env = env;
  }

  // Record the first error only; later errors are usually cascades.
  fn fail(msg: Str) {
    if (self.error == None) { self.error = msg; }
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

The `:history` command shows the last 20 statements. They are kept in a generic ring buffer,
`Ring<T>`, declared in `main.pkn` itself because generic classes cannot be imported from
another module yet:

```pkn
// from: samples/imports/12_calc/main.pkn
class Ring<T> {
  items: T[];
  cap: int;
  start: int; // index of the oldest item once the buffer is full

  fn __init__(cap: int) {
    self.items = [];
    self.cap = cap;
    self.start = 0;
  }

  // Append `x`, overwriting the oldest item when the buffer is full.
  fn add(x: T) {
    if (self.items.len() < self.cap) {
      self.items.push(x);
    } else {
      self.items[self.start] = x;
      self.start = (self.start + 1) % self.cap;
    }
  }
```

The history holds `(Str, float)` tuples, the statement and its value, so its type is
`Ring<(Str, float)>`:

```pkn
// from: samples/imports/12_calc/main.pkn
  history: Ring<(Str, float)> = Ring<(Str, float)>(historySize());
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

`main` takes `args: Str[]` and walks it with an index, so that `-e` can consume the argument
after it. Exit status `2` signals a usage error, a common convention:

```pkn
// from: samples/imports/12_calc/main.pkn
fn main(args: Str[]) -> int {
  quiet: bool = False;
  runDemo: bool = True;
  exprs: Str[] = [];

  // -- command-line parsing: flags, repeatable -e, positional expressions --
  i: int = 1;
  while (i < args.len()) {
    a: Str = args[i];
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
        exprs.push(args[i]);
      } else {
        printerrln("error: " + a + " requires an argument");
        return 2;
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

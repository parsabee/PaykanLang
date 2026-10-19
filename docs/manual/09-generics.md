# Generics

Generic classes and functions are written once for any type. A `Stack<T>` works as a
`Stack<int>`, a `Stack<Str>` or a `Stack<Task>`, each fully type-checked. In this chapter
`tasks` gets an undo history built on a generic stack.

## Generic classes

Type parameters go in angle brackets after the class name, and stand for a type
everywhere in the class:

```pkn
class Pair<A, B> {
  first: A;
  second: B;
  fn swap() -> Pair<B, A> { return Pair<B, A>(self.second, self.first); }
}

impl Pair<A, B> {
  fn __init__(a: A, b: B) { self.first = a; self.second = b; }
}

fn main() -> int {
  p = Pair<Str, int>("answer", 42);
  q = p.swap();                     // Pair<int, Str>
  println(q.second + " = " + Str(q.first));

  r = Pair(2.5, True);              // Pair<float, bool>, inferred
  println(Str(r.first) + " " + Str(r.second));
  return 0;
}
```

Output:

```
answer = 42
2.5 True
```

Each distinct list of type arguments makes a separate, ordinary class: `Pair<Str, int>` and
`Pair<int, Str>` are different types, and so are `Box<Dog>` and `Box<Animal>` even when
`Dog` extends `Animal`. When the type arguments can be worked out from the constructor's
arguments, as in `Pair(2.5, True)`, you can leave them out.

A generic class may extend an ordinary class (`class Wrap<T> : Base`), but not a generic
one.

## Generic functions

A free function can have type parameters too. They are usually inferred from the arguments;
when they cannot be (for example when the parameter appears only in the return type), write
them explicitly:

```pkn
fn lastOf<T>(xs: view T[]) -> view T {
  return xs[xs.len() - 1];
}

fn indexOf<T>(xs: view T[], x: view T) -> int? {
  i = 0;
  while i < xs.len() {
    if xs[i] == x { return i; }
    i = i + 1;
  }
  return None;
}

fn three<T>(x: T) -> T[3] {
  return [x; 3];
}

fn main() -> int {
  println(Str(lastOf([3, 1, 4])));
  println(lastOf(["x", "y"]));
  println(indexOf(["a", "b", "c"], "c"));
  println(indexOf([1, 2], 5));
  words = three<Str>("ab");          // a Str[3]
  println(Str(words.len()) + " " + lastOf(words));
  println(Str(three(7)[1] + 1 == 8));
  return 0;
}
```

Output:

```
4
y
2
None
3 ab
True
```

A slice parameter (`view T[]`, see [Arrays](06-arrays.md#arrays-are-values)) takes `T` from
the array's elements, so `lastOf` works on an array of any type and length. `lastOf`
returns a `view` of the caller's element, and `indexOf` an `int?` that is `None` when the
value is not there.

Inference is exact: in `fn same<T>(a: T, b: T)`, the call `same(1, 2.0)` is an error because
`T` cannot be both `int` and `float`; `same<float>(1, 2.0)` is fine. Only free functions can
have their own type parameters; a method uses the type parameters of its generic class.

## How generic code is checked

There are no constraints on type parameters (no `T: Comparable`). Instead, the body of a
generic class or function is type-checked again for each set of type arguments it is used
with. If an operation does not work for one of them, the error points at the generic code
and says which instantiation needed it:

```pkn
class Doubler<T> {
  v: T;
  fn twice() -> T { return self.v + self.v; }
}

impl Doubler<T> {
  fn __init__(v: T) { self.v = v; }
}

fn main() -> int {
  a = Doubler<int>(21);     // fine: int has +
  s = Doubler<Str>("ab");   // fine: Str has + too
  b = Doubler<bool>(True);  // bool does not
  return 0;
}
```

Error:

```
error: operator '+' is not defined for types 'bool' and 'bool'
note: in instantiation of 'Doubler<bool>' requested here
```

A type parameter names a type, not a value: inside the generic code you can write `T`,
`T[3]`, `view T[]`, `T?`, `Box<T>` or `(T, int)` wherever a type goes, but `T()` and
`x = T` are errors.

Generic classes and functions can be imported like anything else a module declares:
`shapes::Box<int>(1)`, `b: shapes::Box<Str>` and `shapes::first(xs)` instantiate the
templates of `shapes` in your module. The body is checked for your type arguments as
`shapes` would check it, so an error in it points into `shapes`' file, with the note at
your call. Chapter 10 has more on modules.

## Project: undo with a generic stack

`tasks` remembers the tasks it removes, so the last removal can be undone. The history is a
`Stack<Task>`, and since nothing about a stack depends on what it holds, `Stack<T>` is
generic. It keeps its items in a `T?[8]` with a count, so its empty slots are `None`.
`push` returns `False` when the stack is full, and `pop` returns `T?`, `None` when the stack
is empty:

```pkn
class Stack<T> {
  items: T?[8];
  count: int;

  fn push(x: T) -> bool {
    if self.count == self.items.len() { return False; }
    self.items[self.count] = x;
    self.count = self.count + 1;
    return True;
  }

  fn pop() -> T? {
    if self.count == 0 { return None; }
    self.count = self.count - 1;
    top = self.items[self.count];
    self.items[self.count] = None;
    return top;
  }

  view fn size() -> int { return self.count; }
}

impl Stack<T> {
  fn __init__() {
    self.items = [None; 8];
    self.count = 0;
  }
}

class Task {
  title: Str;
}

impl Task {
  fn __init__(title: Str) { self.title = title; }
}

class TaskList {
  tasks: Task[8];
  count: int;
  removed: Stack<Task>;

  fn add(t: Task) -> bool {
    if self.count == self.tasks.len() { return False; }
    self.tasks[self.count] = t;
    self.count = self.count + 1;
    return True;
  }

  // Removes task number `n`, moving the ones after it down.
  fn remove(n: int) {
    if n < 1 || n > self.count { return; }
    self.removed.push(self.tasks[n - 1]);
    i = n;
    while i < self.count {
      self.tasks[i - 1] = self.tasks[i];
      i = i + 1;
    }
    self.count = self.count - 1;
  }

  fn undo() {
    match self.removed.pop() {
      t: Task {
        if self.add(t) { println("restored '" + t.title + "'"); }
        else { println("no room for '" + t.title + "'"); }
      }
      None    { println("nothing to undo"); }
    }
  }

  view fn titles() -> Str {
    out = "";
    i = 0;
    while i < self.count {
      out = out + (if i > 0 then ", " else "") + self.tasks[i].title;
      i = i + 1;
    }
    return out;
  }
}

impl TaskList {
  fn __init__() {
    self.tasks = [Task(""); 8];
    self.count = 0;
    self.removed = Stack<Task>();
  }
}

fn main() -> int {
  list = TaskList();
  list.add(Task("buy milk"));
  list.add(Task("fix the leak"));
  list.add(Task("call mum"));
  list.remove(1);
  list.remove(1);
  println(list.titles());
  list.undo();
  println(list.titles());
  list.undo();
  list.undo();

  numbers = Stack<int>();       // the same Stack, for ints
  numbers.push(1);
  numbers.push(2);
  size = numbers.size();
  match numbers.pop() {
    top: int { println(Str(size) + " numbers, top " + Str(top)); }
    None     { }
  }
  return 0;
}
```

Output:

```
call mum
restored 'fix the leak'
call mum, fix the leak
restored 'buy milk'
nothing to undo
2 numbers, top 2
```

The reference chapter is [Generics](../language/11-generics.md). Next:
[Modules and imports](10-modules.md), where `tasks` is split into several files.

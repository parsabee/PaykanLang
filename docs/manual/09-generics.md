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
  fn __init__(a: A, b: B) { self.first = a; self.second = b; }
  fn swap() -> Pair<B, A> { return Pair<B, A>(self.second, self.first); }
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
fn lastOf<T>(xs: T[]) -> T? {
  if (xs.len() == 0) { return None; }
  return xs[xs.len() - 1];
}

fn repeat<T>(x: T, n: int) -> T[] {
  out: T[] = [];
  i = 0;
  while (i < n) {
    out.push(x);
    i = i + 1;
  }
  return out;
}

fn main() -> int {
  println(lastOf([3, 1, 4]));
  println(lastOf(["x", "y"]));
  empty: Str[] = [];
  println(lastOf(empty));
  println(Str(repeat<Str>("ab", 3).len()));
  println(Str(repeat(7, 2)[1] + 1 == 8));
  return 0;
}
```

Output:

```
4
y
None
3
True
```

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
  fn __init__(v: T) { self.v = v; }
  fn twice() -> T { return self.v + self.v; }
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
`T[]`, `T?`, `Box<T>` or `(T, int)` wherever a type goes, but `T()` and `x = T` are errors.

Generic classes and functions **cannot be imported from another module yet**
([#57](https://github.com/parsabee/PaykanLang/issues/57)): keep each generic declaration in
the module that uses it. A module can still return values of its own instantiations, such as
a `Box<int>`, to its importers. Chapter 10 has more on modules.

## Project: undo with a generic stack

`tasks` remembers the tasks it removes, so the last removal can be undone. The history is a
`Stack<Task>`, and since nothing about a stack depends on what it holds, `Stack<T>` is
generic. `pop` returns `T?`, `None` when the stack is empty:

```pkn
class Stack<T> {
  items: T[];
  fn __init__() { self.items = []; }
  fn push(x: T) { self.items.push(x); }
  fn pop() -> T? {
    if (self.items.len() == 0) { return None; }
    return self.items.pop();
  }
  fn size() -> int { return self.items.len(); }
}

class Task {
  title: Str;
  fn __init__(title: Str) { self.title = title; }
}

class TaskList {
  tasks: Task[];
  removed: Stack<Task>;

  fn __init__() {
    self.tasks = [];
    self.removed = Stack<Task>();
  }

  fn add(title: Str) { self.tasks.push(Task(title)); }

  fn remove(n: int) {
    kept: Task[] = [];
    i = 0;
    while (i < self.tasks.len()) {
      if (i == n - 1) { self.removed.push(self.tasks[i]); }
      else { kept.push(self.tasks[i]); }
      i = i + 1;
    }
    self.tasks = kept;
  }

  fn undo() {
    match self.removed.pop() {
      t: Task { self.tasks.push(t); println("restored '" + t.title + "'"); }
      None    { println("nothing to undo"); }
    }
  }

  fn titles() -> Str {
    out = "";
    i = 0;
    while (i < self.tasks.len()) {
      out = out + (if i > 0 then ", " else "") + self.tasks[i].title;
      i = i + 1;
    }
    return out;
  }
}

fn main() -> int {
  list = TaskList();
  list.add("buy milk");
  list.add("fix the leak");
  list.add("call mum");
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
  println(Str(numbers.size()) + " numbers, top " + Str(numbers.items[1]));
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

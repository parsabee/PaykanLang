# Optionals and None

Sometimes a value may be missing: a search finds nothing, a string is not a number, a task
has no deadline. PaykanLang expresses this with **optional types**. A `T?` holds either a
`T` or `None`, and the compiler makes sure you check which before using the value. In this
chapter `tasks` learns to look tasks up and to give them an optional deadline.

## Optional types

Add `?` to a type to make it optional: `Str?`, `int?`, `Task?`, `int[]?`. Any class, `Str`,
array, `int`, `float`, `bool` and `char` can be optional. `None` is the absent value:

```pkn
fn main() -> int {
  nickname: Str? = None;
  age: int? = 36;          // an int converts to int? automatically
  println(nickname);
  println(age);
  nickname = "Gracie";
  println(nickname);
  return 0;
}
```

Output:

```
None
36
Gracie
```

A `T` converts to `T?` automatically, so functions returning `T?` simply `return` a `T` or
`None`. The other direction never happens implicitly: a `T?` cannot be used as a `T`, not in
arithmetic, not to call a method, not to pass to a parameter of type `T`:

```pkn
fn main() -> int {
  age: int? = 36;
  nextYear = age + 1;
  return 0;
}
```

Error:

```
error: cannot use optional 'int?' as 'int' without unwrapping (use match)
```

## Unwrapping with match

`match` on an optional has an arm for the present case, which names the wrapped type and
binds the value, and an arm for `None`. Together the two are exhaustive:

```pkn
fn half(n: int) -> int? {
  if (n % 2 != 0) { return None; }
  return n / 2;
}

fn describe(n: int) -> Str {
  match half(n) {
    h: int { return Str(n) + " halves to " + Str(h); }
    None   { return Str(n) + " is odd"; }
  }
}

fn main() -> int {
  println(describe(10));
  println(describe(7));
  return 0;
}
```

Output:

```
10 halves to 5
7 is odd
```

Inside the first arm, `h` is a plain `int`. With a class type the present arm matches every
object of that class *or any subclass*, unlike a class `match` on a non-optional value,
which compares exact classes.

To just test for presence, compare with `None`: `x == None` and `x != None` work on any
optional. But a test does not unwrap: after `if (x != None)` the type of `x` is still `T?`,
so you still need a `match` to use the value. Two optionals of the same type can be
compared with `==` as well; they are equal when both are `None` or both hold equal values.

## Optional fields

A field whose type is optional starts out as `None`, so `__init__` does not have to assign
it. This is the natural way to model something that is set later, or not at all:

```pkn
class Person {
  name: Str;
  email: Str?;
  fn __init__(name: Str) { self.name = name; }

  view fn contact() -> Str {
    match self.email {
      e: Str { return self.name + " <" + e + ">"; }
      None   { return self.name + " (no email)"; }
    }
  }
}

fn main() -> int {
  p = Person("Ada");
  println(p.contact());
  p.email = "ada@example.org";
  println(p.contact());
  return 0;
}
```

Output:

```
Ada (no email)
Ada <ada@example.org>
```

## Linked structures

Optionals make linked structures easy to express, because "no next node" is just `None`.
Here is a minimal linked list with a search that returns `Node?`:

```pkn
class Node {
  value: int;
  next: Node?;
  fn __init__(v: int, next: Node?) { self.value = v; self.next = next; }
}

fn find(head: Node?, wanted: int) -> Node? {
  cur = head;
  while (cur != None) {
    match cur {
      n: Node {
        if (n.value == wanted) { return n; }
        cur = n.next;
      }
      None { }
    }
  }
  return None;
}

fn main() -> int {
  list = Node(1, Node(2, Node(3, None)));
  match find(list, 2) {
    n: Node { println("found " + Str(n.value)); }
    None    { println("not found"); }
  }
  println(Str(find(list, 9) == None));
  return 0;
}
```

Output:

```
found 2
True
```

## Things to know

- `None` on its own has the type `Obj`, the root class. So `x = None;` declares an `Obj`
  variable, not an optional: write the type, as in `x: Task? = None;`. The same goes for
  an array literal holding `None`: `ts: Task?[] = [None]` needs its annotation.
- `T?[]` is an array whose elements may be `None`; `T[]?` is an array that may itself be
  absent.
- `if c then value else None` has the type `T?` when `value` is a `T`.
- An enum cannot be optional yet (`Direction?` is rejected), and neither can a tuple.
  Return a class, or a tuple with a `bool` that says whether the value is valid.
- There is no `?.`, `??` or `if let` shorthand in this release; `match` is the way to unwrap.
- Some older built-in functions use `Obj` rather than optionals to say "nothing": the file
  reading methods return a `Str` or `None` as an `Obj`. [Files and I/O](11-files-and-io.md)
  shows how to `match` on those.

## Project: finding tasks and optional deadlines

Two changes to `tasks`. A deadline becomes an optional field instead of a subclass, so any
task can get one later. And `TaskList.find` looks a task up by its title, returning `None`
when there is none, which lets the caller decide what to do. It isn't a `view fn`, because
the caller may change the task it returns:

```pkn
class Task {
  title: Str;
  done: bool;
  due: Str?;

  fn __init__(title: Str) {
    self.title = title;
    self.done = False;
  }

  view fn toString() -> Str {
    text = (if self.done then "[x] " else "[ ] ") + self.title;
    match self.due {
      d: Str { return text + " (due " + d + ")"; }
      None   { return text; }
    }
  }
}

class TaskList {
  tasks: Task[];
  fn __init__() { self.tasks = []; }
  fn add(title: Str) { self.tasks.push(Task(title)); }

  fn find(title: Str) -> Task? {
    i = 0;
    while (i < self.tasks.len()) {
      if (self.tasks[i].title == title) { return self.tasks[i]; }
      i = i + 1;
    }
    return None;
  }
}

fn setDue(list: TaskList, title: Str, due: Str) {
  match list.find(title) {
    t: Task { t.due = due; println(t); }
    None    { println("no task called '" + title + "'"); }
  }
}

fn main() -> int {
  list = TaskList();
  list.add("file taxes");
  list.add("buy milk");
  setDue(list, "file taxes", "April 15");
  setDue(list, "walk the dog", "today");
  println(list.tasks[1]);
  return 0;
}
```

Output:

```
[ ] file taxes (due April 15)
no task called 'walk the dog'
[ ] buy milk
```

The reference chapter, [Optional types](../language/10-optionals.md), has the complete
rules. Next: [Generics](09-generics.md).

# `::result`

The results of what can fail. A `Result<T, E>` is either the value an operation made,
`Ok(value)`, or the error saying why it could not, `Err(error)`: a [generic tagged
union](../language/07-enums.md#generic-enums), taken apart with `match`. A function of the
standard library that can fail returns a `Result<T, Error>`, its error an
[`::error`](error.md) `Error`, and a program can return results of its own, with the
standard `Error` or an error type of its own. The module is imported by convention as
`import ::result as res;`, and its names are written `res::Result`, `res::ok(...)`,
`res::fail(...)`.

```pkn
import ::io;
import ::error as err;
import ::result as res;

fn parsePort(s: Str) -> res::Result<int, err::Error> {
  match s.toInt() {
    n: int {
      if n > 0 && n < 65536 { return res::ok(n); }
      return res::fail(err::Error(err::ErrorCode::InvalidArgument, "out of range: " + s));
    }
    None { return res::fail(err::Error(err::ErrorCode::InvalidArgument, None)); }
  }
}

fn main() -> int {
  let inputs: Str[3] = ["8080", "99999", "http"];
  for i in 0..3 {
    match parsePort(inputs[i]) {
      p: Ok  { io::println("port " + Str(p.value)); }
      e: Err { io::println("error: " + e.error.message()); }
    }
  }
  return 0;
}
```

Output:

```
port 8080
error: out of range: 99999
error: InvalidArgument
```

---

## Using a result

A `Result` is an enum whose variants have fields, a value held in place and copied on
assignment ([Variants With Fields](../language/07-enums.md#variants-with-fields)). A
`match` takes it apart: the arm `p: Ok` binds the `Ok` variant's field, `p.value`, and the
arm `e: Err` the `Err` variant's, `e.error`, as `main` does above. An arm for each of the
two covers every case, so a function may return from both without a `return` after the
`match`; a `_` arm stands for the one not named (`match f.readln() { l: Ok { ... } _ {
break; } }`). To test for one code of an `Error`, compare it in the `Err` arm:
`e.error.code == err::ErrorCode::NotFound`.

A result is made with [`ok`](#ok) or `Result::Ok(value)` when the operation made a value,
and with [`fail`](#fail) or `Result::Err(error)` when it did not. Each takes the type
arguments it does not get from its argument from the type the result flows into: `return
res::ok(n);` in a function returning a `res::Result<int, err::Error>` is that `Result`
([Generic Enums](../language/07-enums.md#generic-enums)). Where nothing gives them, write
them: `res::Result<int, Str>::Err("no")`, or give the value a type, `r:
res::Result<int, Str> = res::ok(1);`.

`Result<T, E>` works for every `T` and `E`. For an optional `T` the value is that optional:
a `Result<Str?, err::Error>` that is `Ok` with the value `None` is a success that found
nothing, and an `Err` a failure, so the two are told apart.

Printing a `Result` with `io::println` shows its variant and its field:
`Ok(value: 8080)`, `Err(error: Error(code: NotFound, msg: No such file or directory))`.
`==` and `!=` compare two results of the same type, their variant and its field.

---

## API

### `Result`

```pkn
enum Result<T, E> {
  Ok(value: T),
  Err(error: E)
}
```

The value an operation made, `Ok`, or the error saying why it could not, `Err`. The
standard library's results are `Result<T, Error>`, with [`::error`](error.md)'s `Error`.

### `ok`

```pkn
fn ok<T, E>(value: T) -> Result<T, E>
```

The result of an operation that made `value`: `Result::Ok(value)`. `T` is inferred from the
argument and `E` from the type the result flows into (`return res::ok(n);` in a function
returning a `Result<int, Error>`, `r: Result<int, Error> = res::ok(1)`).

### `fail`

```pkn
fn fail<T>(error: err::Error) -> Result<T, err::Error>
```

The result of an operation that failed with `error`, an [`::error`](error.md) `Error`:
`Result::Err(error)`. `T` is taken from the type the result flows into (`return
res::fail(e);` in a function returning a `res::Result<int, err::Error>`); where nothing
gives it, write it, `res::fail<int>(e)`. A result whose error is of another type is made
with `Result::Err(error)`.

# `::result`

Errors as values. An `Error` says what went wrong: a code from the standard set,
`ErrorCode`, and, when there is more to say, a message. A function of the standard library
that can fail returns a `Result<T>`, an error and the value it made when there was none (or
just an `Error`, when it makes no value), and a program can use the same types for its own
errors. The module is imported by convention as `import ::result as res;`, and its names are
written `res::Error`, `res::ok(...)`.

```pkn
import ::io;
import ::result as res;

fn parsePort(s: Str) -> res::Result<int> {
  match int(s) {
    n: int {
      if n > 0 && n < 65536 { return res::ok(n); }
      return res::Result<int>(
          res::Error(res::ErrorCode::InvalidArgument, "out of range: " + s), None);
    }
    None {
      return res::Result<int>(res::Error(res::ErrorCode::InvalidArgument, None), None);
    }
  }
}

fn main() -> int {
  inputs: Str[3] = ["8080", "99999", "http"];
  i = 0;
  while i < inputs.len() {
    r = parsePort(inputs[i]);
    match r.value {
      port: int { io::println("port " + Str(port)); }
      None      { io::println("error: " + r.error.message()); }
    }
    i = i + 1;
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

`Error` and `Result` are [structs](../language/11-structs.md), values copied on
assignment. A struct is not a `match` subject, so a result is taken apart by matching its
`value`, in [optional mode](../language/08-match.md#optional-mode--unwrapping-a-t), and
reading its `error` in the `None` arm, as `main` does above. To test for one code, compare
it: `r.error.code == res::ErrorCode::NotFound`, or `match r.error.code { ... }` with an arm
for each code of interest and a `_` arm.

`Result<T>` works for every `T`. For an optional `T` the `value` is a `T` itself, not a
nested optional (`Result<Str?>`'s `value` is a `Str?`, see
[Generics](../language/13-generics.md)), so a value of `None` does not tell a failure from
a success that made `None`: test `isOk()` instead.

The rule that `value` is present exactly when the code is `Ok` is the library's
convention, kept by its functions and by `ok`. The struct's initializer does not check
it: `res::Result<int>(res::none(), None)` is a result that says `isOk()` and has no value.

Printing an `Error` or a `Result` with `io::println` shows its fields, with the code as
its ordinal (`Error(code: 1, msg: No such file or directory)`); `message()` and `name(code)`
are the texts meant for people.

---

## API

### `ErrorCode`

```pkn
enum ErrorCode { Ok, NotFound, PermissionDenied, ... }
```

What went wrong; `Ok` is no error. The variants, in their order, with the C `errno` values
that [`fromErrno`](#fromerrno) maps to each:

| Variant | Meaning | From `errno` |
|---------|---------|--------------|
| `Ok` | no error | `0` |
| `NotFound` | there is no such file or entry | `ENOENT` |
| `PermissionDenied` | not allowed | `EACCES`, `EPERM` |
| `AlreadyExists` | it exists already | `EEXIST` |
| `InvalidArgument` | an argument is not one the operation takes | `EINVAL`, `EBADF` (a file not open for the operation) |
| `IsADirectory` | a directory where a file is needed | `EISDIR` |
| `NotADirectory` | a file where a directory is needed | `ENOTDIR` |
| `TooManyOpenFiles` | the process or the system has too many open | `EMFILE`, `ENFILE` |
| `NoSpace` | the device is full | `ENOSPC` |
| `ReadOnly` | the file system is read-only | `EROFS` |
| `Busy` | the resource is in use | `EBUSY` |
| `Interrupted` | a signal interrupted the operation | `EINTR` |
| `WouldBlock` | the operation would block | `EAGAIN`, `EWOULDBLOCK` |
| `BrokenPipe` | the other end of a pipe is closed | `EPIPE` |
| `TimedOut` | the operation took too long | `ETIMEDOUT` |
| `OutOfMemory` | there is not enough memory | `ENOMEM` |
| `Unsupported` | the operation is not supported | `ENOSYS`, `ENOTSUP`, `EOPNOTSUPP` |
| `Io` | a low-level input or output error | `EIO` |
| `EndOfFile` | a read found nothing left | (none: made by `::io`'s reads) |
| `OutOfRange` | an index or a count past the end | (none: made by `::memory`) |
| `Other` | any other error | every other `errno` |

### `Error`

```pkn
struct Error {
  code: ErrorCode;
  msg: Str?;
}
```

An error: what went wrong, and a message saying more, or `None`. It is made with the
struct's initializer, `res::Error(res::ErrorCode::InvalidArgument, "out of range")` (a
`Str` literal for `msg` is fine, or `None`), or by [`none`](#none) and
[`fromErrno`](#fromerrno). Two errors are `==` when their codes and messages are.

#### `Error.isOk`

```pkn
view fn isOk() -> bool
```

Whether this is no error: its code is `Ok`.

#### `Error.message`

```pkn
view fn message() -> Str
```

The message, or the code's name ([`name`](#name)) when `msg` is `None`: `"InvalidArgument"`.

### `Result<T>`

```pkn
struct Result<T> {
  error: Error;
  value: T?;
}
```

The result of an operation that can fail: its error (code `Ok` when it did not), and the
value it made, present when it did not fail. A success is made with [`ok`](#ok), a failure
with the initializer and `None`: `res::Result<int>(res::Error(res::ErrorCode::NotFound,
None), None)`.

#### `Result.isOk`

```pkn
view fn isOk() -> bool
```

Whether the operation did not fail: `error.code` is `Ok`.

### `none`

```pkn
fn none() -> Error
```

No error: `Error(ErrorCode::Ok, None)`. A function that returns an `Error` returns this when
it succeeded.

### `ok`

```pkn
fn ok<T>(value: T) -> Result<T>
```

The result of an operation that made `value`: `Result<T>(none(), value)`. `T` is inferred
from the argument (`res::ok(n)` with `n: int` is a `Result<int>`).

### `name`

```pkn
fn name(code: ErrorCode) -> Str
```

The code's name, as it is spelled in `ErrorCode`: `"NotFound"` for `ErrorCode::NotFound`.

### `fromErrno`

```pkn
fn fromErrno(errno: int) -> Error
```

The `Error` of a C library failure whose `errno` is `errno`: its code by the table under
[`ErrorCode`](#errorcode), and C's `strerror` text as its message (`No such file or
directory` for `ENOENT`; for a number C does not know, the code `Other` and C's text for
it, `Unknown error 99999` with glibc). `fromErrno(0)` is `none()`. It is for code that calls C through
[native functions](../language/03-functions.md#native-functions), as `::io` does.

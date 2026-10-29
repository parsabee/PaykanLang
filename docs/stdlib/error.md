# `::error`

What went wrong, as a value. An `Error` says what went wrong: a code from the standard
set, `ErrorCode`, and, when there is more to say, a message. A function of the standard
library that can fail returns a [`::result`](result.md) `Result<T, Error>`, the value it
made or the `Error` saying why it could not, or just an `Error` when it makes no value, and
a program can use the same types for its own errors. The module is imported by convention
as `import ::error as err;`, and its names are written `err::Error`, `err::none()`.

```pkn
import ::io;
import ::error as err;
import ::math;
import ::result as res;

// The square root of a number that has one.
fn root(x: float) -> res::Result<float, err::Error> {
  if x < 0.0 {
    return res::fail(err::Error(err::ErrorCode::InvalidArgument, "negative: " + Str(x)));
  }
  return res::ok(math::sqrt(x));
}

fn main() -> int {
  match root(-4.0) {
    r: Ok  { io::println(r.value); }
    e: Err { io::println(err::name(e.error.code) + ": " + e.error.message()); }
  }
  match io::open("/no/such/file", "r") {
    f: Ok  { io::println("opened"); }
    e: Err { io::println(e.error); }
  }
  let b = err::none();
  io::println(Str(b.isOk()) + " " + b.message());
  return 0;
}
```

Output:

```
InvalidArgument: negative: -4
Error(code: NotFound, msg: No such file or directory)
True Ok
```

---

## Using an error

`Error` is a [struct](../language/11-structs.md), a value copied on assignment. Its `code`
is an enum, so one code is tested by comparing it, `e.code == err::ErrorCode::NotFound`, or
several with `match e.code { NotFound { ... } PermissionDenied { ... } _ { ... } }`.
`message()` is the text meant for people: the message, or the code's name when there is
none. Printing an `Error` with `io::println` shows its fields, `Error(code: NotFound, msg:
No such file or directory)`, and two errors are `==` when their codes and messages are.

An operation that makes no value returns just its `Error`: [`none`](#none) when it did not
fail, whose code is `Ok`, so `isOk()` tells the two apart (`if !f.write(s).isOk() { ...
}`). One that makes a value returns a `Result`, and `::result`'s [`fail`](result.md#fail)
makes the result of one that failed.

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
struct's initializer, `err::Error(err::ErrorCode::InvalidArgument, "out of range")` (a
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

### `none`

```pkn
fn none() -> Error
```

No error: `Error(ErrorCode::Ok, None)`. A function that returns an `Error` returns this when
it succeeded.

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
it, `Unknown error 99999` with glibc, and for one outside C's `int`, which `strerror`
cannot take, `Other` and `Unknown error <errno>` on every platform). `fromErrno(0)` is
`none()`. It is for code that calls C through
[native functions](../language/03-functions.md#native-functions), as `::io` does.

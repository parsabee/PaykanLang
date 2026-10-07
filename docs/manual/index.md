# The PaykanLang manual

This manual teaches PaykanLang to a programmer who already knows another language (C,
Java, Python, Go, Swift or similar) but has never seen this one. It starts with installing
the compiler and printing a line, and ends with programs split across modules that read
files, take command-line arguments and manage their memory without leaks.

Along the way the chapters build a real program: **`tasks`**, a small to-do list manager.
It starts as a few functions in chapter 2, gets a `Task` class in chapter 4, priorities in
chapter 5, a list in chapter 6, and by chapter 11 it is a multi-file project that saves its
tasks to a file and takes commands from the command line. Each chapter introduces the
features first with small programs and then puts them to work on `tasks`.

## Chapters

1. [Getting started](01-getting-started.md): install PaykanLang, run and build your first
   program.
2. [Basics](02-basics.md): variables and type inference, the built-in types, operators,
   control flow and functions.
3. [Strings and conversions](03-strings-and-conversions.md): `Str`, `char`, and converting
   between types.
4. [Classes and inheritance](04-classes.md): fields, methods, constructors, overriding,
   `toString` and `equals`.
5. [Enums and `match`](05-enums-and-match.md): closed sets of values and the four ways to
   `match`.
6. [Arrays](06-arrays.md): growable arrays, nested arrays, arrays of objects.
7. [Tuples](07-tuples.md): returning several values and destructuring them.
8. [Optionals and `None`](08-optionals.md): values that may be absent, and how to unwrap
   them.
9. [Generics](09-generics.md): generic classes and functions.
10. [Modules and imports](10-modules.md): projects with several files.
11. [Files and I/O](11-files-and-io.md): reading and writing files, command-line arguments
    and standard input.
12. [Memory](12-memory.md): reference counting, `--track-heap` and reference cycles.
13. [The `paykan` command](13-the-paykan-command.md): every option, the backends and the
    compilation cache.
14. [Extending PaykanLang](14-extending.md): frontend and backend plugins.
15. [Appendix: a calculator](15-appendix-calculator.md): a complete multi-module program,
    walked through.

## Conventions

Code is shown in blocks like this one, and a block labelled **Output** shows exactly what
the program prints:

```pkn
fn main() -> int {
  println("Hello from the manual");
  return 0;
}
```

Output:

```
Hello from the manual
```

Every program in this manual is compiled and run by PaykanLang's test suite (the
`DocExamples` test), which checks that it prints exactly the output shown and leaks no
memory. A block labelled **Error** shows a program the compiler rejects, with the
diagnostic it prints. Shell commands are shown with a `$` prompt.

When you want the precise rules for a feature, every chapter links to the matching chapter
of the [language reference](../language/index.md).

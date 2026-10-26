// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: the types a `native fn` may take and return (#198), and how:
// only a scalar crosses by value, anything else as a `view` or an `inout`.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Native, BoundaryTypesAreAccepted) {
  auto r = semaCheck(R"(
    native fn put(fd: int, s: view Str, x: float, b: bool, c: char,
                  o: view Obj) -> int = "pk_test_put";
    native fn get(fd: int) -> Str? = "pk_test_get";
    native fn any() -> Obj? = "pk_test_any";
    native fn name() -> Str = "pk_test_name";
    native fn done() = "pk_test_done";
    fn main() -> int {
      n: int = put(1, "x", 1.5, True, 'c', "o");
      line: Str? = get(0);
      done();
      return n;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Native, CallsAreTypeChecked) {
  auto r = semaCheck(R"(
    native fn put(fd: int, s: view Str) -> int = "pk_test_put";
    fn main() -> int { return put("x", 1); }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Native, OtherTypesCannotCross) {
  for (const char *decl : {
           "native fn f(xs: view int[]) = \"pk_f\";",
           "native fn f(p: view P) = \"pk_f\";",
           "native fn f(t: view (int, int)) = \"pk_f\";",
           "native fn f(s: view Str?) = \"pk_f\";",
           "native fn f() -> int? = \"pk_f\";",
           "native fn f() -> P = \"pk_f\";",
           "native fn f() -> Str[2] = \"pk_f\";",
       }) {
    auto r = semaCheck(std::string("class P { x: int; }\n") + decl +
                       "\nfn main() -> int { return 0; }");
    EXPECT_FALSE(r.Ok) << decl;
    EXPECT_NE(r.Diagnostics.find("cannot cross into C"), std::string::npos)
        << decl << "\n"
        << r.Diagnostics;
  }
}

// C borrows a reference (`view`) or changes it in place (`inout`); a call
// never shares one with it.  Only a scalar is passed by value.
TEST(Native, OnlyScalarsCrossByValue) {
  auto r = semaCheck(R"(
    native fn f(n: int, x: float, b: bool, c: char, k: view int,
                m: inout int, s: view Str, o: inout Obj) -> int = "pk_f";
    native fn g<T>(n: int, x: view T, y: inout T) = "pk_g";
    enum Dir { Up, Down }
    native fn h(d: Dir) -> Dir = "pk_h"; // an enum, as its ordinal
    fn main() -> int {
      m = 1;
      o: Obj = "o";
      a = 1;
      g<int>(1, 2, a);
      return f(1, 2.0, True, 'c', 3, m, "s", o);
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  for (const char *decl : {
           "native fn f(s: Str) = \"pk_f\";",
           "native fn f(n: int, o: Obj) -> int = \"pk_f\";",
           "native fn f<T>(x: T) = \"pk_f\";", // whatever T will be
       }) {
    r = semaCheck(std::string(decl) + "\nfn main() -> int { return 0; }");
    EXPECT_FALSE(r.Ok) << decl;
    EXPECT_NE(r.Diagnostics.find("must be 'view' or 'inout'"),
              std::string::npos)
        << decl << "\n"
        << r.Diagnostics;
  }
  r = semaCheck("native fn f(s: Str) = \"pk_f\";\n"
                "fn main() -> int { return 0; }");
  EXPECT_NE(r.Diagnostics.find("('s: view Str'); only int, float, bool, "
                               "char, a sized numeric type and an enum "
                               "without fields cross into C by value"),
            std::string::npos)
      << r.Diagnostics;
}

// A sized numeric type crosses by value, as its 8-byte slot, and both
// messages that list what crosses say so.
TEST(Native, SizedTypesCrossByValue) {
  auto r = semaCheck(R"(
    native fn f(a: u8, b: i32, c: unsigned, d: f32) -> u16 = "pk_f";
    fn main() -> int { return int(f(1, 2, 3, 4.0)); }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  r = semaCheck("class A {}\n"
                "native fn g(a: A?) -> int = \"pk_g\";\n"
                "fn main() -> int { return 0; }");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("parameter 'a' of type 'A?' cannot cross into "
                               "C (int, float, bool, char, a sized numeric "
                               "type, an enum without fields, Str and Obj "
                               "can,"),
            std::string::npos)
      << r.Diagnostics;
}

// A `view` result is part of a `view` or `inout` parameter.
TEST(Native, ViewResultNeedsABorrowedParameter) {
  auto r = semaCheck(R"(
    native fn f(s: view Str) -> view Str = "pk_f";
    native fn g(s: inout Str) -> view Str = "pk_g";
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  for (const char *decl : {
           "native fn f(n: int) -> view Str = \"pk_f\";",
           "native fn f() -> view Str = \"pk_f\";",
       }) {
    r = semaCheck(std::string(decl) + "\nfn main() -> int { return 0; }");
    EXPECT_FALSE(r.Ok) << decl;
    EXPECT_NE(r.Diagnostics.find("has no 'view' or 'inout' parameter"),
              std::string::npos)
        << decl << "\n"
        << r.Diagnostics;
  }

  r = semaCheck("native fn f(n: inout int) -> inout int = \"pk_f\";\n"
                "fn main() -> int { return 0; }");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("cannot return 'inout'"), std::string::npos)
      << r.Diagnostics;
}

// An instance of a generic native function passes what its template typed
// `T` by address, so any type crosses there; the rest crosses as for any
// native function.  Its calls are checked like any generic function's.
TEST(Native, GenericInstancesTakeAnyType) {
  auto r = semaCheck(R"(
    class P { x: int; }
    native fn pick<T>(first: bool, a: view T, b: view T) -> T = "pk_pick";
    native fn get<T>(b: view Obj, i: int) -> view T = "pk_get";
    native fn swap<T>(a: inout T, b: inout T) = "pk_swap";
    fn main() -> int {
      p: P = pick<P>(True, P(1), P(2));
      t = pick<(int, Str)>(False, (1, "a"), (2, "b"));
      o: int? = pick<int?>(True, None, 3);
      b: Obj = P(3);
      y: int = get<int>(b, 0);
      n: int = pick(True, 1, 2);
      q = P(4);
      swap<P>(p, q);
      return n + t.0 + y;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  r = semaCheck(R"(
    native fn at<T>(xs: view T[], i: int) -> view T = "pk_at";
    fn main() -> int { xs: int[2] = [1, 2]; return at<int>(xs, 0); }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("parameter 'xs' of type 'int[]' cannot cross "
                               "into C"),
            std::string::npos)
      << r.Diagnostics;

  r = semaCheck(R"(
    native fn pick<T>(first: bool, a: view T, b: view T) -> T = "pk_pick";
    fn main() -> int { return pick<int>(True, 1, "x"); }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Native, MainCannotBeNative) {
  auto r = semaCheck("native fn main() -> int = \"pk_main\";");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'main' cannot be a native function"),
            std::string::npos)
      << r.Diagnostics;
}

// A C symbol names one C function: native functions may bind it again only
// with the C signature it has (finding 39).  Paykan types that cross as
// the same C type give the same signature.
TEST(Native, OneCSymbolHasOneSignature) {
  auto r = semaCheck(R"(
    enum Dir { Up, Down }
    native fn f(x: int) -> int = "llabs";
    native fn g(y: int) -> int = "llabs";
    native fn h(x: u32) -> i16 = "llabs";
    native fn d(x: Dir) -> int = "llabs";
    native fn len(s: view Str) -> int = "pk_len";
    native fn size(o: view Obj) -> int = "pk_len";
    native fn pick<T>(a: view T) -> T = "pk_pick";
    fn main() -> int {
      x = pick<int>(1);
      s = pick<Str>("s");
      return f(-3) + g(-4) + int(h(5)) + d(Dir::Up) + len(s) + size("b") + x;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  for (const char *decls : {
           // a reference against a scalar
           "native fn f(s: view Str) -> int = \"pk_s\";\n"
           "native fn g(s: int) -> int = \"pk_s\";",
           // a float against an int, in a parameter and the result
           "native fn f(x: int) -> int = \"llabs\";\n"
           "native fn g(x: float) -> float = \"llabs\";",
           // a result against none
           "native fn f(x: int) = \"pk_s\";\n"
           "native fn g(x: int) -> int = \"pk_s\";",
           // a value against its address
           "native fn f(x: int) -> int = \"pk_s\";\n"
           "native fn g(x: inout int) -> int = \"pk_s\";",
           // another parameter
           "native fn f(x: int) -> int = \"pk_s\";\n"
           "native fn g(x: int, y: int) -> int = \"pk_s\";",
       }) {
    r = semaCheck(std::string(decls) + "\nfn main() -> int { return 0; }");
    EXPECT_EQ(r.ErrorCount, 1u) << decls << "\n" << r.Diagnostics;
    EXPECT_NE(r.Diagnostics.find("native function 'g' binds C symbol '"),
              std::string::npos)
        << decls << "\n"
        << r.Diagnostics;
    EXPECT_NE(r.Diagnostics.find(
                  "', which 'f' already binds with a different signature"),
              std::string::npos)
        << decls << "\n"
        << r.Diagnostics;
  }

  // A generic native function's instances pass a PaykanType and the
  // result's storage first: not what a plain one with its types takes.
  r = semaCheck(R"(
    native fn plain(a: int) -> int = "pk_p";
    native fn pick<T>(a: view T) -> T = "pk_p";
    fn main() -> int { return plain(1) + pick<int>(2); }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("native function 'pick<int>' binds C symbol "
                               "'pk_p', which 'plain' already binds with a "
                               "different signature"),
            std::string::npos)
      << r.Diagnostics;
}

// The error of a clash is the later binding's, and a note points at the
// first: here the program's own native function, against the instance of
// the standard library's generic one that a List<int> makes, whose error
// is in the standard library's source.
TEST(Native, AClashPointsAtTheFirstBinding) {
  auto r = semaCheck(R"(native fn f(x: int) -> int = "llabs";
native fn g(x: float) -> float = "llabs";
fn main() -> int { return 0; })");
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(":2:1: error: native function 'g' binds C "
                               "symbol 'llabs', which 'f' already binds"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(":1:1: note: 'f' binds it here"),
            std::string::npos)
      << r.Diagnostics;

  r = semaCheck(R"(import ::collections;
native fn bl(b: view Obj) -> int = "PaykanBuffer_len";
fn main() -> int {
  xs = collections::List<int>();
  xs.push(1);
  return int(u32(xs.len()));
})");
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("memory.pkn:"), std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("binds C symbol 'PaykanBuffer_len', which 'bl' "
                               "already binds with a different signature"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(".pkn:2:1: note: 'bl' binds it here"),
            std::string::npos)
      << r.Diagnostics;
}

// A native function may bind a C function of the runtime, which compiled
// code calls with its own signature, only with that signature (finding
// new-F3-1): `PaykanString_concat` is what `+` on strings calls, and
// `Paykan_malloc` what a backend allocates an object with.
TEST(Native, RuntimeSymbolsKeepTheirSignature) {
  for (const char *decl : {
           "native fn cat(a: int) -> int = \"PaykanString_concat\";",
           "native fn cat(a: view Str, b: view Str) -> int = "
           "\"PaykanString_concat\";",
           "native fn grab(n: int) -> int = \"Paykan_malloc\";",
           "native fn rem(x: int, y: int) -> int = \"fmod\";",
       }) {
    auto r = semaCheck(std::string(decl) +
                       "\nfn main() -> int { s = \"a\" + \"b\"; return 0; }");
    EXPECT_EQ(r.ErrorCount, 1u) << decl << "\n" << r.Diagnostics;
    EXPECT_NE(r.Diagnostics.find("', a function of the runtime, with a "
                                 "different signature"),
              std::string::npos)
        << decl << "\n"
        << r.Diagnostics;
  }
  auto r = semaCheck("native fn cat(a: int) -> int = \"PaykanString_concat\";"
                     "\nfn main() -> int { return 0; }");
  EXPECT_NE(r.Diagnostics.find("native function 'cat' binds C symbol "
                               "'PaykanString_concat', a function of the "
                               "runtime, with a different signature"),
            std::string::npos)
      << r.Diagnostics;

  // Its own signature is fine, and so are the standard library's bindings
  // (`Paykan_io_write`, `fmod`, the Buffer functions, ...), the same one
  // twice in a program included.
  r = semaCheck(R"(
    import ::io;
    import ::math;
    import ::collections;
    native fn len(s: view Str) -> int = "PaykanString_length";
    native fn rem(x: float, y: float) -> float = "fmod";
    native fn write(file: view Obj, s: view Str) -> int = "Paykan_io_write";
    fn main() -> int {
      xs = collections::List<int>();
      xs.push(1);
      io::println(math::sqrt(rem(9.0, 7.0)));
      return len("ab") + int(u32(xs.len()));
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// So may it one that an executable's entry code or the C of --emit-c calls
// itself (`Paykan_heap_reset` in C's main(), `memcpy` for a cast between a
// float and an int, ...), which that C calls through the native function's
// declaration: it did not compile ("too few arguments to function
// 'Paykan_heap_reset'").  One that takes a C `int` (`Paykan_heap_set_tracking`,
// `Paykan_args_new`, `memset`) has a signature no native function has.
TEST(Native, EntryAndCPathSymbolsKeepTheirSignature) {
  for (const char *decl : {
           "native fn hr(x: int) -> int = \"Paykan_heap_reset\";",
           "native fn hd(x: int) -> int = \"Paykan_heap_dump\";",
           "native fn ts(x: float) = \"Paykan_heap_set_tracking\";",
           "native fn ts(x: int) = \"Paykan_heap_set_tracking\";",
           "native fn an(n: int, c: inout int) = \"Paykan_args_new\";",
           "native fn af(x: int) -> int = \"Paykan_args_free\";",
           "native fn ge(s: view Str) -> int = \"getenv\";",
           "native fn mc(d: inout int, n: int) -> int = \"memcpy\";",
           "native fn mm(d: inout int, s: inout int, n: int) = \"memmove\";",
           "native fn ms(d: inout int, c: int, n: int) = \"memset\";",
           "native fn ri(s: view Str) = \"Paykan_retain_inline\";",
           "native fn rl(s: view Str) = \"Paykan_release_inline\";",
       }) {
    auto r = semaCheck(std::string(decl) + "\nfn main() -> int { return 0; }");
    EXPECT_EQ(r.ErrorCount, 1u) << decl << "\n" << r.Diagnostics;
    EXPECT_NE(r.Diagnostics.find("', a function of the runtime, with a "
                                 "different signature"),
              std::string::npos)
        << decl << "\n"
        << r.Diagnostics;
  }
  auto r = semaCheck("native fn hr(x: int) -> int = \"Paykan_heap_reset\";"
                     "\nfn main() -> int { return 0; }");
  EXPECT_NE(r.Diagnostics.find("native function 'hr' binds C symbol "
                               "'Paykan_heap_reset', a function of the "
                               "runtime, with a different signature"),
            std::string::npos)
      << r.Diagnostics;

  // Their own signatures are fine.
  r = semaCheck(R"(
    native fn hr() = "Paykan_heap_reset";
    native fn hd() = "Paykan_heap_dump";
    native fn af(args: inout Str, n: int) = "Paykan_args_free";
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

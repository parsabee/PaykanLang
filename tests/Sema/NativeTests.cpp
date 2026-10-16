// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
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
           "native fn f() -> Str[] = \"pk_f\";",
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
  EXPECT_NE(r.Diagnostics.find("('s: view Str'); only int, float, bool and "
                               "char cross into C by value"),
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
    fn main() -> int { xs: int[] = [1, 2]; return at<int>(xs, 0); }
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

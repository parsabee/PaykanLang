// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: the types a `native fn` may take and return (#198).

#include "TestUtils.h"
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using namespace paykan::test;

TEST(Native, BoundaryTypesAreAccepted) {
  auto r = semaCheck(R"(
    native fn put(fd: int, s: Str, x: float, b: bool, c: char, o: Obj) -> int
      = "pk_test_put";
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
    native fn put(fd: int, s: Str) -> int = "pk_test_put";
    fn main() -> int { return put("x", 1); }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Native, OtherTypesCannotCross) {
  for (const char *decl : {
           "native fn f(xs: int[]) = \"pk_f\";",
           "native fn f(p: P) = \"pk_f\";",
           "native fn f(t: (int, int)) = \"pk_f\";",
           "native fn f(s: Str?) = \"pk_f\";",
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

TEST(Native, MainCannotBeNative) {
  auto r = semaCheck("native fn main() -> int = \"pk_main\";");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'main' cannot be a native function"),
            std::string::npos)
      << r.Diagnostics;
}

// The bodies of a module's native functions are its sibling C file; without
// it the module is an error, at its first native function.
TEST(Native, TheCFileMustExist) {
  auto path = paykan::test::tempDir() / "native_without_c.pkn";
  std::ofstream(path) << "fn helper() -> int { return 1; }\n"
                         "native fn put(s: Str) = \"pk_put\";\n"
                         "fn main() -> int { return 0; }\n";
  paykan::parser::ParserDriver drv(testFrontend());
  ASSERT_EQ(drv.parseFile(path.string()), 0);
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  diag.setSourceInfo(drv.getCurrentFile(), &drv.getSourceLines());
  paykan::sema::Sema sema(drv.getASTContext(), diag, "", drv.getFrontendName());
  EXPECT_FALSE(sema.run(drv.getRoot()).Ok);
  EXPECT_NE(os.str().find(":2:1: error: native function 'put' needs its C "
                          "source 'native_without_c.c' next to this file"),
            std::string::npos)
      << os.str();
  std::filesystem::remove(path);
}

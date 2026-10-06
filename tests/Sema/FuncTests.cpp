// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: functions and scoping.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// -- Function declaration checks

TEST(Func, UndeclaredFunction) {
  auto r = semaCheck(wrapMain("foo();"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("undeclared function"), std::string::npos);
}

TEST(Func, FunctionRedefinition) {
  auto r = semaCheck(R"(
    fn foo() {}
    fn foo() {}
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("redefinition"), std::string::npos);
}

// Functions are forward-declared, so a function may call another that is
// defined later in the module (and mutually-recursive functions resolve).
TEST(Func, CallFunctionDefinedLater) {
  auto r = semaCheck(R"(
    fn first() -> int { return second(); }
    fn second() -> int { return 1; }
    fn main() -> int { return first(); }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A class method may call a module-level free function (forward-declared).
TEST(Func, MethodCallsFreeFunction) {
  auto r = semaCheck(R"(
    class C {
      fn __init__() {}
      fn get() -> int { return helper(); }
    }
    fn helper() -> int { return 7; }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A free function and a class cannot share a name (constructor vs function),
// whichever is declared first.
TEST(Func, FunctionClassNameCollision) {
  auto r = semaCheck(R"(
    fn Foo() -> int { return 1; }
    class Foo { fn __init__() {} }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'Foo' is already declared as a class"),
            std::string::npos)
      << r.Diagnostics;

  r = semaCheck(R"(
    class Foo { fn __init__() {} }
    fn Foo() -> int { return 1; }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'Foo' is already declared as a class"),
            std::string::npos)
      << r.Diagnostics;
}

// -- Builtin names are reserved: a free function may not redeclare one

// Every builtin function registered by Sema::run(), including those a user
// might plausibly want to "override".
TEST(Func, FunctionShadowsBuiltinFunctionRejected) {
  for (const char *name :
       {"print", "println", "printerr", "printerrln", "open"}) {
    auto r = semaCheck(std::string("fn ") + name +
                       "(x: int) -> int { return x; }\n" + wrapMain(""));
    EXPECT_FALSE(r.Ok) << name;
    EXPECT_NE(r.Diagnostics.find(std::string("'") + name +
                                 "' is a builtin function and cannot be "
                                 "redeclared"),
              std::string::npos)
        << r.Diagnostics;
  }
}

// The collision is the only diagnostic: the rest of the rejected signature
// and its body are not checked (neither against the builtin's entry nor on
// their own), and the builtin keeps its original signature afterwards.
TEST(Func, FunctionShadowsBuiltinReportsCollisionOnly) {
  auto r = semaCheck("fn print(x: NoSuchType) { return x + 1; }\n" +
                     wrapMain("println(\"still one Obj argument\");"));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(
                "'print' is a builtin function and cannot be redeclared"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("unknown class type"), std::string::npos)
      << r.Diagnostics;
}

TEST(Func, FunctionShadowsBuiltinClassRejected) {
  for (const char *name : {"Obj", "Str", "File", "Error", "Int"}) {
    auto r = semaCheck(std::string("fn ") + name + "() -> int { return 0; }\n" +
                       wrapMain(""));
    EXPECT_FALSE(r.Ok) << name;
    EXPECT_NE(
        r.Diagnostics.find(std::string("'") + name +
                           "' is a builtin class and cannot be redeclared"),
        std::string::npos)
        << r.Diagnostics;
  }
}

TEST(Func, FunctionShadowsEnumRejected) {
  auto r = semaCheck("enum Color { Red }\nfn Color() -> int { return 0; }\n" +
                     wrapMain(""));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'Color' is already declared as an enum"),
            std::string::npos)
      << r.Diagnostics;
}

// A name that merely resembles a builtin is fine.
TEST(Func, FunctionNamedNearBuiltinOk) {
  auto r = semaCheck(R"(
    fn print2(x: int) { println(Str<int>(x)); }
    fn Print(x: int) { print2(x); }
    fn main() -> int { Print(1); return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Func, ArgumentCountMismatch) {
  auto r = semaCheck(R"(
    fn add(a: int, b: int) -> int { return a + b; }
    fn main() -> int { return add(1); }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Func, ReturnTypeMismatch) {
  auto r = semaCheck(R"(
    fn foo() -> int { return "hello"; }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Func, UnknownParamType) {
  auto r = semaCheck(R"(
    fn foo(x: FooBar) -> int { return 0; }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("unknown class type 'FooBar'"),
            std::string::npos);
}

TEST(Func, ClassTypeAsParam) {
  auto r = semaCheck(R"(
    class A {}
    fn foo(a: A) -> bool { return a == a; }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Scoping

TEST(Func, BlockScopeIsolation) {
  auto r = semaCheck(wrapMain(R"(
    {
      x: int = 1;
    }
    println(Str<int>(x));
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("undeclared"), std::string::npos);
}

TEST(Func, InnerScopeCanAccessOuter) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 1;
    {
      y: int = x + 1;
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- The print family is single-argument (no variadic functions / overloading)

TEST(Func, PrintlnSingleArgOk) {
  auto r = semaCheck(wrapMain(R"(println("hello" + " world");)"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Func, PrintlnRejectsMultipleArgs) {
  auto r = semaCheck(wrapMain(R"(println("hello", "world");)"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("expects 1 argument"), std::string::npos)
      << r.Diagnostics;
}

TEST(Func, PrintRejectsMultipleArgs) {
  auto r = semaCheck(wrapMain(R"(print("a", Str<int>(1), "b");)"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("expects 1 argument"), std::string::npos)
      << r.Diagnostics;
}

// -- The program's entry point (#132)
//
// A program that is built or run needs `fn main() -> int` (or `fn main(args:
// Str[]) -> int`).  Sema reports a missing or ill-typed one with a source
// location, before the PIR verifier would.

namespace {

SemaResult semaCheckProgram(const std::string &source,
                            paykan::sema::Sema::EntryPoint check =
                                paykan::sema::Sema::EntryPoint::Required) {
  auto path = writeTempFile(source);
  paykan::parser::ParserDriver drv(testFrontend());
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  diag.setSourceInfo("prog.pkn", &drv.getSourceLines());
  drv.setDiagEngine(&diag);
  int rc = drv.parseFile(path);
  std::filesystem::remove(path);
  if (rc != 0)
    return {false, "parse error: " + os.str(), drv.getErrorCount()};
  paykan::sema::Sema sema(drv.getASTContext(), diag, "", drv.getFrontendName());
  sema.setEntryPointCheck(check);
  auto ctx = sema.run(drv.getRoot());
  return {ctx.Ok, os.str(), ctx.ErrorCount};
}

void expectEntryPointError(const std::string &source,
                           const std::string &expected) {
  auto r = semaCheckProgram(source);
  EXPECT_FALSE(r.Ok) << source;
  EXPECT_EQ(r.ErrorCount, 1u) << source << "\n" << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(expected), std::string::npos) << source << "\n"
                                                             << r.Diagnostics;
}

} // namespace

TEST(Func, EntryPointSignaturesOk) {
  for (const char *src : {"fn main() -> int { return 0; }",
                          "fn main(args: Str[]) -> int { return args.len(); }",
                          "fn helper() -> int { return 1; }\n"
                          "fn main() -> int { return helper(); }"}) {
    auto r = semaCheckProgram(src);
    EXPECT_TRUE(r.Ok) << src << "\n" << r.Diagnostics;
    EXPECT_EQ(r.ErrorCount, 0u) << src << "\n" << r.Diagnostics;
  }
}

TEST(Func, MissingMainIsOneLocatedError) {
  const std::string missing =
      "prog.pkn:1:1: error: program has no entry point 'fn main() -> int' "
      "(or 'fn main(args: Str[]) -> int')";
  expectEntryPointError("", missing);
  expectEntryPointError("// nothing here\n", missing);
  expectEntryPointError("fn helper() -> int { return 1; }\n", missing);
  expectEntryPointError("class Main { x: int; }\nfn mainly() -> int { return "
                        "0; }\n",
                        missing);
}

TEST(Func, IllTypedMainIsOneErrorAtItsDeclaration) {
  const std::string must = "error: the program's entry point must be 'fn "
                           "main() -> int' or 'fn main(args: Str[]) -> int', "
                           "not ";
  expectEntryPointError("fn f() -> int { return 1; }\n"
                        "fn main() -> Str { return \"x\"; }\n",
                        "prog.pkn:2:1: " + must + "'fn main() -> Str'");
  expectEntryPointError("fn main() { }\n", must + "'fn main() -> void'");
  expectEntryPointError("fn main(n: int) -> int { return n; }\n",
                        must + "'fn main(int) -> int'");
  expectEntryPointError("fn main(args: Str[], n: int) -> int { return n; }\n",
                        must + "'fn main(Str[], int) -> int'");
  expectEntryPointError("fn main(args: int[]) -> int { return 0; }\n",
                        must + "'fn main(int[]) -> int'");
  expectEntryPointError("fn main() -> int? { return 0; }\n",
                        must + "'fn main() -> int?'");
  expectEntryPointError("fn main<T>() -> int { return 0; }\n",
                        "prog.pkn:1:1: error: 'main' cannot be generic");
}

// A `main` whose declaration is rejected is reported once, for that.
TEST(Func, RejectedMainDeclarationIsNotReportedAgain) {
  auto r = semaCheckProgram("fn main() -> Nope { return 0; }\n");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("entry point"), std::string::npos)
      << r.Diagnostics;
}

TEST(Func, EntryPointCheckModes) {
  using EP = paykan::sema::Sema::EntryPoint;
  // A module on its own (or imported) needs no `main` ...
  EXPECT_TRUE(semaCheckProgram("fn f() -> int { return 1; }", EP::None).Ok);
  EXPECT_TRUE(
      semaCheckProgram("fn f() -> int { return 1; }", EP::IfDeclared).Ok);
  // ... and with IfDeclared, a declared one is still checked.
  EXPECT_TRUE(
      semaCheckProgram("fn main() -> Str { return \"\"; }", EP::None).Ok);
  auto r =
      semaCheckProgram("fn main() -> Str { return \"\"; }", EP::IfDeclared);
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}

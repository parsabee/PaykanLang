// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: functions and scoping.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "import ::io; fn main() -> int {\n" + body + "\n  return 0;\n}\n";
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
      fn get() -> int { return helper(); }
    }

    impl C {
      fn __init__() {}
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
    class Foo {}
    impl Foo { fn __init__() {} }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'Foo' is already declared as a class"),
            std::string::npos)
      << r.Diagnostics;

  r = semaCheck(R"(
    class Foo {}
    impl Foo { fn __init__() {} }
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
// Printing and files are the standard library's (::io): a program may
// declare functions of the same names, beside io's.
TEST(Func, TheStandardLibrarysNamesAreNotReserved) {
  for (const char *name :
       {"print", "println", "printerr", "printerrln", "open"}) {
    auto r = semaCheck(std::string("fn ") + name +
                       "(x: int) -> int { return x; }\n" +
                       wrapMain(std::string("io::println(") + name + "(1));"));
    EXPECT_TRUE(r.Ok) << name << "\n" << r.Diagnostics;
  }
}

// The collision is the only diagnostic: the rest of the rejected signature
// and its body are not checked (neither against the builtin's entry nor on
// their own), and the builtin keeps its original signature afterwards.
TEST(Func, FunctionShadowsBuiltinReportsCollisionOnly) {
  auto r = semaCheck("fn Str(x: NoSuchType) { return x + 1; }\n" +
                     wrapMain("s = Str(\"still the builtin\");"));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_NE(
      r.Diagnostics.find("'Str' is a builtin class and cannot be redeclared"),
      std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("unknown class type"), std::string::npos)
      << r.Diagnostics;
}

TEST(Func, FunctionShadowsBuiltinClassRejected) {
  for (const char *name : {"Obj", "Str"}) {
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
  auto r = semaCheck(R"(import ::io;
    fn print2(x: int) { io::println(Str<int>(x)); }
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
    io::println(Str<int>(x));
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
  auto r = semaCheck(wrapMain(R"(io::println("hello" + " world");)"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Func, PrintlnRejectsMultipleArgs) {
  auto r = semaCheck(wrapMain(R"(io::println("hello", "world");)"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("expects 1 argument"), std::string::npos)
      << r.Diagnostics;
}

TEST(Func, PrintRejectsMultipleArgs) {
  auto r = semaCheck(wrapMain(R"(io::print("a", Str<int>(1), "b");)"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("expects 1 argument"), std::string::npos)
      << r.Diagnostics;
}

// -- The program's entry point (#132)
//
// A program that is built or run needs `fn main() -> int` (or `fn main(args:
// view Str[]) -> int`).  Sema reports a missing or ill-typed one with a
// source location, before the PIR verifier would.

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
  for (const char *src :
       {"fn main() -> int { return 0; }",
        "fn main(args: view Str[]) -> int { return args.len(); }",
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
      "(or 'fn main(args: view Str[]) -> int')";
  expectEntryPointError("", missing);
  expectEntryPointError("// nothing here\n", missing);
  expectEntryPointError("fn helper() -> int { return 1; }\n", missing);
  expectEntryPointError("class Main { x: int; }\nfn mainly() -> int { return "
                        "0; }\n",
                        missing);
}

TEST(Func, IllTypedMainIsOneErrorAtItsDeclaration) {
  const std::string must = "error: the program's entry point must be 'fn "
                           "main() -> int' or 'fn main(args: view Str[]) -> "
                           "int', not ";
  expectEntryPointError("fn f() -> int { return 1; }\n"
                        "fn main() -> Str { return \"x\"; }\n",
                        "prog.pkn:2:1: " + must + "'fn main() -> Str'");
  expectEntryPointError("fn main() { }\n", must + "'fn main() -> void'");
  expectEntryPointError("fn main(n: int) -> int { return n; }\n",
                        must + "'fn main(int) -> int'");
  expectEntryPointError(
      "fn main(args: view Str[], n: int) -> int { return n; }\n",
      must + "'fn main(view Str[], int) -> int'");
  expectEntryPointError("fn main(args: view int[]) -> int { return 0; }\n",
                        must + "'fn main(view int[]) -> int'");
  expectEntryPointError("fn main(args: inout Str[]) -> int { return 0; }\n",
                        must + "'fn main(inout Str[]) -> int'");
  expectEntryPointError("fn main() -> int? { return 0; }\n",
                        must + "'fn main() -> int?'");
  expectEntryPointError("fn main<T>() -> int { return 0; }\n",
                        "prog.pkn:1:1: error: 'main' cannot be generic");
  // Generic for leaving out a parameter's type: that is what is reported.
  expectEntryPointError("fn main(args) -> int { return 0; }\n",
                        "prog.pkn:1:1: error: parameter 'args' of 'main' needs "
                        "a type; the program's entry point is");
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

// -- Expression bodies: `fn f(...) => e;` has the type of `e` as its result

TEST(Func, InferredResults) {
  // A use before the declaration, a chain of inferred functions, a
  // function value, a `static fn`, `main`, and generic functions, inferred
  // for each instantiation.
  auto r = semaCheck(R"(
fn useEarly(x: int) => quad(x) + 1;
fn quad(x: int) => double(double(x));
fn double(x: int) -> int => x * 2;
fn half(x: u8) => x / 2;
fn later() => quad;
fn twice<T>(x: T) => x + x;
fn both<T>(x: T) => twice(twice(x));
struct V { x: int; y: int; }
impl V { static fn origin() => V(0, 0); }
fn main() => useEarly(1) + Str(half(9)).len() + later()(1) + V::origin().x
    + twice(3) + twice("ab").len() + both(1);
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  auto typed = semaCheck(R"(
fn half(x: u8) => x / 2;
fn words() => "hi";
fn main() -> int {
  a: u8 = half(9);
  b: int = half(9);
  return 0;
}
)");
  EXPECT_FALSE(typed.Ok);
  EXPECT_NE(typed.Diagnostics.find("initializer of type 'u8' does not match "
                                   "declared type 'int' for variable 'b'"),
            std::string::npos)
      << typed.Diagnostics;
  EXPECT_EQ(typed.ErrorCount, 1u) << typed.Diagnostics;
}

TEST(Func, InferredResultErrors) {
  auto r = semaCheck(R"(import ::io;
fn fact(n: int) => if n <= 1 then 1 else n * fact(n - 1);
fn ping(n: int) => pong(n) + 1;
fn pong(n: int) => ping(n) - 1;
fn say() => io::println("x");
fn deep<T>(x: T) => deep(x);
fn twice<T>(x: T) => x + x;
class C {
  n: int;
  fn get() => self.n;
}
class G<T> {
  v: T;
  fn get() => self.v;
}
struct S { a: int; }
impl S { view fn a2() => self.a * 2; }
fn main() -> int {
  d = deep(1);
  b = twice(True);
  c = C(1).get() + G<int>(1).get() + G<Str>("x").get().len() + fact(3);
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  for (const char *e :
       {"the result type of 'fact' is inferred from its expression, which "
        "uses 'fact' itself: write the result type, 'fn fact(...) -> T => "
        "...'",
        "the result type of 'ping' is inferred from its expression, which "
        "uses 'ping' itself",
        "the result of 'say' is the value of its expression, which has none "
        "(its type is 'void'): write its body in braces, 'fn say(...) { ...; "
        "}'",
        "the result type of 'deep' is inferred from its expression, which "
        "uses 'deep' itself",
        "operator '+' is not defined for types 'bool' and 'bool'",
        "in instantiation of 'twice<bool>' requested here",
        "the result type of method 'get' is not inferred: write it, 'fn "
        "get(...) -> T => ...'",
        "the result type of method 'a2' is not inferred"})
    EXPECT_NE(r.Diagnostics.find(e), std::string::npos) << e << "\n"
                                                        << r.Diagnostics;
  // One error each: `get` is reported for C and once for the template G,
  // and no use of a function in error is reported again.
  EXPECT_EQ(r.ErrorCount, 8u) << r.Diagnostics;
}

// An untyped parameter makes the function generic: each call instantiates it
// for the argument types, one type per parameter, with an expression body's
// result inferred per instantiation and a block's written (or void).  The
// added type parameters come after the written ones, so explicit type
// arguments name both.
TEST(Func, UntypedParameters) {
  auto r = semaCheck(R"(
fn double(x) => x * 2;
fn add(a, b) => a + b;
fn pair(a, b) => (a, b);
fn useEarly(x) => later(x) + 1;
fn later(x) => double(x);
fn larger<T>(a: T, b: T, tag) -> T => if a > b then a else b;
fn count(n) -> int {
  total = 0;
  for _ in 0..n { total = total + 1; }
  return total;
}
fn ignore(x) { }
struct P { x: int; y: int; }
fn sum(p) => p.x + p.y;
fn main() -> int {
  a: int = double(2);
  f: float = double(1.5);
  s: Str = add("a", "b");
  t: (int, Str) = pair(1, "x");
  u: int = useEarly(3);
  k: u8 = larger(u8(1), u8(2), "tag");
  e: float = larger<float, Str>(1.5, 2.5, "tag");
  n: unsigned = 3;
  c: int = count(n) + count(4);
  ignore(P(1, 2));
  return a + sum(P(1, 2)) + add<int, int>(1, 2) + c + u + t.0 + s.len();
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Func, UntypedParameterErrors) {
  auto r = semaCheck(R"(
fn double(x) => x * 2;
fn add(a, b) => a + b;
fn larger(a, b) {
  return if a > b then a else b;
}
fn main() -> int {
  s = double("ab");
  n = add<int>(1, 2);
  f = double;
  larger(1, 2);
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  for (const char *e :
       {"operator '*' is not defined for types 'Str' and 'int'",
        "in instantiation of 'double<Str>' requested here",
        "generic function 'add' expects 2 type argument(s), got 1",
        "'double' is a generic function; a generic function cannot be a "
        "function value yet",
        "return value of type 'int' does not match function return type "
        "'void'"})
    EXPECT_NE(r.Diagnostics.find(e), std::string::npos) << e << "\n"
                                                        << r.Diagnostics;
}

// Two parameters of one name are an error, typed or not, in a function, a
// template and a method, reported once (calls to the function are not).
TEST(Func, DuplicateParameters) {
  auto r = semaCheck(R"(
fn f(x, x) => 1;
fn g(x: int, x: Str) -> int { return 1; }
fn h<T>(x: T, x: T) -> int { return 1; }
class A {
  v: int;
  fn m(x: int, x: int) -> int { return 1; }
}
fn main() -> int { return f(1, 2) + g(1, "a") + h(1, 2); }
)");
  EXPECT_FALSE(r.Ok);
  for (const char *e : {"duplicate parameter 'x' in function 'f'",
                        "duplicate parameter 'x' in function 'g'",
                        "duplicate parameter 'x' in function 'h'",
                        "duplicate parameter 'x' in method 'm' of 'A'"})
    EXPECT_NE(r.Diagnostics.find(e), std::string::npos) << e << "\n"
                                                        << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
}

// `void` is a function's result type only: nothing holds a value of it, and
// a function that returns nothing returns no value.  Each is reported by
// Sema, none reaches the lowering.
TEST(Func, VoidHasNoValue) {
  auto r = semaCheck(R"(
fn g() {}
fn k(v: void) {}
fn h() { return g(); }
fn id<T>(v: T) -> T { return v; }
class C { v: void; }
class D { fn m(v: void) {} }
class Box<T> { v: T; }
fn main() -> int {
  x = g();
  y: void = g();
  z: view = g();
  id(g());
  b: Box<void>? = None;
  match g() { _ { } }
  w = [g(), g()];
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  for (const char *e :
       {"3:6: error: parameter 'v' cannot have type 'void'",
        "4:10: error: return value has type 'void' (the expression produces "
        "no value): make it a statement, then 'return;'",
        "6:11: error: field 'v' in class 'C' cannot have type 'void'",
        "7:16: error: parameter 'v' of method 'm' in 'D' cannot have type "
        "'void'",
        "10:3: error: variable 'x' cannot have type 'void' (the expression "
        "produces no value)",
        "11:3: error: variable 'y' cannot have type 'void'",
        "12:3: error: variable 'z' cannot have type 'void' (the expression "
        "produces no value)",
        "13:3: error: type argument 1 of 'id' cannot be 'void': it has no "
        "value (it is a function's result type only)",
        "14:3: error: type argument 1 of 'Box' cannot be 'void'",
        "15:9: error: match subject has type 'void' (the expression produces "
        "no value)",
        "16:8: error: array element 0 has type 'void' (the expression "
        "produces no value)"})
    EXPECT_NE(r.Diagnostics.find(e), std::string::npos) << e << "\n"
                                                        << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 11u) << r.Diagnostics;

  // Nor is it a trait method's parameter or an array's element.
  auto more = semaCheck(R"(
fn g() {}
trait Tr { fn m(v: void) -> int; }
class E { a: void[2]; }
fn s(a: view void[]) {}
fn main() -> int { y: void[2] = [g(), g()]; return 0; }
)");
  EXPECT_FALSE(more.Ok);
  for (const char *e :
       {"3:17: error: parameter 'v' of method 'm' of trait 'Tr' cannot have "
        "type 'void'",
        "4:11: error: field 'a' in class 'E' has type 'void[2]': an array "
        "element cannot have type 'void'",
        "5:6: error: parameter 'a' has type 'void[]': an array element cannot "
        "have type 'void'",
        "6:20: error: variable 'y' has type 'void[2]': an array element cannot "
        "have type 'void'"})
    EXPECT_NE(more.Diagnostics.find(e), std::string::npos) << e << "\n"
                                                           << more.Diagnostics;
  EXPECT_EQ(more.ErrorCount, 4u) << more.Diagnostics;
}

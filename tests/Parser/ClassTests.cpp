// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Parser tests: class declarations, inheritance, member access, match

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// ─── class declarations ─────────────────────────────────────────────────────

TEST(Class, EmptyClass) {
  auto [ok, _] = parse(R"(
    class A {}
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, ClassWithFields) {
  auto [ok, _] = parse(R"(
    class Point {
      x: int;
      y: int;
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, ClassWithMethod) {
  auto [ok, _] = parse(R"(
    class Counter {
      count: int;
      fn getCount() -> int {
        return 0;
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, ClassWithInitAndMethod) {
  auto [ok, _] = parse(R"(
    class A {
      a: int;
      fn getA() -> int {
        return 0;
      }
    }

    impl A {
      fn __init__(a: int) {
        self.a = a;
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, ClassWithSuperclass) {
  auto [ok, _] = parse(R"(
    class Animal {}
    class Dog : Animal {}
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, ClassMemberAccess) {
  auto [ok, _] = parse(R"(
    class Foo {
      x: int;
      fn getX() -> int {
        return self.x;
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, ClassMemberAssign) {
  auto [ok, _] = parse(R"(
    class Foo {
      x: int;
      fn setX(v: int) {
        self.x = v;
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MultipleClasses) {
  auto [ok, _] = parse(R"(
    class A {
      a: int;
    }
    class B {
      b: float;
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, FuncBeforeClass) {
  auto [ok, _] = parse(R"(
    fn helper() -> int { return 1; }
    class A { a: int; }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, InterleavedDecls) {
  auto [ok, _] = parse(R"(
    class A { a: int; }
    fn helper() -> int { return 0; }
    class B { b: float; }
    fn main() -> int { return 0; }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MissingCloseBrace) {
  auto [ok, _] = parse(R"(import ::io; 
    class A {
      x: int;
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(ok);
}

TEST(Class, FieldMissingSemicolon) {
  auto [ok, _] = parse(R"(
    class A {
      x: int
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(ok);
}

// ─── match ──────────────────────────────────────────────────────────────────

TEST(Class, MatchBasicTypeArms) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x {
        Dog { }
        Cat { }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MatchWithBinding) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x {
        d: Dog { }
        c: Cat { }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MatchWithWildcard) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x {
        Dog { }
        _ { }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MatchAllArmForms) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x {
        Dog { }
        c: Cat { }
        _ { }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MatchEmptyBody) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x { }
      return 0;
    }
  )");
  EXPECT_FALSE(ok); // empty match arm list is a parse error
}

TEST(Class, MatchWithStmtsInArm) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x {
        d: Dog {
          y: int = 1;
        }
        _ {
          y: int = 2;
        }
      }
      return 0;
    }
  )");
  EXPECT_TRUE(ok);
}

TEST(Class, MatchMissingCloseBrace) {
  auto [ok, _] = parse(R"(import ::io; 
    fn main() -> int {
      x: Obj = 0;
      match x {
        Dog { }
    }
  )");
  EXPECT_FALSE(ok);
}

TEST(Class, MatchArmMissingBodyBrace) {
  auto [ok, _] = parse(R"(
    fn main() -> int {
      x: Obj = 0;
      match x {
        Dog
      }
      return 0;
    }
  )");
  EXPECT_FALSE(ok);
}

// ─── pure virtual methods ───────────────────────────────────────────────────

// A class body's method whose signature ends with `;` is pure virtual: a
// FuncDecl without a body, `fn` or `view fn`, with parameters and a result
// mode as usual.
TEST(Class, PureVirtualMethodsInTheClassBody) {
  auto [ok, driver] = parse(R"(
    class Shape {
      name: Str;
      fn area() -> float;
      view fn label() -> view Str;
      view fn scaled(by: float, n: inout int) -> float;
      fn reset();
      view fn describe() -> Str { return self.name; }
    }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(ok);
  const auto &classes = driver->getRoot()->getClassDecls();
  ASSERT_EQ(classes.size(), 1u);
  const auto &methods = classes[0]->getMethods();
  ASSERT_EQ(methods.size(), 5u);
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(methods[i]->getBody(), nullptr) << methods[i]->getName();
    EXPECT_FALSE(methods[i]->isNative()) << methods[i]->getName();
  }
  EXPECT_FALSE(methods[0]->isView());
  EXPECT_TRUE(methods[1]->isView());
  EXPECT_EQ(methods[1]->getResultMode(), paykan::ast::ParamMode::View);
  ASSERT_EQ(methods[2]->getParams().size(), 2u);
  EXPECT_EQ(methods[2]->getParams()[1].Mode, paykan::ast::ParamMode::Inout);
  EXPECT_EQ(methods[3]->getReturnType(), nullptr);
  EXPECT_NE(methods[4]->getBody(), nullptr);
  // The AST printer shows a pure virtual method without a body: the blocks
  // are `describe`'s and `main`'s.
  const std::string ast = dumpAST(*driver);
  EXPECT_NE(ast.find("'area' ->"), std::string::npos) << ast;
  size_t blocks = 0;
  for (size_t at = ast.find("CompoundStmt"); at != std::string::npos;
       at = ast.find("CompoundStmt", at + 1))
    ++blocks;
  EXPECT_EQ(blocks, 2u) << ast;
}

// A generic class's methods may be pure virtual too.
TEST(Class, PureVirtualMethodOfAGenericClass) {
  auto [ok, driver] = parse(R"(
    class Source<T> { view fn next() -> T; }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(ok);
  ASSERT_EQ(driver->getRoot()->getGenericClassDecls().size(), 1u);
  EXPECT_EQ(
      driver->getRoot()->getGenericClassDecls()[0]->getMethods()[0]->getBody(),
      nullptr);
}

// Only a class body's method may leave out its body: a free function, an
// `impl` block's method or `static fn` (a struct's methods are in one) is a
// syntax error at the `;`.  An initializer and a `static fn` are never in
// a class body, so their errors only ask for the body (naming an
// initializer as written), and `destroy` is never declared at all.
TEST(Class, BodilessFunctionsOutsideAClassBodyAreRejected) {
  auto free = parseWithDiags("fn f(x: int) -> int;\n"
                             "fn main() -> int { return 0; }\n");
  EXPECT_FALSE(free.Ok);
  EXPECT_NE(free.Text.find("1:20: error: function 'f' has no body: only a "
                           "method in a class body can end its signature "
                           "with ';' (a pure virtual method)"),
            std::string::npos)
      << free.Text;

  struct Case {
    const char *Member;
    const char *Error;
  };
  const char *const kImplMethod =
      "error: 'g' in an 'impl' block has no body: an 'impl' block's methods "
      "are called directly, so only a method in a class body can end its "
      "signature with ';' (a pure virtual method)";
  for (const Case &c :
       {Case{"fn g() -> int;", kImplMethod},
        Case{"view fn g() -> int;", kImplMethod},
        Case{"static fn g() -> int;",
             "error: 'g' has no body: a 'static fn' needs one"},
        Case{"fn __init__(x: int);",
             "error: '__init__' has no body: an initializer needs one"},
        Case{"fn ___init__();",
             "error: '___init__' has no body: an initializer needs one"},
        Case{"fn destroy();", "error: 'destroy' is reserved for the "
                              "compiler-generated destructor and cannot be "
                              "declared"}}) {
    auto impl = parseWithDiags(std::string("class A {}\nimpl A { ") + c.Member +
                               " }\nfn main() -> int { return 0; }\n");
    EXPECT_FALSE(impl.Ok) << c.Member;
    EXPECT_NE(impl.Text.find(c.Error), std::string::npos) << c.Member << "\n"
                                                          << impl.Text;
  }

  auto structImpl = parseWithDiags("struct P { x: int; }\n"
                                   "impl P { fn get() -> int; }\n"
                                   "fn main() -> int { return 0; }\n");
  EXPECT_FALSE(structImpl.Ok);
  EXPECT_NE(structImpl.Text.find("error: 'get' in an 'impl' block has no body"),
            std::string::npos)
      << structImpl.Text;

  // A trait's methods and a native function keep their own forms.
  auto [traitOk, _t] = parse("trait Named { view fn name() -> Str; }\n"
                             "native fn twice(x: int) -> int = \"twice\";\n"
                             "fn main() -> int { return 0; }\n");
  EXPECT_TRUE(traitOk);
}

// A class body's method that has neither a body nor `;` is told about both.
TEST(Class, MethodWithoutBodyOrSemicolon) {
  auto r = parseWithDiags("class A {\n"
                          "  fn f() -> int\n"
                          "  fn g() -> int { return 1; }\n"
                          "}\n"
                          "fn main() -> int { return 0; }\n");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Text.find("3:3: error: unexpected 'fn'; expected '{' or '=>' "
                        "to start the method's body, or ';' to end a pure "
                        "virtual method's signature"),
            std::string::npos)
      << r.Text;
}

// A class body's method cannot have type parameters of its own, with a body
// or pure virtual: they were parsed and ignored (`fn f<T>(x: int)` compiled,
// `fn map<U>(x: U)` was `unknown class type 'U'`).  Only a free function is
// generic; a generic class's methods use its type parameters.
TEST(Class, MethodsAreNotGeneric) {
  auto r = parseWithDiags("class A {\n"
                          "  fn f<T>(x: int) -> int { return x; }\n"
                          "  view fn g<T: Show>() -> Str;\n"
                          "  fn h() -> int => 1;\n"
                          "}\n"
                          "fn main() -> int { return 0; }\n");
  EXPECT_FALSE(r.Ok);
  ASSERT_EQ(r.Diags.size(), 2u) << r.Text;
  for (const auto &d : r.Diags)
    EXPECT_EQ(d.Message, paykan::frontend::kGenericMethod) << r.Text;
  EXPECT_EQ(r.Diags[0].Loc.getLineStart(), 2u);
  EXPECT_EQ(r.Diags[0].Loc.getColumnStart(), 7u);
  EXPECT_EQ(r.Diags[1].Loc.getLineStart(), 3u);
  EXPECT_EQ(r.Diags[1].Loc.getColumnStart(), 12u);
  auto [ok, _] = parse("class Box<T> { v: T; view fn get() -> T { return "
                       "self.v; } fn put(x: T) { self.v = x; } }\n"
                       "fn main() -> int { return 0; }\n");
  EXPECT_TRUE(ok);
}

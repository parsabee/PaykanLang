// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Parser tests: `impl` blocks and `static fn` (#216).

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Impl, BlockParses) {
  auto [ok, driver] = parse(R"(
    class A { n: int; }
    impl A {
      view fn read() -> int { return self.n; }
      fn bump() { self.n = self.n + 1; }
      static fn make() -> A { return A(); }
    }
    impl geo::Point { view fn norm() -> float { return 0.0; } }
  )");
  ASSERT_TRUE(ok);
  const auto &impls = driver->getRoot()->getImplDecls();
  ASSERT_EQ(impls.size(), 2u);
  EXPECT_EQ(impls[0]->getTypeName(), "A");
  ASSERT_EQ(impls[0]->getMethods().size(), 3u);
  EXPECT_TRUE(impls[0]->getMethods()[0]->isView());
  EXPECT_FALSE(impls[0]->getMethods()[1]->isStatic());
  EXPECT_TRUE(impls[0]->getMethods()[2]->isStatic());
  EXPECT_EQ(impls[1]->getTypeName(), "geo::Point");
}

// `impl !Sendable for A {}` (#378): the block of a trait a type is opted
// out of.  The `!` goes before a trait, which `for` follows.
TEST(Impl, NegativeBlockParses) {
  auto [ok, driver] = parse(R"(
    class A { n: int; }
    impl !Sendable for A {}
    impl Hashable for A { view fn hash() -> int { return 1; } }
  )");
  ASSERT_TRUE(ok);
  const auto &impls = driver->getRoot()->getImplDecls();
  ASSERT_EQ(impls.size(), 2u);
  EXPECT_TRUE(impls[0]->isNegative());
  EXPECT_EQ(typeName(impls[0]->getTrait()), "Sendable");
  EXPECT_EQ(impls[0]->getTypeName(), "A");
  EXPECT_FALSE(impls[1]->isNegative());
  for (const char *src :
       {"impl !A {}", "impl ! for A {}", "impl !Box<T: Sendable> {}",
        "impl !!Sendable for A {}"}) {
    auto [bad, _] =
        parse(std::string(src) + "\nfn main() -> int { return 0; }");
    EXPECT_FALSE(bad) << src;
  }
}

TEST(Impl, MalformedBlocksAreRejected) {
  for (const char *src : {
           "impl { fn f() {} }",               // no type
           "impl A { x: int; }",               // a field
           "impl A { static view fn f() {} }", // static and view
           "impl A { fn f() {} ",              // unclosed
           "impl Box<int> { fn f() {} }",      // a type, not a parameter
           "impl Box<T[2]> { fn f() {} }",
           "impl Box<Str> { fn f() {} }",
           "impl Box<a::T> { fn f() {} }",
           "impl Box? { fn f() {} }",
           "static fn f() {}",             // outside an impl
           "class A { static fn f() {} }", // in a class body
       }) {
    auto [ok, _] = parse(std::string(src) + "\nfn main() -> int { return 0; }");
    EXPECT_FALSE(ok) << src;
  }
}

// A type where a type parameter's name goes is reported at the argument,
// a builtin type too, whose type node is shared and has no location of its
// own (the error had no position, so no file, line or caret).
TEST(Impl, TypeInPlaceOfAParameterIsLocated) {
  struct Case {
    const char *Src;
    unsigned Column;
    const char *Type;
  };
  for (const Case &c :
       {Case{"impl Box<int> { }", 10, "int"},
        Case{"impl Box<Str> { }", 10, "Str"},
        Case{"impl Box<T[2]> { }", 10, "T[2]"},
        Case{"impl Showable for Box<T, float> { }", 26, "float"},
        Case{"impl Box<List<int>> { }", 10, "List<int>"}}) {
    auto r = parseWithDiags(std::string("class Box<T> { v: T; }\n") + c.Src +
                            "\nfn main() -> int => 0;\n");
    EXPECT_FALSE(r.Ok) << c.Src;
    ASSERT_EQ(r.Diags.size(), 1u) << c.Src << r.Text;
    EXPECT_EQ(r.Diags[0].Message,
              std::string("expected a type parameter name in 'impl Box<...>', "
                          "not the type '") +
                  c.Type + "'")
        << c.Src;
    const paykan::ast::SourceLocation &loc = r.Diags[0].Loc;
    EXPECT_EQ(loc.getLineStart(), 2u) << c.Src;
    EXPECT_EQ(loc.getColumnStart(), c.Column) << c.Src;
    EXPECT_NE(r.Text.find(":2:" + std::to_string(c.Column) + ": error: "),
              std::string::npos)
        << r.Text;
  }
}

// A generic type's `impl` block names its type parameters, in either form.
TEST(Impl, GenericBlockNamesItsTypeParameters) {
  auto [ok, driver] = parse(R"(
    impl Box<T> { view fn get() -> T { return self.v; } }
    impl Showable for Pair<K, V> { view fn toString() -> Str { return ""; } }
    impl Into<T> for gen::Box<T> { view fn into() -> T { return self.v; } }
  )");
  ASSERT_TRUE(ok);
  const auto &impls = driver->getRoot()->getImplDecls();
  ASSERT_EQ(impls.size(), 3u);
  EXPECT_EQ(impls[0]->getTypeName(), "Box");
  ASSERT_EQ(impls[0]->getTypeParams().size(), 1u);
  EXPECT_EQ(*impls[0]->getTypeParams()[0], "T");
  EXPECT_EQ(impls[1]->getTypeName(), "Pair");
  ASSERT_EQ(impls[1]->getTypeParams().size(), 2u);
  EXPECT_EQ(*impls[1]->getTypeParams()[1], "V");
  EXPECT_NE(impls[1]->getTrait(), nullptr);
  EXPECT_EQ(impls[2]->getTypeName(), "gen::Box");
  EXPECT_EQ(impls[2]->getTypeParams().size(), 1u);
}

// A static function is called like a module function: `A::make()`.
TEST(Impl, StaticCallParsesAsAQualifiedCall) {
  auto [ok, driver] = parse("fn main() -> int { a = A::make(); return 0; }");
  ASSERT_TRUE(ok);
}

// An `impl` block's method and a `static fn` can have type parameters of
// their own (#353), with bounds, next to the block's.  A constructor and a
// method of an `impl ... for` block cannot: an error at the `<`.
TEST(Impl, MethodsAndStaticFnsAreGeneric) {
  auto [ok, driver] = parse("impl A {\n"
                            "  fn f<T>(x: T) {}\n"
                            "  static fn id<T: Showable, U>(y: T) -> T { "
                            "return y; }\n"
                            "  view fn g() -> int => 1;\n"
                            "}\n"
                            "impl Box<T> {\n"
                            "  view fn map<U>(f: (view T) -> U) -> Box<U> { "
                            "return Box<U>(f(self.v)); }\n"
                            "}\n"
                            "fn main() -> int { return 0; }\n");
  ASSERT_TRUE(ok);
  const auto &impls = driver->getRoot()->getImplDecls();
  ASSERT_EQ(impls.size(), 2u);
  const auto &methods = impls[0]->getMethods();
  ASSERT_EQ(methods.size(), 3u);
  ASSERT_EQ(methods[0]->getTypeParams().size(), 1u);
  EXPECT_EQ(*methods[0]->getTypeParams()[0], "T");
  EXPECT_TRUE(methods[1]->isStatic());
  ASSERT_EQ(methods[1]->getTypeParams().size(), 2u);
  EXPECT_EQ(methods[1]->getTypeParamBounds()[0].size(), 1u);
  EXPECT_TRUE(methods[2]->getTypeParams().empty());
  ASSERT_EQ(impls[1]->getMethods()[0]->getTypeParams().size(), 1u);
  EXPECT_EQ(*impls[1]->getMethods()[0]->getTypeParams()[0], "U");

  auto r = parseWithDiags("impl A {\n"
                          "  fn __init__<T>(x: T) {}\n"
                          "}\n"
                          "impl Showable for A {\n"
                          "  view fn toString<T>() -> Str { return \"\"; }\n"
                          "}\n"
                          "fn main() -> int { return 0; }\n");
  EXPECT_FALSE(r.Ok);
  ASSERT_EQ(r.Diags.size(), 2u) << r.Text;
  EXPECT_EQ(r.Diags[0].Message, paykan::frontend::kGenericInit);
  EXPECT_EQ(r.Diags[0].Loc.getLineStart(), 2u);
  EXPECT_EQ(r.Diags[0].Loc.getColumnStart(), 14u);
  EXPECT_EQ(r.Diags[1].Message, paykan::frontend::kGenericTraitImplMethod);
  EXPECT_EQ(r.Diags[1].Loc.getLineStart(), 5u);
  EXPECT_EQ(r.Diags[1].Loc.getColumnStart(), 19u);
}

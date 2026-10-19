// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser tests: function types and lambdas
// (docs/language/15-function-types.md): the `(T1, T2) -> R` type with its
// modes and suffixes, how it is told from a tuple type, the lambda
// expression `fn(x: int) -> int { ... }`, calls of function values, and the
// forms the grammar rejects.

#include "ASTPrinter.h"
#include "TestUtils.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace paykan::test;

// Parse and return the printed AST (empty string on parse failure).
static std::string dumpAST(const std::string &src) {
  auto [ok, driver] = parse(src);
  if (!ok)
    return "";
  std::ostringstream os;
  paykan::ast::ASTPrinter printer(os);
  printer.visit(driver->getRoot());
  return os.str();
}

TEST(FunctionType, TypeForms) {
  auto dump = dumpAST(R"(
fn main() -> int {
  a: (int, float) -> bool = f;
  b: (int) -> (bool, int) = g;
  c: () -> void = h;
  d: (view Str, inout int) -> Str = k;
  e: (int) -> (int) -> int = m;
  return 0;
}
)");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("FunctionType <3:6-3:26> 2 params ->"), std::string::npos)
      << dump;
  // The result of `b` is a tuple type; of `c`, void.
  EXPECT_NE(dump.find("FunctionType <4:6-4:26> 1 params ->"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("TupleType <4:15-4:26> 2 elements"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("FunctionType <5:6-5:16> 0 params ->"), std::string::npos)
      << dump;
  // The modes, by parameter index.
  EXPECT_NE(dump.find("FunctionType <6:6-6:34> 2 params view 0 inout 1 ->"),
            std::string::npos)
      << dump;
  // `->` is right-associative: a function returning a function.
  EXPECT_NE(dump.find("FunctionType <7:6-7:27> 1 params ->"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("FunctionType <7:15-7:27> 1 params ->"),
            std::string::npos)
      << dump;
}

TEST(FunctionType, SuffixesNeedParentheses) {
  // `(int) -> int[]` returns an `int[]`; the array and the optional of a
  // function type are written in parentheses.
  auto dump = dumpAST(R"(
fn main() -> int {
  xs: ((int) -> int)[2] = [f, g];
  o: ((int) -> int)? = None;
  s: (int) -> int[] = h;
  return 0;
}
)");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("FixedArrayType"), std::string::npos) << dump;
  EXPECT_NE(dump.find("OptionalType"), std::string::npos) << dump;
  EXPECT_NE(dump.find("ArrayType"), std::string::npos) << dump;
}

TEST(FunctionType, InEveryTypePosition) {
  auto [ok, _] = parse(R"(
class Button { label: Str; onClick: (Str) -> void; }
struct Op { name: Str; run: (int, int) -> int; }
fn apply(f: (int) -> int, x: int) -> int { return f(x); }
fn twice(f: (int) -> int) -> (int) -> int { return f; }
fn main() -> int {
  t: ((int) -> int, Str) = (f, "t");
  b = Box<(int) -> int>(f);
  return 0;
}
)");
  EXPECT_TRUE(ok);
}

TEST(FunctionType, Lambdas) {
  auto dump = dumpAST(R"(import ::io;
fn main() -> int {
  add = fn(x: int, y: int) -> int { return x + y; };
  log = fn(s: view Str) { io::println(s); };
  nested = fn() -> () -> int { return fn() -> int { return 1; }; };
  return 0;
}
)");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("LambdaExpr <3:9-3:52> ->"), std::string::npos) << dump;
  EXPECT_NE(dump.find("LambdaExpr <4:9-4:44> view 's'"), std::string::npos)
      << dump;
  // The outer lambda's result type is a function type; the inner lambda is
  // in its body.
  EXPECT_NE(dump.find("LambdaExpr <5:12-5:67> ->"), std::string::npos) << dump;
  EXPECT_NE(dump.find("LambdaExpr <5:39-5:64> ->"), std::string::npos) << dump;
}

TEST(FunctionType, CallsOfValues) {
  // A named value is a call; another expression's value is a CallValueExpr.
  auto dump = dumpAST(R"(
fn main() -> int {
  n = f(1);
  m = b.cb(2);
  k = fs[0](3);
  j = t.1(4);
  i = mk()(5);
  return 0;
}
)");
  ASSERT_FALSE(dump.empty());
  EXPECT_NE(dump.find("CallExpr <3:7-3:11> 'f'"), std::string::npos) << dump;
  EXPECT_NE(dump.find("MethodCallExpr <4:7-4:14> 'cb'"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("CallValueExpr <5:7-5:15> 1 args"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("CallValueExpr <6:7-6:13> 1 args"), std::string::npos)
      << dump;
  EXPECT_NE(dump.find("CallValueExpr <7:7-7:14> 1 args"), std::string::npos)
      << dump;
}

TEST(FunctionType, Rejections) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"fn main() -> int { x: (int) = 1; return 0; }",
       "expected ',' (a tuple type needs at least two element types; there "
       "are no parenthesised types)"},
      {"fn main() -> int { x: (view int, int) = (1, 2); return 0; }",
       "a tuple element has no mode: 'view' and 'inout' go on a function "
       "type's parameters ('(view Str) -> int')"},
      {"fn main() -> int { f: (int) -> view Str = g; return 0; }",
       "a function type's result is a copy: '-> view T' is not supported in "
       "a function type"},
      {"fn main() -> int { f: (int) -> inout int = g; return 0; }",
       "a function type's result is a copy: '-> inout T' is not supported "
       "in a function type"},
      {"fn main() -> int { f = fn(x: int) -> view Str { return x; }; "
       "return 0; }",
       "a lambda's result is a copy: '-> view T' is not supported"},
      {"fn main() -> int { f = fn square(x: int) -> int { return x; }; "
       "return 0; }",
       "after 'fn' (a lambda has no name: 'fn(x: int) -> int { ... }')"},
      {"fn main() -> int { f = fn(x: int) -> int return x; return 0; }",
       "expected '{' to start the lambda's body"},
      {"fn main() -> int { f = view fn(x: int) -> int { return x; }; "
       "return 0; }",
       "unexpected 'view'"},
  };
  for (const auto &[src, msg] : cases) {
    auto res = parseWithDiags(src);
    EXPECT_FALSE(res.Ok) << src;
    EXPECT_NE(res.Text.find(msg), std::string::npos) << src << "\n" << res.Text;
  }
}

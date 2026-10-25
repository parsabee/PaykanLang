// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Grammar corners where frontends have disagreed with docs/grammar.md (see
// its section 10).  These run on every frontend, out-of-tree ones included
// (paykan_add_frontend_tests, docs/writing-a-frontend-plugin.md).

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;
using namespace paykan::ast;

// -- Leading commas are syntax errors

TEST(GrammarEdge, LeadingCommaInArgumentListRejected) {
  EXPECT_FALSE(parse("fn main() -> int { f(, 1); return 0; }").Ok);
  EXPECT_FALSE(parse("fn main() -> int { f(,); return 0; }").Ok);
  EXPECT_FALSE(parse("fn main() -> int { x.m(, 1); return 0; }").Ok);
  EXPECT_TRUE(parse("fn main() -> int { f(1, 2); f(); return 0; }").Ok);
}

TEST(GrammarEdge, LeadingCommaInArrayLiteralRejected) {
  EXPECT_FALSE(parse("fn main() -> int { x: int[2] = [, 1]; return 0; }").Ok);
  EXPECT_FALSE(
      parse("fn main() -> int { x: int[2] = [, 1; 2]; return 0; }").Ok);
  EXPECT_TRUE(parse("fn main() -> int { x: int[2] = [1, 2]; y = [1; 2]; "
                    "z = []; return 0; }")
                  .Ok);
}

TEST(GrammarEdge, LeadingCommaInParamListRejected) {
  EXPECT_FALSE(parse("fn f(, a: int) {} fn main() -> int { return 0; }").Ok);
  EXPECT_TRUE(
      parse("fn f(a: int, b: Str) {} fn g() {} fn main() -> int { return 0; }")
          .Ok);
}

TEST(GrammarEdge, RepeatedCommaInEnumRejected) {
  EXPECT_FALSE(parse("enum E { a,, b } fn main() -> int { return 0; }").Ok);
  EXPECT_FALSE(parse("enum E { a, , } fn main() -> int { return 0; }").Ok);
  EXPECT_FALSE(parse("enum E { , a } fn main() -> int { return 0; }").Ok);
  // Exactly one trailing comma stays permitted.
  EXPECT_TRUE(parse("enum E { a, b, } fn main() -> int { return 0; }").Ok);
  EXPECT_TRUE(parse("enum E { a } fn main() -> int { return 0; }").Ok);
}

// -- A '<' opens a generic call only when `< types > (` fits

TEST(GrammarEdge, ComparisonOfCallResultIsNotAGenericCall) {
  // `f(a < b, c) > (d)`: the ')' closes the call's own parenthesis, so the
  // '<' cannot open a type argument list.
  auto [ok, driver] =
      parse("fn main() -> int { x: bool = f(a < b, c) > (d); return 0; }");
  ASSERT_TRUE(ok);
  auto *body = driver->getRoot()->getFuncDecls()[0]->getBody();
  auto *decl = cast<DeclStmt>(body->getStatements()[0]);
  auto *init = cast<VarDecl>(decl->getDecl())->getInitExpr();
  auto *gt = dyn_cast<BinaryExpr>(init);
  ASSERT_NE(gt, nullptr);
  EXPECT_EQ(gt->getOpcode(), BinaryOpcode::Gt);
  auto *call = dyn_cast<CallExpr>(gt->getLHS());
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->getCalleeName(), "f");
  EXPECT_FALSE(call->hasTypeArgs());
  ASSERT_EQ(call->getNumArguments(), 2u);
  auto *lt = dyn_cast<BinaryExpr>(call->getArguments()[0]);
  ASSERT_NE(lt, nullptr);
  EXPECT_EQ(lt->getOpcode(), BinaryOpcode::Lt);
}

TEST(GrammarEdge, GenericCallInsideArgumentListStillParses) {
  // `f(a < b, c > (d))` has exactly the shape of a generic call a<b, c>(d).
  auto [ok, driver] =
      parse("fn main() -> int { x = f(a < b, c > (d)); return 0; }");
  ASSERT_TRUE(ok);
  auto *body = driver->getRoot()->getFuncDecls()[0]->getBody();
  auto *assign = cast<AssignStmt>(body->getStatements()[0]);
  auto *f = cast<CallExpr>(assign->getValue());
  ASSERT_EQ(f->getNumArguments(), 1u);
  auto *a = dyn_cast<CallExpr>(f->getArguments()[0]);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->getCalleeName(), "a");
  EXPECT_EQ(a->getTypeArgs().size(), 2u);
}

TEST(GrammarEdge, SubscriptComparisonIsNotAGenericCall) {
  EXPECT_TRUE(
      parse("fn main() -> int { x: bool = xs[a < b] > (d); return 0; }").Ok);
}

// -- The nesting limit is the same on every frontend (#120)
//
// docs/grammar.md section 9: nesting deeper than 512 levels is rejected with
// `nesting too deep`.  Each construct is built exactly at the limit (accepted)
// and one level past it (rejected), counting the function body as a level.

namespace {

std::string repeat(const std::string &s, int n) {
  std::string out;
  out.reserve(s.size() * static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out += s;
  return out;
}

std::string inMain(const std::string &stmts) {
  return "import ::io; fn main() -> int { " + stmts + " return 0; }\n";
}

// Parse @p src with every frontend: true if every one accepts it; false if
// every one rejects it, first with `nesting too deep (more than @p limit
// levels)`.
bool parsesOnEveryFrontend(const std::string &src, const std::string &what,
                           unsigned limit = 512) {
  auto path = writeTempFile(src);
  int accepted = 0, rejected = 0;
  for (const std::string &fe : paykan::frontend::Registry::get().names()) {
    paykan::parser::ParserDriver drv(fe);
    std::ostringstream os;
    paykan::sema::DiagEngine diag(os);
    drv.setDiagEngine(&diag);
    if (drv.parseFile(path) == 0) {
      ++accepted;
      continue;
    }
    ++rejected;
    // The first error is the nesting error (recursive descent may report
    // follow-on errors after it).
    const std::string text = os.str();
    EXPECT_EQ(text.find("error: "),
              text.find("error: nesting too deep (more than " +
                        std::to_string(limit) + " levels)"))
        << fe << ", " << what << ":\n"
        << text.substr(0, 2000);
  }
  std::filesystem::remove(path);
  EXPECT_TRUE(accepted == 0 || rejected == 0)
      << "frontends disagree on " << what;
  return rejected == 0;
}

// The construct @p make(n) nests n levels inside the function body.
template <typename F> void expectLimit(const char *what, F make) {
  EXPECT_TRUE(parsesOnEveryFrontend(make(511), std::string(what) + " x511"));
  EXPECT_FALSE(parsesOnEveryFrontend(make(512), std::string(what) + " x512"));
}

} // namespace

TEST(GrammarEdge, NestingLimitIsTheSameOnEveryFrontend) {
  expectLimit("blocks",
              [](int n) { return inMain(repeat("{ ", n) + repeat("} ", n)); });
  expectLimit("if statements", [](int n) {
    return inMain(repeat("if a { ", n) + repeat("} ", n));
  });
  expectLimit("parentheses", [](int n) {
    return inMain("x = " + repeat("(", n) + "1" + repeat(")", n) + ";");
  });
  expectLimit("array literals", [](int n) {
    return inMain("x = " + repeat("[", n) + "1" + repeat("]", n) + ";");
  });
  expectLimit("calls", [](int n) {
    return inMain("x = " + repeat("f(", n) + "1" + repeat(")", n) + ";");
  });
  expectLimit("subscripts", [](int n) {
    return inMain("x = " + repeat("a[", n) + "1" + repeat("]", n) + ";");
  });
  expectLimit("prefix operators",
              [](int n) { return inMain("x = " + repeat("!", n) + "a;"); });
  expectLimit("unary minus", [](int n) {
    return inMain("x = a - " + repeat("- ", n) + "a;");
  });
  expectLimit("conditionals", [](int n) {
    return inMain("x = " + repeat("if a then 1 else ", n) + "2;");
  });
  expectLimit("nested then-branches", [](int n) {
    return inMain("x = " + repeat("if a then ", n) + "1" +
                  repeat(" else 2", n) + ";");
  });
  expectLimit("type applications", [](int n) {
    return inMain("x: " + repeat("B<", n) + "int" + repeat(">", n) + " = 1;");
  });
  expectLimit("tuple types", [](int n) {
    return inMain("x: " + repeat("(int, ", n) + "int" + repeat(")", n) +
                  " = 1;");
  });
  // A lambda's body, a function's `=>` body, a function type's result and a
  // lambda's result type are a level each, as a block is (#16).
  expectLimit("lambda bodies", [](int n) {
    return inMain("f = " + repeat("fn() => ", n) + "0;");
  });
  expectLimit("expression bodies", [](int n) {
    return "fn f() => " + repeat("-", n) + "1;\n" + inMain("");
  });
  expectLimit("function-type results", [](int n) {
    return inMain("x: " + repeat("() -> ", n) + "int = f;");
  });
  expectLimit("lambda result types", [](int n) {
    return inMain("f = fn() -> " + repeat("() -> ", n - 1) + "int => g;");
  });
  // Each type suffix is a level around the type it wraps.
  expectLimit("array type suffixes", [](int n) {
    return inMain("x: int" + repeat("[]", n) + " = y;");
  });
  expectLimit("fixed-size array suffixes", [](int n) {
    return inMain("x: int" + repeat("[1]", n) + " = y;");
  });
  expectLimit("optional suffixes", [](int n) {
    return inMain("x: int" + repeat("?[]", n / 2) + (n % 2 ? "?" : "") +
                  " = y;");
  });
  expectLimit("suffixes of a type application", [](int n) {
    return inMain("x: B<int>" + repeat("[]", n - 1) + " = y;");
  });
  // An `elif` is the `if` of an else branch, a level of its own; its block
  // is one more.
  expectLimit("elif chains", [](int n) {
    return inMain("if a { }" + repeat(" elif a { }", n - 1));
  });
  // Mixed: a prefix operator and a parenthesis are a level each.
  expectLimit("mixed", [](int n) {
    std::string s = "x = " + repeat("-(", n / 2) + (n % 2 ? "-1" : "1") +
                    repeat(")", n / 2) + ";";
    return inMain(s);
  });
  // Empty brackets at the limit add no level.
  EXPECT_TRUE(parsesOnEveryFrontend(
      inMain(repeat("{ ", 511) + "f(); x = []; " + repeat("} ", 511)),
      "empty brackets at the limit"));
}

// -- The chain limit is the same on every frontend (#17)
//
// docs/grammar.md section 9: each binary operator and each postfix suffix is
// a link of a chain over the deeper of its operands, and a chain of more
// than 2048 links along any path down the AST is rejected with `nesting too
// deep (more than 2048 levels)`.  Each construct is built with exactly 2048
// links (accepted) and 2049 (rejected).

namespace {

// The chain @p make(n) is n links long.
template <typename F> void expectChainLimit(const char *what, F make) {
  EXPECT_TRUE(
      parsesOnEveryFrontend(make(2048), std::string(what) + " x2048", 2048));
  EXPECT_FALSE(
      parsesOnEveryFrontend(make(2049), std::string(what) + " x2049", 2048));
}

// @p n copies of @p term joined by @p op: n - 1 links.
std::string joined(const std::string &term, const std::string &op, int n) {
  std::string out = term;
  for (int i = 1; i < n; ++i)
    out += op + term;
  return out;
}

} // namespace

TEST(GrammarEdge, ChainLimitIsTheSameOnEveryFrontend) {
  expectChainLimit("+ chains", [](int n) {
    return inMain("x = " + joined("1", " + ", n + 1) + ";");
  });
  expectChainLimit("&& chains", [](int n) {
    return inMain("x = " + joined("a", " && ", n + 1) + ";");
  });
  expectChainLimit("method calls", [](int n) {
    return inMain("x = s" + repeat(".m()", n) + ";");
  });
  expectChainLimit(
      "fields", [](int n) { return inMain("x = s" + repeat(".f", n) + ";"); });
  expectChainLimit("tuple indexes", [](int n) {
    return inMain("x = t" + repeat(".0", n) + ";");
  });
  expectChainLimit("subscripts", [](int n) {
    return inMain("x = a" + repeat("[0]", n) + ";");
  });
  // `f()` is a call of `f`; `(f)()` one of the value `f` holds.
  expectChainLimit("calls of function values", [](int n) {
    return inMain("x = (f)" + repeat("()", n) + ";");
  });
  // A field assigned to is a link, and `+=` is one more over it.
  expectChainLimit("compound assignments", [](int n) {
    return inMain("s" + repeat(".f", n - 1) + " += 1;");
  });
  // Links count along every path down the AST: through parentheses, the
  // operand of a higher precedence and the arguments of a call.
  expectChainLimit("parenthesised chains", [](int n) {
    return inMain("x = " + repeat("(", 64) + "1" +
                  repeat(repeat(" + 1", 32) + ")", 64) +
                  repeat(" + 1", n - 2048) + ";");
  });
  expectChainLimit("chains of higher precedence", [](int n) {
    return inMain("x = 1 + " + joined("1", " * ", 1025) +
                  repeat(" + 1", n - 1025) + ";");
  });
  expectChainLimit("chains in arguments", [](int n) {
    return inMain("x = f(fn() => a" + repeat(".b", n - 1) + ").c;");
  });
}

// Past the limit, the nested block is skipped whole: the nesting error is the
// only one, on every frontend (#132; recursive descent used to resume one
// '}' early and report the rest of the function at top level).
TEST(GrammarEdge, NestingTooDeepInBlocksIsTheOnlyError) {
  const std::string blocks = repeat("{ ", 600) + repeat("} ", 600);
  for (const std::string &src :
       {inMain(blocks),
        inMain(blocks + "x: int = 1; f(x);") + "fn g() -> int { return 1; }\n",
        inMain("a = True; " + repeat("if a { ", 600) + repeat("} ", 600)),
        inMain("a = True; " + repeat("while a { ", 600) + repeat("} ", 600)),
        "class C { fn m() -> int { " + blocks + " return 0; } }\n" +
            inMain("")}) {
    auto path = writeTempFile(src);
    for (const std::string &fe : paykan::frontend::Registry::get().names()) {
      paykan::parser::ParserDriver drv(fe);
      std::ostringstream os;
      paykan::sema::DiagEngine diag(os);
      drv.setDiagEngine(&diag);
      EXPECT_NE(drv.parseFile(path), 0) << fe;
      EXPECT_EQ(drv.getErrorCount(), 1u) << fe << ":\n" << os.str();
      EXPECT_NE(os.str().find("error: nesting too deep (more than 512 "
                              "levels)"),
                std::string::npos)
          << fe << ":\n"
          << os.str();
    }
    std::filesystem::remove(path);
  }
}

TEST(GrammarEdge, PathologicalNestingIsRejectedNotCrashed) {
  // 100,000 levels used to overflow the stack in the passes after a parse
  // that kept its own stack on the heap (Sema, the AST printer), and lambda
  // bodies, function-type results and type suffixes in the parser or in
  // Sema (#16).
  for (const char *open : {"{ ", "(", "-", "if a then ", "fn() => "}) {
    std::string src = "import ::io; fn main() -> int { ";
    std::string o(open);
    if (o == "{ ")
      src += repeat(o, 100000) + repeat("} ", 100000);
    else
      src += "x = " + repeat(o, 100000) + "1;";
    EXPECT_FALSE(parsesOnEveryFrontend(src + " return 0; }", open));
  }
  // And 100,000 links of a chain, in Sema and the lowering (#17).
  for (const char *link : {" + 1", ".m()", ".0", "[0]"}) {
    EXPECT_FALSE(parsesOnEveryFrontend(
        inMain("x = a" + repeat(link, 100000) + ";"), link, 2048));
  }
  for (const char *suffix : {"() -> ", "[]", "[1]", "?[]"}) {
    const std::string s(suffix);
    const std::string type =
        s == "() -> " ? repeat(s, 100000) + "int" : "int" + repeat(s, 100000);
    EXPECT_FALSE(parsesOnEveryFrontend(
        "fn g(x: " + type + ") -> int { return 0; }\n" + inMain(""), suffix));
  }
}

// -- Error locations agree where they cheaply can (#120)

namespace {

// The diagnostics every frontend prints for @p src, by frontend.
std::vector<std::string> diagnosticsOfEveryFrontend(const std::string &src) {
  auto path = writeTempFile(src);
  std::vector<std::string> out;
  for (const std::string &fe : paykan::frontend::Registry::get().names()) {
    paykan::parser::ParserDriver drv(fe);
    std::ostringstream os;
    paykan::sema::DiagEngine diag(os);
    diag.setSourceInfo("t.pkn", nullptr);
    drv.setDiagEngine(&diag);
    EXPECT_NE(drv.parseFile(path), 0) << fe;
    out.push_back(os.str());
  }
  std::filesystem::remove(path);
  return out;
}

} // namespace

TEST(GrammarEdge, CommonErrorsAreLocatedAlike) {
  for (const auto &d :
       diagnosticsOfEveryFrontend("fn main() -> int { x = t.00; return 0; }"))
    EXPECT_EQ(d.rfind("t.pkn:1:25: error: tuple index must not have leading "
                      "zeros: .00\n",
                      0),
              0u)
        << d;
  for (const auto &d : diagnosticsOfEveryFrontend(
           "class C { items: Str[ cell; }\nfn main() -> int { return 0; }"))
    EXPECT_EQ(d.rfind("t.pkn:1:23: error: ", 0), 0u) << d;
  // A lexical error after a syntax error is reported after it, although the
  // recursive-descent parser lexes it first, while looking ahead for a
  // generic call: diagnostics come in source order.
  for (const auto &d : diagnosticsOfEveryFrontend(
           "fn main() -> int { x = f<int, @>(1) + ; return 0; }")) {
    EXPECT_NE(d.find("t.pkn:1:31: error: invalid character '@'"),
              std::string::npos)
        << d;
    size_t last = 0;
    for (size_t at = 0; (at = d.find("t.pkn:1:", at)) != std::string::npos;
         ++at) {
      size_t col = std::stoul(d.substr(at + 8));
      EXPECT_LE(last, col) << d;
      last = col;
    }
  }
}

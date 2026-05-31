// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: functions, return values, Str variable/ARC regressions.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}
static std::string withFns(const std::string &fns, const std::string &body) {
  return fns + "\n" + wrapMain(body);
}

// ============================================================================
// Functions
// ============================================================================

TEST(Func, FunctionCallReturn) {
  auto r = compileAndRun(R"(
    fn add(a: int, b: int) -> int { return a + b; }
    fn main() -> int {
      x: int = add(3, 4);
      out(StringInt(x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "7\n");
}

TEST(Func, FunctionVoid) {
  auto r = compileAndRun(R"(
    fn greet(s: Str) { out(s); }
    fn main() -> int {
      greet("yo");
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "yo\n");
}

TEST(Func, StringPassToFunction) {
  auto r = compileAndRun(withFns(
    "fn show(s: Str) { out(s); }",
    "a: Str = \"hi\";\n  show(a);"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hi\n");
}

// ============================================================================
// Bug-fix regression tests
// ============================================================================

// Fix #1 — visitVarDecl must use emitSharedNew (not inline triple).
// A Str var declared with explicit type annotation must be usable after
// scope exit without crashing (exercises the shared-box path in VarDecl).
TEST(Func, VarDeclStrExplicitType) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "hello";
    out(s);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hello\n");
}

// Fix #1 — multiple explicit-typed Str vars in the same scope: each must get
// its own independent shared box.
TEST(Func, VarDeclMultipleStrVars) {
  auto r = compileAndRun(wrapMain(R"(
    a: Str = "foo";
    b: Str = "bar";
    c: Str = "baz";
    out(a, b, c);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "foobarbaz\n");
}

// Fix #3/#4 — emitClassVarRebind must not double-box a call result.
// Assign the return value of a Str-returning function to a Str variable.
// If the result were wrapped twice the vtable pointer would be corrupt and
// the subsequent out() call would crash or produce garbage.
TEST(Func, RebindFromFuncReturnNoCrash) {
  auto r = compileAndRun(R"(
    fn greeting() -> Str { return "hello world"; }
    fn main() -> int {
      s: Str = "initial";
      s = greeting();
      out(s);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hello world\n");
}

// Fix #3/#4 — rebind from a ternary that returns a Str must also not
// double-box.
TEST(Func, RebindFromTernaryStr) {
  auto r = compileAndRun(wrapMain(R"(
    cond: bool = True;
    s: Str = "first";
    s = if cond then "yes" else "no";
    out(s);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "yes\n");
}

// Fix #3/#4 — symmetry: emitImplicitVarDecl already had the guard; confirm
// that first-time assignment from a function return also works.
TEST(Func, ImplicitDeclFromFuncReturn) {
  auto r = compileAndRun(R"(
    fn tag() -> Str { return "ok"; }
    fn main() -> int {
      result = tag();
      out(result);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "ok\n");
}

// Fix #6 — emitScopeCleanup now uses emitRelease.
// Declare several Str vars inside nested scopes; if cleanup double-releases
// or leaks the process will crash via the runtime's refcount assertion.
TEST(Func, ScopeCleanupNestedStr) {
  auto r = compileAndRun(wrapMain(R"(
    a: Str = "outer";
    i: int = 0;
    while (i < 3) {
      tmp: Str = "inner";
      out(tmp);
      i = i + 1;
    }
    out(a);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "inner\ninner\ninner\nouter\n");
}

// Fix #6 — ScopeGuard must release exactly once per variable; test with an
// if-else where each branch owns a local Str.
TEST(Func, ScopeCleanupIfElseBranches) {
  auto r = compileAndRun(R"(
    fn pick(flag: bool) -> int {
      if (flag) {
        s: Str = "branch_true";
        out(s);
      } else {
        s: Str = "branch_false";
        out(s);
      }
      return 0;
    }
    fn main() -> int {
      pick(True);
      pick(False);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "branch_true\nbranch_false\n");
}

// Fix #7 — visitMethodCallExpr must wrap class-typed return in a shared box.
// Str.concat() returns Str; assigning it and calling out() exercises the path.
TEST(Func, MethodCallClassReturnBoxed) {
  auto r = compileAndRun(wrapMain(R"(
    a: Str = "hello";
    b: Str = " world";
    c: Str = a.concat(b);
    out(c);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "hello world\n");
}

// Fix #7 — calling toString() on a Str should give back a usable Str.
TEST(Func, MethodCallToStringBoxed) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "paykan";
    t: Str = s.toString();
    out(t);
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "paykan\n");
}

// Fix #7 — primitive-returning method (length) must NOT be wrapped.
TEST(Func, MethodCallPrimitiveReturnUnboxed) {
  auto r = compileAndRun(wrapMain(R"(
    s: Str = "hello";
    n: int = s.length();
    out(StringInt(n));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5\n");
}

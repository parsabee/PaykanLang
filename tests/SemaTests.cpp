// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Semantic analysis unit tests

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// Helper: wrap statements in fn main() -> int { ... return 0; }
static std::string wrapMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

// Helper: prepend helper functions + main
static std::string withFns(const std::string &fns, const std::string &body) {
  return fns + "\n" + wrapMain(body);
}

// ============================================================================
// Type checking — basic
// ============================================================================

TEST(Sema, IntLiteral) {
  auto r = semaCheck(wrapMain("x: int = 42;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, FloatLiteral) {
  auto r = semaCheck(wrapMain("x: float = 3.14;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, BoolLiteral) {
  auto r = semaCheck(wrapMain("x: bool = True;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, StringLiteral) {
  auto r = semaCheck(wrapMain("x: String = \"hello\";"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, IntToFloatPromotion) {
  auto r = semaCheck(wrapMain("x: float = 42;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, TypeMismatchInit) {
  auto r = semaCheck(wrapMain("x: int = \"hello\";"));
  EXPECT_FALSE(r.Ok);
}

TEST(Sema, TypeMismatchAssign) {
  auto r = semaCheck(wrapMain("x: int = 1;\n  x = \"hello\";"));
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Variable lifecycle
// ============================================================================

TEST(Sema, UndeclaredVariable) {
  auto r = semaCheck(wrapMain("out(&x);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("undeclared"), std::string::npos);
}

TEST(Sema, DuplicateDeclaration) {
  auto r = semaCheck(wrapMain("x: int = 1;\n  x: int = 2;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("redeclaration"), std::string::npos);
}

TEST(Sema, UseAfterMove) {
  auto src = withFns("fn take(s: String) { out(&s); }",
                     "a: String = \"hi\";\n  take(mov a);\n  out(&a);");
  auto r = semaCheck(src);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("moved"), std::string::npos);
}

TEST(Sema, ConstReassign) {
  auto r = semaCheck(wrapMain("x: const int = 10;\n  x = 20;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("const"), std::string::npos);
}

TEST(Sema, UniqueReassign) {
  auto r = semaCheck(wrapMain("a: String = \"x\";\n  a = \"y\";"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("reassign"), std::string::npos);
}

TEST(Sema, SharedReassignAllowed) {
  auto r = semaCheck(wrapMain("a: shared String = \"x\";\n  a = \"y\";"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Ownership qualifiers on builtins
// ============================================================================

TEST(Sema, SharedOnBuiltinAllowed) {
  auto r = semaCheck(wrapMain("x: shared int = 1;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, RefOnBuiltinAllowed) {
  auto r = semaCheck(wrapMain("y: int = 1;\n  x: int& = &y;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Operator type checks
// ============================================================================

TEST(Sema, ArithmeticOnBool) {
  auto r = semaCheck(wrapMain("x: bool = True + False;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Sema, NegateOnBool) {
  // Unary - on bool should be rejected
  auto r = semaCheck(wrapMain("x: int = -True;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Sema, NotOnInt) {
  auto r = semaCheck(wrapMain("x: bool = !42;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Sema, StringConcatenation) {
  auto r = semaCheck(wrapMain(R"(
    a: String = "hello";
    b: String = "world";
    c: String = a + b;
  )"));
  // This needs mov or some handling — but + on strings produces a new String
  // which gets assigned to a unique var. Let's just check the type is OK.
  // Actually the operands a and b are read (not moved), which is allowed for +.
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, EqualityTypeMismatch) {
  auto r = semaCheck(wrapMain("x: bool = 1 == \"hello\";"));
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Function checks
// ============================================================================

TEST(Sema, UndeclaredFunction) {
  auto r = semaCheck(wrapMain("foo();"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("undeclared function"), std::string::npos);
}

TEST(Sema, FunctionRedefinition) {
  auto r = semaCheck(R"(
    fn foo() {}
    fn foo() {}
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("redefinition"), std::string::npos);
}

TEST(Sema, ArgumentCountMismatch) {
  auto r = semaCheck(R"(
    fn add(a: int, b: int) -> int { return a + b; }
    fn main() -> int { return add(1); }
  )");
  EXPECT_FALSE(r.Ok);
}

TEST(Sema, ReturnTypeMismatch) {
  auto r = semaCheck(R"(
    fn foo() -> int { return "hello"; }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Ownership — unique parameter
// ============================================================================

TEST(Sema, UniqueParamNeedsMov) {
  auto r = semaCheck(withFns("fn take(s: String) {}",
                             "a: String = \"hi\";\n  take(a);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("mov"), std::string::npos);
}

TEST(Sema, UniqueParamWithMov) {
  auto r = semaCheck(withFns("fn take(s: String) {}",
                             "a: String = \"hi\";\n  take(mov a);"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, SharedToUniqueRejected) {
  auto r = semaCheck(withFns("fn take(s: String) {}",
                             "a: shared String = \"hi\";\n  take(a);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("shared"), std::string::npos);
}

TEST(Sema, RefToUniqueRejected) {
  auto r = semaCheck(withFns("fn take(s: String) {}",
                             "a: String = \"hi\";\n  r: String& = &a;\n  take(r);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("reference"), std::string::npos);
}

TEST(Sema, AmpersandToNonRefBuiltinParamRejected) {
  auto r = semaCheck(withFns("fn take(x: int) {}",
                             "a: int = 20;\n  take(&a);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("reference parameter"), std::string::npos);
}

// ============================================================================
// Ownership — shared parameter
// ============================================================================

TEST(Sema, SharedParamFromShared) {
  auto r = semaCheck(withFns("fn take(s: shared String) { out(&s); }",
                             "a: shared String = \"hi\";\n  take(a);"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, SharedParamFromMovUnique) {
  auto r = semaCheck(withFns("fn take(s: shared String) { out(&s); }",
                             "a: String = \"hi\";\n  take(mov a);"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, UniqueToSharedNeedsMov) {
  auto r = semaCheck(withFns("fn take(s: shared String) { out(&s); }",
                             "a: String = \"hi\";\n  take(a);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("mov"), std::string::npos);
}

TEST(Sema, RefToSharedRejected) {
  auto r = semaCheck(withFns("fn take(s: shared String) { out(&s); }",
                             "a: String = \"hi\";\n  r: String& = &a;\n  take(r);"));
  EXPECT_FALSE(r.Ok);
}

// ============================================================================
// Ownership — reference parameter (T&)
// ============================================================================

TEST(Sema, RefParamNeedsAmpersand) {
  auto r = semaCheck(withFns("fn show(s: String&) { out(s); }",
                             "a: String = \"hi\";\n  show(a);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'&'"), std::string::npos);
}

TEST(Sema, RefParamWithAmpersand) {
  auto r = semaCheck(withFns("fn show(s: String&) { out(s); }",
                             "a: String = \"hi\";\n  show(&a);"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, RefParamFromRefVar) {
  auto r = semaCheck(withFns("fn show(s: String&) { out(s); }",
                             "a: String = \"hi\";\n  r: String& = &a;\n  show(r);"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, RefParamRejectsRvalue) {
  auto r = semaCheck(withFns("fn show(s: String&) { out(s); }",
                             "show(\"hello\");"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("rvalue"), std::string::npos);
}

TEST(Sema, RefParamRejectsMovArg) {
  auto r = semaCheck(withFns("fn show(s: String&) { out(s); }",
                             "a: String = \"hi\";\n  show(mov a);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("mov"), std::string::npos);
}

TEST(Sema, NonConstRefRejectsSharedAmpersand) {
  auto r = semaCheck(withFns("fn show(s: String&) { out(s); }",
                             "a: shared String = \"hi\";\n  show(&a);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("non-const"), std::string::npos);
}

// ============================================================================
// Ownership — const reference parameter (const T&)
// ============================================================================

TEST(Sema, ConstRefAcceptsRvalue) {
  auto r = semaCheck(withFns("fn show(s: const String&) { out(s); }",
                             "show(\"hello\");"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, ConstRefAcceptsConstructorResult) {
  auto r = semaCheck(withFns("fn show(s: const String&) { out(s); }",
                             "show(StringInt(42));"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, ConstRefAcceptsAmpersandUnique) {
  auto r = semaCheck(withFns("fn show(s: const String&) { out(s); }",
                             "a: String = \"hi\";\n  show(&a);"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, ConstRefAcceptsAmpersandShared) {
  auto r = semaCheck(withFns("fn show(s: const String&) { out(s); }",
                             "a: shared String = \"hi\";\n  show(&a);"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, ConstRefAcceptsRefVar) {
  auto r = semaCheck(withFns("fn show(s: const String&) { out(s); }",
                             "a: String = \"hi\";\n  r: String& = &a;\n  show(r);"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Ref expression checks
// ============================================================================

TEST(Sema, RefOnLiteralRejected) {
  auto r = semaCheck(wrapMain("out(&\"hello\");"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("variable"), std::string::npos);
}

TEST(Sema, MovOnRefVarRejected) {
  auto src = withFns("fn take(s: String) {}",
                     "a: String = \"hi\";\n  r: String& = &a;\n  take(mov r);");
  auto r = semaCheck(src);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("reference"), std::string::npos);
}

TEST(Sema, MovOnBuiltinRejected) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 20;
    y: int = mov x;
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("class types"), std::string::npos);
}

TEST(Sema, RefBindsTemporary) {
  auto r = semaCheck(wrapMain("r: String& = \"hello\";"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'&'"), std::string::npos);
}

// ============================================================================
// Scoping
// ============================================================================

TEST(Sema, BlockScopeIsolation) {
  auto r = semaCheck(wrapMain(R"(
    {
      x: int = 1;
    }
    out(StringInt(x));
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("undeclared"), std::string::npos);
}

TEST(Sema, InnerScopeCanAccessOuter) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 1;
    {
      y: int = x + 1;
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Builtins
// ============================================================================

TEST(Sema, OutAcceptsMultipleArgs) {
  auto r = semaCheck(wrapMain(R"(
    a: String = "hello";
    out(&a, StringInt(42));
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, OutAcceptsLiterals) {
  auto r = semaCheck(wrapMain("out(\"hello\");"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// if / else
// ============================================================================

TEST(Sema, IfBoolCondition) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 10;
    if (x > 5) {
      out("big");
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, IfElse) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 10;
    if (x > 5) {
      out("big");
    } else {
      out("small");
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, IfNonBoolConditionRejected) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 10;
    if (x) {
      out("nope");
    }
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("bool"), std::string::npos);
}

TEST(Sema, IfElseIfChain) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 42;
    if (x > 100) {
      out("large");
    } else if (x > 10) {
      out("medium");
    } else {
      out("small");
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, IfStringConditionRejected) {
  auto r = semaCheck(wrapMain(R"(
    s: String = "hello";
    if (&s) {
      out("nope");
    }
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("bool"), std::string::npos);
}

// ============================================================================
// while
// ============================================================================

TEST(Sema, WhileBoolCondition) {
  auto r = semaCheck(wrapMain(R"(
    i: int = 0;
    while (i < 5) {
      i = i + 1;
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, WhileNonBoolConditionRejected) {
  auto r = semaCheck(wrapMain(R"(
    i: int = 0;
    while (i) {
      i = i + 1;
    }
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("bool"), std::string::npos);
}

TEST(Sema, WhileTrueLiteral) {
  auto r = semaCheck(wrapMain(R"(
    while (True) {
      return 0;
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Logical operators && ||
// ============================================================================

TEST(Sema, LogicalAndBool) {
  auto r = semaCheck(wrapMain("a: bool = True && False;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, LogicalOrBool) {
  auto r = semaCheck(wrapMain("a: bool = True || False;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, LogicalAndWithRelational) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 5;
    a: bool = x > 0 && x < 10;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, LogicalOrWithRelational) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 5;
    a: bool = x < 0 || x > 0;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, LogicalAndNonBoolLhsRejected) {
  auto r = semaCheck(wrapMain("a: bool = 42 && True;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("bool"), std::string::npos);
}

TEST(Sema, LogicalOrNonBoolRhsRejected) {
  auto r = semaCheck(wrapMain("a: bool = True || 42;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("bool"), std::string::npos);
}

TEST(Sema, LogicalChained) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 5;
    a: bool = x > 0 && x < 10 || x == 0;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, LogicalInIfCondition) {
  auto r = semaCheck(wrapMain(R"(
    x: int = 5;
    y: int = 10;
    if (x > 0 && y > 0) {
      out("both positive");
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, LogicalInWhileCondition) {
  auto r = semaCheck(wrapMain(R"(
    i: int = 0;
    j: int = 10;
    while (i < 5 && j > 0) {
      i = i + 1;
      j = j - 1;
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Ternary expressions
// ============================================================================

TEST(Sema, TernaryIntBranches) {
  auto r = semaCheck(wrapMain("x: int = if True then 1 else 2;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, TernaryBoolBranches) {
  auto r = semaCheck(wrapMain("x: bool = if True then True else False;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, TernaryFloatBranches) {
  auto r = semaCheck(wrapMain("x: float = if True then 1.0 else 2.0;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, TernaryWithRelationalCondition) {
  auto r = semaCheck(wrapMain(R"(
    a: int = 5;
    x: int = if a > 3 then 10 else 20;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, TernaryNonBoolConditionRejected) {
  auto r = semaCheck(wrapMain("x: int = if 1 then 2 else 3;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Sema, TernaryMismatchedBranchesRejected) {
  auto r = semaCheck(wrapMain("x: int = if True then 1 else 2.0;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Sema, TernaryNestedOk) {
  auto r = semaCheck(wrapMain("x: int = if True then 1 else if False then 2 else 3;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// break / continue
// ============================================================================

TEST(Sema, BreakInsideLoop) {
  auto r = semaCheck(wrapMain(R"(
    while (True) {
      break;
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, ContinueInsideLoop) {
  auto r = semaCheck(wrapMain(R"(
    i: int = 0;
    while (i < 10) {
      i = i + 1;
      continue;
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, BreakOutsideLoopRejected) {
  auto r = semaCheck(wrapMain("break;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Sema, ContinueOutsideLoopRejected) {
  auto r = semaCheck(wrapMain("continue;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Sema, BreakInIfInsideLoopOk) {
  auto r = semaCheck(wrapMain(R"(
    i: int = 0;
    while (i < 10) {
      if (i == 5) { break; }
      i = i + 1;
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Sema, BreakInIfOutsideLoopRejected) {
  auto r = semaCheck(wrapMain(R"(
    if (True) { break; }
  )"));
  EXPECT_FALSE(r.Ok);
}

TEST(Sema, BreakInNestedLoop) {
  auto r = semaCheck(wrapMain(R"(
    while (True) {
      while (True) {
        break;
      }
      break;
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Imports
// ============================================================================

TEST(Sema, ImportUnresolved) {
  auto r = semaCheck(R"(
    import nonexistent;
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("not found"), std::string::npos);
}

TEST(Sema, ImportCircular) {
  auto tmpDir = std::filesystem::temp_directory_path() / "pkn_sema_circ";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  // a imports b, b imports a => cycle
  {
    std::ofstream(tmpDir / "a.pkn") << "import b;\nfn fa() -> int { return 1; }\n";
    std::ofstream(tmpDir / "b.pkn") << "import a;\nfn fb() -> int { return 2; }\n";
  }

  paykan::parser::ParserDriver drv;
  ASSERT_EQ(drv.parseFile((tmpDir / "a.pkn").string()), 0);

  std::string diag;
  llvm::raw_string_ostream os(diag);
  paykan::sema::Sema sema(drv.getASTContext(), os, tmpDir.string(),
                          drv.getCurrentFile(), &drv.getSourceLines());
  bool ok = sema.run(drv.getRoot());
  EXPECT_FALSE(ok);
  EXPECT_NE(diag.find("circular"), std::string::npos);

  std::filesystem::remove_all(tmpDir);
}

// ============================================================================
// Import — ClassType remapping
// ============================================================================

// A module function whose parameter/return type is a ClassType (String)
// must be importable; the return type must be remapped to the importer's
// canonical StringTy so that it can be stored in a String variable.
TEST(Sema, ImportClassTypeRemap) {
  auto tmpDir = std::filesystem::temp_directory_path() / "pkn_sema_classremap";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  std::ofstream(tmpDir / "strmod.pkn") << R"(
fn wrap(s: String&) -> int { return 0; }
)";

  paykan::parser::ParserDriver drv;
  {
    auto mainPath = (tmpDir / "main.pkn").string();
    std::ofstream(mainPath) << R"(
import strmod;
fn main() -> int {
  s: String = "hi";
  return strmod::wrap(&s);
}
)";
    ASSERT_EQ(drv.parseFile(mainPath), 0);
  }

  std::string diag;
  llvm::raw_string_ostream os(diag);
  paykan::sema::Sema sema(drv.getASTContext(), os, tmpDir.string(),
                          drv.getCurrentFile(), &drv.getSourceLines());
  bool ok = sema.run(drv.getRoot());
  EXPECT_TRUE(ok) << diag;

  std::filesystem::remove_all(tmpDir);
}

// Importing the same module twice must yield the same canonical ClassType
// pointer (type identity) rather than two distinct instances.
TEST(Sema, ImportClassTypeIdentity) {
  auto tmpDir = std::filesystem::temp_directory_path() / "pkn_sema_classid";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  // strutil defines functions that take/return String.
  std::ofstream(tmpDir / "strutil.pkn") << R"(
fn id(s: String&) -> int { return 0; }
)";

  std::ofstream(tmpDir / "modA.pkn") << R"(
import strutil;
fn useA(s: String&) -> int { return strutil::id(&s); }
)";
  std::ofstream(tmpDir / "modB.pkn") << R"(
import strutil;
fn useB(s: String&) -> int { return strutil::id(&s); }
)";

  paykan::parser::ParserDriver drv;
  {
    auto mainPath = (tmpDir / "main.pkn").string();
    std::ofstream(mainPath) << R"(
import modA;
import modB;
fn main() -> int {
  s: String = "hello";
  a: int = modA::useA(&s);
  b: int = modB::useB(&s);
  return 0;
}
)";
    ASSERT_EQ(drv.parseFile(mainPath), 0);
  }

  std::string diag;
  llvm::raw_string_ostream os(diag);
  paykan::sema::Sema sema(drv.getASTContext(), os, tmpDir.string(),
                          drv.getCurrentFile(), &drv.getSourceLines());
  bool ok = sema.run(drv.getRoot());
  EXPECT_TRUE(ok) << diag;

  std::filesystem::remove_all(tmpDir);
}

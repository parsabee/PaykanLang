// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Parser tests: the syntax of the concurrency prototype
// (docs/design/concurrency.md, samples/concurrency/README.md): `async fn`,
// `await` and `move`, `parallel for`, the `lock` block, and the qualifiers
// in a type annotation.  What the forms mean is Sema's (tests/Sema); here
// is what the parser takes, the AST it builds and the error it gives.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;
namespace ast = paykan::ast;

namespace {

/// The diagnostics of parsing @p source with the frontend under test; "" when
/// it parses.
std::string parseErrors(const std::string &source) {
  auto path = writeTempFile(source);
  paykan::parser::ParserDriver driver(testFrontend());
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  driver.setDiagEngine(&diag);
  int rc = driver.parseFile(path);
  std::filesystem::remove(path);
  EXPECT_EQ(rc != 0, !os.str().empty()) << source;
  return os.str();
}

/// @p source parses with no frontend, and the recursive-descent frontend's
/// error holds @p message.
void expectRejected(const std::string &source, const char *message) {
  auto [ok, _] = parse(source);
  EXPECT_FALSE(ok) << source;
  if (testFrontend() != "recursive-descent")
    return; // the wording of a syntax error is the frontend's
  std::string errs = parseErrors(source);
  EXPECT_NE(errs.find(message), std::string::npos) << source << "\n" << errs;
}

/// @p stmt as the first statement of `main`.
std::string inMain(const char *stmt) {
  return std::string("fn main() -> int {\n  ") + stmt + "\n  return 0;\n}";
}

/// @p decl at the top level, before `main`.
std::string atTopLevel(const char *decl) {
  return std::string(decl) + "\nfn main() -> int { return 0; }";
}

/// The statements of the first free function of @p driver.
const std::vector<ast::Stmt *> &statementsOf(paykan::parser::ParserDriver &d) {
  return d.getRoot()->getFuncDecls()[0]->getBody()->getStatements();
}

/// The qualifier of @p ty as written, or null for a type without one.
const char *qualifierOf(const ast::Type *ty) {
  const auto *q = ast::dyn_cast<ast::QualifiedType>(ty);
  return q ? q->getQualifier().spelling() : nullptr;
}

} // namespace

// -- async fn

// `async fn` marks a free function, with any body a function may have.
// The declaration spans `async` through the body.
TEST(AsyncFn, MarksAFreeFunction) {
  auto [ok, driver] = parse(R"(async fn fib(n: int) -> int { return n; }
async fn run() { }
async fn twice(x: int) => x * 2;
fn plain() -> int { return 0; }
async fn main() -> int { return 0; }
async fn same<T>(x: T) -> T { return x; }
)");
  ASSERT_TRUE(ok);
  const auto &fns = driver->getRoot()->getFuncDecls();
  ASSERT_EQ(fns.size(), 5u);
  EXPECT_TRUE(fns[0]->isAsync());
  EXPECT_TRUE(fns[1]->isAsync());
  EXPECT_TRUE(fns[2]->isAsync());
  EXPECT_FALSE(fns[3]->isAsync());
  EXPECT_TRUE(fns[4]->isAsync());
  const auto &generics = driver->getRoot()->getGenericFuncDecls();
  ASSERT_EQ(generics.size(), 1u);
  EXPECT_TRUE(generics[0]->isAsync());
  std::string dump = dumpAST(*driver);
  // The printer shows the rest of the declaration as a plain function's.
  for (const char *fn :
       {"FuncDecl <1:1-1:42> 'fib' ->\n", "FuncDecl <2:1-2:19> 'run'\n",
        "FuncDecl <3:1-3:33> 'twice' inferred\n",
        "FuncDecl <5:1-5:37> 'main' ->\n"})
    EXPECT_NE(dump.find(fn), std::string::npos) << fn << "\n" << dump;
}

// Only a free function is `async` in the prototype: a method of a class
// body, an `impl` block or a trait is not, nor a `static fn`, a `view fn`
// or a native function.  `async` is followed by `fn` and nothing else.
TEST(AsyncFn, OnlyAFreeFunction) {
  const char *const cases[][2] = {
      {"class C { n: int; async fn f() -> int { return 1; } }",
       "unexpected 'async'; expected a field ('name: Type;') or a method "
       "('fn') in the class body"},
      {"class C { n: int; }\nimpl C { async fn f() -> int { return 1; } }",
       "unexpected 'async'; expected a method ('fn', 'view fn' or 'static "
       "fn') in the impl block"},
      {"class C { n: int; }\n"
       "impl C { static async fn f() -> int { return 1; } }",
       "unexpected 'async'; expected 'fn' after 'static'"},
      {"trait T { async fn f() -> int; }",
       "unexpected 'async'; expected a method signature ('fn' or 'view fn') "
       "in the trait"},
      {"async view fn f() -> int { return 1; }",
       "unexpected 'async'; expected 'fn' after 'async'"},
      {"view async fn f() -> int { return 1; }",
       "unexpected 'view'; expected 'import', 'class', 'struct', 'enum', "
       "'trait', 'impl' or 'fn' at top level"},
      {"async class C { }", "unexpected 'async'; expected 'fn' after 'async'"},
      {"async native fn f() -> int = \"f\";",
       "unexpected 'async'; expected 'fn' after 'async'"},
      {"native async fn f() -> int = \"f\";",
       "unexpected 'async'; expected 'fn' after 'native'"},
      {"async fn<T> f(x: T) -> T { return x; }",
       "unexpected '<'; expected a function name after 'fn'"},
  };
  for (const auto &c : cases)
    expectRejected(atTopLevel(c[0]), c[1]);
}

// -- await and move

// `await e` and `move e` are unary operators, binding like `-` and `!`:
// tighter than every binary operator, looser than a call, a member or an
// index, and nesting with each other and the other unary operators.
TEST(AwaitMove, AreUnaryOperators) {
  auto [ok, driver] = parse(R"(fn main() -> int {
  x = await a + await b;
  y = move p.q;
  z = -move a;
  u = await f(1) * 2;
  v = move a == b;
  w = await await t;
  k = move xs[0];
  f(move a, await b);
  return await t;
})");
  ASSERT_TRUE(ok);
  std::string dump = dumpAST(*driver);
  for (const char *line :
       {// await a + await b: the sum of two awaits
        "BinaryExpr <2:7-2:24> '+'\n", "UnaryExpr <2:7-2:14> 'await'\n",
        "UnaryExpr <2:17-2:24> 'await'\n",
        // move p.q: the member is the operand
        "UnaryExpr <3:7-3:15> 'move'\n", "MemberAccessExpr 'q' <3:12-3:15>\n",
        // -move a
        "UnaryExpr <4:7-4:14> '-'\n", "UnaryExpr <4:8-4:14> 'move'\n",
        // await f(1) * 2: the call is awaited, then doubled
        "BinaryExpr <5:7-5:21> '*'\n", "UnaryExpr <5:7-5:17> 'await'\n",
        "CallExpr <5:13-5:17> 'f'\n",
        // move a == b
        "BinaryExpr <6:7-6:18> '=='\n", "UnaryExpr <6:7-6:13> 'move'\n",
        // await await t
        "UnaryExpr <7:7-7:20> 'await'\n", "UnaryExpr <7:13-7:20> 'await'\n",
        // move xs[0]
        "UnaryExpr <8:7-8:17> 'move'\n", "SubscriptExpr <8:12-8:17>\n",
        // arguments
        "UnaryExpr <9:5-9:11> 'move'\n", "UnaryExpr <9:13-9:20> 'await'\n",
        // return await t
        "UnaryExpr <10:10-10:17> 'await'\n"})
    EXPECT_NE(dump.find(line), std::string::npos) << line << "\n" << dump;

  const auto &stmts = statementsOf(*driver);
  ASSERT_EQ(stmts.size(), 9u);
  const auto &args =
      ast::cast<ast::CallExpr>(ast::cast<ast::ExprStmt>(stmts[7])->getExpr())
          ->getArguments();
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(ast::cast<ast::UnaryExpr>(args[0])->getOpcode(),
            ast::UnaryOpcode::Move);
  EXPECT_EQ(ast::cast<ast::UnaryExpr>(args[1])->getOpcode(),
            ast::UnaryOpcode::Await);
  const auto *ret = ast::cast<ast::UnaryExpr>(
      ast::cast<ast::ReturnStmt>(stmts[8])->getReturnValue());
  EXPECT_EQ(ret->getOpcode(), ast::UnaryOpcode::Await);
  EXPECT_TRUE(ast::isa<ast::Identifier>(ret->getOperand()));
}

// Each takes an operand after it, and nothing before it.
TEST(AwaitMove, TakeAnOperand) {
  const char *const cases[][2] = {
      {"x = await;", "unexpected ';'; expected an expression"},
      {"x = move;", "unexpected ';'; expected an expression"},
      {"x = 1 + move;", "unexpected ';'; expected an expression"},
      {"x = a await b;", "unexpected 'await'; expected ';' after the "
                         "assignment"},
      {"x = a move;", "unexpected 'move'; expected ';' after the assignment"},
      {"await = 1;", "unexpected '='; expected an expression"},
      {"move = 1;", "unexpected '='; expected an expression"},
  };
  for (const auto &c : cases)
    expectRejected(inMain(c[0]), c[1]);
}

// -- parallel for

// `parallel` before a `for` marks the loop; the loop is otherwise any `for`
// over a range (`..=`, `step`, `_`, a typed variable).  The statement
// spans `for` through the block, as a plain loop's does.
TEST(ParallelFor, MarksAForLoop) {
  auto [ok, driver] = parse(R"(fn main() -> int {
  s = 0;
  parallel for i in 0..10 {
    s += i;
  }
  parallel for j in 1..=9 step 2 { }
  parallel for _ in 0..3 { }
  parallel for m: int in 0..1 { }
  for k in 0..2 { }
  return s;
})");
  ASSERT_TRUE(ok);
  const auto &stmts = statementsOf(*driver);
  ASSERT_EQ(stmts.size(), 7u);
  const auto *first = ast::cast<ast::ForStmt>(stmts[1]);
  EXPECT_TRUE(first->isParallel());
  EXPECT_FALSE(first->isInclusive());
  EXPECT_FALSE(first->hasStep());
  const auto *second = ast::cast<ast::ForStmt>(stmts[2]);
  EXPECT_TRUE(second->isParallel());
  EXPECT_TRUE(second->isInclusive());
  EXPECT_TRUE(second->hasStep());
  EXPECT_TRUE(ast::cast<ast::ForStmt>(stmts[3])->isParallel());
  EXPECT_TRUE(ast::cast<ast::ForStmt>(stmts[4])->isParallel());
  EXPECT_FALSE(ast::cast<ast::ForStmt>(stmts[5])->isParallel());
  std::string dump = dumpAST(*driver);
  for (const char *line :
       {"ForStmt <3:12-5:4> ..\n", "VarDecl <3:16-3:17> 'i'\n",
        "ForStmt <6:12-6:37> ..= step\n", "ForStmt <7:12-7:29> ..\n",
        "VarDecl <7:16-7:17> '_'\n", "ForStmt <8:12-8:34> ..\n",
        "VarDecl <8:16-8:22> 'm' type\n", "ForStmt <9:3-9:20> ..\n"})
    EXPECT_NE(dump.find(line), std::string::npos) << line << "\n" << dump;
}

// `parallel` goes before `for` and nowhere else: not before another
// statement, not inside one, and not at the top level.
TEST(ParallelFor, OnlyBeforeFor) {
  const char *const cases[][2] = {
      {"parallel while True { }",
       "unexpected 'while'; expected 'for' after 'parallel'"},
      {"parallel { }", "unexpected '{'; expected 'for' after 'parallel'"},
      {"parallel = 1;", "unexpected '='; expected 'for' after 'parallel'"},
      {"x = parallel for i in 0..10 { };",
       "unexpected 'parallel'; expected an expression"},
      {"for parallel i in 0..10 { }",
       "unexpected 'parallel'; expected the loop variable's name or '_' "
       "after 'for'"},
      {"parallel for i in 0..10;",
       "unexpected ';'; expected 'step' or '{' after the 'for' loop's range"},
  };
  for (const auto &c : cases)
    expectRejected(inMain(c[0]), c[1]);
  expectRejected(atTopLevel("parallel for i in 0..10 { }"),
                 "unexpected 'parallel'; expected 'import', 'class', "
                 "'struct', 'enum', 'trait', 'impl', 'fn' or 'native' at top "
                 "level");
}

// -- lock

// `lock cell as name: inout { }` or `: view { }`: the cell is any
// expression, the binding a borrow of its contents.  A nested `lock`
// parses (Sema rejects it).
TEST(Lock, BindsTheContents) {
  auto [ok, driver] = parse(R"(fn main() -> int {
  lock c as v: inout {
    v.n = 2;
  }
  lock self.counts as w: view { }
  lock c as x: view { lock c as y: inout { } }
  return 0;
})");
  ASSERT_TRUE(ok);
  const auto &stmts = statementsOf(*driver);
  ASSERT_EQ(stmts.size(), 4u);
  const auto *first = ast::cast<ast::LockStmt>(stmts[0]);
  EXPECT_EQ(first->getBinding()->getName(), "v");
  EXPECT_EQ(first->getBinding()->getMode(), ast::ParamMode::Inout);
  EXPECT_EQ(first->getBinding()->getType(), nullptr); // Sema writes it
  EXPECT_TRUE(ast::isa<ast::Identifier>(first->getCell()));
  EXPECT_EQ(first->getBody()->getStatements().size(), 1u);
  const auto *second = ast::cast<ast::LockStmt>(stmts[1]);
  EXPECT_EQ(second->getBinding()->getName(), "w");
  EXPECT_EQ(second->getBinding()->getMode(), ast::ParamMode::View);
  EXPECT_TRUE(ast::isa<ast::MemberAccessExpr>(second->getCell()));
  EXPECT_TRUE(second->getBody()->getStatements().empty());
  std::string dump = dumpAST(*driver);
  for (const char *line :
       {"LockStmt <2:3-4:4> 'v' inout\n", "Identifier <2:8-2:9> 'c'\n",
        "LockStmt <5:3-5:34> 'w' view\n",
        "MemberAccessExpr 'counts' <5:8-5:19>\n",
        "LockStmt <6:3-6:47> 'x' view\n", "LockStmt <6:23-6:45> 'y' inout\n"})
    EXPECT_NE(dump.find(line), std::string::npos) << line << "\n" << dump;
}

// Every piece of the statement is required, in its order: the cell, `as`,
// a name, `:`, `view` or `inout` (no type, no qualifier), the block.
TEST(Lock, Errors) {
  const char *const cases[][2] = {
      {"lock c { }",
       "unexpected '{'; expected 'as' after the 'lock' statement's cell"},
      {"lock as v: inout { }", "unexpected 'as'; expected an expression"},
      {"lock = 1;", "unexpected '='; expected an expression"},
      {"lock c as { }",
       "unexpected '{'; expected a name for the cell's contents after 'as'"},
      {"lock c as _: view { }",
       "unexpected '_'; expected a name for the cell's contents after 'as'"},
      {"lock c as inout v { }",
       "unexpected 'inout'; expected a name for the cell's contents after "
       "'as'"},
      {"lock c as v { }", "unexpected '{'; expected ':' after the name"},
      {"lock c as v: { }",
       "unexpected '{'; expected 'view' or 'inout': how the block reaches "
       "the cell's contents"},
      {"lock c as v: int { }",
       "unexpected identifier 'int'; expected 'view' or 'inout': how the "
       "block reaches the cell's contents"},
      {"lock c as v: shared { }",
       "unexpected 'shared'; expected 'view' or 'inout': how the block "
       "reaches the cell's contents"},
      {"lock c as v: inout int { }",
       "unexpected identifier 'int'; expected '{' to open the 'lock' block"},
      {"lock c as v: inout;",
       "unexpected ';'; expected '{' to open the 'lock' block"},
      {"lock c as v: view",
       "unexpected 'return'; expected '{' to open the 'lock' block"},
  };
  for (const auto &c : cases)
    expectRejected(inMain(c[0]), c[1]);
}

// -- Qualifiers

// A qualifier goes after the colon (or the arrow), before the type, and
// covers the whole type: `const`, `shared`, `shared const`,
// `atomic shared const` and `locked` make a QualifiedType around it,
// `atomic T` an AtomicType.
// In a local declaration the qualifier may stand alone, with the type
// inferred.  The parser records the qualifier in the type only; Sema moves
// it to the place (Param::Qual, VarDecl's and FuncDecl's qualifier).
TEST(Qualifiers, OnFieldsParametersLocalsAndResults) {
  auto [ok, driver] = parse(R"(class Node {
  id: int;
  right: shared Node;
  left: const Node;
  both: shared const Node;
  cfg: atomic shared const Node;
  counts: locked Node;
  hits: atomic int;
}
struct Pair {
  left: shared Node;
}
fn f(a: const Node, b: shared Node, c: shared const Node,
     d: atomic shared const Node, e: locked Node, h: atomic float, n: int)
    -> locked Node { return e; }
fn g() -> shared Node { return a; }
fn h() -> atomic int { return 1; }
fn main() -> int {
  let a: const Node = Node();
  b: shared = a;
  c: shared const = a;
  let d: locked = a;
  e: atomic shared const = a;
  f: const = a;
  let g: atomic int = 1;
  h: shared Node[] = a;
  i: const Node? = None;
  let k: List<shared Node> = a;
  l: (shared Node, int) = b;
  return 0;
})");
  ASSERT_TRUE(ok);
  std::string dump = dumpAST(*driver);
  for (const char *line :
       {// fields
        "VarDecl <3:3-3:21> 'right' type\n",
        "QualifiedType <3:10-3:21> 'shared'\n",
        "ClassType <3:17-3:21> 'Node'\n", "QualifiedType <4:9-4:19> 'const'\n",
        "QualifiedType <5:9-5:26> 'shared const'\n",
        "QualifiedType <6:8-6:32> 'atomic shared const'\n",
        "QualifiedType <7:11-7:22> 'locked'\n",
        "VarDecl <8:3-8:19> 'hits' type\n", "AtomicType <8:9-8:19>\n",
        "QualifiedType <11:9-11:20> 'shared'\n",
        // results
        "QualifiedType <15:8-15:19> 'locked'\n",
        "QualifiedType <16:11-16:22> 'shared'\n", "AtomicType <17:11-17:21>\n",
        // locals, with the type written
        "VarDecl <19:3-19:29> 'a' let type\n",
        "QualifiedType <19:10-19:20> 'const'\n",
        "VarDecl <25:3-25:24> 'g' let type\n", "AtomicType <25:10-25:20>\n",
        // ... and inferred: the QualifiedType has no inner type
        "VarDecl <20:3-20:16> 'b' type\n",
        "QualifiedType <20:6-20:12> 'shared'\n",
        "QualifiedType <21:6-21:18> 'shared const'\n",
        "VarDecl <22:3-22:20> 'd' let type\n",
        "QualifiedType <22:10-22:16> 'locked'\n",
        "QualifiedType <23:6-23:25> 'atomic shared const'\n",
        "QualifiedType <24:6-24:11> 'const'\n",
        // the qualifier covers an array, an optional, and goes inside a
        // generic's argument or a tuple's element
        "QualifiedType <26:6-26:19> 'shared'\n", "ArrayType <26:13-26:19>\n",
        "QualifiedType <27:6-27:17> 'const'\n", "OptionalType <27:12-27:17>\n",
        "GenericType <28:10-28:27> 'List'\n",
        "QualifiedType <28:15-28:26> 'shared'\n",
        "TupleType <29:6-29:24> 2 elements\n",
        "QualifiedType <29:7-29:18> 'shared'\n"})
    EXPECT_NE(dump.find(line), std::string::npos) << line << "\n" << dump;

  // The parameters are not printed: check their types directly.
  const auto &fns = driver->getRoot()->getFuncDecls();
  ASSERT_EQ(fns.size(), 4u);
  const auto &params = fns[0]->getParams();
  ASSERT_EQ(params.size(), 7u);
  EXPECT_STREQ(qualifierOf(params[0].ParamType), "const");
  EXPECT_STREQ(qualifierOf(params[1].ParamType), "shared");
  EXPECT_STREQ(qualifierOf(params[2].ParamType), "shared const");
  EXPECT_STREQ(qualifierOf(params[3].ParamType), "atomic shared const");
  EXPECT_STREQ(qualifierOf(params[4].ParamType), "locked");
  EXPECT_TRUE(ast::isa<ast::AtomicType>(params[5].ParamType));
  EXPECT_EQ(qualifierOf(params[6].ParamType), nullptr);
  for (const ast::Param &p : params) {
    EXPECT_EQ(p.Mode, ast::ParamMode::Value) << p.getName();
    EXPECT_TRUE(p.Qual.isUnique()) << p.getName(); // Sema fills it in
  }
  EXPECT_TRUE(ast::isa<ast::ClassType>(
      ast::cast<ast::QualifiedType>(params[0].ParamType)->getInnerType()));
  const auto *shared = ast::cast<ast::QualifiedType>(params[1].ParamType);
  EXPECT_EQ(shared->getQualifier().Form, ast::QualForm::Shared);
  EXPECT_FALSE(shared->getQualifier().Const);
  const auto *sharedConst = ast::cast<ast::QualifiedType>(params[2].ParamType);
  EXPECT_EQ(sharedConst->getQualifier().Form, ast::QualForm::Shared);
  EXPECT_TRUE(sharedConst->getQualifier().Const);
  EXPECT_STREQ(qualifierOf(fns[0]->getReturnType()), "locked");
  EXPECT_EQ(fns[0]->getResultMode(), ast::ParamMode::Value);
  EXPECT_TRUE(fns[0]->getResultQual().isUnique()); // Sema fills it in
  EXPECT_STREQ(qualifierOf(fns[1]->getReturnType()), "shared");
  EXPECT_TRUE(ast::isa<ast::AtomicType>(fns[2]->getReturnType()));

  const auto &stmts = fns[3]->getBody()->getStatements();
  auto localType = [&](size_t i) {
    return ast::cast<ast::VarDecl>(
               ast::cast<ast::DeclStmt>(stmts[i])->getDecl())
        ->getType();
  };
  EXPECT_NE(ast::cast<ast::QualifiedType>(localType(0))->getInnerType(),
            nullptr);
  for (size_t i : {1u, 2u, 3u, 4u, 5u})
    EXPECT_EQ(ast::cast<ast::QualifiedType>(localType(i))->getInnerType(),
              nullptr)
        << "local " << i;
  EXPECT_EQ(ast::cast<ast::QualifiedType>(localType(1))->getQualifier().Form,
            ast::QualForm::Shared);
  EXPECT_EQ(ast::cast<ast::QualifiedType>(localType(4))->getQualifier().Form,
            ast::QualForm::AtomicSharedConst);
  EXPECT_TRUE(ast::isa<ast::BuiltinType>(
      ast::cast<ast::AtomicType>(localType(6))->getInnerType()));
}

// Forms the parser takes and leaves to Sema: a qualifier after `view` or
// `inout` (a borrow carries no qualifier), the qualifier alone outside a
// local declaration (only a local infers its type), and on a `match` arm's
// binding or a destructuring target.
TEST(Qualifiers, FormsSemaJudges) {
  auto [ok, driver] = parse(R"(class C { n: shared; }
fn f(x: view shared C, y: inout const C, z: shared, w: const, v: int)
    -> view shared C { return x; }
fn main() -> int {
  j: view shared C = a;
  k: inout const = a;
  match a { n: shared C { } m: view const C { } _ { } }
  p: shared C, q = (1, 2);
  return 0;
})");
  ASSERT_TRUE(ok);
  const auto &params = driver->getRoot()->getFuncDecls()[0]->getParams();
  ASSERT_EQ(params.size(), 5u);
  EXPECT_EQ(params[0].Mode, ast::ParamMode::View);
  EXPECT_STREQ(qualifierOf(params[0].ParamType), "shared");
  EXPECT_EQ(params[1].Mode, ast::ParamMode::Inout);
  EXPECT_STREQ(qualifierOf(params[1].ParamType), "const");
  EXPECT_STREQ(qualifierOf(params[2].ParamType), "shared");
  EXPECT_EQ(ast::cast<ast::QualifiedType>(params[2].ParamType)->getInnerType(),
            nullptr);
  EXPECT_STREQ(qualifierOf(params[3].ParamType), "const");
  EXPECT_EQ(ast::cast<ast::QualifiedType>(params[3].ParamType)->getInnerType(),
            nullptr);
  const auto *fn = driver->getRoot()->getFuncDecls()[0];
  EXPECT_EQ(fn->getResultMode(), ast::ParamMode::View);
  EXPECT_STREQ(qualifierOf(fn->getReturnType()), "shared");
  const auto *field = driver->getRoot()->getClassDecls()[0]->getFields()[0];
  EXPECT_STREQ(qualifierOf(field->getType()), "shared");
  EXPECT_EQ(ast::cast<ast::QualifiedType>(field->getType())->getInnerType(),
            nullptr);
  std::string dump = dumpAST(*driver);
  for (const char *line :
       {"'f' view 'x' inout 'y' -> view\n",
        "VarDecl <5:3-5:23> 'j' view type\n",
        "QualifiedType <5:11-5:19> 'shared'\n",
        "VarDecl <6:3-6:21> 'k' inout type\n",
        "QualifiedType <6:12-6:17> 'const'\n", "MatchArm binding='n'\n",
        "MatchArm binding='m' view\n"})
    EXPECT_NE(dump.find(line), std::string::npos) << line << "\n" << dump;
}

// A type takes one qualifier, written before it; the qualifier alone is a
// local declaration's, and needs the initializer; `atomic` alone names no
// type; a qualifier is not an operator or a prefix of a name.
TEST(Qualifiers, OneQualifierBeforeTheType) {
  const char *const cases[][2] = {
      {"x: shared shared Node = a;",
       "a type takes one qualifier: 'shared shared' is two"},
      {"x: const shared Node = a;",
       "a type takes one qualifier: 'const shared' is two"},
      {"x: shared const const Node = a;",
       "a type takes one qualifier: 'shared const const' is two"},
      {"x: const locked Node = a;",
       "a type takes one qualifier: 'const locked' is two"},
      {"x: locked atomic int = a;",
       "a type takes one qualifier: 'locked atomic' is two"},
      {"x: shared atomic int = a;",
       "a type takes one qualifier: 'shared atomic' is two"},
      {"x: atomic shared const const Node = a;",
       "a type takes one qualifier: 'atomic shared const const' is two"},
      {"x: atomic shared const shared Node = a;",
       "a type takes one qualifier: 'atomic shared const shared' is two"},
      {"x: atomic = 1;", "unexpected '='; expected a type"},
      {"x: shared;", "unexpected ';'; expected '=' (a variable declaration "
                     "needs an initial value)"},
      {"x: locked = ;", "unexpected ';'; expected an expression"},
      {"x: shared = a, b;", "unexpected ','; expected ';' after the "
                            "declaration"},
      {"shared x = a;", "unexpected 'shared'; expected an expression"},
      {"let shared x = a;",
       "unexpected 'shared'; expected a variable name after 'let'"},
      {"x = C() as shared;", "unexpected 'as'; expected ';' after the "
                             "assignment"},
      {"x = shared C();", "unexpected 'shared'; expected an expression"},
  };
  for (const auto &c : cases)
    expectRejected(inMain(c[0]), c[1]);
  const char *const topLevel[][2] = {
      {"fn f(shared x: Node) { }",
       "unexpected 'shared'; expected a parameter name"},
      {"class C { shared n: Node; }",
       "unexpected 'shared'; expected a field ('name: Type;') or a method "
       "('fn') in the class body"},
      {"fn f() -> shared { }", "unexpected '{'; expected a type"},
      {"fn f() -> atomic { }", "unexpected '{'; expected a type"},
  };
  for (const auto &c : topLevel)
    expectRejected(atTopLevel(c[0]), c[1]);
}

// The frozen, thread-crossing object is `atomic shared const T`: one
// qualifier of three words, each saying one thing (`atomic`: the reference
// crosses threads; `shared`: it has several owners; `const`: nothing
// writes it).  A spelling that leaves a word out is rejected at `atomic`,
// wherever the type is written, with the full form to write; a fourth
// word is a second qualifier.
TEST(Qualifiers, AtomicSharedConstIsSpelledInFull) {
  auto [ok, driver] = parse(R"(class Node { id: int; }
fn f(n: atomic shared const Node) -> atomic shared const Node { return n; }
fn main() -> int {
  let a: atomic shared const Node = Node(1);
  b: atomic shared const = a;
  return 0;
})");
  ASSERT_TRUE(ok);
  // One QualifiedType, spelled `atomic shared const`, around the class (or
  // around nothing, in a local whose type is inferred).
  auto expectForm = [](const ast::Type *ty, bool inferred) {
    const auto *q = ast::dyn_cast<ast::QualifiedType>(ty);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->getQualifier().Form, ast::QualForm::AtomicSharedConst);
    EXPECT_STREQ(q->getQualifier().spelling(), "atomic shared const");
    if (inferred)
      EXPECT_EQ(q->getInnerType(), nullptr);
    else
      EXPECT_TRUE(ast::isa<ast::ClassType>(q->getInnerType()));
  };
  const auto &fns = driver->getRoot()->getFuncDecls();
  ASSERT_EQ(fns.size(), 2u);
  ASSERT_EQ(fns[0]->getParams().size(), 1u);
  expectForm(fns[0]->getParams()[0].ParamType, false);
  expectForm(fns[0]->getReturnType(), false);
  const auto &stmts = fns[1]->getBody()->getStatements();
  ASSERT_EQ(stmts.size(), 3u);
  auto localType = [&](size_t i) {
    return ast::cast<ast::VarDecl>(
               ast::cast<ast::DeclStmt>(stmts[i])->getDecl())
        ->getType();
  };
  expectForm(localType(0), false);
  expectForm(localType(1), true);
  std::string dump = dumpAST(*driver);
  for (const char *line :
       {"QualifiedType <2:38-2:62> 'atomic shared const'\n",
        "ClassType <2:58-2:62> 'Node'\n", "VarDecl <4:3-4:44> 'a' let type\n",
        "QualifiedType <4:10-4:34> 'atomic shared const'\n",
        "ClassType <4:30-4:34> 'Node'\n", "VarDecl <5:3-5:29> 'b' type\n",
        "QualifiedType <5:6-5:25> 'atomic shared const'\n"})
    EXPECT_NE(dump.find(line), std::string::npos) << line << "\n" << dump;

  // `atomic const` and `atomic shared` each leave a word out.
  const char *const kAtomicConst =
      "'atomic const' is 'atomic shared const': an object read from several "
      "threads has several owners; write 'atomic shared const T'";
  const char *const kAtomicShared =
      "'atomic shared T' would be written from several threads with no lock; "
      "write 'atomic shared const T' to freeze it, or 'locked T' to change "
      "it under a lock";
  for (const char *stmt :
       {"x: atomic const Node = a;", "let x: atomic const Node = a;",
        "x: atomic const = a;"})
    expectRejected(inMain(stmt), kAtomicConst);
  for (const char *stmt :
       {"x: atomic shared Node = a;", "let x: atomic shared Node = a;",
        "x: atomic shared = a;"})
    expectRejected(inMain(stmt), kAtomicShared);
  const char *const topLevel[][2] = {
      {"fn f(x: atomic const Node) { }", kAtomicConst},
      {"fn f(x: atomic shared Node) { }", kAtomicShared},
      {"fn f() -> atomic const Node { }", kAtomicConst},
      {"fn f() -> atomic shared Node { }", kAtomicShared},
      {"class C { n: atomic const Node; }", kAtomicConst},
      {"class C { n: atomic shared Node; }", kAtomicShared},
  };
  for (const auto &c : topLevel)
    expectRejected(atTopLevel(c[0]), c[1]);
  // A fourth word is a second qualifier (OneQualifierBeforeTheType).
  expectRejected(inMain("x: atomic shared const shared Node = a;"),
                 "a type takes one qualifier: 'atomic shared const shared' is "
                 "two");
  expectRejected(inMain("x: atomic shared const const Node = a;"),
                 "a type takes one qualifier: 'atomic shared const const' is "
                 "two");
}

// -- The keywords

// `move`, `async`, `await`, `parallel`, `atomic`, `shared`, `const`,
// `locked` and `lock` are reserved: none names a variable, a function, a
// parameter, a field, a class or a type.
TEST(ConcurrencyKeywords, AreNotIdentifiers) {
  const char *const keywords[] = {"move",     "async",  "await",
                                  "parallel", "atomic", "shared",
                                  "const",    "locked", "lock"};
  for (const char *kw : keywords) {
    const std::string k(kw);
    const std::string cases[] = {
        inMain((k + " = 1;").c_str()),
        inMain(("x = 1 + " + k + ";").c_str()),
        atTopLevel(("fn " + k + "() { }").c_str()),
        atTopLevel(("fn f(" + k + ": int) { }").c_str()),
        atTopLevel(("class C { " + k + ": int; }").c_str()),
        atTopLevel(("class " + k + " { n: int; }").c_str()),
        "class C { n: int; }\nfn main() -> int { return C(1)." + k + "; }",
    };
    for (const std::string &src : cases) {
      auto [ok, _] = parse(src);
      EXPECT_FALSE(ok) << src;
    }
  }
  // As a type: the qualifiers are one, and `atomic` wants the cell's type
  // (the Qualifiers tests); the rest never name a type.
  for (const char *kw : {"move", "async", "await", "parallel", "lock"})
    expectRejected(
        inMain(("x: " + std::string(kw) + " = 1;").c_str()),
        ("unexpected '" + std::string(kw) + "'; expected a type").c_str());

  const char *const cases[][2] = {
      {"fn main() -> int { shared = 1; return 0; }",
       "unexpected 'shared'; expected an expression"},
      {"fn main() -> int { x = 1 + const; return 0; }",
       "unexpected 'const'; expected an expression"},
      {"fn move() { }\nfn main() -> int { return 0; }",
       "unexpected 'move'; expected a function name after 'fn'"},
      {"fn f(await: int) { }\nfn main() -> int { return 0; }",
       "unexpected 'await'; expected a parameter name"},
      {"class C { atomic: int; }\nfn main() -> int { return 0; }",
       "unexpected 'atomic'; expected a field ('name: Type;') or a method "
       "('fn') in the class body"},
      {"class locked { n: int; }\nfn main() -> int { return 0; }",
       "unexpected 'locked'; expected a class name after 'class'"},
      {"class C { n: int; }\nfn main() -> int { return C(1).lock; }",
       "unexpected 'lock'; expected a field or method name after '.'"},
      // `parallel` and `lock` start a statement, `await` and `move` an
      // expression: the error is at what follows them.
      {"fn main() -> int { parallel = 1; return 0; }",
       "unexpected '='; expected 'for' after 'parallel'"},
      {"fn main() -> int { lock = 1; return 0; }",
       "unexpected '='; expected an expression"},
      {"fn main() -> int { await = 1; return 0; }",
       "unexpected '='; expected an expression"},
  };
  for (const auto &c : cases)
    expectRejected(c[0], c[1]);
}

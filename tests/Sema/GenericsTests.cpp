// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: generic classes and functions (monomorphisation prototype).

#include "TestUtils.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace paykan::ast;
using namespace paykan::test;

namespace {

const char *kBox = R"(
  class Box<T> {
    v: T;
    fn __init__(v: T) { self.v = v; }
    fn get() -> T { return self.v; }
    fn set(v: T) { self.v = v; }
  }
)";

std::string withMain(const std::string &decls, const std::string &body) {
  return decls + "\nfn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

bool has(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

/// Parse + Sema, keeping the driver alive so the ASTContext can be inspected.
struct CheckedProgram {
  std::unique_ptr<paykan::parser::ParserDriver> Driver;
  bool Ok = false;
  std::string Diagnostics;
};

CheckedProgram check(const std::string &source) {
  CheckedProgram r;
  auto [parseOk, driver] = parse(source);
  r.Driver = std::move(driver);
  if (!parseOk) {
    r.Diagnostics = "parse error";
    return r;
  }
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  diag.setSourceInfo(r.Driver->getCurrentFile(), &r.Driver->getSourceLines());
  paykan::sema::Sema sema(r.Driver->getASTContext(), diag, "");
  r.Ok = (bool)sema.run(r.Driver->getRoot());
  r.Diagnostics = os.str();
  return r;
}

} // namespace

// ============================================================================
// Instantiation identity and registration
// ============================================================================

// Two uses of `Box<int>` resolve to one ClassType (cached per canonical
// argument tuple, like Array<T>), and the annotation slots are rewritten to it.
TEST(Generics, SameInstantiationIsSameClassType) {
  auto r = check(withMain(std::string(kBox) + R"(
    class Holder { b: Box<int>; fn __init__() { self.b = Box<int>(1); } }
    fn take(a: Box<int>, b: Box<int>) -> Box<int> { return a; }
  )",
                          "x: Box<int> = Box<int>(2); take(x, x);"));
  ASSERT_TRUE(r.Ok) << r.Diagnostics;
  auto &ctx = r.Driver->getASTContext();
  ClassType *inst = ctx.lookupClassType("Box<int>");
  ASSERT_NE(inst, nullptr);
  EXPECT_EQ(inst->getName(), "Box<int>");
  // The template itself is not a class.
  EXPECT_EQ(ctx.lookupClassType("Box"), nullptr);

  auto *tu = r.Driver->getRoot();
  FuncDecl *take = nullptr;
  for (auto *fn : tu->getFuncDecls())
    if (fn->getName() == "take")
      take = fn;
  ASSERT_NE(take, nullptr);
  EXPECT_EQ(take->getParams()[0].ParamType, inst);
  EXPECT_EQ(take->getParams()[1].ParamType, inst);
  EXPECT_EQ(take->getReturnType(), inst);
  // Field of instantiation type.
  ClassType *holder = ctx.lookupClassType("Holder");
  ASSERT_NE(holder, nullptr);
  ASSERT_EQ(holder->getFields().size(), 1u);
  EXPECT_EQ(holder->getFields()[0].second, inst);
  // The instantiation is a real class: fields, methods, constructor.
  ASSERT_EQ(inst->getFields().size(), 1u);
  EXPECT_EQ(inst->getFields()[0].second, ctx.getIntTy());
  ASSERT_NE(inst->findMethod("get"), nullptr);
  EXPECT_EQ(inst->findMethod("get")->getReturnType(), ctx.getIntTy());
  ASSERT_NE(inst->findMethod("__init__"), nullptr);
  // Only one instantiation exists in the registry (no per-use duplicates).
  unsigned count = 0;
  for (auto &[name, ct] : ctx.getClassTypes())
    if (ct->getName() == "Box<int>")
      ++count;
  EXPECT_EQ(count, 1u);
}

// Instantiations become ordinary declarations of the TU (for CodeGen), while
// the template stays in its own list.
TEST(Generics, InstantiationsAreInjectedIntoTU) {
  auto r = check(withMain(std::string(kBox) + R"(
    fn first<T>(xs: T[]) -> T { return xs[0]; }
  )",
                          "a = Box<int>(1); b = Box<Str>(\"s\"); "
                          "c = first([1]); d = first<Str>([\"x\"]);"));
  ASSERT_TRUE(r.Ok) << r.Diagnostics;
  auto *tu = r.Driver->getRoot();
  std::vector<std::string> classes, funcs;
  for (auto *cd : tu->getClassDecls())
    classes.push_back(cd->getName());
  for (auto *fn : tu->getFuncDecls())
    funcs.push_back(fn->getName());
  // Instantiations are appended after the hand-written classes (none here)
  // in creation order.
  EXPECT_EQ(classes, (std::vector<std::string>{"Box<int>", "Box<Str>"}));
  EXPECT_EQ(funcs,
            (std::vector<std::string>{"main", "first<int>", "first<Str>"}));
  EXPECT_EQ(tu->getGenericClassDecls().size(), 1u);
  EXPECT_EQ(tu->getGenericFuncDecls().size(), 1u);
}

// Distinct argument tuples are distinct, unrelated classes.
TEST(Generics, BoxIntNotAssignableToBoxStr) {
  auto r = semaCheck(withMain(kBox, "a: Box<int> = Box<Str>(\"x\");"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics, "initializer of type 'Box<Str>' does not "
                                 "match declared type 'Box<int>'"))
      << r.Diagnostics;
}

TEST(Generics, InstantiationAsFieldParamReturnAndArray) {
  auto r = semaCheck(withMain(std::string(kBox) + R"(
    class Pair<K, V> {
      k: K; v: V;
      fn __init__(k: K, v: V) { self.k = k; self.v = v; }
      fn key() -> K { return self.k; }
    }
    class Holder {
      b: Box<int>;
      ps: Pair<Str, int>[];
      fn __init__() { self.b = Box<int>(1); self.ps = []; }
      fn get() -> Box<int> { return self.b; }
    }
    fn unwrap(b: Box<int>) -> int { return b.get(); }
    fn nest(b: Box<Box<int>>) -> int { return b.get().get(); }
  )",
                              R"(
    h: Holder = Holder();
    n: int = unwrap(h.get());
    m: int = nest(Box<Box<int>>(h.get()));
    arr: Box<int>[] = [Box<int>(1), Box<int>(2)];
    k: Str = Pair<Str, int>("a", 1).key();
    ba: Box<int[]> = Box<int[]>([1, 2]);
    xs: int[] = ba.get();
    h.ps.push(Pair<Str, int>("b", 2));
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Generics, MatchOnInstantiationArms) {
  auto r = semaCheck(withMain(std::string(kBox) + R"(
    fn describe(o: Obj) -> Str {
      match o {
        b: Box<int> { return StrInt(b.get()); }
        Box<Str> { return "str"; }
        _ { return "other"; }
      }
    }
  )",
                              "describe(Box<int>(1));"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Generics, MovOfInstantiation) {
  auto r = semaCheck(withMain(kBox, R"(
    a: Box<int> = Box<int>(1);
    b: Box<int> = mov a;
    c: int = a.get();
  )"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics, "use of moved variable 'a'")) << r.Diagnostics;
}

TEST(Generics, RecursiveInstantiationInFieldIsFine) {
  auto r = semaCheck(withMain(R"(
    class Node<T> {
      v: T;
      next: Obj;
      fn __init__(v: T) { self.v = v; self.next = None; }
      fn link(n: Node<T>) { self.next = n; }
    }
  )",
                              "a = Node<int>(1); a.link(Node<int>(2));"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Generics, GenericClassWithConcreteSuperclass) {
  auto r = semaCheck(withMain(R"(
    class Base { n: int; fn __init__(n: int) { self.n = n; } fn num() -> int { return self.n; } }
    class Wrap<T> : Base {
      v: T;
      fn __init__(v: T, n: int) { __super__(n); self.v = v; }
      fn get() -> T { return self.v; }
    }
  )",
                              "w: Wrap<Str> = Wrap<Str>(\"a\", 5); "
                              "x: int = w.num(); b: Base = w;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A template whose body is never instantiated is never checked (C++-style).
TEST(Generics, UninstantiatedTemplateBodyIsNotChecked) {
  auto r = semaCheck(withMain(R"(
    class Broken<T> {
      v: T;
      fn __init__(v: T) { self.v = v; }
      fn bad() -> int { return undefined_name; }
    }
  )",
                              ""));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// ============================================================================
// Diagnostics
// ============================================================================

TEST(Generics, ArityErrorOnType) {
  auto r = semaCheck(withMain(kBox, "b: Box<int, Str> = Box<int>(1);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics,
                  "generic class 'Box' expects 1 type argument(s), got 2"))
      << r.Diagnostics;
}

TEST(Generics, ArityErrorOnCall) {
  auto r = semaCheck(withMain(kBox, "b = Box<int, Str>(1);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics,
                  "generic class 'Box' expects 1 type argument(s), got 2"))
      << r.Diagnostics;

  auto r2 = semaCheck(
      withMain("fn id<T>(x: T) -> T { return x; }", "id<int, int>(1);"));
  EXPECT_FALSE(r2.Ok);
  EXPECT_TRUE(has(r2.Diagnostics,
                  "generic function 'id' expects 1 type argument(s), got 2"))
      << r2.Diagnostics;
}

TEST(Generics, UnknownTemplate) {
  auto r = semaCheck(withMain("", "b: Nope<int> = None;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics, "unknown generic class 'Nope'"))
      << r.Diagnostics;
}

TEST(Generics, GenericClassUsedWithoutTypeArgs) {
  auto r = semaCheck(withMain(kBox, "b: Box = Box<int>(1);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics, "names generic class 'Box' without type "
                                 "arguments (write 'Box<...>')"))
      << r.Diagnostics;
}

TEST(Generics, TypeArgsOnNonGeneric) {
  auto r =
      semaCheck(withMain("fn f(x: int) -> int { return x; }", "f<int>(1);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics, "'f' is not generic and takes no type "
                                 "arguments"))
      << r.Diagnostics;

  auto r2 = semaCheck(withMain("", "s: Str<int> = \"x\";"));
  EXPECT_FALSE(r2.Ok);
  EXPECT_TRUE(has(r2.Diagnostics, "'Str' is not a generic class"))
      << r2.Diagnostics;
}

// An error inside an instantiation is reported at the template's source and
// says which instantiation was being checked.
TEST(Generics, ErrorInsideInstantiationNamesIt) {
  auto r = semaCheck(withMain(R"(
    class Adder<T> {
      v: T;
      fn __init__(v: T) { self.v = v; }
      fn twice() -> T { return self.v + self.v; }
    }
  )",
                              "a = Adder<bool>(True);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics,
                  "operator '+' is not defined for types 'bool' and 'bool'"))
      << r.Diagnostics;
  EXPECT_TRUE(
      has(r.Diagnostics, "in instantiation of 'Adder<bool>' requested here"))
      << r.Diagnostics;
  // The same template instantiated with int is fine.
  auto r2 = semaCheck(withMain(R"(
    class Adder<T> {
      v: T;
      fn __init__(v: T) { self.v = v; }
      fn twice() -> T { return self.v + self.v; }
    }
  )",
                               "a = Adder<int>(1); n: int = a.twice();"));
  EXPECT_TRUE(r2.Ok) << r2.Diagnostics;
}

TEST(Generics, ErrorInsideFunctionInstantiationNamesIt) {
  auto r =
      semaCheck(withMain("fn neg<T>(x: T) -> T { return -x; }", "neg(True);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(
      has(r.Diagnostics, "in instantiation of 'neg<bool>' requested here"))
      << r.Diagnostics;
}

TEST(Generics, TypeParamUsedAsValue) {
  auto r =
      semaCheck(withMain("fn f<T>(x: T) -> T { y = T; return x; }", "f(1);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(
      has(r.Diagnostics, "type parameter 'T' cannot be used as a value"))
      << r.Diagnostics;

  auto r2 = semaCheck(withMain("fn f<T>(x: T) -> T { return T(); }", "f(1);"));
  EXPECT_FALSE(r2.Ok);
  EXPECT_TRUE(
      has(r2.Diagnostics, "type parameter 'T' cannot be used as a value"))
      << r2.Diagnostics;
}

TEST(Generics, DuplicateTypeParam) {
  auto r = semaCheck(withMain(
      "class P<T, T> { a: T; fn __init__(a: T) { self.a = a; } }", ""));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(
      has(r.Diagnostics, "duplicate type parameter 'T' in generic class 'P'"))
      << r.Diagnostics;
}

TEST(Generics, TypeParamShadowsType) {
  auto r = semaCheck(withMain(R"(
    class Foo {}
    class Bar<Foo> { x: Foo; fn __init__(x: Foo) { self.x = x; } }
  )",
                              ""));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics, "type parameter 'Foo' of generic class 'Bar' "
                                 "shadows a type of the same name"))
      << r.Diagnostics;

  auto r2 = semaCheck(withMain("fn baz<Str>(s: Str) -> Str { return s; }", ""));
  EXPECT_FALSE(r2.Ok);
  EXPECT_TRUE(has(r2.Diagnostics, "type parameter 'Str' of generic function "
                                  "'baz' shadows a type of the same name"))
      << r2.Diagnostics;
}

TEST(Generics, TemplateNameCollisions) {
  auto r = semaCheck(withMain(std::string(kBox) + "\nclass Box {}", ""));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(
      has(r.Diagnostics, "'Box' is already declared as a generic class"))
      << r.Diagnostics;

  auto r2 = semaCheck(withMain(
      "fn f<T>(x: T) -> T { return x; }\nfn f(x: int) -> int { return x; }",
      ""));
  EXPECT_FALSE(r2.Ok);
  EXPECT_TRUE(
      has(r2.Diagnostics, "'f' is already declared as a generic function"))
      << r2.Diagnostics;

  auto r3 = semaCheck(withMain("class print<T> {}", ""));
  EXPECT_FALSE(r3.Ok);
  EXPECT_TRUE(has(r3.Diagnostics, "builtin function")) << r3.Diagnostics;
}

TEST(Generics, InstantiationDepthGuard) {
  auto r = semaCheck(
      withMain("class Bad<T> { inner: Bad<Bad<T>>; fn __init__() {} }",
               "b: Bad<int> = Bad<int>();"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics, "exceeds the maximum instantiation depth"))
      << r.Diagnostics;
}

TEST(Generics, WrongConstructorArgumentNamesInstantiation) {
  auto r = semaCheck(withMain(kBox, "b: Box<int> = Box<int>(\"s\");"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics,
                  "argument 1 of 'Box<int>' has type 'Str', expected 'int'"))
      << r.Diagnostics;
}

// ============================================================================
// Type-argument inference for generic functions
// ============================================================================

TEST(Generics, InferenceFromPlainArrayAndBoxParams) {
  auto r = semaCheck(withMain(std::string(kBox) + R"(
    fn first<T>(xs: T[]) -> T { return xs[0]; }
    fn unbox<T>(b: Box<T>) -> T { return b.get(); }
    fn pick<A, B>(a: A, b: B) -> B { return b; }
  )",
                              R"(
    i: int = first([1, 2]);
    s: Str = first(["a"]);
    n: int = unbox(Box<int>(4));
    t: Str = unbox(Box<Str>("s"));
    f: float = pick("x", 1.5);
    e: int = first<int>([3]);
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Generics, InferenceFailsWithoutArguments) {
  auto r = semaCheck(withMain("fn mk<T>() -> int { return 1; }", "mk();"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics, "cannot infer type parameter 'T' of 'mk' "
                                 "from the call arguments"))
      << r.Diagnostics;
  EXPECT_TRUE(has(r.Diagnostics, "specify the type arguments explicitly"))
      << r.Diagnostics;
  // Explicit arguments fix it.
  auto r2 =
      semaCheck(withMain("fn mk<T>() -> int { return 1; }", "mk<int>();"));
  EXPECT_TRUE(r2.Ok) << r2.Diagnostics;
}

TEST(Generics, InferenceAmbiguous) {
  auto r = semaCheck(
      withMain("fn pick<T>(a: T, b: T) -> T { return a; }", "pick(1, 2.0);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics, "cannot infer type parameter 'T' of 'pick': "
                                 "deduced as both 'int' and 'float'"))
      << r.Diagnostics;
  // Explicit arguments resolve the ambiguity (int promotes to float).
  auto r2 = semaCheck(withMain("fn pick<T>(a: T, b: T) -> T { return a; }",
                               "f: float = pick<float>(1, 2.0);"));
  EXPECT_TRUE(r2.Ok) << r2.Diagnostics;
}

TEST(Generics, InferenceFromEmptyArrayLiteralFails) {
  auto r = semaCheck(
      withMain("fn first<T>(xs: T[]) -> T { return xs[0]; }", "first([]);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics, "cannot infer type parameter 'T'"))
      << r.Diagnostics;
}

TEST(Generics, ConstructorArgumentInference) {
  auto r = semaCheck(withMain(kBox, "b = Box(5); n: int = b.get(); "
                                    "c: Box<Str> = Box(\"s\");"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  auto r2 = semaCheck(withMain("class E<T> { fn __init__() {} }", "e = E();"));
  EXPECT_FALSE(r2.Ok);
  EXPECT_TRUE(has(r2.Diagnostics, "cannot infer type parameter 'T' of 'E'"))
      << r2.Diagnostics;
}

// ============================================================================
// Imports
// ============================================================================

namespace {
std::string writeProjectFile(const std::filesystem::path &dir,
                             const std::string &rel, const std::string &src) {
  auto full = dir / rel;
  std::filesystem::create_directories(full.parent_path());
  std::ofstream ofs(full);
  ofs << src;
  return full.string();
}

SemaResult semaCheckFile(const std::string &path) {
  paykan::parser::ParserDriver driver(paykan::test::testFrontend());
  if (driver.parseFile(path) != 0)
    return {false, "parse error", 1};
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  diag.setSourceInfo(driver.getCurrentFile(), &driver.getSourceLines());
  paykan::sema::Sema sema(driver.getASTContext(), diag,
                          std::filesystem::path(path).parent_path().string(),
                          driver.getFrontendName());
  auto ctx = sema.run(driver.getRoot());
  return {ctx.Ok, os.str(), ctx.ErrorCount};
}

const char *kGenLib = R"(
class Box<T> { v: T; fn __init__(v: T) { self.v = v; } fn get() -> T { return self.v; } }
fn mk() -> Box<int> { return Box<int>(41); }
fn first<T>(xs: T[]) -> T { return xs[0]; }
)";
} // namespace

TEST(Generics, ImportedTemplatesAreRejected) {
  auto dir = std::filesystem::temp_directory_path() / "pkn_sema_generics_imp";
  std::filesystem::remove_all(dir);
  writeProjectFile(dir, "lib/gen.pkn", kGenLib);

  // Type position.
  auto p1 = writeProjectFile(dir, "t1.pkn", R"(
import lib::gen;
fn main() -> int { b: gen::Box<int> = gen::mk(); return 0; }
)");
  auto r1 = semaCheckFile(p1);
  EXPECT_FALSE(r1.Ok);
  EXPECT_TRUE(has(r1.Diagnostics, "generic types cannot be imported yet"))
      << r1.Diagnostics;

  // Constructor call.
  auto p2 = writeProjectFile(dir, "t2.pkn", R"(
import lib::gen;
fn main() -> int { b = gen::Box<int>(1); return 0; }
)");
  auto r2 = semaCheckFile(p2);
  EXPECT_FALSE(r2.Ok);
  EXPECT_TRUE(has(r2.Diagnostics, "cannot be imported yet")) << r2.Diagnostics;

  // Generic function call.
  auto p3 = writeProjectFile(dir, "t3.pkn", R"(
import lib::gen;
fn main() -> int { x: int = gen::first<int>([1]); return 0; }
)");
  auto r3 = semaCheckFile(p3);
  EXPECT_FALSE(r3.Ok);
  EXPECT_TRUE(has(r3.Diagnostics, "cannot be imported yet")) << r3.Diagnostics;

  std::filesystem::remove_all(dir);
}

// A module's own instantiation is exported as a concrete class, so a value of
// that type returned by the module can be used by the importer …
TEST(Generics, ExportedInstantiationIsUsableAsConcreteClass) {
  auto dir = std::filesystem::temp_directory_path() / "pkn_sema_generics_exp";
  std::filesystem::remove_all(dir);
  writeProjectFile(dir, "lib/gen.pkn", kGenLib);
  auto p = writeProjectFile(dir, "main.pkn", R"(
import lib::gen;
fn main() -> int { b = gen::mk(); n: int = b.get(); return n; }
)");
  auto r = semaCheckFile(p);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  // … but the importer's own `Box<int>` would clash with it by name.
  auto p2 = writeProjectFile(dir, "clash.pkn", R"(
import lib::gen;
class Box<T> { v: T; fn __init__(v: T) { self.v = v; } }
fn main() -> int { b = Box<int>(1); return 0; }
)");
  auto r2 = semaCheckFile(p2);
  EXPECT_FALSE(r2.Ok);
  EXPECT_TRUE(has(r2.Diagnostics, "instantiation 'Box<int>' conflicts with an "
                                  "imported class of the same name"))
      << r2.Diagnostics;
  std::filesystem::remove_all(dir);
}

// ============================================================================
// Interaction with tuples (#4) and optionals (#5)
// ============================================================================

// Tuple and optional types in template signatures and bodies are substituted
// per instantiation (tuple literals, `.N`, destructuring, `T?`, match arms).
TEST(GenericsTypes, TupleAndOptionalBodiesInstantiate) {
  auto r = semaCheck(R"(
    class Node { v: int; fn __init__(x: int) { self.v = x; } }
    class Pair<A, B> {
      p: (A, B);
      fn __init__(a: A, b: B) { self.p = (a, b); }
      fn first() -> A { x, _ = self.p; return x; }
      fn swap() -> (B, A) { return (self.p.1, self.p.0); }
    }
    class Slot<T> {
      v: T?;
      fn __init__() { }
      fn get() -> T? { return self.v; }
    }
    fn orElse<T>(x: T?, d: T) -> T {
      match x {
        v: T { return v; }
        None { return d; }
      }
    }
    fn main() -> int {
      q = Pair<Str, int>("a", 1);
      s: (int, Str) = q.swap();
      k: Str = q.first();
      n: Node = orElse(Slot<Node>().get(), Node(0));
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(GenericsTypes, InferenceThroughTuplesAndOptionals) {
  auto r = semaCheck(R"(
    class Node { v: int; fn __init__(x: int) { self.v = x; } }
    fn firstOf<A, B>(p: (A, B)) -> A { return p.0; }
    fn unwrapOr<T>(x: T?, d: T) -> T {
      match x {
        v: T { return v; }
        _    { return d; }
      }
    }
    fn main() -> int {
      i: int = firstOf((1, "x"));
      s: Str = firstOf(("s", 2.5));
      m: Node? = Node(1);
      a: Node = unwrapOr(m, Node(2));       // T? against Node?
      b: Node = unwrapOr(None, Node(3));    // None carries no information
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(GenericsTypes, NoneAloneCannotInferOptionalParam) {
  auto r = semaCheck(R"(
    fn wrap<T>(x: T?) -> T? { return x; }
    fn main() -> int { w = wrap(None); return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics,
                  "cannot infer type parameter 'T' of 'wrap' from the call "
                  "arguments"))
      << r.Diagnostics;
}

TEST(GenericsTypes, TupleArityMismatchLeftToArgumentCheck) {
  auto r = semaCheck(R"(
    fn firstOf<A, B>(p: (A, B)) -> A { return p.0; }
    fn main() -> int { x = firstOf<int, Str>((1, "a", 2)); return 0; }
  )");
  EXPECT_FALSE(r.Ok);
}

// A `T?` inside a template is checked per instantiation: with T = int it is
// the usual optional-primitive error, attributed to the instantiation.
TEST(GenericsTypes, OptionalOfPrimitiveArgumentIsDiagnosed) {
  auto r = semaCheck(R"(
    class Slot<T> { v: T?; fn __init__() { } }
    fn main() -> int { b = Slot<int>(); return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics, "has type 'int?': optional primitive types "
                                 "are not supported yet"))
      << r.Diagnostics;
  EXPECT_TRUE(has(r.Diagnostics, "in instantiation of 'Slot<int>'"))
      << r.Diagnostics;

  // Likewise an optional tuple (not supported yet, see #15).
  auto r2 = semaCheck(R"(
    class Slot<T> { v: T?; fn __init__() { } }
    fn main() -> int { b = Slot<(int, Str)>(); return 0; }
  )");
  EXPECT_FALSE(r2.Ok);
  EXPECT_TRUE(has(r2.Diagnostics, "optional tuple types are not supported yet"))
      << r2.Diagnostics;
  EXPECT_TRUE(has(r2.Diagnostics, "in instantiation of 'Slot<(int, Str)>'"))
      << r2.Diagnostics;
}

TEST(GenericsTypes, TupleAndOptionalTypeArgs) {
  auto r = semaCheck(
      withMain(std::string(kBox) + "class Node { fn __init__() { } }\n",
               R"(
    a: Box<(int, Str)> = Box<(int, Str)>((1, "a"));
    t: (int, Str) = a.get();
    n: Node? = None;
    b: Box<Node?> = Box<Node?>(n);
    c: Box<Node?[]> = Box<Node?[]>([]);
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// Exported instantiations whose names contain ',' and '[]' round-trip: the
// import type parser keeps `Pair<Str, int>` / `Box<int[]>` together as names.
TEST(GenericsTypes, ExportedInstantiationNamesWithCommasAndArrays) {
  auto dir = std::filesystem::temp_directory_path() / "pkn_sema_generics_names";
  std::filesystem::remove_all(dir);
  writeProjectFile(dir, "lib.pkn", R"(
class Pair<A, B> { a: A; b: B; fn __init__(a: A, b: B) { self.a = a; self.b = b; } fn first() -> A { return self.a; } }
class Box<T> { v: T; fn __init__(v: T) { self.v = v; } fn get() -> T { return self.v; } }
fn mkPair() -> Pair<Str, int> { return Pair<Str, int>("k", 1); }
fn mkBoxArr() -> Box<int[]> { return Box<int[]>([1, 2, 3]); }
fn mkBoxes() -> (Pair<Str, int>, Box<int[]>?) { b: Box<int[]>? = None; return (mkPair(), b); }
)");
  auto p = writeProjectFile(dir, "main.pkn", R"(
import lib;
fn main() -> int {
  s: Str = lib::mkPair().first();
  n: int = lib::mkBoxArr().get().len();
  x, y = lib::mkBoxes();
  k: Str = x.first();
  return n;
}
)");
  auto r = semaCheckFile(p);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(dir);
}

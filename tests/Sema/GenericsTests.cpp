// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: generic classes and functions (monomorphisation).

#include "TestUtils.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace paykan::ast;
using namespace paykan::test;

namespace {

const char *kBox = R"(
  class Box<T> {
    v: T;
    fn get() -> T { return self.v; }
    fn set(v: T) { self.v = v; }
  }

  impl Box<T> {
    fn __init__(v: T) { self.v = v; }
  }
)";

std::string withMain(const std::string &decls, const std::string &body) {
  return decls + "import ::io; \nfn main() -> int {\n" + body +
         "\n  return 0;\n}\n";
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

// -- Instantiation identity and registration

// Two uses of `Box<int>` resolve to one ClassType (cached per canonical
// argument tuple), and the annotation slots are rewritten to it.
TEST(Generics, SameInstantiationIsSameClassType) {
  auto r = check(withMain(std::string(kBox) + R"(
    class Holder { b: Box<int>; }
    impl Holder { fn __init__() { self.b = Box<int>(1); } }
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
    fn first<T>(xs: view T[]) -> view T { return xs[0]; }
  )",
                          "a = Box<int>(1); b = Box<Str>(\"s\"); "
                          "c = first([1]); d = first<Str>([\"x\"]).len();"));
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
      fn key() -> K { return self.k; }
    }

    impl Pair<K, V> {
      fn __init__(k: K, v: V) { self.k = k; self.v = v; }
    }
    class Holder {
      b: Box<int>;
      ps: Pair<Str, int>[2];
      fn get() -> Box<int> { return self.b; }
    }

    impl Holder {
      fn __init__() {
        self.b = Box<int>(1);
        self.ps = [Pair<Str, int>("a", 1), Pair<Str, int>("b", 2)];
      }
    }
    fn unwrap(b: Box<int>) -> int { return b.get(); }
    fn nest(b: Box<Box<int>>) -> int { return b.get().get(); }
    fn total(bs: view Box<int>[]) -> int { return bs[0].v + bs[1].v; }
  )",
                              R"(
    h: Holder = Holder();
    n: int = unwrap(h.get());
    m: int = nest(Box<Box<int>>(h.get()));
    arr: Box<int>[2] = [Box<int>(1), Box<int>(2)];
    k: Str = Pair<Str, int>("a", 1).key();
    h.ps[1] = Pair<Str, int>("c", 3);
    j: int = total(arr) + h.ps[0].v;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Generics, MatchOnInstantiationArms) {
  auto r = semaCheck(withMain(std::string(kBox) + R"(
    fn describe(o: Obj) -> Str {
      match o {
        b: Box<int> { return Str<int>(b.get()); }
        Box<Str> { return "str"; }
        _ { return "other"; }
      }
    }
  )",
                              "describe(Box<int>(1));"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Generics, RecursiveInstantiationInFieldIsFine) {
  auto r = semaCheck(withMain(R"(
    class Node<T> {
      v: T;
      next: Obj;
      fn link(n: Node<T>) { self.next = n; }
    }

    impl Node<T> {
      fn __init__(v: T) { self.v = v; self.next = None; }
    }
  )",
                              "a = Node<int>(1); a.link(Node<int>(2));"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Generics, GenericClassWithConcreteSuperclass) {
  auto r = semaCheck(withMain(R"(
    class Base { n: int; fn num() -> int { return self.n; } }
    impl Base { fn __init__(n: int) { self.n = n; } }
    class Wrap<T> : Base {
      v: T;
      fn get() -> T { return self.v; }
    }

    impl Wrap<T> {
      fn __init__(v: T, n: int) { __super__(n); self.v = v; }
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
      fn bad() -> int { return undefined_name; }
    }

    impl Broken<T> {
      fn __init__(v: T) { self.v = v; }
    }
  )",
                              ""));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Diagnostics

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
      fn twice() -> T { return self.v + self.v; }
    }

    impl Adder<T> {
      fn __init__(v: T) { self.v = v; }
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
      fn twice() -> T { return self.v + self.v; }
    }

    impl Adder<T> {
      fn __init__(v: T) { self.v = v; }
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
  auto r =
      semaCheck(withMain("class P<T, T> { a: T; }\n"
                         "impl P<T, T> { fn __init__(a: T) { self.a = a; } }",
                         ""));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(
      has(r.Diagnostics, "duplicate type parameter 'T' in generic class 'P'"))
      << r.Diagnostics;
}

TEST(Generics, TypeParamShadowsType) {
  auto r = semaCheck(withMain(R"(
    class Foo {}
    class Bar<Foo> { x: Foo; }
    impl Bar<Foo> { fn __init__(x: Foo) { self.x = x; } }
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

  auto r3 = semaCheck(withMain("class Str<T> {}", ""));
  EXPECT_FALSE(r3.Ok);
  EXPECT_TRUE(has(r3.Diagnostics, "builtin class")) << r3.Diagnostics;
}

TEST(Generics, InstantiationDepthGuard) {
  auto r = semaCheck(withMain("class Bad<T> { inner: Bad<Bad<T>>; }\n"
                              "impl Bad<T> { fn __init__() {} }",
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

// -- Type-argument inference for generic functions

TEST(Generics, InferenceFromSliceAndBoxParams) {
  auto r = semaCheck(withMain(std::string(kBox) + R"(
    fn first<T>(xs: view T[]) -> view T { return xs[0]; }
    fn unbox<T>(b: Box<T>) -> T { return b.get(); }
    fn pick<A, B>(a: A, b: B) -> B { return b; }
  )",
                              R"(
    i: int = first([1, 2]);
    s: int = first(["a"]).len();
    n: int = unbox(Box<int>(4));
    t: Str = unbox(Box<Str>("s"));
    f: float = pick("x", 1.5);
    e: int = first<int>([3]);
    fs: float[2] = [1.5, 2];
    g: float = first(fs);
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

TEST(Generics, ConstructorArgumentInference) {
  auto r = semaCheck(withMain(kBox, "b = Box(5); n: int = b.get(); "
                                    "c: Box<Str> = Box(\"s\");"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  auto r2 = semaCheck(withMain("class E<T> {}\n"
                               "impl E<T> { fn __init__() {} }",
                               "e = E();"));
  EXPECT_FALSE(r2.Ok);
  EXPECT_TRUE(has(r2.Diagnostics, "cannot infer type parameter 'T' of 'E'"))
      << r2.Diagnostics;
}

// -- Imports

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
class Box<T> { v: T; view fn get() -> view T { return self.v; } }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
fn mk() -> Box<int> { return Box<int>(41); }
fn first<T>(xs: view T[]) -> view T { return xs[0]; }
)";
} // namespace

// Another module's generic classes and functions are instantiated in the
// importer (docs/language/13-generics.md), in every position a local
// template may be used.
TEST(Generics, ImportedTemplates) {
  auto dir = paykan::test::tempDir() / "pkn_sema_generics_imp";
  std::filesystem::remove_all(dir);
  writeProjectFile(dir, "lib/gen.pkn", kGenLib);
  const std::vector<std::pair<std::string, std::string>> cases = {
      // Type position, holding the module's own Box<int>.
      {"t1", "import lib::gen;\n"
             "fn main() -> int { b: gen::Box<int> = gen::mk(); "
             "return b.get(); }"},
      // Constructor call, explicit and inferred.
      {"t2", "import lib::gen;\n"
             "fn main() -> int { b = gen::Box<int>(1); c = gen::Box(\"s\"); "
             "return b.get(); }"},
      // Generic function, explicit and inferred.
      {"t3", "import lib::gen;\n"
             "fn main() -> int { x: int = gen::first<int>([1]); "
             "return x + gen::first([2]); }"},
      // Aliased import and the full module path.
      {"t4", "import lib::gen as g;\n"
             "fn main() -> int { b: lib::gen::Box<int> = g::Box<int>(1); "
             "return b.get(); }"},
      // Inside this module's own templates.
      {"t5", "import lib::gen;\n"
             "fn wrap<T>(x: T) -> gen::Box<T> { return gen::Box<T>(x); }\n"
             "fn head<T>(xs: view T[]) -> view T { "
             "return gen::first<T>(xs); }\n"
             "class W<T> { b: gen::Box<T>; }\n"
             "impl W<T> { fn __init__(x: T) { "
             "self.b = gen::Box<T>(x); } }\n"
             "fn main() -> int { w = W<int>(1); s = W<Str>(\"a\"); "
             "t: int = head([\"t\"]).len(); "
             "return wrap(2).get() + head([1]); }"},
  };
  for (const auto &[name, src] : cases) {
    auto r = semaCheckFile(writeProjectFile(dir, name + ".pkn", src));
    EXPECT_TRUE(r.Ok) << name << ":\n" << r.Diagnostics;
    EXPECT_EQ(r.Diagnostics, "") << name;
  }
  std::filesystem::remove_all(dir);
}

// A template's body names what its module declares and imports, whatever
// the importer binds those names to.
TEST(Generics, ImportedTemplateBodiesUseTheirModule) {
  auto dir = paykan::test::tempDir() / "pkn_sema_generics_imp_body";
  std::filesystem::remove_all(dir);
  writeProjectFile(dir, "lib/util.pkn", "fn ten() -> int { return 10; }\n");
  writeProjectFile(dir, "other.pkn", "fn ten() -> Str { return \"x\"; }\n");
  writeProjectFile(dir, "lib/gen.pkn", R"(
import lib::util as u;
class Inner { n: int; }
impl Inner { fn __init__() { self.n = _helper(); } }
class Box<T> {
  v: T;
  view fn total() -> int { i = Inner(); return i.n + u::ten(); }
}

impl Box<T> {
  fn __init__(v: T) { self.v = v; }
}
fn _helper() -> int { return 1; }
fn sum<T>(b: Box<T>) -> int { return b.total() + _helper(); }
)");
  // `u` and `_helper` are the importer's own here.
  auto r = semaCheckFile(writeProjectFile(dir, "main.pkn", R"(
import lib::gen;
import other as u;
fn _helper() -> Str { return u::ten(); }
fn main() -> int { b = gen::Box(3); s: Str = _helper(); return gen::sum(b); }
)"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics, "");
  std::filesystem::remove_all(dir);
}

// One class per instantiation per program: another module's Box<int> (one
// it instantiated, or the template module's own) is the importer's
// Box<int>, and type arguments are inferred through it.
TEST(Generics, ImportedInstantiationsAreOneClass) {
  auto dir = paykan::test::tempDir() / "pkn_sema_generics_imp_one";
  std::filesystem::remove_all(dir);
  writeProjectFile(dir, "lib/gen.pkn", R"(
class Box<T> { v: T; view fn get() -> view T { return self.v; } }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
fn own() -> Box<int> { return Box<int>(1); }
fn two<T>(b: Box<T>) -> T[2] { return [b.v, b.v]; }
)");
  writeProjectFile(dir, "a.pkn",
                   "import lib::gen;\n"
                   "fn mk() -> gen::Box<Str> { "
                   "return gen::Box(\"a\"); }\n");
  auto r = semaCheckFile(writeProjectFile(dir, "main.pkn", R"(
import lib::gen;
import a;
fn main() -> int {
  s: gen::Box<Str> = a::mk();
  t: Str[2] = gen::two(a::mk());
  n: int[2] = gen::two(gen::own());
  b: gen::Box<int> = gen::own();
  return b.get();
}
)"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics, "");
  std::filesystem::remove_all(dir);
}

// An error in another module's template is reported in that module's file,
// with a note at the request (in that file too, here), and names no hidden
// qualifier.
TEST(Generics, ImportedTemplateErrorsPointIntoTheirModule) {
  auto dir = paykan::test::tempDir() / "pkn_sema_generics_imp_err";
  std::filesystem::remove_all(dir);
  writeProjectFile(dir, "lib/show.pkn", R"(
fn show<T>(x: T) -> Str { return x.label(); }
fn twice<T>(x: T) -> Str { return show(x) + show(x); }
)");
  auto r = semaCheckFile(writeProjectFile(dir, "main.pkn", R"(
import lib::show as s;
fn main() -> int { a = s::twice(1); b = s::twice(2); return 0; }
)"));
  EXPECT_FALSE(r.Ok);
  // The cached instantiation is checked once.
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_TRUE(has(r.Diagnostics, "lib/show.pkn:2:34: error: method call on "
                                 "non-class type 'int'"))
      << r.Diagnostics;
  EXPECT_TRUE(has(r.Diagnostics, "lib/show.pkn:3:35: note: in instantiation "
                                 "of 'lib::show::show<int>' requested here"))
      << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find('$'), std::string::npos) << r.Diagnostics;
  std::filesystem::remove_all(dir);
}

// The diagnostic engine's debug-build guard (#112): the same diagnostic group
// (an error plus its notes) reported twice is an internal error, while the
// same message with different notes or at another location is not.  The
// check is an assertion in the library, so it needs a library built with
// assertions: PAYKAN_TEST_LIBRARY_NDEBUG marks an installed one built
// without (paykan_add_frontend_tests).
#if !defined(NDEBUG) && !defined(PAYKAN_TEST_LIBRARY_NDEBUG)
TEST(Generics, DiagEngineRejectsIdenticalRepeat) {
  const paykan::ast::SourceLocation a(4, 7, 4, 26);
  const paykan::ast::SourceLocation b(5, 7, 5, 26);
  {
    std::ostringstream os;
    paykan::sema::DiagEngine diag(os);
    diag.error(a, "msg");
    diag.note(b, "in instantiation of 'g<int>' requested here");
    diag.error(a, "msg"); // other notes: a distinct group
    diag.note(b, "in instantiation of 'g<Str>' requested here");
    diag.error(b, "msg");   // another location
    diag.warning(a, "msg"); // another severity
    EXPECT_EQ(diag.getErrorCount(), 3u);
  }
  EXPECT_DEATH(
      {
        std::ostringstream os;
        paykan::sema::DiagEngine diag(os);
        diag.error(a, "msg");
        diag.error(a, "msg");
        diag.error(b, "next"); // closes the repeated group
      },
      "reported twice");
  EXPECT_DEATH(
      {
        std::ostringstream os;
        paykan::sema::DiagEngine diag(os);
        diag.error(a, "msg");
        diag.error(a, "msg");
      }, // closed by the destructor
      "reported twice");
}
#endif

// A module's own instantiation is exported as a concrete class, so a value of
// that type returned by the module can be used by the importer …
TEST(Generics, ExportedInstantiationIsUsableAsConcreteClass) {
  auto dir = paykan::test::tempDir() / "pkn_sema_generics_exp";
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
class Box<T> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
fn main() -> int { b = Box<int>(1); return 0; }
)");
  auto r2 = semaCheckFile(p2);
  EXPECT_FALSE(r2.Ok);
  EXPECT_TRUE(has(r2.Diagnostics, "instantiation 'Box<int>' conflicts with an "
                                  "imported class of the same name"))
      << r2.Diagnostics;

  // So would its own `Box<Str>` and the imported template's, either way
  // round.
  for (const char *body : {"a = Box<Str>(\"x\"); b = gen::Box<Str>(\"y\");",
                           "b = gen::Box<Str>(\"y\"); a = Box<Str>(\"x\");"}) {
    auto r3 = semaCheckFile(writeProjectFile(
        dir, "clash2.pkn",
        std::string("import ::io; import lib::gen;\n"
                    "class Box<T> { v: T; }\n"
                    "impl Box<T> { fn __init__(v: T) { self.v = v; } }\n"
                    "fn main() -> int { ") +
            body + " return 0; }\n"));
    EXPECT_FALSE(r3.Ok) << body;
    EXPECT_EQ(r3.ErrorCount, 1u) << r3.Diagnostics;
    EXPECT_TRUE(has(r3.Diagnostics, "conflicts with another template's "
                                    "instantiation of the same name"))
        << r3.Diagnostics;
  }
  std::filesystem::remove_all(dir);
}

// -- Interaction with tuples (#4) and optionals (#5)

// Tuple and optional types in template signatures and bodies are substituted
// per instantiation (tuple literals, `.N`, destructuring, `T?`, match arms).
TEST(GenericsTypes, TupleAndOptionalBodiesInstantiate) {
  auto r = semaCheck(R"(
    class Node { v: int; }
    impl Node { fn __init__(x: int) { self.v = x; } }
    class Pair<A, B> {
      p: (A, B);
      fn first() -> A { x, _ = self.p; return x; }
      fn swap() -> (B, A) { return (self.p.1, self.p.0); }
    }

    impl Pair<A, B> {
      fn __init__(a: A, b: B) { self.p = (a, b); }
    }
    class Slot<T> {
      v: T?;
      fn get() -> T? { return self.v; }
    }

    impl Slot<T> {
      fn __init__() { }
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
    class Node { v: int; }
    impl Node { fn __init__(x: int) { self.v = x; } }
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
// an optional primitive `int?` (#66); with T = an enum or an already
// optional type it is diagnosed, attributed to the instantiation.
TEST(GenericsTypes, OptionalOfPrimitiveArgumentIsAccepted) {
  auto r = semaCheck(R"(
    class Slot<T> {
      v: T?;
      fn put(x: T) { self.v = x; }
      fn get() -> T? { return self.v; }
    }

    impl Slot<T> {
      fn __init__() { }
    }
    fn main() -> int {
      b = Slot<int>();
      b.put(3);
      match b.get() { n: int { x: int = n + 1; } None { } }
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  // An enum's optional is a value optional; a `T?` of an optional argument
  // is the argument itself (no nested optional).
  auto re = semaCheck(R"(
    enum Color { Red, Green }
    class Slot<T> { v: T?; }
    impl Slot<T> { fn __init__() { } }
    fn main() -> int { b = Slot<Color>(); n = Slot<int?>(); return 0; }
  )");
  EXPECT_TRUE(re.Ok) << re.Diagnostics;

  // An optional tuple, an optional enum and an optional type argument (whose
  // `T?` is the argument itself) are all fine.
  auto r2 = semaCheck(R"(
    enum Color { Red }
    class Slot<T> { v: T?; }
    impl Slot<T> { fn __init__() { } }
    fn main() -> int {
      b = Slot<(int, Str)>(); c = Slot<Color>(); d = Slot<int?>();
      return 0;
    }
  )");
  EXPECT_TRUE(r2.Ok) << r2.Diagnostics;
}

TEST(GenericsTypes, TupleAndOptionalTypeArgs) {
  auto r = semaCheck(withMain(std::string(kBox) +
                                  "class Node {}\n"
                                  "impl Node { fn __init__() { } }\n",
                              R"(
    a: Box<(int, Str)> = Box<(int, Str)>((1, "a"));
    t: (int, Str) = a.get();
    n: Node? = None;
    b: Box<Node?> = Box<Node?>(n);
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// Exported instantiations whose names contain ',' round-trip: the import
// type parser keeps `Pair<Str, int>` / `Box<Pair<Str, int>>` together as
// names.
TEST(GenericsTypes, ExportedInstantiationNamesWithCommas) {
  auto dir = paykan::test::tempDir() / "pkn_sema_generics_names";
  std::filesystem::remove_all(dir);
  writeProjectFile(dir, "lib.pkn", R"(
class Pair<A, B> { a: A; b: B; fn first() -> A { return self.a; } }
impl Pair<A, B> { fn __init__(a: A, b: B) { self.a = a; self.b = b; } }
class Box<T> { v: T; fn get() -> T { return self.v; } }
impl Box<T> { fn __init__(v: T) { self.v = v; } }
fn mkPair() -> Pair<Str, int> { return Pair<Str, int>("k", 1); }
fn mkBoxPair() -> Box<Pair<Str, int>> { return Box<Pair<Str, int>>(mkPair()); }
fn mkBoxes() -> (Pair<Str, int>, Box<Pair<Str, int>>?) { b: Box<Pair<Str, int>>? = None; return (mkPair(), b); }
)");
  auto p = writeProjectFile(dir, "main.pkn", R"(
import lib;
fn main() -> int {
  s: Str = lib::mkPair().first();
  n: int = lib::mkBoxPair().get().b;
  x, y = lib::mkBoxes();
  k: Str = x.first();
  return n;
}
)");
  auto r = semaCheckFile(p);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(dir);
}

// -- static fn of a generic class's impl block (`Box<int>::of(1)`)

static const char *const kBoxWithStatics = R"(
  class Box<T> {
    __v: T;
    fn get() -> T { return self.__v; }
  }
  impl Box<T> {
    fn __init__(v: T) { self.__v = v; }
    static fn of(v: T) -> Box<T> { return Box<T>(v); }
    static fn pair(v: T) -> (Box<T>, Box<T>) {
      b = Box<T>(v);
      return (b, Box<T>(b.__v)); // a static reaches the private members
    }
  }
)";

TEST(Generics, StaticFnOfAnInstantiation) {
  auto r = check(withMain(kBoxWithStatics, R"(
    b = Box<int>::of(1);
    s: Box<Str> = Box<Str>::of("s");
    t = Box<int>::pair(2);
    n = t.0.get() + t.1.get() + b.get();
    io::println(s.get() + Str(n));
  )"));
  ASSERT_TRUE(r.Ok) << r.Diagnostics;
  // The statics of an instantiation are its functions, declared with it.
  std::vector<std::string> funcs;
  for (auto *fn : r.Driver->getRoot()->getFuncDecls())
    funcs.push_back(fn->getName());
  EXPECT_EQ(funcs,
            (std::vector<std::string>{"main", "Box<int>::of", "Box<int>::pair",
                                      "Box<Str>::of", "Box<Str>::pair"}));
}

TEST(Generics, StaticFnWithoutTypeArgs) {
  auto r = semaCheck(withMain(kBoxWithStatics, "b = Box::of(1);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_TRUE(has(r.Diagnostics,
                  "'Box::of' is a function of the generic class 'Box': write "
                  "'Box<...>::of(...)' with its type arguments"))
      << r.Diagnostics;
}

TEST(Generics, StaticFnUnknown) {
  auto r = semaCheck(withMain(kBoxWithStatics, "b = Box<int>::make(1);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_TRUE(has(r.Diagnostics, "'Box<int>' has no static function 'make'"))
      << r.Diagnostics;
}

TEST(Generics, StaticFnArgumentIsChecked) {
  auto r = semaCheck(withMain(kBoxWithStatics, "b = Box<int>::of(\"s\");"));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_TRUE(has(r.Diagnostics, "'Box<int>::of'")) << r.Diagnostics;
  EXPECT_TRUE(has(r.Diagnostics, "expected 'int'")) << r.Diagnostics;
}

// The name must be free in the instantiation, as in a plain class; the call
// is a follow-on.
TEST(Generics, StaticFnNameTakenByAMethod) {
  auto r = semaCheck(withMain(R"(
    class Box<T> {
      v: T;
      fn of() -> int { return 1; }
    }
    impl Box<T> {
      static fn of(v: T) -> Box<T> { return Box<T>(v); }
    }
  )",
                              "b = Box<int>::of(1);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_TRUE(has(r.Diagnostics, "class 'Box<int>' already has a method 'of'"))
      << r.Diagnostics;
}

// The body is checked per instantiation, with the instantiation note.
TEST(Generics, StaticFnBodyIsCheckedPerInstantiation) {
  auto r = semaCheck(withMain(R"(
    class Box<T> { v: T; }
    impl Box<T> {
      static fn of(v: T) -> Box<T> { return Box<T>(v, v); }
    }
  )",
                              "b = Box<int>::of(1);"));
  EXPECT_FALSE(r.Ok);
  EXPECT_TRUE(has(r.Diagnostics, "function 'Box<int>' expects 1 argument(s), "
                                 "got 2"))
      << r.Diagnostics;
  EXPECT_TRUE(has(r.Diagnostics, "in instantiation of 'Box<int>' requested "
                                 "here"))
      << r.Diagnostics;
}

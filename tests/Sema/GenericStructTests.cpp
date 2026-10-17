// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: generic structs (docs/language/14-structs.md, "Generic
// structs") — templates and their `impl` blocks, instantiation by type and
// by constructor call (explicit and inferred), bounds, nominal instances,
// errors inside an instantiation, self-containment through an instantiation,
// imports, and the diagnostics of a template's declaration.

#include "ParserDriver.h"
#include "Sema.h"
#include "TestUtils.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

void expectAll(const SemaResult &r, std::initializer_list<const char *> diags) {
  for (const char *d : diags)
    EXPECT_NE(r.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                        << r.Diagnostics;
}

const char *kPair = R"(
struct Pair<A, B> { a: A; b: B; }
impl Pair<A, B> {
  view fn first() -> view A { return self.a; }
  fn swapWith(other: inout Pair<A, B>) { t = self.a; self.a = other.a; other.a = t; }
  fn flipped() -> Pair<B, A> { return Pair<B, A>(self.b, self.a); }
}
)";

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

} // namespace

// -- Instantiation

TEST(GenericStruct, BoundsAreCheckedForInstantiationsFromFields) {
  // A field's type instantiates the template before the trait impls are
  // known: the bound check is queued and run later, for a struct template
  // as for a class template (it stopped the compiler).
  auto r = semaCheck(R"(
struct Keyed<K: Hashable> { key: K; n: int; }
struct Outer { k: Keyed<int>; }
class COuter { k: Keyed<Str>; }
struct Bad { k: Keyed<float>; }
fn main() -> int { o = Outer(Keyed(1, 3)); return o.k.n; }
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'float' does not implement trait 'Hashable', "
                               "which 'Keyed' requires of 'K'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}

TEST(GenericStruct, TypeParametersShadowingLaterTemplatesAreReported) {
  // Whatever the declaration order (the template is a type throughout).
  auto r = semaCheck(R"(
struct S<Box> { v: Box; }
class K<Box> { v: Box; }
struct Box<T> { v: T; }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("type parameter 'Box' of generic struct 'S' "
                               "shadows a type of the same name"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("type parameter 'Box' of generic class 'K' "
                               "shadows a type of the same name"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(GenericStruct, InstantiatedByTypeAndByCall) {
  auto r = semaCheck(std::string(kPair) + R"(
struct Box<T> { v: T; }
impl Box<T> {
  fn __init__(v: T) { self.v = v; }
  fn set(v: T) { self.v = v; }
  view fn get() -> view T { return self.v; }
}
class Holder<T> { p: Pair<T, Str>; }
impl Holder<T> { fn __init__(x: T) { self.p = Pair<T, Str>(x, "h"); } }
fn sumInts(p: view Pair<int, int>) -> int { return p.a + p.b; }
fn firstOf<A, B>(p: view Pair<A, B>) -> view A { return p.a; }
fn mkBox<T>(v: T) -> Box<T> { return Box(v); }
fn main() -> int {
  p = Pair(1, "one");                     // inferred from the fields
  q: Pair<int, Str> = Pair<int, Str>(2, "two");
  p.swapWith(q);
  f: Pair<Str, int> = p.flipped();
  b = Box(Pair(3, 4));                    // inferred from __init__
  b.set(Pair(5, 6));
  h = Holder(9);
  h.p.a = 10;
  bs: Box<int>[2] = [Box(1), Box(2)];
  bs[1].v = 7;
  t = (Pair(1.5, 'c'), Box(True));
  n: int = firstOf(f).len();
  return sumInts(b.get()) + p.first() + bs[0].v + h.p.a + mkBox(1).v + n +
         (if p == q || t.1 == Box(False) then 1 else 0);
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics, "");
}

TEST(GenericStruct, SameInstantiationIsOneType) {
  auto r = semaCheck(std::string(kPair) + R"(
fn take(p: Pair<int, Str>) -> int { return p.a; }
fn main() -> int {
  a = Pair(1, "a");
  b: Pair<int, Str> = a;
  c = Pair<int, Str>(2, "c");
  a.swapWith(c);
  return take(a) + take(b) + (if a == c then 1 else 0);
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(GenericStruct, InstancesAreNominal) {
  auto r = semaCheck(std::string(kPair) + R"(
struct Plain { x: int; }
fn main() -> int {
  t: Pair<int, Str> = Pair<Str, int>("a", 1);
  u = Pair<int, Str>(1, "a") == Pair<Str, int>("a", 1);
  v: (int, Str) = Pair(1, "a");
  o: Obj = Pair(1, 2);
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"initializer of type 'Pair<Str, int>' does not match declared "
                "type 'Pair<int, Str>'",
                "operands of '==' have mismatched types 'Pair<int, Str>' and "
                "'Pair<Str, int>'",
                "initializer of type 'Pair<int, Str>' does not match declared "
                "type '(int, Str)'",
                "initializer of type 'Pair<int, int>' does not match declared "
                "type 'Obj'"});
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
}

TEST(GenericStruct, TypeArgumentErrors) {
  auto r = semaCheck(std::string(kPair) + R"(
struct Plain { x: int; }
struct Bad<T> { xs: T; }
fn main() -> int {
  p: Pair<int> = Pair<int>(1);
  q: Pair = Pair(1, 2);
  r: Plain<int> = Plain<int>(1);
  s = Plain<int>(1);
  b: Bad<int[2]> = Bad<int[2]>([1, 2]);
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"generic struct 'Pair' expects 2 type argument(s), got 1",
                "variable 'q' names generic struct 'Pair' without type "
                "arguments (write 'Pair<...>')",
                "'Plain' is not a generic struct and takes no type arguments",
                "'Plain' is not generic and takes no type arguments",
                "type argument 1 of 'Bad': a fixed-size array ('int[2]') can "
                "only be a local variable, a parameter, a result or a field "
                "for now"});
}

TEST(GenericStruct, Inference) {
  auto r = semaCheck(R"(
struct E<T> { x: int; }
struct Same<T> { a: T; b: T; }
struct Init<T> { v: T; n: int; }
impl Init<T> { fn __init__(v: T) { self.v = v; self.n = 0; } }
struct Nested<T> { inner: Same<T>; }
fn main() -> int {
  e = E(1);
  s = Same(1, "a");
  i: Init<Str> = Init("s");
  n: Nested<int> = Nested(Same(1, 2));
  ok: Same<float> = Same(1.5, 2.5);
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"cannot infer type parameter 'T' of 'E' from the call "
                "arguments",
                "specify the type arguments explicitly: 'E<...>(...)'",
                "cannot infer type parameter 'T' of 'Same': deduced as both "
                "'int' and 'Str'"});
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

TEST(GenericStruct, BoundsAndTraits) {
  auto r = semaCheck(R"(
struct H<T: Hashable> { v: T; }
impl Hashable for H<T> { view fn hash() -> int { return self.v.hash(); } }
trait Named { view fn name() -> Str; }
struct Tag<T> { v: T; }
impl Named for Tag<T> { view fn name() -> Str { return "tag"; } }
struct NoName<T> { v: T; }
impl Named for NoName<T> {}
fn greet<T: Named>(x: view T) -> Str { return x.name(); }
fn hashOf<T: Hashable>(x: view T) -> int { return x.hash(); }
fn show<T: Show>(x: view T) -> Str { return x.toString(); }
fn twice<T: ValueType>(x: T) -> (T, T) { return (x, x); }
fn main() -> int {
  h = H(2.5);
  g = H("s");
  pair = twice(Tag(1));
  println(greet(Tag(1)) + show(g) + Str(hashOf(g)) + Str(hashOf(H(1))));
  n = NoName(1);
  return pair.1.v;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'float' does not implement trait 'Hashable', which 'H' "
                "requires of 'T'",
                "struct 'NoName<int>' does not implement 'view fn name() -> "
                "Str' of trait 'Named'"});
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

TEST(GenericStruct, ErrorInsideInstantiationNamesIt) {
  auto r = semaCheck(R"(
struct Box<T> { v: T; }
impl Box<T> { view fn bad() -> int { return self.v; } }
fn main() -> int {
  b = Box("s");
  return b.bad();
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"return value of type 'Str' does not match function return "
                "type 'int'",
                "note: in instantiation of 'Box<Str>' requested here"});
}

TEST(GenericStruct, UninstantiatedTemplateIsNotChecked) {
  auto r = semaCheck(R"(
struct Box<T> { v: T; }
impl Box<T> { view fn bad() -> int { return self.v + undefinedName; } }
fn main() -> int { return 0; }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(GenericStruct, CannotContainItselfThroughAnInstantiation) {
  auto r = semaCheck(R"(
struct Node<T> { v: T; next: Node<T>; }
struct A<T> { b: B<T>; }
struct B<T> { a: (int, A<T>); }
class Link<T> { v: T; next: Link<T>?; }
struct Fine<T> { l: Link<T>; }
fn main() -> int {
  n = Node<int>(1, n);
  a = A<int>(B<int>((1, a)));
  f: Fine<int> = Fine<int>(Link<int>(1, None));
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"struct 'Node<int>' contains itself: field 'next' holds a "
                "'Node<int>'; a struct is held in place, so it cannot hold "
                "one of its own (hold it in a class instead)",
                "struct 'A<int>' contains itself: field 'b' holds a 'A<int>'"});
}

TEST(GenericStruct, InstantiationDepthGuard) {
  // Through a field (as for classes) and through a method body, which
  // instantiates after the module's bodies are checked.
  auto r = semaCheck(R"(
struct Bad<T> { inner: Bad<Bad<T>>; }
fn main() -> int { b: Bad<int> = Bad<int>(b); return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"exceeds the maximum instantiation depth"});

  auto r2 = semaCheck(R"(
struct Deep<T> { v: T; }
impl Deep<T> { fn go() { d = Deep<Deep<T>>(self); d.go(); } }
fn main() -> int { d = Deep<int>(1); d.go(); return 0; }
)");
  EXPECT_FALSE(r2.Ok);
  expectAll(r2, {"exceeds the maximum instantiation depth"});

  auto r3 = semaCheck(R"(
class Deep<T> { fn go() { d = Deep<Deep<T>>(); d.go(); } }
fn main() -> int { d = Deep<int>(); d.go(); return 0; }
)");
  EXPECT_FALSE(r3.Ok);
  expectAll(r3, {"exceeds the maximum instantiation depth"});
}

// -- Declarations and impl blocks

TEST(GenericStruct, TemplateDeclarationErrors) {
  auto r = semaCheck(R"(
struct Pair<T> { a: T; }
class Pair { x: int; }
fn Pair() -> int { return 1; }
struct S<T, T> { a: T; }
struct U<Str> { a: int; }
struct V<T> { a: T; }
struct V<T> { b: T; }
struct W { x: int; }
struct W<T> { a: T; }
struct X<T> { a: T; }
fn X<T>(x: T) -> T { return x; }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'Pair' is already declared as a generic struct",
                "duplicate type parameter 'T' in generic struct 'S'",
                "type parameter 'Str' of generic struct 'U' shadows a type",
                "redefinition of generic struct 'V'",
                "'W' is already declared as a struct",
                "'X' is already declared as a generic struct"});
}

TEST(GenericStruct, ImplBlockRules) {
  auto r = semaCheck(R"(
struct Pair<A, B> { a: A; b: B; }
impl Pair { fn f() {} }
impl Pair<A> { fn g() {} }
impl Pair<A, B> { static fn mk() -> int { return 1; } }
impl Pair<X, Y> { fn h() {} }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'Pair' is a generic struct: write 'impl Pair<A, B>'",
                "'impl Pair' names 1 type parameter(s); generic struct 'Pair' "
                "has 2",
                "a 'static fn' in the 'impl' block of a generic struct is not "
                "supported yet"});
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
}

TEST(GenericStruct, ImplMethodsAreCheckedPerInstantiation) {
  auto r = semaCheck(R"(
struct Pair<A, B> { a: A; b: B; }
impl Pair<A, B> { fn __init__(a: A, b: B) { self.a = a; self.b = b; } }
impl Pair<X, Y> { fn __init__(x: X, y: Y) { self.a = x; self.b = y; } }
impl Pair<A, B> { view fn toString() -> Str { return "p"; } }
fn main() -> int { p = Pair(1, 2); return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"struct 'Pair<int, int>' already has a method '__init__'",
                "struct 'Pair<int, int>' already has a method 'toString'"});
}

TEST(GenericStruct, StructRulesHoldForInstances) {
  auto r = semaCheck(std::string(kPair) + R"(
fn main() -> int {
  p = Pair(1, 2);
  o: Pair<int, int>? = None;
  match p { _ { } }
  let q = Pair(1, 2);
  q.a = 3;
  x = p.0;
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"variable 'o' has type 'Pair<int, int>?': optional struct "
                "types are not supported yet",
                "match on a struct ('Pair<int, int>') is not supported yet",
                "'q' is declared with 'let'; its fields are part of its value "
                "and cannot be assigned",
                "tuple index '.0' applied to struct type 'Pair<int, int>': a "
                "struct's fields are named (a, b)"});
}

// -- Modules

namespace {
const char *kGenLib = R"(
struct Pair<A, B> { a: A; b: B; }
impl Pair<A, B> {
  view fn first() -> view A { return self.a; }
  fn bump(d: int) { self.a = self.a; }
}
struct Box<T> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = v; } view fn get() -> view T { return self.v; } }
fn mk() -> Pair<int, Str> { return Pair<int, Str>(41, "x"); }
fn boxed<T>(v: T) -> Box<T> { return Box<T>(v); }
)";
} // namespace

TEST(GenericStruct, ImportedTemplates) {
  auto dir = paykan::test::tempDir() / "pkn_sema_generic_structs_imp";
  std::filesystem::remove_all(dir);
  writeProjectFile(dir, "lib/gen.pkn", kGenLib);
  const std::vector<std::pair<std::string, std::string>> cases = {
      // Type position, holding the module's own Pair<int, Str>.
      {"t1", "import lib::gen;\n"
             "fn main() -> int { p: gen::Pair<int, Str> = gen::mk(); "
             "p.a = 1; return p.a + p.first(); }"},
      // Constructor call, explicit and inferred; the module's own
      // instantiation and a new one.
      {"t2", "import lib::gen;\n"
             "fn main() -> int { q = gen::Pair<int, Str>(1, \"q\"); "
             "r = gen::Pair(\"s\", 2.5); b: gen::Box<Str> = gen::boxed(\"b\"); "
             "c = gen::Box(r); return q.a + (if q == gen::mk() then 1 else "
             "0); }"},
      // Aliased import and the full module path.
      {"t3", "import lib::gen as g;\n"
             "fn main() -> int { b: lib::gen::Box<int> = g::Box<int>(1); "
             "return b.get(); }"},
      // Inside this module's own templates.
      {"t4", "import lib::gen;\n"
             "struct W<T> { b: gen::Box<T>; }\n"
             "impl W<T> { fn __init__(x: T) { self.b = gen::Box<T>(x); } }\n"
             "fn wrap<T>(x: T) -> gen::Pair<T, T> { return gen::Pair<T, T>(x, "
             "x); }\n"
             "fn main() -> int { w = W<int>(1); s = W<Str>(\"a\"); "
             "return wrap(2).b + w.b.get(); }"},
  };
  for (const auto &[name, src] : cases) {
    auto r = semaCheckFile(writeProjectFile(dir, name + ".pkn", src));
    EXPECT_TRUE(r.Ok) << name << ":\n" << r.Diagnostics;
    EXPECT_EQ(r.Diagnostics, "") << name;
  }
  // An `impl` block for another module's generic struct is not supported.
  auto bad =
      semaCheckFile(writeProjectFile(dir, "bad.pkn",
                                     "import lib::gen;\n"
                                     "impl gen::Pair<A, B> { fn f() {} }\n"
                                     "fn main() -> int { return 0; }\n"));
  EXPECT_FALSE(bad.Ok);
  expectAll(bad, {"an 'impl' block for a struct of another module is not "
                  "supported yet"});
  std::filesystem::remove_all(dir);
}

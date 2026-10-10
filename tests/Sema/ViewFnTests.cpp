// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: `view fn` methods.  A method may change `self` unless it is a
// `view fn`, and only a `view fn` may be called on a `view`.

#include "TestUtils.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace paykan::test;

namespace {

/// Write @p content to @p dir / @p name; its path.
std::string writeModule(const std::filesystem::path &dir,
                        const std::string &name, const std::string &content) {
  std::filesystem::create_directories(dir);
  std::ofstream(dir / name) << content;
  return (dir / name).string();
}

/// Sema over the file @p path, importing from its directory.
SemaResult semaCheckPath(const std::string &path) {
  paykan::parser::ParserDriver drv(testFrontend());
  if (drv.parseFile(path) != 0)
    return {false, "parse error", 1};
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  diag.setSourceInfo(drv.getCurrentFile(), &drv.getSourceLines());
  paykan::sema::Sema sema(drv.getASTContext(), diag,
                          std::filesystem::path(path).parent_path().string(),
                          drv.getFrontendName());
  auto ctx = sema.run(drv.getRoot());
  return {ctx.Ok, os.str(), ctx.ErrorCount};
}

void expectAll(const SemaResult &r, std::initializer_list<const char *> diags) {
  for (const char *d : diags)
    EXPECT_NE(r.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                        << r.Diagnostics;
}

} // namespace

// Only a method is a `view fn`, never `__init__`, and an override keeps the
// marker of the method it overrides.
TEST(ViewFn, Declarations) {
  auto r = semaCheck(R"(class Base {
  fn get() -> int { return 1; }
  view fn show() -> Str { return "b"; }
}
class Sub : Base {
  view fn get() -> int { return 2; }
  fn show() -> Str { return "s"; }
}
class Pt { x: int; view fn __init__() { self.x = 0; } }
view fn free() { }
class Shape { view fn toString() -> Str { return "s"; } }
class Bad { fn toString() -> Str { return "b"; } }
fn main() -> int { return 0; }
)");
  EXPECT_EQ(r.ErrorCount, 5u) << r.Diagnostics;
  expectAll(r,
            {
                ":6:3: error: override of 'get' cannot be a 'view fn': the "
                "method it overrides may change 'self'",
                ":7:3: error: override of 'show' must be a 'view fn', like "
                "the method it overrides",
                ":9:20: error: '__init__' cannot be a 'view fn': it sets up "
                "'self'",
                ":10:1: error: only a method can be a 'view fn': 'free' is a "
                "free function, with no 'self'",
                ":12:13: error: override of 'toString' must be a 'view fn', "
                "like the method it overrides",
            });
}

// `let` only fixes the variable: any method can be called on what it holds,
// its own and the builtins', `view fn` or not.
TEST(ViewFn, LetCanCallAnyMethod) {
  auto r = semaCheck(R"(class Counter {
  n: int;
  fn __init__() { self.n = 0; }
  fn tick() { self.n = self.n + 1; }
  view fn get() -> int { return self.n; }
}
fn main() -> int {
  let c = Counter();
  let s = "abc";
  let xs = [1, 2];
  c.tick();
  s.concat("d");
  xs.push(3);
  println(c.toString() + s.toString());
  return c.get() + s.len() + xs.len();
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A `view fn` of the class, an inherited one and an override of one can be
// called on a `view`; a plain `fn` cannot.
TEST(ViewFn, CallableOnAView) {
  auto r = semaCheck(R"(class Counter {
  n: int;
  fn __init__() { self.n = 0; }
  fn tick() { self.n = self.n + 1; }
  view fn get() -> int { return self.n; }
  view fn toString() -> Str { return "c" + Str(self.n); }
}
class Fast : Counter {
  fn __init__() { __super__(); }
  view fn get() -> int { return 2 * self.n; }
}
fn show(c: view Counter, f: view Fast) -> int {
  println(c.toString() + f.toString());
  f.tick();
  return c.get() + f.get();
}
fn main() -> int { return show(Counter(), Fast()); }
)");
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  expectAll(r, {":14:3: error: 'f' is a 'view' parameter; 'tick' is not a "
                "'view fn'"});
}

// A module's `view fn` markers are part of its interface: overrides in the
// importer are checked against them.
TEST(ViewFn, AcrossModules) {
  auto dir = tempDir() / "view_fn_modules";
  std::filesystem::remove_all(dir);
  writeModule(dir, "base.pkn",
              "class P { n: int;\n"
              "  fn __init__() { self.n = 0; }\n"
              "  view fn get() -> int { return self.n; }\n"
              "  fn set(v: int) { self.n = v; } }\n");
  auto r = semaCheckPath(writeModule(dir, "main.pkn", R"(import base;
class Q : base::P { fn __init__() { __super__(); } fn get() -> int { return 1; } }
fn main() -> int {
  let p = base::P();
  p.set(2);
  return p.get();
})"));
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  expectAll(r, {":2:52: error: override of 'get' must be a 'view fn', like "
                "the method it overrides"});
  std::filesystem::remove_all(dir);
}

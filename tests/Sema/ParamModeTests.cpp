// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: `view` and `inout` parameters.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

/// Expect @p r to have failed with each of @p diags (`line:col: error: ...`).
void expectErrors(const SemaResult &r,
                  std::initializer_list<const char *> diags) {
  EXPECT_FALSE(r.Ok);
  for (const char *diag : diags)
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
}

} // namespace

// A `view` parameter is passed by value and read-only; any argument of its
// type does, and reading it gives a copy.
TEST(ParamMode, ViewParameters) {
  auto ok = semaCheck(R"(
    enum Color { Red, Green }
    class Counter {
      n: int;
      fn __init__(view start: int) { self.n = start; }
      fn add(view k: int, view c: Color) -> int { self.n = self.n + k; return k; }
    }
    fn twice(view x: float) -> float { y = x; y = y * 2; return y; }
    fn pass(view x: int) -> int { return x + x; }
    fn main() -> int {
      let k = 2;
      t = twice(k);
      c = Counter(k);
      c.add(c.n + 1, Color::Red);
      return pass(k) + pass(3);
    }
  )");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;

  auto r = semaCheck(R"(fn f(view x: int) {
  x = 1;
  x, y = (2, 3);
  { x = 4; }
  { x: int = 5; x = 6; }
}
fn main() -> int { return 0; })");
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
  expectErrors(r, {":2:3: error: cannot assign to 'view' parameter 'x'",
                   ":3:3: error: cannot assign to 'view' parameter 'x'",
                   ":4:5: error: cannot assign to 'view' parameter 'x'"});
}

// Only int, float, bool, char and enum parameters take a mode.
TEST(ParamMode, ModesApplyOnlyToValueTypes) {
  auto r = semaCheck(R"(class Counter {
  fn __init__(view s: Str) { }
  fn put(view c: Counter) { }
}
fn f(view o: int?, view xs: int[], view t: (int, int), view b: Obj) { }
fn main() -> int { return 0; })");
  EXPECT_EQ(r.ErrorCount, 6u) << r.Diagnostics;
  const char *const kRule = "' applies only to int, float, bool, char and enum "
                            "parameters; '";
  for (const std::string &diag :
       {":2:3: error: 'view" + std::string(kRule) + "Str' is not a value type",
        ":3:3: error: 'view" + std::string(kRule) +
            "Counter' is not a value type",
        ":5:1: error: 'view" + std::string(kRule) + "int?' is not a value type",
        ":5:1: error: 'view" + std::string(kRule) +
            "int[]' is not a value type",
        ":5:1: error: 'view" + std::string(kRule) +
            "(int, int)' is not a value type",
        ":5:1: error: 'view" + std::string(kRule) + "Obj' is not a value type"})
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
}

// Not on a type parameter, for now: the template is rejected, and its uses
// are follow-ons.  A mode on a value-type parameter of a template is fine.
TEST(ParamMode, ModesOnTypeParametersAreRejected) {
  auto r = semaCheck(R"(fn first<T>(view x: T) { }
class Box<T> { fn put(view v: T) { } }
fn second<T>(xs: T[], view i: int) -> T { return xs[i]; }
fn main() -> int {
  first(1);
  first<int>(2);
  b = Box<int>();
  return second([1, 2], 1);
})");
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  expectErrors(r,
               {":1:1: error: 'view' does not apply to type parameter 'T' yet",
                ":2:16: error: 'view' does not apply to type parameter 'T' "
                "yet"});
}

// An override keeps every parameter's mode.
TEST(ParamMode, OverridesKeepModes) {
  auto r = semaCheck(R"(class Base {
  fn a(view k: int) { }
  fn b(k: int) { }
  fn c(view k: int, j: int) { }
}
class Sub : Base {
  fn a(k: int) { }
  fn b(view k: int) { }
  fn c(view k: int, j: int) { }
}
fn main() -> int { return 0; })");
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  expectErrors(r, {":7:3: error: override of 'a' must keep 'view' on "
                   "parameter 'k'",
                   ":8:3: error: override of 'b' must not add 'view' to "
                   "parameter 'k'"});
}

// The arguments of an `inout` parameter: a variable or a field of exactly
// its type, not a `view` parameter or a `let` local.  (`inout` parameters
// are not lowered yet, so their declarations are errors too.)
TEST(ParamMode, InoutArguments) {
  auto r = semaCheck(R"(class C {
  n: int;
  fn __init__(inout n: int) { self.n = n; }
  fn add(inout to: float) { }
}
fn bump(inout n: int) { n = n + 1; }
fn put(inout c: char) { }
fn take(view v: int, p: int, inout q: int) {
  let k = 1;
  x = 2;
  c = C(x);
  bump(v);
  bump(k);
  bump(3);
  bump(x + 1);
  c.add(x);
  xs = [1];
  bump(xs[0]);
  s = "ab";
  put(s[0]);
  bump(p);
  bump(q);
  bump(x);
  bump(c.n);
  c2 = C(k);
  bump(Str(1));
}
fn main() -> int { return 0; })");
  expectErrors(
      r,
      {":12:8: error: 'view' parameter 'v' cannot be passed to 'inout' "
       "parameter 'n'",
       ":13:8: error: 'k' is declared with 'let' and cannot be passed to "
       "'inout' parameter 'n'",
       ":14:8: error: argument 1 of 'bump' must be a variable or a field: "
       "parameter 'n' is 'inout'",
       ":15:8: error: argument 1 of 'bump' must be a variable or a field: "
       "parameter 'n' is 'inout'",
       ":16:9: error: argument 1 of 'add' has type 'int', but 'inout' "
       "parameter 'to' has type 'float'",
       ":18:8: error: an array element cannot be passed to 'inout' parameter "
       "'n' yet",
       ":20:7: error: a character of a string cannot be passed to 'inout' "
       "parameter 'c'",
       ":25:10: error: 'k' is declared with 'let' and cannot be passed to "
       "'inout' parameter 'n'",
       ":26:8: error: argument 1 of 'bump' has type 'Str', expected 'int'"});
  // Plain and `inout` parameters, variables and fields are accepted.
  for (const char *line : {":21:", ":22:", ":23:", ":24:"})
    EXPECT_EQ(r.Diagnostics.find(line), std::string::npos) << line << "\n"
                                                           << r.Diagnostics;
}

// Checked, but not lowered yet: every `inout` parameter is an error, in
// functions, methods, constructors and generic templates alike.
TEST(ParamMode, InoutIsNotSupportedYet) {
  auto r = semaCheck(R"(
    fn bump(inout n: int) { }
    fn second<T>(xs: T[], inout i: int) { }
    class C { fn __init__(inout n: int) { } }
    fn main() -> int { return 0; }
  )");
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
  expectErrors(r, {":2:5: error: 'inout' parameters are not supported yet",
                   ":3:5: error: 'inout' parameters are not supported yet",
                   ":4:15: error: 'inout' parameters are not supported yet"});
}

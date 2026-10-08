// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: the ownership prototype (docs/design/ownership-proto.md).  Its
// syntax is an error without Sema::setOwnership(true), the way an AST from
// the interchange format reaches a compiler run without --ownership; with
// it, `view` / `inout` apply to value parameters only and `let` locals are
// not reassigned.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

SemaResult ownershipCheck(const std::string &src) {
  return semaRun(parseOwnership(src), /*ownership=*/true);
}

void expectOwnershipError(const std::string &src, const std::string &msg) {
  auto r = ownershipCheck(src);
  EXPECT_FALSE(r.Ok) << src;
  EXPECT_NE(r.Diagnostics.find("error: " + msg), std::string::npos)
      << src << "\n"
      << r.Diagnostics;
}

void expectAccepted(const std::string &src) {
  auto r = ownershipCheck(src);
  EXPECT_TRUE(r.Ok) << src << "\n" << r.Diagnostics;
}

// The functions the cases below call; each case is the body of main.
const char *kPrelude = R"(
enum Color { Red, Green }
class P {
  n: int;
  xs: int[];
  s: Str;
  fn __init__(inout k: int) { self.n = k; self.xs = [k]; self.s = "ab"; }
  fn add(inout to: int, k: int) { to = to + k; }
}
class Q : P { fn __init__(inout k: int) { __super__(k); } }
fn bump(inout n: int) { n = n + 1; }
fn twice(inout n: int) { bump(n); bump(n); }
fn next(inout c: char) { }
fn scale(inout x: float) { }
fn green(inout c: Color) { c = Color::Green; }
fn read(view n: int) -> int { m = n; m = m + 1; return m; }
fn main() -> int {
)";

std::string withPrelude(const std::string &body) {
  return kPrelude + body + "\nreturn 0;\n}";
}

} // namespace

TEST(Ownership, SyntaxNeedsTheFlag) {
  const std::pair<const char *, const char *> cases[] = {
      {"fn main() -> int { let x = 1; return x; }", "let"},
      {"fn f(view n: int) { }\nfn main() -> int { return 0; }", "view"},
      {"class C { fn m(inout n: int) { } }\nfn main() -> int { return 0; }",
       "inout"},
  };
  for (const auto &[src, what] : cases) {
    auto r = semaRun(parseOwnership(src));
    EXPECT_FALSE(r.Ok) << src;
    EXPECT_NE(r.Diagnostics.find(std::string("error: '") + what +
                                 "' needs --ownership (prototype)"),
              std::string::npos)
        << src << "\n"
        << r.Diagnostics;
  }
  // With it, the same programs are accepted.
  for (const auto &[src, what] : cases)
    expectAccepted(src);
}

TEST(Ownership, ReferencesAreSharedWithoutQualifiers) {
  // Passing or assigning a reference shares it; any reference may change the
  // object (fields, elements, push, concat, a method that changes self).
  expectAccepted(R"(
class Counter {
  n: int;
  fn __init__() { self.n = 0; }
  fn tick() { self.n = self.n + 1; }
}
fn poke(c: Counter, xs: int[], s: Str) {
  c.tick(); c.n = c.n + 1; xs.push(1); xs[0] = 2; s.concat("!");
}
fn main() -> int {
  a = Counter(); b = a; xs = [0]; ys = xs; s = "a"; t = s;
  poke(b, ys, t);
  return a.n;
})");
}

TEST(Ownership, QualifiersApplyToValueTypesOnly) {
  const std::string tail = "\nfn main() -> int { return 0; }";
  const std::string values = "(int, float, bool, char, enums and optionals); '";
  expectOwnershipError("class Counter { }\nfn f(inout c: Counter) { }" + tail,
                       "'inout' applies only to value types " + values +
                           "Counter' is a reference");
  expectOwnershipError("fn f(view s: Str) { }" + tail,
                       "'view' applies only to value types " + values +
                           "Str' is a reference");
  expectOwnershipError("class C { fn m(inout xs: int[]) { } }" + tail,
                       "'inout' applies only to value types " + values +
                           "int[]' is a reference");
  expectAccepted("enum E { A }\nfn f(view a: int, inout b: float, view c: "
                 "bool, inout d: char, inout e: E) { }" +
                 tail);
  // Every optional is a value (a value and a flag), a class optional too.
  expectAccepted("class C { }\nfn f(inout a: int?, view b: C?, inout c: "
                 "Str?, inout d: int[]?) { a = None; c = \"s\"; d = [1]; }" +
                 tail);
}

TEST(Ownership, InoutOptionalsTakeTheirExactType) {
  const std::string pre = "class C { }\nfn set(inout x: int?) { x = 1; }\n"
                          "fn put(inout c: C?) { c = None; }\n"
                          "fn main() -> int {\n";
  expectAccepted(pre + "a: int? = None; set(a); c: C? = C(); put(c); "
                       "return 0; }");
  const std::pair<const char *, const char *> cases[] = {
      {"a = 1; set(a);", "argument 1 of 'set' has type 'int', but 'inout' "
                         "parameter 'x' has type 'int?'"},
      {"c = C(); put(c);", "argument 1 of 'put' has type 'C', but 'inout' "
                           "parameter 'c' has type 'C?'"},
      {"set(None);", "argument 1 of 'set' must be a variable, field or "
                     "element: parameter 'x' is 'inout'"},
  };
  for (const auto &[body, msg] : cases)
    expectOwnershipError(pre + body + " return 0; }", msg);
}

TEST(Ownership, ViewParametersAreReadOnly) {
  expectAccepted(withPrelude("k = 1; read(k); bump(k);"));
  // A view's value is a copy: it may be stored, returned or passed on.
  expectAccepted("fn f(view n: int) -> int { m = n; if (n > 0) { n: int = 2; "
                 "n = 3; } return n + m; }\nfn g(view n: int, k: int) -> int "
                 "{ k = n; return f(n) + k; }\nfn main() -> int { return 0; }");
  const std::string msg = "cannot assign to 'view' parameter 'n'";
  expectOwnershipError("fn f(view n: int) { n = 1; }\n"
                       "fn main() -> int { return 0; }",
                       msg);
  expectOwnershipError("fn f(view n: int) { n, m = (1, 2); }\n"
                       "fn main() -> int { return 0; }",
                       msg);
  expectOwnershipError("fn bump(inout x: int) { }\nfn f(view n: int) { "
                       "bump(n); }\nfn main() -> int { return 0; }",
                       "'view' parameter 'n' cannot be passed to 'inout' "
                       "parameter 'x'");
}

TEST(Ownership, InoutArgumentsAreChangeablePlaces) {
  // Variables, fields and elements, through functions, methods,
  // constructors and __super__; an inout parameter is passed on.
  expectAccepted(withPrelude(
      "k = 1; bump(k); twice(k); p = P(k); p.add(k, 2); bump(p.n); "
      "bump(p.xs[0]); p.add(p.xs[k - 1], 1); q = Q(p.n); c = Color::Red; "
      "green(c); ch = 'a'; next(ch); i: int = 0; bump(i);"));
  const std::pair<const char *, const char *> cases[] = {
      {"bump(1);", "argument 1 of 'bump' must be a variable, field or "
                   "element: parameter 'n' is 'inout'"},
      {"k = 1; bump(k + 1);", "argument 1 of 'bump' must be a variable, field "
                              "or element: parameter 'n' is 'inout'"},
      {"green(Color::Red);", "argument 1 of 'green' must be a variable, field "
                             "or element: parameter 'c' is 'inout'"},
      {"t = (1, 2); bump(t.0);", "argument 1 of 'bump' must be a variable, "
                                 "field or element: parameter 'n' is 'inout'"},
      {"p = P(1);", "argument 1 of 'P' must be a variable, field or element: "
                    "parameter 'k' is 'inout'"},
      {"k = 1; p = P(k); p.add(3, k);",
       "argument 1 of 'add' must be a variable, field or element: parameter "
       "'to' is 'inout'"},
      {"let k = 1; bump(k);", "'k' is declared with 'let' and cannot be passed "
                              "to 'inout' parameter 'n'"},
      {"f = 1.0; k = 1; p = P(k); p.add(f, 1);",
       "argument 1 of 'add' has type 'float', expected 'int'"},
      {"k = 1; scale(k);", "argument 1 of 'scale' has type 'int', but 'inout' "
                           "parameter 'x' has type 'float'"},
      {"k = 1; p = P(k); next(p.s[0]);",
       "a character of a string cannot be passed to 'inout' parameter 'c'"},
  };
  for (const auto &[body, msg] : cases)
    expectOwnershipError(withPrelude(body), msg);
}

TEST(Ownership, LetCannotBeReassigned) {
  const std::string msg = "'x' is declared with 'let' and cannot be reassigned";
  expectOwnershipError("fn main() -> int { let x = 1; x = 2; return x; }", msg);
  expectOwnershipError(
      "fn main() -> int { let x = 1; x, y = (2, 3); return x; }", msg);
  expectOwnershipError(
      "fn main() -> int { let x = 1; if (x > 0) { x = 2; } return x; }", msg);
  // A shadowing declaration in an inner scope is a new variable, and a `let`
  // object can still be changed.
  expectAccepted("fn main() -> int { let x = 1; if (x > 0) { x: int = 2; "
                 "x = 3; } return x; }");
  expectAccepted(withPrelude("k = 1; let p = P(k); p.n = 2; p.xs.push(3); "
                             "bump(p.n); let c: Color = Color::Red;"));
}

TEST(Ownership, OverridesKeepParameterQualifiers) {
  auto src = [](const char *base, const char *derived) {
    return std::string("class A { fn m(") + base +
           ") { } }\nclass B : A { fn m(" + derived +
           ") { } }\nfn main() -> int { return 0; }";
  };
  expectOwnershipError(src("inout n: int", "n: int"),
                       "override of 'm' must keep 'inout' on parameter 'n'");
  expectOwnershipError(src("n: int", "inout n: int"),
                       "override of 'm' must not add 'inout' to parameter 'n'");
  expectOwnershipError(src("view n: int", "inout n: int"),
                       "override of 'm' must keep 'view' on parameter 'n'");
  expectAccepted(src("inout n: int", "inout n: int"));
  expectAccepted(src("view n: int, s: Str", "view n: int, s: Str"));
}

TEST(Ownership, SpecExampleChecks) {
  expectAccepted(R"(
class Counter {
  n: int;
  fn __init__() { self.n = 0; }
  fn tick() { self.n = self.n + 1; }
}
fn tickTwice(c: Counter) { c.tick(); c.tick(); }
fn bump(inout n: int) { n = n + 1; }
fn report(view n: int) -> Str {
  m = n;
  m = m * 10;
  return Str<int>(n) + " -> " + Str<int>(m);
}
fn main() -> int {
  a = Counter();
  b = a;
  tickTwice(b);
  bump(a.n);
  let k = a.n;
  x = k;
  bump(x);
  println(report(x));
  return 0;
})");
}

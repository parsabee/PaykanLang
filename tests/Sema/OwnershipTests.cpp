// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: the ownership prototype's syntax (docs/design/ownership-proto.md)
// is accepted with Sema::setOwnership(true) and an error without it, the way
// an AST from the interchange format or a frontend with the option reaches
// a compiler run without --ownership.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

TEST(Ownership, SyntaxNeedsTheFlag) {
  const std::pair<const char *, const char *> cases[] = {
      {"fn main() -> int { x: own int = 1; return x; }", "own"},
      {"fn main() -> int { let x = 1; return x; }", "let"},
      {"fn main() -> int { x = 1; return cp x; }", "cp"},
      {"fn main() -> int { x = 1; return mv x; }", "mv"},
      {"fn f(s: mut Str) { }\nfn main() -> int { return 0; }", "mut"},
      {"fn f() -> own Str { return \"\"; }\nfn main() -> int { return 0; }",
       "own"},
      {"class C { s: own Str; }\nfn main() -> int { return 0; }", "own"},
      {"class C { fn m(self) { } }\nfn main() -> int { return 0; }", "self"},
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
}

TEST(Ownership, SyntaxIsAcceptedWithTheFlag) {
  auto r = semaRun(parseOwnership(R"(
class P {
  n: own Str;
  fn __init__(self: mut, n: own Str) { self.n = mv n; }
  fn name(self) -> own Str { return cp self.n; }
}
fn main() -> int {
  p: own = P("a");
  let q: own P = cp p;
  s: Str = q.name();
  k: own int = 1;
  return mv k - cp k;
})"),
                   /*ownership=*/true);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Access checks (view / mut / own)

namespace {

// A class with an own, a mut and a view field, and the functions the cases
// below call.  Each case is the body of main after it.
const char *kPrelude = R"(
class P {
  xs: own int[];
  m: mut Str[];
  v: Str[];
  fn __init__(a: mut Str[]) { self.xs = [0]; self.m = a; self.v = a; }
  fn set(self: mut, k: int) { self.xs[0] = cp k; }
  fn view(self) -> Str[] { return self.m; }
  fn grab(self: mut) -> mut Str[] { return self.m; }
}
fn fill(a: mut Str[]) { a.push("x"); }
fn bump(n: mut int) { n = n + 1; }
fn main() -> int {
)";

SemaResult ownershipMain(const std::string &body) {
  return semaRun(parseOwnership(kPrelude + body + "\nreturn 0;\n}"),
                 /*ownership=*/true);
}

void expectOwnershipError(const std::string &src, const std::string &msg) {
  auto r = semaRun(parseOwnership(src), /*ownership=*/true);
  EXPECT_FALSE(r.Ok) << src;
  EXPECT_NE(r.Diagnostics.find("error: " + msg), std::string::npos)
      << src << "\n"
      << r.Diagnostics;
}

} // namespace

TEST(Ownership, ChangeableAccessIsAccepted) {
  const char *bodies[] = {
      // own local, own and mut fields, a mut self method, mut parameters
      "p: own = P([]); p.xs[0] = 1; p.xs.push(2); p.xs.pop(); p.set(3);",
      "p: own = P([]); p.m.push(\"z\"); fill(p.m); p.m[0] = \"y\";",
      "p: own = P([]); p.v = p.m; q: mut = p; q.set(1); fill(p.grab());",
      "a: own Str[] = []; b: mut = a; fill(b); b[0] = \"y\"; b = a;",
      "k: own int = 1; bump(k); p: own = P([]); bump(p.xs[0]);",
      // fresh values are changeable; a view may be rebound
      "fill([]); P([]).set(1); a = [\"a\"]; a = [\"b\"]; let x = 1; y = x;",
      "f = Stdin; s = Str(1); return s.len() - 1;",
  };
  for (const char *body : bodies) {
    auto r = ownershipMain(body);
    EXPECT_TRUE(r.Ok) << body << "\n" << r.Diagnostics;
  }
}

TEST(Ownership, ChangingThroughAViewIsAnError) {
  const std::pair<const char *, const char *> cases[] = {
      {"p: own = P([]); q = p; q.xs = [];",
       "cannot change field 'xs' through read-only 'q'"},
      {"p: own = P([]); q = p; q.xs[0] = 1;",
       "cannot change an element through read-only 'q'"},
      {"p: own = P([]); p.v[0] = \"a\";",
       "cannot change an element through read-only 'p.v'"},
      {"p: own = P([]); p.v.push(\"a\");",
       "cannot change an array with 'push' through read-only 'p.v'"},
      {"p: own = P([]); q = p; q.xs.pop();",
       "cannot change an array with 'pop' through read-only 'q'"},
      {"p: own = P([]); q = p; q.set(1);",
       "cannot change an object with 'set' through read-only 'q'"},
      {"p: own = P([]); p.view().push(\"a\");",
       "cannot change an array with 'push' through a read-only value"},
      {"p: own = P([]); q = p; q.m.push(\"a\");",
       "cannot change an array with 'push' through read-only 'q'"},
      {"f = Stdin; g = f; g.write(\"a\");",
       "cannot change a file with 'write' through read-only 'g'"},
      {"i = 1; bump(i);",
       "cannot change 'mut' parameter 'n' through read-only 'i'"},
      {"bump(1);",
       "argument 1 of 'bump' must be a variable, field or element: parameter "
       "'n' is 'mut'"},
  };
  for (const auto &[body, msg] : cases)
    expectOwnershipError(kPrelude + std::string(body) + "\nreturn 0;\n}", msg);
}

TEST(Ownership, MutAccessOnlyNarrows) {
  const std::string through =
      "cannot give 'mut' access to an object reached through ";
  const std::pair<const char *, std::string> cases[] = {
      {"p: own = P([]); fill(p.v);", through + "read-only 'p.v'"},
      {"p: own = P([]); q = p; m: mut = q.m;", through + "read-only 'q'"},
      {"p: own = P([]); m: mut Str[] = p.m; m = p.v;",
       through + "read-only 'p.v'"},
      {"p: own = P([]); q: own = P(p.v);", through + "read-only 'p.v'"},
      {"p: own = P([]); p.m = p.view();", through + "a read-only value"},
  };
  for (const auto &[body, msg] : cases)
    expectOwnershipError(kPrelude + std::string(body) + "\nreturn 0;\n}", msg);
  expectOwnershipError("fn f(a: Str[]) -> mut Str[] { return a; }\n"
                       "fn main() -> int { return 0; }",
                       through + "read-only 'a'");
  // `mut` parameters of __init__ (constructor and __super__) and methods.
  const std::string a = "class A {\n  fn __init__(a: mut Str[]) { }\n"
                        "  fn put(self, a: mut Str[]) { }\n}\n";
  expectOwnershipError(a + "class B : A { fn __init__(a: Str[]) { "
                           "__super__(a); } }\nfn main() -> int { return 0; }",
                       through + "read-only 'a'");
  expectOwnershipError(a + "fn main() -> int { b: own Str[] = []; v = b; "
                           "A(b).put(v); return 0; }",
                       through + "read-only 'v'");
  auto r = semaRun(parseOwnership("fn f(a: mut Str[]) -> mut Str[] {\n"
                                  "  a.push(\"x\"); a[0] = \"y\"; return a;\n"
                                  "}\nfn main() -> int { return 0; }"),
                   /*ownership=*/true);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Ownership, ParametersAreViewsByDefault) {
  expectOwnershipError("fn f(a: Str[]) { a.push(\"x\"); }\n"
                       "fn main() -> int { return 0; }",
                       "cannot change an array with 'push' through read-only "
                       "'a'");
}

TEST(Ownership, LetCannotBeReassigned) {
  const std::string msg = "'x' is declared with 'let' and cannot be reassigned";
  expectOwnershipError("fn main() -> int { let x = 1; x = 2; return x; }", msg);
  expectOwnershipError(
      "fn main() -> int { let x = 1; x, y = (2, 3); return x; }", msg);
  // A shadowing declaration in an inner scope is a new variable.
  auto r = semaRun(
      parseOwnership("fn main() -> int { let x = 1; if (x > 0) { x: own = 2; "
                     "x = 3; } return x; }"),
      /*ownership=*/true);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Ownership, MutOnAValueLocalIsAWarning) {
  auto r = semaRun(parseOwnership("fn main() -> int { x: mut = 1; return x; }"),
                   /*ownership=*/true);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_NE(
      r.Diagnostics.find(
          "warning: 'mut' has no effect on local 'x' of value type 'int'"),
      std::string::npos)
      << r.Diagnostics;
}

TEST(Ownership, SelfQualifiers) {
  const char *cls =
      "class C {\n  x: own int;\n  fn __init__() { self.x = 0; }\n";
  const std::pair<const char *, const char *> cases[] = {
      {"fn bad(self) { self.x = 1; }",
       "cannot change field 'x' through read-only 'self'"},
      {"fn bad() { self.x = 1; }",
       "cannot change field 'x' through read-only 'self'"},
      {"fn a(self: mut) { }\nfn b(self) { self.a(); }",
       "cannot change an object with 'a' through read-only 'self'"},
      {"fn m(self: own) { }", "'self: own' is not supported"},
  };
  for (const auto &[method, msg] : cases)
    expectOwnershipError(
        cls + std::string(method) + "\n}\nfn main() -> int { return 0; }", msg);
  auto r = semaRun(parseOwnership(cls + std::string(R"(
  fn a(self: mut) { self.x = 1; }
  fn b(self: mut Self) { self.a(); }
}
fn main() -> int { c: own = C(); c.b(); return c.x; })")),
                   /*ownership=*/true);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Ownership, OverridesKeepTheSelfQualifier) {
  auto src = [](const char *base, const char *derived) {
    return std::string("class A { fn m(") + base +
           ") { } }\nclass B : A { fn m(" + derived +
           ") { } }\nfn main() -> int { return 0; }";
  };
  expectOwnershipError(src("self: mut", "self"),
                       "override of 'm' must keep 'self: mut'");
  expectOwnershipError(src("", "self: mut"),
                       "override of 'm' must keep 'self'");
  auto r = semaRun(parseOwnership(src("self: mut", "self: mut")),
                   /*ownership=*/true);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  r = semaRun(parseOwnership(src("", "self")), /*ownership=*/true);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Ownership, ChecksNeedTheFlag) {
  // The same program is accepted without --ownership (no qualifiers used).
  auto r = semaRun(parse("class C {\n  x: int;\n  fn __init__() { self.x = 0; }"
                         "\n  fn set() { self.x = 1; }\n}\nfn f(a: Str[]) { "
                         "a.push(\"x\"); a[0] = \"y\"; }\nfn main() -> int { c "
                         "= C(); c.set(); c.x = 2; return 0; }"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// -- Transfer: `own` needs `cp`, `mv` or a fresh value

namespace {

// Each case is the body of main after this prelude.
const char *kTransfer = R"(
class Q {
  s: own Str;
  ps: own Q[];
  v: Q[];
  fn __init__(s: own Str) { self.s = mv s; self.ps = []; self.v = []; }
  fn view(self) -> Str { return self.s; }
  fn copy(self) -> own Str { return cp self.s; }
}
fn take(q: own Q) { }
fn make() -> own Q { return Q("m"); }
fn main() -> int {
  q: own = Q("a");
  w = q;
  xs: own Str[] = [];
)";

std::string transferMain(const std::string &body) {
  return kTransfer + body + "\nreturn 0;\n}";
}

} // namespace

TEST(Ownership, FreshValuesNeedNoKeyword) {
  const char *bodies[] = {
      "a: own = Q(\"b\"); b: own = make(); c: own Str = q.copy();",
      "d: own = [1, 2]; e: own = (1, \"x\"); f: own = \"s\" + \"t\";",
      "g: own = cp w; h: own = mv q; take(Q(\"c\")); take(cp w);",
      "k: own int = 1 + 2; k = xs.len(); m: own = Str(3); m = cp w.s;",
      "xs.push(\"x\"); xs.push(cp w.s); xs[0] = q.copy(); q.ps.push(make());",
      "q.s = \"t\"; q.s = w.view() + \"\";",
      "n: own = if 1 > 0 then Q(\"x\") else make();",
  };
  for (const char *body : bodies) {
    auto r = semaRun(parseOwnership(transferMain(body)), /*ownership=*/true);
    EXPECT_TRUE(r.Ok) << body << "\n" << r.Diagnostics;
  }
}

TEST(Ownership, OwnNeedsCpOrMv) {
  const std::pair<const char *, const char *> cases[] = {
      {"a: own = w;", "'own' local 'a' needs 'cp' or 'mv': 'w' is not fresh"},
      {"a: own Q = make(); a = w;",
       "'own' local 'a' needs 'cp' or 'mv': 'w' is not fresh"},
      {"s: own = q.view();",
       "'own' local 's' needs 'cp' or 'mv': 'q.view()' is not fresh"},
      {"take(w);", "'own' parameter 'q' needs 'cp' or 'mv': 'w' is not fresh"},
      {"q.s = w.s;", "'own' field 's' needs 'cp' or 'mv': 'w.s' is not fresh"},
      {"xs.push(w.s);",
       "'own' element of 'xs' needs 'cp' or 'mv': 'w.s' is not fresh"},
      {"q.ps.push(w);",
       "'own' element of 'q.ps' needs 'cp' or 'mv': 'w' is not fresh"},
      {"xs[0] = w.s;",
       "'own' element of 'xs' needs 'cp' or 'mv': 'w.s' is not fresh"},
      {"i = 1; k: own int = i;",
       "'own' local 'k' needs 'cp' or 'mv': 'i' is not fresh"},
      {"a: own Q = make(); a, b = (w, 1);",
       "'own' local 'a' needs 'cp' or 'mv': 'w' is not fresh"},
  };
  for (const auto &[body, msg] : cases)
    expectOwnershipError(transferMain(body), msg);
  expectOwnershipError("fn f(s: Str) -> own Str { return s; }\n"
                       "fn main() -> int { return 0; }",
                       "'own' result needs 'cp' or 'mv': 's' is not fresh");
  // A view of a value escapes into an own field only as a copy.
  expectOwnershipError("class C {\n  n: own int;\n  fn __init__(n: int) { "
                       "self.n = n; }\n}\nfn main() -> int { return 0; }",
                       "'own' field 'n' needs 'cp' or 'mv': 'n' is not fresh");
  // Elements of a view or `mut` array are not owned: a link is stored.
  auto r = semaRun(parseOwnership(transferMain("q.v.push(w);") +
                                  "\nfn put(a: mut Q[], q: Q) { a.push(q); }"),
                   /*ownership=*/true);
  EXPECT_FALSE(r.Ok); // q.v is a view field: read-only
  r = semaRun(parseOwnership(transferMain("") +
                             "\nfn put(a: mut Q[], q: Q) { a.push(q); }"),
              /*ownership=*/true);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Ownership, CpNeedsACloneableValue) {
  expectOwnershipError("fn main() -> int { f: own = cp Stdin; return 0; }",
                       "cannot 'cp' a value of type 'File'");
  auto r = semaRun(
      parseOwnership("class C {\n  n: own int;\n  fn __init__() { self.n = 0; }"
                     "\n  fn twin(self) -> own C { return cp self; }\n}\n"
                     "fn main() -> int { c: own = C(); d: own = cp c.twin(); "
                     "t: own = cp (1, \"a\"); return cp c.n; }"),
      /*ownership=*/true);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Ownership, FieldAndResultKinds) {
  const std::string tail = "\nfn main() -> int { return 0; }";
  expectOwnershipError("class C { n: int; }" + tail,
                       "field 'n' of value type 'int' must be 'own'");
  expectOwnershipError("class C { b: mut bool?; }" + tail,
                       "field 'b' of value type 'bool?' must be 'own'");
  expectOwnershipError("fn f(n: mut int) -> mut int { return n; }" + tail,
                       "a value type result must be '-> own int'");
  expectOwnershipError("class C { fn f(self) -> mut float { return 1.0; } }" +
                           tail,
                       "a value type result must be '-> own float'");
  // `-> int` is `-> own int`: the value is copied out.
  auto r = semaRun(parseOwnership("class C { n: own int; s: Str; m: mut C?; "
                                  "}\nfn f(n: int) -> int { return n; }\n"
                                  "fn g(n: int) -> own int { return cp n; }" +
                                  tail),
                   /*ownership=*/true);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

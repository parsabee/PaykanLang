// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The lowering of `cp` (deep clone) and `mv` (hand-over) of the ownership
// prototype (--ownership, docs/design/ownership-proto.md), on every backend.
// Every test also checks that the program leaves no live heap block.

#include "CodeGenTestUtils.h"
#include "paykan/pir/Printer.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

// The spec's example classes (docs/design/ownership-proto.md).
const std::string kTeam = R"(
  class Person {
    name: own Str;
    fn __init__(self: mut, n: own Str) { self.name = mv n; }
    fn rename(self: mut, n: own Str) { self.name = mv n; }
  }
  class Team {
    members: own Person[];
    lead: mut Person;
    fn __init__(self: mut, l: mut Person) { self.members = []; self.lead = l; }
    fn add(self: mut, p: own Person) { self.members.push(mv p); }
    fn leader(self) -> Person { return self.lead; }
  }
)";

void expectRun(const std::string &src, const std::string &out,
               const char *label) {
  LeakGuard g;
  auto r = compileAndRunOwnership(src);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, out);
  g.expectNoLeaks(label);
}

} // namespace

// `mv` hands the box over without a retain and clears the old variable: the
// receiver is the only owner left, and the old slot releases nothing.
TEST(OwnershipProto, MoveTransfersAndClears) {
  expectRun(kTeam + R"(
    fn take(p: own Person) -> own Person { return mv p; }
    fn main() -> int {
      a: own = Person("Ana");
      b: own = mv a;
      c: own = take(mv b);
      println(c.name);
      a = Person("New");
      println(a.name);
      n: own int = 3;
      m: own int = mv n;
      println(Str<int>(m));
      return 0;
    }
  )",
            "Ana\nNew\n3\n", "MoveTransfersAndClears");
}

// `mv` loads the box and stores null into the old slot (no retain), so the
// old variable's scope cleanup releases nothing.
TEST(OwnershipProto, MoveClearsOldSlot) {
  auto a = detail::analyse(kTeam + R"(
    fn main() -> int {
      a: own = Person("Ana");
      b: own = mv a;
      return 0;
    }
  )",
                           "", "recursive-descent", true);
  ASSERT_TRUE(a.Ok) << a.Failure.StdErr;
  paykan::pir::Program program;
  std::ostringstream errs, text;
  ASSERT_TRUE(paykan::lowering::lowerProgram(a.Ctx, a.Driver->getRoot(), a.Path,
                                             "", program, errs))
      << errs.str();
  paykan::pir::print(program, text);
  std::string pir = text.str();
  std::string main = pir.substr(pir.find("fn @main"));
  main = main.substr(0, main.find("\n}\n"));
  EXPECT_NE(main.find("= load %a.0\n  store %a.0, null box\n"),
            std::string::npos)
      << main;
  EXPECT_EQ(main.find("retain"), std::string::npos) << main;
}

// `cp` of a class: `own` fields are cloned, `mut` fields shared.
TEST(OwnershipProto, CopyClassDeepAndShared) {
  expectRun(kTeam + R"(
    fn main() -> int {
      ana: own = Person("Ana");
      t: own = Team(ana);
      t.add(Person("Bo"));
      t.add(cp ana);
      u: own = cp t;
      u.lead.rename("Ann");
      println(t.lead.name);
      println(t.leader().name);
      u.members[1].rename("Anna");
      u.add(Person("Cy"));
      println(t.members[1].name);
      println(u.members[1].name);
      println(Str<int>(t.members.len()));
      println(Str<int>(u.members.len()));
      w: own = mv t;
      println(w.lead.name);
      return 0;
    }
  )",
            "Ann\nAnn\nAna\nAnna\n2\n3\nAnn\n", "CopyClassDeepAndShared");
}

// `cp` of a Str copies its bytes; `cp` of an array clones object elements
// and copies primitive ones.
TEST(OwnershipProto, CopyStrAndArrays) {
  expectRun(kTeam + R"(
    fn main() -> int {
      a: own = "ab";
      c: own = cp a;
      c.concat("!");
      println(a + c);
      ps: own Person[] = [Person("A"), Person("B")];
      qs: own = cp ps;
      qs[0].rename("Z");
      qs.push(Person("C"));
      println(ps[0].name + qs[0].name);
      println(Str<int>(ps.len()) + Str<int>(qs.len()));
      xs: own int[] = [1, 2];
      ys: own = cp xs;
      ys[0] = 9;
      println(Str<int>(xs[0]) + Str<int>(ys[0]));
      ss: own Str[][] = [["a"], ["b"]];
      tt: own = cp ss;
      tt[1][0].concat("!");
      println(ss[1][0] + tt[1][0]);
      return 0;
    }
  )",
            "abab!\nAZ\n23\n19\nbb!\n", "CopyStrAndArrays");
}

// `cp` through a base-typed view clones the dynamic class, with the fields
// of every level; tuples, optionals and `Obj` clone through the same path.
TEST(OwnershipProto, CopyDynamicClassTupleOptional) {
  expectRun(R"(
    class Base {
      n: own int;
      fn __init__(self: mut) { self.n = 1; }
      fn name(self) -> Str { return "Base"; }
    }
    class Derived : Base {
      s: own Str;
      fn __init__(self: mut) { __super__(); self.s = "d"; }
      fn name(self) -> Str { return "Derived" + Str<int>(self.n) + self.s; }
    }
    fn main() -> int {
      d: own = Derived();
      d.n = 7;
      b: Base = d;
      c: own = cp b;
      d.n = 8;
      println(c.name());
      t: own (Str, int) = ("x", 2);
      t2: own = cp t;
      t2.0.concat("y");
      println(t.0 + t2.0 + Str<int>(t2.1));
      o: own Derived? = None;
      o2: own = cp o;
      println(o2);
      x: Obj = d;
      y: own = cp x;
      println(Str<bool>(y == x));
      return 0;
    }
  )",
            "Derived7d\nxxy2\nNone\nFalse\n", "CopyDynamicClassTupleOptional");
}

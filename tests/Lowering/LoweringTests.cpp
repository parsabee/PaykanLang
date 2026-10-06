// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// AST -> PIR lowering tests: every program is lowered, verified, printed, and
// the printed PIR is checked for the ownership shapes the lowering must
// produce (docs/pir.md §7).  Behavioural parity with the LLVM backend is
// covered by the CodeGen suite run on every backend.

#include "ModuleName.h"
#include "TestUtils.h"
#include "paykan/lowering/Lowering.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace paykan;
using namespace paykan::test;

namespace {

struct Lowered {
  bool Ok = false;
  std::string Error;
  std::string Text; // printed PIR of the whole program
  pir::Program Program;
};

/// Parse, check, lower, verify and print @p source (@p projectRoot for
/// imports).
Lowered lower(const std::string &source, const std::string &projectRoot = "") {
  Lowered l;
  auto [parseOk, driver] = parse(source);
  if (!parseOk) {
    l.Error = "parse error";
    return l;
  }
  std::ostringstream diagOS;
  sema::DiagEngine diag(diagOS);
  diag.setSourceInfo(driver->getCurrentFile(), &driver->getSourceLines());
  sema::Sema sema(driver->getASTContext(), diag, projectRoot);
  auto ctx = sema.run(driver->getRoot());
  if (!ctx) {
    l.Error = diagOS.str();
    return l;
  }
  std::ostringstream errs;
  if (!lowering::lowerProgram(ctx, driver->getRoot(), "main.pkn", projectRoot,
                              l.Program, errs)) {
    l.Error = "lowering failed: " + errs.str();
    return l;
  }
  auto errors = pir::verify(l.Program);
  if (!errors.empty()) {
    l.Error = "verifier: " + pir::formatErrors(errors) + "\n" +
              pir::toString(l.Program);
    return l;
  }
  l.Text = pir::toString(l.Program);
  l.Ok = true;
  return l;
}

/// Number of occurrences of @p needle in @p text.
size_t count(const std::string &text, const std::string &needle) {
  size_t n = 0;
  for (size_t pos = text.find(needle); pos != std::string::npos;
       pos = text.find(needle, pos + needle.size()))
    ++n;
  return n;
}

/// The printed body of function @p name ("fn @name(...)" up to the closing
/// brace at column 0).
std::string function(const std::string &text, const std::string &name) {
  size_t start = text.find("fn @" + name + "(");
  if (start == std::string::npos)
    return "";
  size_t end = text.find("\n}\n", start);
  return text.substr(start, end == std::string::npos ? std::string::npos
                                                     : end + 3 - start);
}

} // namespace

// -- Variables, scope cleanup, strings

TEST(Lowering, OwnedVariablesAreReleasedInReverseOrderAtScopeExit) {
  auto l = lower(R"(
    fn main() -> int {
      a: Str = "a";
      b: Str = "b";
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  // Each literal: PaykanString_new, boxed into the variable's slot.
  EXPECT_EQ(count(m, "call @$rt.PaykanString_new("), 2u) << m;
  EXPECT_EQ(count(m, " = box "), 2u) << m;
  // Two releases before `ret`: b (declared last) first, then a.
  size_t relA = m.rfind("release");
  size_t relB = m.rfind("release", relA - 1);
  size_t ret = m.find("ret 0");
  ASSERT_NE(relA, std::string::npos);
  ASSERT_NE(relB, std::string::npos);
  EXPECT_LT(relB, relA);
  EXPECT_LT(relA, ret);
  size_t loadB = m.rfind("load %b", relB);
  size_t loadA = m.rfind("load %a", relA);
  EXPECT_NE(loadB, std::string::npos) << m;
  EXPECT_NE(loadA, std::string::npos) << m;
  EXPECT_LT(loadB, loadA);
}

TEST(Lowering, StringTemporaryPassedToBuiltinIsDestroyedAfterTheCall) {
  auto l = lower(R"(
    fn main() -> int { println("hi"); return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  size_t newStr = m.find("call @$rt.PaykanString_new(");
  size_t println = m.find("call @$rt.Paykan_println(");
  size_t destroy = m.find("call @$rt.PaykanString_destroy(");
  ASSERT_NE(newStr, std::string::npos) << m;
  ASSERT_NE(println, std::string::npos) << m;
  ASSERT_NE(destroy, std::string::npos) << m;
  EXPECT_LT(newStr, println);
  EXPECT_LT(println, destroy);
  // No box is created for a borrowed temporary.
  EXPECT_EQ(count(m, " = box "), 0u) << m;
}

TEST(Lowering, ConcatReleasesOwnedOperandsAndTracksTheResult) {
  auto l = lower(R"(
    fn main() -> int {
      s: Str = "a" + Str<int>(1);
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  // Literal and Str<int> temporaries are destroyed after the concat; the
  // concat result is boxed into `s` and released at scope exit.
  EXPECT_EQ(count(m, "call @$rt.PaykanString_destroy("), 2u) << m;
  size_t concat = m.find("call @$rt.PaykanString_concat(");
  size_t box = m.find(" = box ");
  ASSERT_NE(concat, std::string::npos) << m;
  ASSERT_NE(box, std::string::npos) << m;
  EXPECT_LT(concat, box);
  EXPECT_EQ(count(m, "release"), 1u) << m;
}

TEST(Lowering, ReturnReleasesEveryScopeAfterEvaluatingTheValue) {
  auto l = lower(R"(
    fn pick(flag: bool) -> Str {
      a: Str = "x";
      if (flag) {
        b: Str = "y";
        return b;
      }
      return a;
    }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "pick");
  // `return b` inside the if: retain b (the return value is +1), then
  // release b (inner scope), release a (outer scope), release flag? no:
  // bool is not a box.  Then ret.
  size_t ifPos = f.find("if %");
  ASSERT_NE(ifPos, std::string::npos) << f;
  std::string inner = f.substr(ifPos, f.find("\n  }", ifPos) - ifPos);
  EXPECT_EQ(count(inner, "retain"), 1u) << inner;
  EXPECT_EQ(count(inner, "release"), 2u) << inner;
  EXPECT_NE(inner.find("ret %"), std::string::npos) << inner;
}

// -- mov

TEST(Lowering, MovOfOwnedVariableNullsTheSlotAndSkipsTheRetain) {
  auto l = lower(R"(
    fn take(s: Str) { println(s); }
    fn main() -> int {
      s: Str = "x";
      take(mov s);
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  // The slot is nulled right after its box is loaded for the call.
  EXPECT_NE(m.find("store %s"), std::string::npos) << m;
  EXPECT_EQ(count(m, "null box"), 1u) << m;
  EXPECT_EQ(count(m, "retain"), 0u) << m;
  // Scope exit still releases the (now null) slot: exactly one release.
  EXPECT_EQ(count(m, "release"), 1u) << m;
  // The callee owns its parameter and releases it.
  std::string t = function(l.Text, "take");
  EXPECT_EQ(count(t, "release"), 1u) << t;
}

// -- Calls: callee-consumes ABI for user functions, borrow for builtins

TEST(Lowering, UserCallRetainsAVariableArgument) {
  auto l = lower(R"(
    fn use(s: Str) -> int { return 1; }
    fn main() -> int {
      s: Str = "x";
      n = use(s);
      return n;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  size_t retain = m.find("retain");
  size_t call = m.find("call @use(");
  ASSERT_NE(retain, std::string::npos) << m;
  ASSERT_NE(call, std::string::npos) << m;
  EXPECT_LT(retain, call);
  // `n` is an i64 local: no box for it.
  EXPECT_NE(m.find("local %n"), std::string::npos) << m;
}

TEST(Lowering, FreshCallResultIsStoredWithoutRetain) {
  auto l = lower(R"(
    fn mk() -> Str { return "x"; }
    fn main() -> int {
      s = mk();
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_EQ(count(m, "retain"), 0u) << m;
  EXPECT_EQ(count(m, "release"), 1u) << m;
}

TEST(Lowering, IntegerDivisionIsGuarded) {
  auto l = lower(R"(
    fn main() -> int { a: int = 7; b: int = 2; return a / b + a % b; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_EQ(count(m, "call @$rt.Paykan_panic_div_by_zero()"), 2u) << m;
  EXPECT_EQ(count(m, "call @$rt.Paykan_panic_div_overflow()"), 1u) << m;
  EXPECT_EQ(count(m, "unreachable"), 3u) << m;
  EXPECT_NE(m.find(" = div "), std::string::npos) << m;
  EXPECT_NE(m.find(" = rem "), std::string::npos) << m;
}

TEST(Lowering, FloatComparisonsAreSingleCmpInstructions) {
  // Every float comparison is one `cmp` with the operator's predicate; `!=`
  // in particular is `cmp ne` (unordered on f64 by definition, docs/pir.md),
  // not an ordered `lt || gt` expansion or a negated `eq`.
  auto l = lower(R"(
    fn f(a: float, b: float) -> int {
      r = 0;
      if (a != b) { r = r + 1; }
      if (a == b) { r = r + 2; }
      if (a < b) { r = r + 4; }
      if (a <= b) { r = r + 8; }
      if (a > b) { r = r + 16; }
      if (a >= b) { r = r + 32; }
      return r;
    }
    fn main() -> int { return f(1.0, 2.0); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "f");
  for (const char *pred : {"ne", "eq", "lt", "le", "gt", "ge"})
    EXPECT_EQ(count(f, std::string("= cmp ") + pred + " %a"), 1u)
        << pred << "\n"
        << f;
  EXPECT_EQ(count(f, " = not "), 0u) << f;
  EXPECT_EQ(count(f, " = cmp "), 6u) << f;
}

// -- Classes: layout, vtable, constructor, destructor, fields

TEST(Lowering, ClassItemCarriesFlattenedLayoutAndVTable) {
  auto l = lower(R"(
    class Base {
      name: Str;
      fn __init__(name: Str) { self.name = name; }
      fn describe() -> Str { return self.name; }
    }
    class Derived : Base {
      n: int;
      fn __init__(name: Str, n: int) { __super__(name); self.n = n; }
      fn describe() -> Str { return "derived"; }
      fn extra() -> int { return self.n; }
    }
    fn main() -> int { d = Derived("d", 1); return d.extra(); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  const std::string &t = l.Text;
  EXPECT_NE(
      t.find("class Derived : Base {\n  field name: box\n  field n: i64\n"),
      std::string::npos)
      << t;
  EXPECT_NE(t.find("destroy = @Derived.destroy : (obj) -> void"),
            std::string::npos)
      << t;
  EXPECT_NE(t.find("describe = @Derived.describe : (obj) -> box"),
            std::string::npos)
      << t;
  EXPECT_NE(t.find("extra = @Derived.extra : (obj) -> i64"), std::string::npos)
      << t;
  // Inherited runtime slots name the runtime implementation.
  EXPECT_NE(t.find("equals = @$rt.PaykanObject_equals : (obj, box) -> i64"),
            std::string::npos)
      << t;
  // The constructor allocates, boxes BEFORE __init__, and returns the box.
  std::string ctor = function(t, "Derived");
  size_t nw = ctor.find(" = new Derived");
  size_t bx = ctor.find(" = box ");
  size_t init = ctor.find("call @Derived.__init__(");
  ASSERT_NE(nw, std::string::npos) << ctor;
  ASSERT_NE(bx, std::string::npos) << ctor;
  ASSERT_NE(init, std::string::npos) << ctor;
  EXPECT_LT(nw, bx);
  EXPECT_LT(bx, init);
  // __super__ passes a +1 box of the argument.
  std::string init2 = function(t, "Derived.__init__");
  EXPECT_NE(init2.find("call @Base.__init__("), std::string::npos) << init2;
  // The destructor releases the ref-typed field and frees the struct.
  std::string dtor = function(t, "Derived.destroy");
  EXPECT_NE(dtor.find("field.load %"), std::string::npos) << dtor;
  EXPECT_EQ(count(dtor, "release"), 1u) << dtor;
  EXPECT_NE(dtor.find("free %"), std::string::npos) << dtor;
  // Virtual dispatch names the class and the slot.
  std::string m = function(t, "main");
  EXPECT_NE(m.find("vcall %"), std::string::npos) << m;
  EXPECT_NE(m.find(" : Derived ["), std::string::npos) << m;
}

TEST(Lowering, FieldStoreRetainsNewAndReleasesOldValue) {
  auto l = lower(R"(
    class H { s: Str; fn __init__() { self.s = "a"; } }
    fn main() -> int {
      h = H();
      x: Str = "b";
      h.s = x;
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  size_t store = m.find("field.store %");
  ASSERT_NE(store, std::string::npos) << m;
  std::string before = m.substr(0, store);
  EXPECT_NE(before.find("retain"), std::string::npos) << m;
  EXPECT_NE(before.find("field.load %"), std::string::npos) << m;
  // The old value is released only when non-null.
  EXPECT_NE(before.find("cmp eq"), std::string::npos) << m;
  EXPECT_NE(before.find("release"), std::string::npos) << m;
}

TEST(Lowering, CallRootedFieldReadRetainsTheFieldAndReleasesTheReceiver) {
  auto l = lower(R"(
    class H { s: Str; fn __init__() { self.s = "a"; } }
    fn mk() -> H { return H(); }
    fn main() -> int {
      t: Str = mk().s;
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  size_t call = m.find("call @mk()");
  size_t load = m.find("field.load %", call);
  size_t retain = m.find("retain", load);
  size_t release = m.find("release", retain);
  ASSERT_NE(call, std::string::npos) << m;
  ASSERT_NE(load, std::string::npos) << m;
  ASSERT_NE(retain, std::string::npos) << m;
  ASSERT_NE(release, std::string::npos) << m;
  // The field is retained before the temporary receiver is released, and
  // the stored value is not retained a second time.
  EXPECT_EQ(count(m, "retain"), 1u) << m;
}

// -- match

// #72: a string-literal subject is built as a Str (not passed as its raw C
// string) and released on every exit of the match.
TEST(Lowering, StringLiteralMatchSubjectIsAStrReleasedOnEveryExit) {
  auto l = lower(R"(
    fn f() -> int {
      match "s" { "t" { return 1; } _ { return 2; } }
    }
    fn main() -> int {
      match "s" { "t" { println("t"); } _ { println("other"); } }
      return f();
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error; // the verifier ran
  std::string m = function(l.Text, "main");
  // The subject: a Str built from the literal, boxed for the match.
  size_t subj = m.find("%str.1 = call @$rt.PaykanString_new(@.str");
  size_t box = m.find("%subj.box.2 = box %str.1");
  size_t eq = m.find("call @$rt.PaykanString_equals(%str.1, ");
  ASSERT_NE(subj, std::string::npos) << m;
  ASSERT_NE(box, std::string::npos) << m;
  ASSERT_NE(eq, std::string::npos) << m;
  EXPECT_LT(subj, box) << m;
  EXPECT_LT(box, eq) << m;
  EXPECT_EQ(count(m, "release %subj.box."), 1u) << m;
  std::string fn = function(l.Text, "f");
  EXPECT_EQ(count(fn, "release %subj.box."), 2u) << fn; // both returns
}

TEST(Lowering, ClassMatchComparesVTablesAndBindsAnOwnedVariable) {
  auto l = lower(R"(
    class A { fn __init__() {} }
    class B : A { fn __init__() { __super__(); } }
    fn mk() -> A { return B(); }
    fn main() -> int {
      match mk() {
        b: B { println("b"); }
        A { println("a"); }
        _ { println("other"); }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find("vtable.load %"), std::string::npos) << m;
  EXPECT_NE(m.find("vtable.addr B"), std::string::npos) << m;
  EXPECT_NE(m.find("vtable.addr A"), std::string::npos) << m;
  // Nested if / else chain with the wildcard in the innermost else.
  EXPECT_EQ(count(m, "if %is."), 2u) << m;
  // The binding is an owned box local: the arm retains the subject box
  // (a call result the match owns) and releases it at arm exit; the match
  // releases the subject itself once, at the end.
  size_t bLocal = m.find("local %b");
  ASSERT_NE(bLocal, std::string::npos) << m;
  EXPECT_NE(m.find(": box", bLocal), std::string::npos) << m;
  EXPECT_EQ(count(m, "retain"), 1u) << m;
  EXPECT_EQ(count(m, "release"), 2u) << m;
}

TEST(Lowering, MatchBindingOnAVariableSubjectAcquiresItsBox) {
  auto l = lower(R"(
    class A { v: int; fn __init__(v: int) { self.v = v; } }
    fn main() -> int {
      x: A = A(1);
      match x {
        a: A { println(Str<int>(a.v)); }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  // The binding acquires the subject's unique box (+1) and owns it, so both
  // x and a are released: a at arm exit, x at function exit.
  EXPECT_NE(m.find("= box %"), std::string::npos) << m;
  EXPECT_EQ(count(m, "release"), 2u) << m;
}

// A binding re-assigned on only some paths used to be "promoted" to a
// second slot at lowering time, so the paths that skipped the assignment read
// a null box (crash), and promotion inside a loop leaked.  The binding is now
// an ordinary owned variable: re-assignment releases the old box and stores
// the new one into the same slot, with no twin slot.
TEST(Lowering, ReassigningAMatchBindingIsAnOrdinaryRebind) {
  auto l = lower(R"(
    class A { v: int; fn __init__(v: int) { self.v = v; } }
    fn main() -> int {
      o: A? = A(7);
      k: int = 0;
      match o {
        a: A {
          if (k == 1) { a = A(1); }
          i: int = 0;
          while (i < 3) { a = A(i); i = i + 1; }
          println(Str<int>(a.v));
        }
        None { }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_EQ(m.find("local %a.box"), std::string::npos) << m;
  EXPECT_EQ(count(m, "local %a."), 1u) << m; // exactly one slot for `a`
}

// Re-assigning the subject inside an arm must not free the object the binding
// still refers to: the binding holds its own reference.
TEST(Lowering, ReassigningTheSubjectInsideAnArmKeepsTheBindingAlive) {
  auto l = lower(R"(
    class A { fn __init__() {} fn name() -> Str { return "A"; } }
    class B : A { fn __init__() { __super__(); } fn name() -> Str { return "B"; } }
    fn main() -> int {
      x: A = B();
      match x {
        b: B { x = A(); println(b.name()); }
        _ { }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  // The arm acquires its own reference before the body runs, so the release
  // of the old x by `x = A()` cannot drop the object to zero.
  size_t acquire = m.find("= box %");
  size_t rebind = m.find("call @A(");
  ASSERT_NE(acquire, std::string::npos) << m;
  ASSERT_NE(rebind, std::string::npos) << m;
  EXPECT_LT(acquire, rebind) << m;
}

TEST(Lowering, OptionalMatchTestsNullBeforeTheVTable) {
  auto l = lower(R"(
    class N { v: int; fn __init__(v: int) { self.v = v; } }
    fn find(f: bool) -> N? { if (f) { return N(1); } return None; }
    fn main() -> int {
      match find(True) {
        n: N { println(Str<int>(n.v)); }
        None { println("none"); }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  size_t isNone = m.find("cmp eq %");
  size_t vt = m.find("vtable.load %");
  ASSERT_NE(isNone, std::string::npos) << m;
  // The arm naming the inner type itself matches every non-None value: no
  // vtable test is needed at all.
  EXPECT_EQ(vt, std::string::npos) << m;
  EXPECT_NE(m.find("null obj"), std::string::npos) << m;
}

TEST(Lowering, ValueMatchOnStrBoxesTheLiteralForEquals) {
  auto l = lower(R"(
    fn main() -> int {
      s: Str = "b";
      match s {
        "a" { return 1; }
        "b" { return 2; }
        _ { return 3; }
      }
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_EQ(count(m, "call @$rt.PaykanString_equals("), 2u) << m;
  EXPECT_EQ(count(m, " = box "), 3u) << m; // s itself and the two literals
}

TEST(Lowering, EnumMatchComparesConstants) {
  auto l = lower(R"(
    enum Color { Red, Green }
    fn main() -> int {
      c: Color = Color::Green;
      match c { Red { return 1; } Green { return 2; } }
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_EQ(count(m, "cmp eq %"), 2u) << m; // one test per variant arm
  EXPECT_EQ(count(m, "release"), 0u) << m;  // no boxes anywhere
}

// -- Arrays, tuples, optionals

TEST(Lowering, ConstantPrimitiveArrayLiteralUsesADataGlobal) {
  auto l = lower(R"(
    fn main() -> int { xs = [1, 2, 3]; return xs.len(); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  EXPECT_NE(l.Text.find("data @.arr.data0 = [1, 2, 3]"), std::string::npos)
      << l.Text;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find("call @$rt.PaykanArray_new_from_data(3, @.arr.data0)"),
            std::string::npos)
      << m;
  // len is a vtable slot of the runtime Array class (explicit signature).
  EXPECT_NE(m.find("vcall %"), std::string::npos) << m;
}

TEST(Lowering, ObjectArrayLiteralSetsAndReleasesEachElement) {
  auto l = lower(R"(
    fn main() -> int { xs = ["a", "b"]; return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find("call @$rt.PaykanArray_new_obj(2)"), std::string::npos) << m;
  EXPECT_EQ(count(m, "call @$rt.PaykanArray_set_obj("), 2u) << m;
  // set_obj retains: each +1 element box is released, plus xs at scope exit.
  EXPECT_EQ(count(m, "release"), 3u) << m;
}

TEST(Lowering, AssigningAnObjectElementEvaluatesTheIndexOnce) {
  // Issue #52: `x = arr[idx()]` used to evaluate the subscript once for the
  // value and again for the ownership handling.
  auto l = lower(R"(
    class C { v: int; fn __init__(v: int) { self.v = v; } }
    fn idx() -> int { println("idx called"); return 0; }
    fn mk() -> C[] { return [C(3)]; }
    fn main() -> int {
      arr: C[] = [C(1), C(2)];
      x: C = C(9);
      x = arr[idx()];
      y: C = arr[idx()];
      z = arr[idx()];
      x = mk()[idx()];
      return x.v + y.v + z.v;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_EQ(count(m, "call @idx()"), 4u) << m;
  EXPECT_EQ(count(m, "call @mk()"), 1u) << m;
  EXPECT_EQ(count(m, "call @$rt.PaykanArray_get("), 4u) << m;
  // Each element read by a declared owner retains the stored box once
  // (x from arr, y, x from the temporary array); z's first assignment
  // acquires it with a single `box`.
  EXPECT_EQ(count(m, "retain"), 3u) << m;
}

TEST(Lowering, SideEffectingReceiversAreEvaluatedOnce) {
  // The other ownership paths that take a value's box: a call-rooted field
  // read, `mov` of a temporary, an element passed to a ref parameter and an
  // element stored into an array slot or a field.
  auto l = lower(R"(
    class C { v: int; fn __init__(v: int) { self.v = v; } }
    class H { c: C; fn __init__(c: C) { self.c = c; } }
    fn idx() -> int { println("idx"); return 0; }
    fn mkh() -> H { return H(C(5)); }
    fn take(c: C) -> int { return c.v; }
    fn main() -> int {
      arr: C[] = [C(1), C(2)];
      x: Obj = C(9);
      x = mkh().c;
      w: C = mkh().c;
      x = mov mkh();
      arr[idx()] = arr[idx()];
      n: int = take(arr[idx()]);
      h: H = H(C(3));
      h.c = arr[idx()];
      return n;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_EQ(count(m, "call @mkh()"), 3u) << m;
  EXPECT_EQ(count(m, "call @idx()"), 4u) << m;
}

TEST(Lowering, ArrayMatchArmBindingDispatchesThroughTheArrayType) {
  // The binding of an `arr: Str[]` arm has the array type: `len` is a slot
  // of the runtime Array class, not of the specialized `Array<Str>` key.
  auto l = lower(R"(
    fn main() -> int {
      x: Obj = ["hello", "world"];
      match x {
        arr: Str[] { arr.push("!"); println(arr[2]); return arr.len(); }
        _ { }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  // `len` is a runtime-class slot (explicit signature), not a slot of a
  // `"Array<Str>"` class item, which would have no slots.
  EXPECT_NE(m.find(" : (obj) -> i64 [3] ()"), std::string::npos) << m;
  EXPECT_EQ(m.find(" : \"Array<Str>\" ["), std::string::npos) << m;
  EXPECT_NE(m.find("call @$rt.PaykanArray_push_obj("), std::string::npos) << m;
}

TEST(Lowering, TupleLiteralUsesAKindsDescriptor) {
  auto l = lower(R"(
    fn main() -> int { t = (1, "s"); return t.0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  EXPECT_NE(l.Text.find("bytes @.tuple.kinds0 = [0, 4]"), std::string::npos)
      << l.Text;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find("call @$rt.PaykanTuple_new(2, @.tuple.kinds0)"),
            std::string::npos)
      << m;
  EXPECT_NE(m.find("call @$rt.PaykanTuple_set("), std::string::npos) << m;
  EXPECT_NE(m.find("call @$rt.PaykanTuple_set_obj("), std::string::npos) << m;
  EXPECT_NE(m.find("call @$rt.PaykanTuple_get("), std::string::npos) << m;
}

TEST(Lowering, OptionalIntoObjSubstitutesTheNoneSingleton) {
  auto l = lower(R"(
    class N { fn __init__() {} }
    fn find() -> N? { return None; }
    fn main() -> int { o: Obj = find(); println(o); return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find("cmp eq %"), std::string::npos) << m;
  EXPECT_NE(m.find("box @$rt.PaykanObject_None"), std::string::npos) << m;
  EXPECT_NE(l.Text.find("extern obj @$rt.PaykanObject_None"), std::string::npos)
      << l.Text;
}

// -- Modules

TEST(Lowering, ImportedFunctionsAndClassesAreExternItems) {
  auto dir = std::filesystem::temp_directory_path() /
             ("lowering_imports_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  {
    std::ofstream lib(dir / "lib.pkn");
    lib << "class Adder { n: int; fn __init__(n: int) { self.n = n; } "
           "fn add(x: int) -> int { return self.n + x; } }\n"
           "fn twice(x: int) -> int { return x * 2; }\n";
  }
  auto l = lower("import lib;\n"
                 "fn main() -> int { a = lib::Adder(1); return "
                 "a.add(lib::twice(2)) + a.n; }\n",
                 dir.string());
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(l.Ok) << l.Error;
  ASSERT_EQ(l.Program.Modules.size(), 2u);
  // Modules are named by their canonical module names, not paths (#102).
  EXPECT_EQ(l.Program.Modules[0].Name, "main");
  EXPECT_EQ(l.Program.Modules[1].Name, "lib");
  const std::string &t = l.Text;
  // The importer declares what it uses from lib, keyed to lib's module and
  // named as the call sites qualify it; `symbol` is lib's own name for it.
  EXPECT_NE(t.find("extern fn @\"lib::twice\"(i64) -> i64 module \"lib\" "
                   "symbol @twice\n"),
            std::string::npos)
      << t;
  EXPECT_NE(t.find("extern fn @\"lib::Adder\"(i64) -> box module \""),
            std::string::npos)
      << t;
  EXPECT_NE(t.find("extern class Adder module \""), std::string::npos) << t;
  std::string m = function(t, "main");
  EXPECT_NE(m.find("call @\"lib::twice\"("), std::string::npos) << m;
  EXPECT_NE(m.find("call @\"lib::Adder\"("), std::string::npos) << m;
  EXPECT_NE(m.find("field.load %"), std::string::npos) << m;
  // lib defines the class and its functions once.
  EXPECT_NE(t.find("class Adder {"), std::string::npos) << t;
  EXPECT_NE(t.find("fn @Adder.add("), std::string::npos) << t;
}

// #102: every module is named by its canonical module name -- the module
// path, never the file's (absolute) path -- and two modules with the same
// stem in different directories keep distinct names.  A module imported under
// two qualifiers is one module.
TEST(Lowering, ModulesAreNamedByTheirCanonicalModuleName) {
  auto dir = std::filesystem::temp_directory_path() /
             ("lowering_canon_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir / "a");
  std::filesystem::create_directories(dir / "b" / "c");
  std::ofstream(dir / "a" / "util.pkn") << "fn tag() -> int { return 1; }\n";
  std::ofstream(dir / "b" / "c" / "util.pkn")
      << "class K { v: int; fn __init__() { self.v = 2; } }\n"
         "fn tag() -> int { return 2; }\n";
  auto l = lower("import a::util; import b::c::util as bu;\n"
                 "import a::util as again;\n"
                 "fn main() -> int { k = bu::K();\n"
                 "  return util::tag() + bu::tag() + again::tag() + k.v; }\n",
                 dir.string());
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(l.Ok) << l.Error;
  ASSERT_EQ(l.Program.Modules.size(), 3u) << l.Text;
  EXPECT_EQ(l.Program.Modules[0].Name, "main");
  EXPECT_EQ(l.Program.Modules[1].Name, "a::util");
  EXPECT_EQ(l.Program.Modules[2].Name, "b::c::util");
  EXPECT_EQ(l.Text.find(dir.string()), std::string::npos) << l.Text;
  EXPECT_EQ(l.Text.find(".pkn"), std::string::npos) << l.Text;
  const std::string &t = l.Text;
  EXPECT_NE(t.find("extern fn @\"util::tag\"() -> i64 module \"a::util\" "
                   "symbol @tag\n"),
            std::string::npos)
      << t;
  EXPECT_NE(t.find("extern fn @\"bu::tag\"() -> i64 module \"b::c::util\" "
                   "symbol @tag\n"),
            std::string::npos)
      << t;
  EXPECT_NE(t.find("extern class K module \"b::c::util\""), std::string::npos)
      << t;
}

TEST(Lowering, CanonicalModuleNameHelpers) {
  using namespace module_name;
  EXPECT_EQ(canonicalImportName("geometry::shapes", false), "geometry::shapes");
  EXPECT_EQ(canonicalImportName("a::::b", false), "a::b");
  EXPECT_EQ(canonicalImportName("io", true), "::io");
  EXPECT_EQ(mainModuleName("zoo.pkn"), "zoo");
  EXPECT_EQ(mainModuleName("../demo/zoo.pkn"), "zoo");
  EXPECT_EQ(mainModuleName("/abs/demo/zoo.pkn"), "zoo");
  EXPECT_EQ(mainModuleName("my.prog.pkn"), "my.prog");
  EXPECT_EQ(mainModuleName("a::b.pkn"), "a__b");
  EXPECT_EQ(cacheRelativePath("geometry::shapes"),
            std::filesystem::path("geometry") / "shapes");
  EXPECT_EQ(cacheRelativePath("zoo"), std::filesystem::path("zoo"));
  EXPECT_EQ(cacheRelativePath("::io"),
            std::filesystem::path(kSystemCacheDir) / "io");
}

// #70: functions are module-qualified, so two modules' (and the importer's
// own) same-named functions are three distinct PIR symbols.
TEST(Lowering, SameNamedFunctionsOfTwoModulesStayDistinct) {
  auto dir = std::filesystem::temp_directory_path() /
             ("lowering_dup_fns_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  {
    std::ofstream x(dir / "x.pkn");
    x << "fn tag() -> Str { return \"x\"; }\n";
    std::ofstream y(dir / "y.pkn");
    y << "fn tag() -> Str { return \"y\"; }\n";
  }
  auto l = lower("import x; import y as r;\n"
                 "fn tag() -> Str { return \"m\"; }\n"
                 "fn main() -> int { println(x::tag() + r::tag() + y::tag() "
                 "+ tag()); return 0; }\n",
                 dir.string());
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(l.Ok) << l.Error;
  ASSERT_EQ(l.Program.Modules.size(), 3u);
  const pir::Module &mainMod = l.Program.Modules[0];
  const pir::Function *own = mainMod.findFunction("tag");
  ASSERT_NE(own, nullptr);
  EXPECT_FALSE(own->IsExtern);
  const pir::Function *fx = mainMod.findFunction("x::tag");
  const pir::Function *fy = mainMod.findFunction("r::tag");
  ASSERT_NE(fx, nullptr) << l.Text;
  ASSERT_NE(fy, nullptr) << l.Text;
  EXPECT_TRUE(fx->IsExtern && fy->IsExtern);
  EXPECT_EQ(fx->linkName(), "tag");
  EXPECT_EQ(fy->linkName(), "tag");
  EXPECT_NE(fx->Module, fy->Module);
  EXPECT_EQ(fx->Module, l.Program.Modules[1].Name);
  EXPECT_EQ(fy->Module, l.Program.Modules[2].Name);
  // `y::tag` reaches the same function as `r::tag`: one declaration.
  EXPECT_EQ(mainMod.findFunction("y::tag"), nullptr) << l.Text;
  std::string m = function(l.Text, "main");
  EXPECT_EQ(count(m, "call @\"x::tag\"("), 1u) << m;
  EXPECT_EQ(count(m, "call @\"r::tag\"("), 2u) << m;
  EXPECT_EQ(count(m, "call @tag("), 1u) << m;
}

// #86: a class's methods and destructor are `<Class>.<method>`; '.' cannot
// occur in a Paykan identifier, so user functions spelled like the old
// `<Class>_<method>` mangling (`K_w`, `K_destroy`) are distinct symbols, and
// the names survive a print -> parse -> print round trip.
TEST(Lowering, MethodSymbolsCannotClashWithUserFunctions) {
  auto l = lower(R"(
    class K {
      fn w() -> int { return 1; }
      fn vtable() -> int { return 4; }
    }
    fn K_w() -> int { return 2; }
    fn K_destroy() -> int { return 3; }
    fn K_vtable() -> int { return 5; }
    fn main() -> int {
      k: K = K();
      return k.w() + K_w() + K_destroy() + k.vtable() + K_vtable();
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  const pir::Module &m = l.Program.Modules[0];
  for (const char *name :
       {"K.w", "K.destroy", "K.vtable", "K_w", "K_destroy", "K_vtable"}) {
    const pir::Function *fn = m.findFunction(name);
    ASSERT_NE(fn, nullptr) << name << "\n" << l.Text;
    EXPECT_FALSE(fn->IsExtern) << name;
  }
  EXPECT_EQ(m.findFunction("K.w")->Sig.Ret, pir::Type::I64);
  EXPECT_EQ(m.findFunction("K.destroy")->Sig.Ret, pir::Type::Void);
  EXPECT_EQ(m.findFunction("K_destroy")->Sig.Ret, pir::Type::I64);
  // Printed bare (no quoting needed) and in the vtable.
  EXPECT_NE(l.Text.find("fn @K.w(%self"), std::string::npos) << l.Text;
  EXPECT_NE(l.Text.find("fn @K.destroy(%self"), std::string::npos) << l.Text;
  EXPECT_NE(l.Text.find("destroy = @K.destroy : (obj) -> void"),
            std::string::npos)
      << l.Text;
  EXPECT_NE(l.Text.find("w = @K.w : (obj) -> i64"), std::string::npos)
      << l.Text;
  std::string mainFn = function(l.Text, "main");
  EXPECT_EQ(count(mainFn, "call @K_w("), 1u) << mainFn;
  EXPECT_EQ(count(mainFn, "call @K_destroy("), 1u) << mainFn;

  pir::ParseError perr;
  auto parsed = pir::parseProgram(l.Text, perr);
  ASSERT_TRUE(parsed) << perr.str();
  auto errors = pir::verify(*parsed);
  EXPECT_TRUE(errors.empty()) << pir::formatErrors(errors);
  EXPECT_EQ(pir::toString(*parsed), l.Text);
  const pir::Module &pm = parsed->Modules[0];
  ASSERT_NE(pm.findFunction("K.w"), nullptr);
  ASSERT_NE(pm.findFunction("K.destroy"), nullptr);
  ASSERT_NE(pm.findFunction("K_w"), nullptr);
}

// #116: every value use of a string literal is a Str object built by
// PaykanString_new (a tracked temporary), never the raw `@.strN` C string:
// as a subscript or method receiver it is destroyed after the borrowing
// call, and under `mov` it is boxed like any other temporary.
TEST(Lowering, StringLiteralUsedAsAnObjectIsAStr) {
  auto l = lower(R"(
    fn main() -> int {
      c = "ab"[1];
      n = "abc".len();
      v = mov "lit";
      return n;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error; // the verifier ran
  std::string m = function(l.Text, "main");
  EXPECT_EQ(count(m, "call @$rt.PaykanString_new(@.str"), 3u) << m;
  // No runtime call takes the raw C string other than PaykanString_new.
  EXPECT_EQ(count(m, "(@.str"), 3u) << m;
  size_t at = m.find("call @$rt.PaykanString_char_at(%str.");
  ASSERT_NE(at, std::string::npos) << m;
  EXPECT_NE(m.find("vcall %str."), std::string::npos) << m; // .len()
  // The subscript's and the receiver's Strs are destroyed after their use;
  // the moved one is boxed into `v` (released at scope exit).
  EXPECT_EQ(count(m, "call @$rt.PaykanString_destroy(%str."), 2u) << m;
  EXPECT_NE(m.find("= box %str."), std::string::npos) << m;
}

// #117: runtime externs are `$rt.<C symbol>` in PIR, a name no Paykan
// identifier can spell, so program functions spelled like runtime symbols
// are ordinary functions, whatever their signature, and the runtime calls
// still reach the runtime.  The names survive a print -> parse -> print
// round trip.
TEST(Lowering, RuntimeExternsCannotClashWithUserFunctions) {
  auto l = lower(R"(
    fn PaykanString_new(x: int) -> int { return x + 1; }
    fn Paykan_println(s: Str) { print(s); }
    fn Paykan_panic_div_by_zero() { println("mine"); }
    fn PaykanObject_None() -> int { return 4; }
    fn z() -> int { return 0; }
    fn main() -> int {
      Paykan_println("x");
      Paykan_panic_div_by_zero();
      a: Str? = None;
      println(a);
      return PaykanString_new(1) + PaykanObject_None() + 5 / z();
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  const pir::Module &m = l.Program.Modules[0];
  for (const char *name : {"PaykanString_new", "Paykan_println",
                           "Paykan_panic_div_by_zero", "PaykanObject_None"}) {
    const pir::Function *fn = m.findFunction(name);
    ASSERT_NE(fn, nullptr) << name << "\n" << l.Text;
    EXPECT_FALSE(fn->IsExtern) << name;
  }
  EXPECT_EQ(m.findFunction("PaykanString_new")->Sig.Params.size(), 1u);
  for (const char *name : {"$rt.PaykanString_new", "$rt.Paykan_println",
                           "$rt.Paykan_panic_div_by_zero"}) {
    const pir::Function *fn = m.findFunction(name);
    ASSERT_NE(fn, nullptr) << name << "\n" << l.Text;
    EXPECT_TRUE(fn->IsExtern) << name;
    EXPECT_TRUE(fn->Module.empty()) << name;
    EXPECT_TRUE(pir::isRuntimeName(fn->Name)) << name;
    EXPECT_EQ(pir::runtimeSymbol(fn->Name), std::string(name).substr(4));
  }
  EXPECT_NE(l.Text.find("extern fn @$rt.PaykanString_new(ptr, i64) -> obj"),
            std::string::npos)
      << l.Text;
  EXPECT_NE(l.Text.find("extern obj @$rt.PaykanObject_None"), std::string::npos)
      << l.Text;
  std::string mainFn = function(l.Text, "main");
  EXPECT_EQ(count(mainFn, "call @Paykan_println("), 1u) << mainFn;
  EXPECT_EQ(count(mainFn, "call @Paykan_panic_div_by_zero()"), 1u) << mainFn;
  EXPECT_EQ(count(mainFn, "call @$rt.Paykan_panic_div_by_zero()"), 1u)
      << mainFn;
  EXPECT_EQ(count(mainFn, "call @PaykanString_new(1)"), 1u) << mainFn;
  EXPECT_EQ(count(mainFn, "call @$rt.PaykanString_new(@.str"), 1u) << mainFn;
  EXPECT_EQ(count(mainFn, "@$rt.PaykanObject_None,"), 1u) << mainFn;

  pir::ParseError perr;
  auto parsed = pir::parseProgram(l.Text, perr);
  ASSERT_TRUE(parsed) << perr.str();
  auto errors = pir::verify(*parsed);
  EXPECT_TRUE(errors.empty()) << pir::formatErrors(errors);
  EXPECT_EQ(pir::toString(*parsed), l.Text);
}

// -- Conversion constructors (#64): the semantics are the lowering's

TEST(Lowering, IntOfFloatGuardsTheRangeBeforeTheFToI) {
  auto l = lower(R"(
    fn conv(f: float) -> int { return int<float>(f); }
    fn main() -> int { return conv(2.5); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "conv");
  size_t lo = m.find("cmp ge");
  size_t hi = m.find("cmp lt");
  size_t panic = m.find("call @$rt.Paykan_panic_float_to_int(");
  size_t ftoi = m.find("ftoi");
  ASSERT_NE(lo, std::string::npos) << m;
  ASSERT_NE(hi, std::string::npos) << m;
  ASSERT_NE(panic, std::string::npos) << m;
  ASSERT_NE(ftoi, std::string::npos) << m;
  EXPECT_NE(m.find("-9.2233720368547758e+18"), std::string::npos) << m;
  EXPECT_EQ(count(m, "9.2233720368547758e+18"), 2u) << m; // -2^63 and 2^63
  EXPECT_LT(panic, ftoi) << m;
  EXPECT_NE(m.find("unreachable"), std::string::npos) << m;
}

TEST(Lowering, CharOfIntGuardsTheByteRange) {
  auto l = lower(R"(
    fn conv(n: int) -> char { return char<int>(n); }
    fn main() -> int { c = conv(97); return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "conv");
  EXPECT_NE(m.find("call @$rt.Paykan_panic_int_to_char("), std::string::npos)
      << m;
  EXPECT_NE(m.find("cmp le"), std::string::npos) << m;
  EXPECT_NE(m.find("to char"), std::string::npos) << m;
}

TEST(Lowering, OtherNumericConversionsAreInline) {
  auto l = lower(R"(
    fn a(n: int) -> float { return float<int>(n); }
    fn b(x: bool) -> int { return int<bool>(x); }
    fn c(n: int) -> bool { return bool<int>(n); }
    fn d(x: char) -> int { return int<char>(x); }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  EXPECT_NE(function(l.Text, "a").find("itof"), std::string::npos) << l.Text;
  EXPECT_NE(function(l.Text, "b").find("to i64"), std::string::npos) << l.Text;
  EXPECT_NE(function(l.Text, "c").find("cmp ne"), std::string::npos) << l.Text;
  EXPECT_NE(function(l.Text, "d").find("to i64"), std::string::npos) << l.Text;
  EXPECT_EQ(count(l.Text, "call @$rt.Paykan_panic"), 0u) << l.Text;
}

TEST(Lowering, ParsesCallTheRuntimeAndReturnAnOptionalBox) {
  auto l = lower(R"(
    fn p(s: Str) -> int? { return int<Str>(s); }
    fn q(s: Str) -> float? { return float<Str>(s); }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string p = function(l.Text, "p");
  EXPECT_NE(p.find("call @$rt.PaykanInt_from_str("), std::string::npos) << p;
  // The fresh box is returned as-is: no extra retain.
  EXPECT_EQ(count(p, "retain"), 0u) << p;
  EXPECT_NE(function(l.Text, "q").find("call @$rt.PaykanFloat_from_str("),
            std::string::npos)
      << l.Text;
}

TEST(Lowering, BoxedParsesShareThePrimitiveParse) {
  // `Int<Str>` & co. (#88) are the primitive parse: its box is the `Int?`.
  auto l = lower(R"(
    fn p(s: Str) -> Int? { return Int<Str>(s); }
    fn q(s: Str) -> Float? { return Float<Str>(s); }
    fn r(s: Str) -> Bool? { return Bool<Str>(s); }
    fn t(s: Str) -> bool? { return bool<Str>(s); }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string p = function(l.Text, "p");
  EXPECT_NE(p.find("call @$rt.PaykanInt_from_str("), std::string::npos) << p;
  EXPECT_EQ(count(p, "retain"), 0u) << p;
  EXPECT_NE(function(l.Text, "q").find("call @$rt.PaykanFloat_from_str("),
            std::string::npos)
      << l.Text;
  EXPECT_NE(function(l.Text, "r").find("call @$rt.PaykanBool_from_str("),
            std::string::npos)
      << l.Text;
  EXPECT_NE(function(l.Text, "t").find("call @$rt.PaykanBool_from_str("),
            std::string::npos)
      << l.Text;
}

TEST(Lowering, BoxedSourcesUnboxThenFormat) {
  auto l = lower(R"(
    fn a(x: Int) -> Str { return Str<Int>(x); }
    fn b(x: Float) -> Str { return Str<Float>(x); }
    fn c(x: Bool) -> Str { return Str<Bool>(x); }
    fn d(x: Char) -> Str { return Str<Char>(x); }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  const char *expected[][3] = {
      {"a", "call @$rt.PaykanInt_value(", "call @$rt.PaykanString_from_int("},
      {"b", "call @$rt.PaykanFloat_value(",
       "call @$rt.PaykanString_from_float("},
      {"c", "call @$rt.PaykanBool_value(", "call @$rt.PaykanString_from_bool("},
      {"d", "call @$rt.PaykanChar_value(", "call @$rt.PaykanString_from_char("},
  };
  for (const auto &e : expected) {
    std::string fn = function(l.Text, e[0]);
    size_t unbox = fn.find(e[1]);
    size_t format = fn.find(e[2]);
    ASSERT_NE(unbox, std::string::npos) << fn;
    ASSERT_NE(format, std::string::npos) << fn;
    EXPECT_LT(unbox, format) << fn;
  }
}

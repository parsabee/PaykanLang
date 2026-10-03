// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// AST -> PIR lowering tests: every program is lowered, verified, printed, and
// the printed PIR is checked for the ownership shapes the lowering must
// produce (docs/pir.md §7).  Behavioural parity with the LLVM backend is
// covered by the CodeGen suite run on every backend.

#include "TestUtils.h"
#include "paykan/lowering/Lowering.h"
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

// ---------------------------------------------------------------------------
// Variables, scope cleanup, strings
// ---------------------------------------------------------------------------

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
  EXPECT_EQ(count(m, "call @PaykanString_new("), 2u) << m;
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
  size_t newStr = m.find("call @PaykanString_new(");
  size_t println = m.find("call @Paykan_println(");
  size_t destroy = m.find("call @PaykanString_destroy(");
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
  EXPECT_EQ(count(m, "call @PaykanString_destroy("), 2u) << m;
  size_t concat = m.find("call @PaykanString_concat(");
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

// ---------------------------------------------------------------------------
// mov
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Calls: callee-consumes ABI for user functions, borrow for builtins
// ---------------------------------------------------------------------------

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
  EXPECT_EQ(count(m, "call @Paykan_panic_div_by_zero()"), 2u) << m;
  EXPECT_EQ(count(m, "call @Paykan_panic_div_overflow()"), 1u) << m;
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

// ---------------------------------------------------------------------------
// Classes: layout, vtable, constructor, destructor, fields
// ---------------------------------------------------------------------------

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
  EXPECT_NE(t.find("destroy = @Derived_destroy : (obj) -> void"),
            std::string::npos)
      << t;
  EXPECT_NE(t.find("describe = @Derived_describe : (obj) -> box"),
            std::string::npos)
      << t;
  EXPECT_NE(t.find("extra = @Derived_extra : (obj) -> i64"), std::string::npos)
      << t;
  // Inherited runtime slots name the runtime implementation.
  EXPECT_NE(t.find("equals = @PaykanObject_equals : (obj, box) -> i64"),
            std::string::npos)
      << t;
  // The constructor allocates, boxes BEFORE __init__, and returns the box.
  std::string ctor = function(t, "Derived");
  size_t nw = ctor.find(" = new Derived");
  size_t bx = ctor.find(" = box ");
  size_t init = ctor.find("call @Derived___init__(");
  ASSERT_NE(nw, std::string::npos) << ctor;
  ASSERT_NE(bx, std::string::npos) << ctor;
  ASSERT_NE(init, std::string::npos) << ctor;
  EXPECT_LT(nw, bx);
  EXPECT_LT(bx, init);
  // __super__ passes a +1 box of the argument.
  std::string init2 = function(t, "Derived___init__");
  EXPECT_NE(init2.find("call @Base___init__("), std::string::npos) << init2;
  // The destructor releases the ref-typed field and frees the struct.
  std::string dtor = function(t, "Derived_destroy");
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

// ---------------------------------------------------------------------------
// match
// ---------------------------------------------------------------------------

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
  EXPECT_EQ(count(m, "call @PaykanString_equals("), 2u) << m;
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

// ---------------------------------------------------------------------------
// Arrays, tuples, optionals
// ---------------------------------------------------------------------------

TEST(Lowering, ConstantPrimitiveArrayLiteralUsesADataGlobal) {
  auto l = lower(R"(
    fn main() -> int { xs = [1, 2, 3]; return xs.len(); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  EXPECT_NE(l.Text.find("data @.arr.data0 = [1, 2, 3]"), std::string::npos)
      << l.Text;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find("call @PaykanArray_new_from_data(3, @.arr.data0)"),
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
  EXPECT_NE(m.find("call @PaykanArray_new_obj(2)"), std::string::npos) << m;
  EXPECT_EQ(count(m, "call @PaykanArray_set_obj("), 2u) << m;
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
  EXPECT_EQ(count(m, "call @PaykanArray_get("), 4u) << m;
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
  EXPECT_NE(m.find("call @PaykanArray_push_obj("), std::string::npos) << m;
}

TEST(Lowering, TupleLiteralUsesAKindsDescriptor) {
  auto l = lower(R"(
    fn main() -> int { t = (1, "s"); return t.0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  EXPECT_NE(l.Text.find("bytes @.tuple.kinds0 = [0, 4]"), std::string::npos)
      << l.Text;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find("call @PaykanTuple_new(2, @.tuple.kinds0)"),
            std::string::npos)
      << m;
  EXPECT_NE(m.find("call @PaykanTuple_set("), std::string::npos) << m;
  EXPECT_NE(m.find("call @PaykanTuple_set_obj("), std::string::npos) << m;
  EXPECT_NE(m.find("call @PaykanTuple_get("), std::string::npos) << m;
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
  EXPECT_NE(m.find("box @PaykanObject_None"), std::string::npos) << m;
  EXPECT_NE(l.Text.find("extern obj @PaykanObject_None"), std::string::npos)
      << l.Text;
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------

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
  EXPECT_EQ(l.Program.Modules[0].Name, "main.pkn");
  const std::string &t = l.Text;
  // The importer declares what it uses from lib, keyed to lib's module and
  // named as the call sites qualify it; `symbol` is lib's own name for it.
  EXPECT_NE(t.find("extern fn @\"lib::twice\"(i64) -> i64 module \""),
            std::string::npos)
      << t;
  EXPECT_NE(t.find("\" symbol @twice\n"), std::string::npos) << t;
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
  EXPECT_NE(t.find("fn @Adder_add("), std::string::npos) << t;
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

// ---------------------------------------------------------------------------
// Conversion constructors (#64): the semantics are the lowering's
// ---------------------------------------------------------------------------

TEST(Lowering, IntOfFloatGuardsTheRangeBeforeTheFToI) {
  auto l = lower(R"(
    fn conv(f: float) -> int { return int<float>(f); }
    fn main() -> int { return conv(2.5); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "conv");
  size_t lo = m.find("cmp ge");
  size_t hi = m.find("cmp lt");
  size_t panic = m.find("call @Paykan_panic_float_to_int(");
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
  EXPECT_NE(m.find("call @Paykan_panic_int_to_char("), std::string::npos) << m;
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
  EXPECT_EQ(count(l.Text, "call @Paykan_panic"), 0u) << l.Text;
}

TEST(Lowering, ParsesCallTheRuntimeAndReturnAnOptionalBox) {
  auto l = lower(R"(
    fn p(s: Str) -> int? { return int<Str>(s); }
    fn q(s: Str) -> float? { return float<Str>(s); }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string p = function(l.Text, "p");
  EXPECT_NE(p.find("call @PaykanInt_from_str("), std::string::npos) << p;
  // The fresh box is returned as-is: no extra retain.
  EXPECT_EQ(count(p, "retain"), 0u) << p;
  EXPECT_NE(function(l.Text, "q").find("call @PaykanFloat_from_str("),
            std::string::npos)
      << l.Text;
}

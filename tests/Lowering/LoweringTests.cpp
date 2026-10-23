// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// AST -> PIR lowering tests: every program is lowered, verified, printed, and
// the printed PIR is checked for the ownership shapes the lowering must
// produce (docs/pir.md §7).  The programs' behaviour is covered by the
// CodeGen suite, run on the mlir backend.

#include "ModuleName.h"
#include "TestUtils.h"
#include "paykan/lowering/Lowering.h"
#include "paykan/pir/MoveAtLastUse.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Pass.h"
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
/// imports), the instantiations of other modules' templates as @p copies
/// says.
Lowered lower(const std::string &source, const std::string &projectRoot = "",
              lowering::Copies copies = lowering::Copies::Shared) {
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
  if (!lowering::lowerProgram(ctx, driver->getRoot(), "main", projectRoot,
                              l.Program, errs, nullptr, copies)) {
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
    fn main() -> int { n = "hi".len(); return n; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  size_t newStr = m.find("call @$rt.PaykanString_new(");
  size_t println = m.find(" = vcall %str.");
  size_t destroy = m.find("call @$rt.Paykan_dispose(");
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
  EXPECT_EQ(count(m, "call @$rt.Paykan_dispose("), 2u) << m;
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
      if flag {
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

// -- native fn (#198)

// A native function calls its C symbol with the builtins' convention: a
// reference argument borrowed as the raw object, a reference result an owned
// box.  A call in its module calls C itself, with the caller's references;
// the function's body, for other modules, releases its own parameters.
TEST(Lowering, NativeFunctionCallsItsCSymbolWithTheBuiltinConvention) {
  auto l = lower(R"(
    native fn put(fd: int, s: view Str) -> int = "pk_test_put";
    native fn get(fd: int) -> Str? = "pk_test_get";
    fn main() -> int { s: Str = "x"; n = put(1, s); t = get(0); return n; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  EXPECT_NE(l.Text.find("extern fn @$c.pk_test_put(i64, obj) -> i64"),
            std::string::npos)
      << l.Text;
  EXPECT_NE(l.Text.find("extern fn @$c.pk_test_get(i64) -> box"),
            std::string::npos)
      << l.Text;
  std::string put = function(l.Text, "put");
  EXPECT_NE(put.find("unbox"), std::string::npos) << put;
  EXPECT_NE(put.find("call @$c.pk_test_put("), std::string::npos) << put;
  EXPECT_EQ(count(put, "retain"), 0u) << put;
  EXPECT_EQ(count(put, "release"), 1u) << put;
  std::string get = function(l.Text, "get");
  EXPECT_EQ(count(get, "release"), 0u) << get;
  // The caller lends `s` to C: no reference is taken for the call.
  std::string main = function(l.Text, "main");
  EXPECT_NE(main.find("call @$c.pk_test_put(1, "), std::string::npos) << main;
  EXPECT_EQ(main.find("call @put("), std::string::npos) << main;
  EXPECT_EQ(count(main, "retain"), 0u) << main;
}

// An instance of a generic native function passes a PaykanType per type
// argument, then what its template typed `T` by address: a `view T` as its
// value's, a `-> T` result through a local C writes, a `-> view T` result as
// an address the caller copies.
TEST(Lowering, GenericNativeFunctionPassesTypesAndAddresses) {
  auto l = lower(R"(import ::io;
    native fn pick<T>(first: bool, a: view T, b: view T) -> T = "pk_test_pick";
    native fn get<T>(b: view Obj, i: int) -> view T = "pk_test_get";
    fn main() -> int {
      a: Str = "a";
      s = pick<Str>(True, a, "b");
      f = pick<float>(False, 1.5, 2.5);
      b: Obj = "x";
      io::println(get<Str>(b, 0));
      return pick<int>(True, 1, 2);
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  // One PaykanType per type: size, alignment, kind, references (offsets).
  EXPECT_NE(l.Text.find("data @.type0 = [8, 8, 4, 1, 0]"), std::string::npos)
      << l.Text;
  EXPECT_NE(l.Text.find("extern fn @$c.pk_test_pick(ptr, ptr, bool, ptr, "
                        "ptr) -> void"),
            std::string::npos)
      << l.Text;
  EXPECT_NE(l.Text.find("extern fn @$c.pk_test_get(ptr, obj, i64) -> ptr"),
            std::string::npos)
      << l.Text;
  std::string main = function(l.Text, "main");
  EXPECT_NE(main.find("call @$c.pk_test_pick(@.type0, %out.addr"),
            std::string::npos)
      << main;
  EXPECT_NE(main.find("call @$c.pk_test_pick(@.type1, %out.addr"),
            std::string::npos)
      << main;
  EXPECT_NE(main.find("call @$c.pk_test_get(@.type0, "), std::string::npos)
      << main;
  // `a` is lent; the literal "b" is a temporary released after the call.
  // Only the view result is copied.
  EXPECT_NE(main.find("ptr.load box, %r"), std::string::npos) << main;
  EXPECT_EQ(count(main, "retain"), 1u) << main;
  // The instance's own body serves other modules.
  std::string str = function(l.Text, "pick<Str>");
  EXPECT_NE(str.find("call @$c.pk_test_pick(@.type0, %out.addr"),
            std::string::npos)
      << str;
  EXPECT_EQ(count(str, "release"), 2u) << str; // its own parameters
}

// C gets an `inout` argument's address, a generic instance's included.
TEST(Lowering, NativeInoutArgumentsAreAddresses) {
  auto l = lower(R"(
    native fn bump(n: inout int) = "pk_test_bump";
    native fn swap<T>(a: inout T, b: inout T) = "pk_test_swap";
    fn main() -> int {
      n = 1;
      bump(n);
      x = True;
      y = False;
      swap<bool>(x, y);
      return n;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  EXPECT_NE(l.Text.find("extern fn @$c.pk_test_bump(ptr)"), std::string::npos)
      << l.Text;
  EXPECT_NE(l.Text.find("extern fn @$c.pk_test_swap(ptr, ptr, ptr)"),
            std::string::npos)
      << l.Text;
  EXPECT_NE(l.Text.find("data @.type0 = [1, 1, 2, 0]"), std::string::npos)
      << l.Text;
  std::string main = function(l.Text, "main");
  EXPECT_NE(main.find("call @$c.pk_test_bump(%n.addr"), std::string::npos)
      << main;
  EXPECT_NE(main.find("call @$c.pk_test_swap(@.type0, %x.addr"),
            std::string::npos)
      << main;
}

// An inline optional is a presence flag and its payload at the payload's
// alignment: an `int?` is 16 bytes, a `char?` 2.
TEST(Lowering, GenericNativeFunctionDescribesInlineOptionals) {
  auto l = lower(R"(
    native fn pick<T>(first: bool, a: view T, b: view T) -> T = "pk_test_pick";
    fn main() -> int {
      o: int? = pick<int?>(True, 1, None);
      c: char? = pick<char?>(False, 'a', None);
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  EXPECT_NE(l.Text.find("data @.type0 = [16, 8, 5, 0]"), std::string::npos)
      << l.Text;
  EXPECT_NE(l.Text.find("data @.type1 = [2, 1, 5, 0]"), std::string::npos)
      << l.Text;
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

// An `inout` parameter is a `ptr` to the caller's storage: the callee reads
// and writes through it, a caller passes a variable's address or passes its
// own `inout` parameter on, and a `view` parameter is an ordinary value.
TEST(Lowering, InoutParameterIsAPointer) {
  auto l = lower(R"(
    fn bump(n: inout int, by: view int) { n = n + by; }
    fn twice(n: inout int) { bump(n, 1); bump(n, 1); }
    fn main() -> int {
      k = 1;
      twice(k);
      return k;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string bump = function(l.Text, "bump");
  EXPECT_NE(bump.find("fn @bump(%n.1: ptr, %by.2: i64) -> void"),
            std::string::npos)
      << bump;
  EXPECT_EQ(count(bump, " = ptr.load i64, "), 1u) << bump;
  EXPECT_EQ(count(bump, "ptr.store "), 1u) << bump;
  std::string twice = function(l.Text, "twice");
  EXPECT_EQ(count(twice, "local.addr"), 0u) << twice; // passed on
  EXPECT_EQ(count(twice, "call @bump("), 2u) << twice;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find(" = local.addr %k"), std::string::npos) << m;
  EXPECT_EQ(count(m, "ptr."), 0u) << m;
}

// A field is passed by its address.  A variable's object stays alive for the
// call on its own; any other receiver is retained for the call and released
// right after it.
TEST(Lowering, InoutFieldIsPassedByAddress) {
  auto l = lower(R"(
    class Inner { n: int; }
    impl Inner { fn __init__() { self.n = 0; } }
    class Holder { inner: Inner; }
    impl Holder { fn __init__() { self.inner = Inner(); } }
    fn bump(n: inout int) { n = n + 1; }
    fn main() -> int {
      i = Inner();
      bump(i.n);
      h = Holder();
      bump(h.inner.n);
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_EQ(count(m, " = field.addr "), 2u) << m;
  EXPECT_EQ(count(m, ", Inner.n"), 2u) << m;
  size_t first = m.find("call @bump(");
  size_t second = m.find("call @bump(", first + 1);
  ASSERT_NE(second, std::string::npos) << m;
  // `i` needs no retain; the chain's Inner is retained for its call and
  // released after it.
  EXPECT_EQ(count(m.substr(0, first), "retain "), 0u) << m;
  EXPECT_EQ(count(m.substr(first, second - first), "retain "), 1u) << m;
  EXPECT_NE(m.find("release ", second), std::string::npos) << m;
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
      if a != b { r = r + 1; }
      if a == b { r = r + 2; }
      if a < b { r = r + 4; }
      if a <= b { r = r + 8; }
      if a > b { r = r + 16; }
      if a >= b { r = r + 32; }
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
      fn describe() -> Str { return self.name; }
    }

    impl Base {
      fn __init__(name: Str) { self.name = name; }
    }
    class Derived : Base {
      n: int;
      fn describe() -> Str { return "derived"; }
      fn extra() -> int { return self.n; }
    }

    impl Derived {
      fn __init__(name: Str, n: int) { __super__(name); self.n = n; }
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
  // The destructor releases the ref-typed field; the runtime frees the
  // struct after it (docs/pir.md §5).
  std::string dtor = function(t, "Derived.destroy");
  EXPECT_NE(dtor.find("field.load %"), std::string::npos) << dtor;
  EXPECT_EQ(count(dtor, "release"), 1u) << dtor;
  EXPECT_EQ(dtor.find("free %"), std::string::npos) << dtor;
  // Virtual dispatch names the class and the slot.
  std::string m = function(t, "main");
  EXPECT_NE(m.find("vcall %"), std::string::npos) << m;
  EXPECT_NE(m.find(" : Derived ["), std::string::npos) << m;
}

TEST(Lowering, FieldStoreRetainsNewAndReleasesOldValue) {
  auto l = lower(R"(
    class H { s: Str; }
    impl H { fn __init__() { self.s = "a"; } }
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
    class H { s: Str; }
    impl H { fn __init__() { self.s = "a"; } }
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
// A match on an enum, or on integer, char or bool literals, is a `switch`:
// a case per value, the wildcard its default, of two arms naming one value
// the first.  One on floats or strings is an if-else chain of tests.
TEST(Lowering, PrimitiveMatchesAreSwitches) {
  auto l = lower(R"(
    enum Dir { N, E, S, W }
    fn d(x: Dir) -> int { match x { N { return 1; } S { return 2; } _ { return 3; } } }
    fn i(x: int) -> int { match x { 5 { return 1; } 9 { return 2; } 5 { return 3; } _ { return 4; } } }
    fn c(x: char) -> int { match x { 'a' { return 1; } _ { return 2; } } }
    fn b(x: bool) -> int { match x { True { return 1; } False { return 2; } } }
    fn f(x: float) -> int { match x { 1.5 { return 1; } _ { return 2; } } }
    fn s(x: Str) -> int { match x { "a" { return 1; } _ { return 2; } } }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error; // the verifier ran
  std::string d = function(l.Text, "d");
  EXPECT_NE(d.find("switch %x."), std::string::npos) << d;
  EXPECT_NE(d.find("case 0 {"), std::string::npos) << d;
  EXPECT_NE(d.find("case 2 {"), std::string::npos) << d;
  EXPECT_NE(d.find("default {"), std::string::npos) << d;
  EXPECT_EQ(d.find("cmp eq"), std::string::npos) << d;
  std::string i = function(l.Text, "i");
  EXPECT_EQ(count(i, "case 5 {"), 1u) << i;
  EXPECT_EQ(i.find("ret 3"), std::string::npos) << i; // the second 5
  EXPECT_NE(function(l.Text, "c").find("case 'a' {"), std::string::npos)
      << l.Text;
  std::string b = function(l.Text, "b");
  EXPECT_NE(b.find("case true {"), std::string::npos) << b;
  EXPECT_NE(b.find("case false {"), std::string::npos) << b;
  for (const char *chain : {"f", "s"}) {
    std::string fn = function(l.Text, chain);
    EXPECT_EQ(fn.find("switch"), std::string::npos) << fn;
    EXPECT_NE(fn.find("if %"), std::string::npos) << fn;
  }
}

TEST(Lowering, StringLiteralMatchSubjectIsAStrReleasedOnEveryExit) {
  auto l = lower(R"(import ::io;
    fn f() -> int {
      match "s" { "t" { return 1; } _ { return 2; } }
    }
    fn main() -> int {
      match "s" { "t" { io::println("t"); } _ { io::println("other"); } }
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
  auto l = lower(R"(import ::io;
    class A {}
    impl A { fn __init__() {} }
    class B : A {}
    impl B { fn __init__() { __super__(); } }
    fn mk() -> A { return B(); }
    fn main() -> int {
      match mk() {
        b: B { io::println("b"); }
        A { io::println("a"); }
        _ { io::println("other"); }
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
  auto l = lower(R"(import ::io;
    class A { v: int; }
    impl A { fn __init__(v: int) { self.v = v; } }
    fn main() -> int {
      x: A = A(1);
      match x {
        a: A { io::println(Str<int>(a.v)); }
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
  auto l = lower(R"(import ::io;
    class A { v: int; }
    impl A { fn __init__(v: int) { self.v = v; } }
    fn main() -> int {
      o: A? = A(7);
      k: int = 0;
      match o {
        a: A {
          if k == 1 { a = A(1); }
          i: int = 0;
          while i < 3 { a = A(i); i = i + 1; }
          io::println(Str<int>(a.v));
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
  auto l = lower(R"(import ::io;
    class A { fn name() -> Str { return "A"; } }
    impl A { fn __init__() {} }
    class B : A { fn name() -> Str { return "B"; } }
    impl B { fn __init__() { __super__(); } }
    fn main() -> int {
      x: A = B();
      match x {
        b: B { x = A(); io::println(b.name()); }
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
  auto l = lower(R"(import ::io;
    class N { v: int; }
    impl N { fn __init__(v: int) { self.v = v; } }
    fn find(f: bool) -> N? { if f { return N(1); } return None; }
    fn main() -> int {
      match find(True) {
        n: N { io::println(Str<int>(n.v)); }
        None { io::println("none"); }
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

TEST(Lowering, EnumMatchIsASwitchOnTheVariants) {
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
  EXPECT_EQ(count(m, "switch %"), 1u) << m;
  EXPECT_EQ(count(m, "case "), 2u) << m; // one per variant arm
  EXPECT_EQ(count(m, "cmp eq %"), 0u) << m;
  EXPECT_EQ(count(m, "release"), 0u) << m; // no boxes anywhere
}

// -- Arrays, tuples, optionals

TEST(Lowering, AssigningAnObjectElementEvaluatesTheIndexOnce) {
  // Issue #52: `x = arr[idx()]` used to evaluate the subscript once for the
  // value and again for the ownership handling.
  auto l = lower(R"(import ::io;
    class C { v: int; }
    impl C { fn __init__(v: int) { self.v = v; } }
    fn idx() -> int { io::println("idx called"); return 0; }
    fn mk() -> C[1] { return [C(3)]; }
    fn main() -> int {
      arr: C[2] = [C(1), C(2)];
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
  EXPECT_EQ(count(m, "call @mk("), 1u) << m;
  // One bounds check per subscript, on the index evaluated once.
  EXPECT_EQ(count(m, "call @$rt.Paykan_panic_index("), 4u) << m;
  // Each element read retains the stored box once (x, y and z from arr, x
  // from the temporary array, whose own element is then released).
  EXPECT_EQ(count(m, "retain"), 4u) << m;
}

TEST(Lowering, SideEffectingReceiversAreEvaluatedOnce) {
  // The other ownership paths that take a value's box: a call-rooted field
  // read, a call result assigned to a variable, an element passed to a ref
  // parameter and an element stored into an array slot or a field.
  auto l = lower(R"(import ::io;
    class C { v: int; }
    impl C { fn __init__(v: int) { self.v = v; } }
    class H { c: C; }
    impl H { fn __init__(c: C) { self.c = c; } }
    fn idx() -> int { io::println("idx"); return 0; }
    fn mkh() -> H { return H(C(5)); }
    fn take(c: C) -> int { return c.v; }
    fn main() -> int {
      arr: C[2] = [C(1), C(2)];
      x: Obj = C(9);
      x = mkh().c;
      w: C = mkh().c;
      x = mkh();
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

// A tuple is a `mem` local holding its elements in place, each stored and
// loaded at its offset; its Str is released at scope end.  Nothing else is
// emitted for it: no runtime call and no data.
TEST(Lowering, TupleIsAMemLocalWithItsElementsInPlace) {
  auto l = lower(R"(
    fn main() -> int { t = (1, "s"); return t.0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find("local %t.0: mem(16, 8)\n"), std::string::npos) << m;
  EXPECT_NE(m.find("ptr.store %t.addr.1, 1\n"), std::string::npos) << m;
  EXPECT_NE(m.find("%elem.addr.2 = ptr.add %t.addr.1, 8\n"), std::string::npos)
      << m;
  EXPECT_NE(m.find("ptr.store %elem.addr.2, %shared."), std::string::npos) << m;
  EXPECT_NE(m.find(" = ptr.load i64, %t.addr."), std::string::npos) << m;
  EXPECT_NE(m.find(" = ptr.load box, %elem.addr."), std::string::npos) << m;
  EXPECT_EQ(count(m, "release %"), 1u) << m;
  EXPECT_EQ(l.Text.find("PaykanTuple_"), std::string::npos) << l.Text;
  EXPECT_EQ(l.Text.find("data @"), std::string::npos) << l.Text;
}

// Printing, `toString` and `==` hand the runtime the tuple's address and its
// layout, a `.tupleN` data global emitted once per tuple type.
TEST(Lowering, PrintedOrComparedTupleUsesALayout) {
  auto l = lower(R"(
    fn main() -> int {
      t = (1, "s");
      u = (1, "s");
      p = t.toString();
      b = t == u;
      n: ((int?, bool), Str?, char?, float) = ((None, True), None, 'c', 2.5);
      s: Str = n.toString();
      q = n.toString();
      return t.0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  // (int, Str): INT at 0, OBJECT at 8.
  EXPECT_NE(l.Text.find("data @.tuple0 = [2, 0, 0, 4, 8]\n"), std::string::npos)
      << l.Text;
  // A nested TUPLE with its own layout, an inline `int?` (OPTIONAL of INT),
  // a `Str?` (OPTIONAL of OBJECT), a `char?` (OPTIONAL of CHAR), a float.
  EXPECT_NE(l.Text.find("data @.tuple1 = [4, 6, 0, 2, 5, 0, 0, 2, 16, 5, 24, "
                        "4, 5, 32, 3, 1, 40]\n"),
            std::string::npos)
      << l.Text;
  EXPECT_EQ(count(l.Text, "data @"), 2u) << l.Text;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find("local %n.4: mem(48, 8)\n"), std::string::npos) << m;
  EXPECT_NE(m.find("call @$rt.PaykanTuple_toString(%t.addr."),
            std::string::npos)
      << m;
  EXPECT_NE(m.find("call @$rt.PaykanTuple_equals(%t.addr."), std::string::npos)
      << m;
  EXPECT_EQ(count(m, ", @.tuple0)"), 2u) << m;
  EXPECT_EQ(count(m, "call @$rt.PaykanTuple_toString(%n.addr."), 2u) << m;
  EXPECT_EQ(count(m, ", @.tuple1)"), 2u) << m;
}

TEST(Lowering, PrintedOptionalSubstitutesTheNoneSingleton) {
  auto l = lower(R"(
    class N {}
    impl N { fn __init__() {} }
    fn find() -> N? { return None; }
    fn main() -> int { s = find().toString(); return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find("cmp eq %"), std::string::npos) << m;
  EXPECT_NE(m.find("select %opt.isnone"), std::string::npos) << m;
  EXPECT_NE(m.find("@$rt.PaykanObject_None"), std::string::npos) << m;
  EXPECT_NE(l.Text.find("extern obj @$rt.PaykanObject_None"), std::string::npos)
      << l.Text;
}

// An `int?` & co. is a value (#96): parameters, results, locals and fields
// of type opt_*, `some` for a widened primitive, `none` for None, `is_some`
// and `unwrap` for comparisons and `match`; nothing is allocated, retained
// or released for it.
TEST(Lowering, OptionalValuesAreNotBoxed) {
  auto l = lower(R"(
    class C { n: int?; }
    impl C { fn __init__() { } }
    fn half(n: int) -> int? {
      if n % 2 != 0 { return None; }
      return n / 2;
    }
    fn get(o: int?) -> int {
      match o { v: int { return v; } None { return -1; } }
    }
    fn same(a: float?, b: float?) -> bool { return a == b; }
    fn main() -> int {
      x: int? = half(4);
      y = half(3);
      f: float? = 3;
      c = C();
      c.n = x;
      if y == None { x = 7; }
      return get(x) + get(c.n) + get(y);
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  EXPECT_NE(l.Text.find("fn @half(%n.1: i64) -> opt_i64"), std::string::npos)
      << l.Text;
  EXPECT_NE(l.Text.find("field n: opt_i64"), std::string::npos) << l.Text;
  std::string half = function(l.Text, "half");
  EXPECT_NE(half.find("none opt_i64"), std::string::npos) << half;
  EXPECT_NE(half.find(" = some %"), std::string::npos) << half;
  std::string get = function(l.Text, "get");
  EXPECT_NE(get.find(" = is_some %o"), std::string::npos) << get;
  EXPECT_NE(get.find(" = unwrap %o"), std::string::npos) << get;
  std::string same = function(l.Text, "same");
  EXPECT_EQ(count(same, " = is_some %"), 2u) << same;
  EXPECT_EQ(count(same, " = unwrap %"), 2u) << same;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find("local %x.0: opt_i64"), std::string::npos) << m;
  EXPECT_NE(m.find(" = some 3.0"), std::string::npos) << m; // promoted
  for (const char *fn : {"half", "get", "same"}) {
    std::string body = function(l.Text, fn);
    for (const char *op : {"box", "retain", "release", "new "})
      EXPECT_EQ(body.find(op), std::string::npos) << op << "\n" << body;
  }
}

// An array or tuple of optional values holds them inline: the elements are
// read and written in place, a tuple's at its offset.
TEST(Lowering, ArrayAndTupleElementsOfOptionalsAreInline) {
  auto l = lower(R"(
    fn main() -> int {
      xs: int?[3] = [1, None, 3];
      xs[0] = None;
      a = xs[1];
      i = 2;
      b = xs[i];
      t: (bool?, Str) = (True, "s");
      c = t.0;
      d = t.1;
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  // Three 16-byte elements, stored three times by the literal and once by
  // `xs[0] = None`, and loaded as they are; the tuple's two elements are
  // stored once each.
  EXPECT_NE(m.find("local %xs.0: mem(48, 8)"), std::string::npos) << m;
  EXPECT_EQ(count(m, "ptr.store %"), 6u) << m;
  EXPECT_EQ(count(m, "ptr.load opt_i64, %"), 2u) << m;
  // The tuple: the `bool?` (two bytes) at 0, the Str at 8.
  EXPECT_NE(m.find("local %t.4: mem(16, 8)"), std::string::npos) << m;
  EXPECT_NE(m.find("local %c.5: opt_bool"), std::string::npos) << m;
  EXPECT_NE(m.find(" = some true\n  ptr.store %t.addr."), std::string::npos)
      << m;
  EXPECT_NE(m.find(" = ptr.load opt_bool, %t.addr."), std::string::npos) << m;
  EXPECT_EQ(count(m, " = ptr.load box, %elem.addr."), 2u) << m; // t.1, its drop
  EXPECT_EQ(l.Text.find("PaykanTuple_"), std::string::npos) << l.Text;
}

// An optional value is its presence flag and then its payload at its
// alignment: an array's elements and a native function's PaykanType have
// that size, 16 bytes for `int?` and `float?` and 2 for `bool?` and `char?`.
TEST(Lowering, OptionalValueSizesFollowTheirPayloads) {
  auto l = lower(R"(
    native fn keep<T>(v: view T) = "pk_keep";
    fn main() -> int {
      a: int?[1] = [1];  b: float?[1] = [None];
      c: bool?[1] = [True];  d: char?[2] = ['x', None];
      i: int? = 1;  f: bool? = None;
      keep<int?>(i);
      keep<bool?>(f);
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  for (const char *local : {"local %a.0: mem(16, 8)", "local %b.1: mem(16, 8)",
                            "local %c.2: mem(2, 1)", "local %d.3: mem(4, 1)"})
    EXPECT_NE(m.find(local), std::string::npos) << local << "\n" << m;
  // d's second element is one `char?` in.
  EXPECT_NE(m.find("ptr.add %d.addr.7, 2\n"), std::string::npos) << m;
  EXPECT_NE(l.Text.find("= [16, 8, 5, 0]"), std::string::npos) << l.Text;
  EXPECT_NE(l.Text.find("= [2, 1, 5, 0]"), std::string::npos) << l.Text;
}

// A fixed-size array is a `mem` local reached through its address: an
// element at a constant index is a `ptr.add`, one at a computed index is
// bounds-checked first, and a copy is a `mem.copy` of its bytes.  Nothing
// is boxed or counted.
TEST(Lowering, FixedArraysAreMemSlots) {
  auto l = lower(R"(
    fn main() -> int {
      grid: char[2][3] = [['a', 'b', 'c'], ['d', 'e', 'f']];
      row = grid[1];
      i = 2;
      return int(row[i]) + int(grid[0][1]);
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  for (const char *e :
       {"local %grid.0: mem(6, 1)", "local %row.1: mem(3, 1)",
        "ptr.add %grid.addr", "mem.copy %row.addr", ", 3\n",
        "call @$rt.Paykan_panic_index(%i.", "ptr.load char, %elem.addr"})
    EXPECT_NE(m.find(e), std::string::npos) << e << "\n" << m;
  // Only the computed index is checked.
  EXPECT_EQ(count(m, "Paykan_panic_index"), 1u) << m;
  EXPECT_EQ(count(m, "retain"), 0u) << m;
}

// A fixed-size array parameter is an address in every mode; a by-value one
// is copied on entry, a `view` one is not.  A result is written where a last
// `ptr` parameter points, and the function returns nothing.
TEST(Lowering, FixedArrayParametersAndResults) {
  auto l = lower(R"(
    fn first(xs: view int[3]) -> int { return xs[0]; }
    fn twice(xs: int[3]) -> int[3] { return [xs[0] * 2, xs[1], xs[2]]; }
    fn main() -> int {
      xs: int[3] = [1, 2, 3];
      ys = twice(xs);
      return first(ys);
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string first = function(l.Text, "first");
  std::string twice = function(l.Text, "twice");
  std::string m = function(l.Text, "main");
  EXPECT_NE(first.find("fn @first(%xs.1: ptr) -> i64"), std::string::npos)
      << first;
  EXPECT_EQ(count(first, "mem.copy"), 0u) << first;
  EXPECT_NE(twice.find("fn @twice(%xs.1: ptr, %result.2: ptr) -> void"),
            std::string::npos)
      << twice;
  EXPECT_NE(twice.find("local %xs.0: mem(24, 8)"), std::string::npos) << twice;
  EXPECT_NE(twice.find("mem.copy %xs.addr"), std::string::npos) << twice;
  EXPECT_NE(m.find("call @twice(%xs.addr"), std::string::npos) << m;
  EXPECT_NE(m.find(", %res.addr"), std::string::npos) << m;
}

// An element of a reference type is its box, owned by the slot: a copy
// retains it, the scope's exit releases it (in a loop past a few), and a
// call's result moves into the variable it initialises.
TEST(Lowering, FixedArraysOfReferencesOwnTheirElements) {
  auto l = lower(R"(
    fn pair() -> Str[2] { return ["a", "b"]; }
    fn main() -> int {
      xs: Str[2] = pair();
      ys = xs;
      big: Str[5] = ["1", "2", "3", "4", ys[1]];
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  for (const char *e :
       {"local %xs.0: mem(16, 8)", "local %big.", ": mem(40, 8)",
        "ptr.load box, %", "while {", "cmp lt %i.", ", 5\n"})
    EXPECT_NE(m.find(e), std::string::npos) << e << "\n" << m;
  // ys's two elements and ys[1] into big; nothing for pair()'s result.
  EXPECT_EQ(count(m, "retain"), 3u) << m;
  // xs's and ys's two each, unrolled; big's five in one loop.
  EXPECT_EQ(count(m, "release"), 5u) << m;
}

// `[v; n]`: a zero of a plain scalar element is one `mem.zero` of the whole
// array; any other v is stored in slot 0 and then in the others, one by one
// for up to four more and in a `while` loop past that.
TEST(Lowering, RepeatLiteralsOfScalars) {
  auto l = lower(R"(
    fn zeros() -> int {
      xs: int[64] = [0; 64];
      fs = [0.0; 8];
      bs = [False; 4];
      cs = ['\0'; 2];
      return xs[3];
    }
    fn fill(x: int) -> int {
      xs: int[64] = [x; 64];
      return xs[3];
    }
    fn few(x: int) -> int {
      xs = [x + 1; 5];
      return xs[4];
    }
    fn main() -> int { return zeros() + fill(1) + few(2); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string zeros = function(l.Text, "zeros");
  EXPECT_NE(zeros.find("mem.zero %xs.addr.1, 512\n"), std::string::npos)
      << zeros;
  EXPECT_EQ(count(zeros, "mem.zero"), 4u) << zeros;
  EXPECT_EQ(count(zeros, "ptr.store"), 0u) << zeros;
  EXPECT_EQ(count(zeros, "while"), 0u) << zeros;
  std::string fill = function(l.Text, "fill");
  for (const char *e : {"while {", "cmp lt %i.", ", 64\n"})
    EXPECT_NE(fill.find(e), std::string::npos) << e << "\n" << fill;
  // Slot 0, then the loop's one store.
  EXPECT_EQ(count(fill, "ptr.store"), 2u) << fill;
  EXPECT_EQ(count(fill, "mem.zero"), 0u) << fill;
  std::string few = function(l.Text, "few");
  EXPECT_EQ(count(few, "while"), 0u) << few;
  EXPECT_EQ(count(few, "ptr.store"), 5u) << few;
  // v is computed once.
  EXPECT_EQ(count(few, " = add "), 1u) << few;
}

// A reference v is retained once for each slot past the first (a variable's
// value once more, as any copy of it is), a nested array element is a
// `mem.copy` of the first row with its references retained, and v is
// evaluated once.
TEST(Lowering, RepeatLiteralsOfReferences) {
  auto l = lower(R"(
    fn shared() -> int {
      s = "a";
      xs: Str[3] = [s; 3];
      return xs[2].len();
    }
    fn fresh(n: int) -> int {
      xs: Str[3] = [Str(n); 3];
      return xs[2].len();
    }
    fn rows() -> int {
      s = "a";
      g: Str[3][2] = [[s; 2]; 3];
      return g[2][1].len();
    }
    fn main() -> int { return shared() + fresh(7) + rows(); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string shared = function(l.Text, "shared");
  EXPECT_EQ(count(shared, "retain"), 3u) << shared;
  EXPECT_EQ(count(shared, "ptr.store %"), 3u) << shared;
  // The literal's three and s.
  EXPECT_EQ(count(shared, "release"), 4u) << shared;
  std::string fresh = function(l.Text, "fresh");
  EXPECT_EQ(count(fresh, "call @$rt.PaykanString_from_int("), 1u) << fresh;
  EXPECT_EQ(count(fresh, "retain"), 2u) << fresh;
  std::string rows = function(l.Text, "rows");
  EXPECT_EQ(count(rows, "mem.copy"), 2u) << rows;
  // s into the first row's two slots, and each copied row's two elements.
  EXPECT_EQ(count(rows, "retain"), 6u) << rows;
}

// A fixed-size array field is a `mem` field of the object, written in
// place, and the destructor releases the references in it.
TEST(Lowering, FixedArrayFieldsAreMemFields) {
  auto l = lower(R"(
    class Grid { cells: int[2][3]; tags: Str[2]; }
    fn main() -> int {
      g = Grid([[1, 2, 3], [4, 5, 6]], ["a", "b"]);
      g.cells[1][2] = 7;
      return g.cells[1][2];
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  for (const char *e : {"field cells: mem(48, 8)", "field tags: mem(16, 8)"})
    EXPECT_NE(l.Text.find(e), std::string::npos) << e << "\n" << l.Text;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find("field.addr %"), std::string::npos) << m;
  std::string d = function(l.Text, "Grid.destroy");
  EXPECT_NE(d.find("field.addr %self"), std::string::npos) << d;
  EXPECT_EQ(count(d, "release"), 2u) << d;
}

// A slice parameter is two PIR parameters, the address of the elements and
// their number: a fixed-size array argument passes its address and its
// length, a slice passes both on.
TEST(Lowering, SlicesAreAnAddressAndALength) {
  auto l = lower(R"(
    fn sum(xs: view int[]) -> int { return xs[0] + xs.len(); }
    fn pass(xs: view int[]) -> int { return sum(xs); }
    fn main() -> int {
      a: int[3] = [1, 2, 3];
      return pass(a);
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string sum = function(l.Text, "sum");
  EXPECT_NE(sum.find("fn @sum(%xs.1: ptr, %xs.len.2: i64) -> i64"),
            std::string::npos)
      << sum;
  EXPECT_NE(sum.find("call @$rt.Paykan_panic_index"), std::string::npos) << sum;
  std::string pass = function(l.Text, "pass");
  EXPECT_NE(pass.find("call @sum(%xs.addr"), std::string::npos) << pass;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find("call @pass(%a.addr"), std::string::npos) << m;
  EXPECT_NE(m.find(", 3)"), std::string::npos) << m;
}

// Printing an optional value formats it, "None" or the value's text; it is
// never boxed.
TEST(Lowering, PrintingAnOptionalValueFormatsIt) {
  auto l = lower(R"(
    fn main() -> int {
      a: int? = 5;  b: bool? = None;
      s = a.toString();  t = b.toString();
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_NE(m.find("call @$rt.PaykanString_from_int("), std::string::npos) << m;
  EXPECT_NE(m.find("call @$rt.PaykanString_from_bool("), std::string::npos)
      << m;
  // An optional value is formatted in place: not through the None object.
  EXPECT_EQ(m.find("PaykanObject_None"), std::string::npos) << m;
}

// -- Modules

TEST(Lowering, ImportedFunctionsAndClassesAreExternItems) {
  auto dir = std::filesystem::temp_directory_path() /
             ("lowering_imports_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  {
    std::ofstream lib(dir / "lib.pkn");
    lib << "class Adder { n: int; "
           "fn add(x: int) -> int { return self.n + x; } }\n"
           "impl Adder { fn __init__(n: int) { self.n = n; } }\n"
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
      << "class K { v: int; }\n"
         "impl K { fn __init__() { self.v = 2; } }\n"
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

// Another module's templates are instantiated by their importers, and each
// instantiation is emitted once per program, by the first module lowered
// that needs it; the others declare it extern.  Its body reaches the
// template module's functions by their symbols there.
TEST(Lowering, ImportedInstantiationsAreEmittedOnce) {
  auto dir = std::filesystem::temp_directory_path() /
             ("lowering_generic_imports_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir / "lib");
  std::ofstream(dir / "lib" / "gen.pkn")
      << "class Box<T> { v: T; "
         "view fn n() -> int { return _h(); } }\n"
         "impl Box<T> { fn __init__(v: T) { self.v = v; } }\n"
         "fn _h() -> int { return 1; }\n"
         "fn first<T>(xs: view T[]) -> T { return xs[0]; }\n";
  for (const char *m : {"a", "b"})
    std::ofstream(dir / (std::string(m) + ".pkn"))
        << "import lib::gen;\n"
           "fn "
        << m
        << "() -> int { box = gen::Box<Str>(\"s\"); "
           "return box.n() + gen::first<int>([1]); }\n";
  auto l = lower("import lib::gen; import a; import b;\n"
                 "fn main() -> int { b = gen::Box<Str>(\"m\");\n"
                 "  return b.n() + gen::first<int>([2]) + a::a() + b::b(); }\n",
                 dir.string());
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(l.Ok) << l.Error;
  const std::string &t = l.Text;
  EXPECT_EQ(count(t, "\nshared class Box<Str> {"), 1u) << t;
  EXPECT_EQ(count(t, "\nextern shared class Box<Str> module \"a\""), 2u) << t;
  EXPECT_EQ(count(t, "\nshared fn @\"$lib::gen::first<int>\"("), 1u) << t;
  EXPECT_EQ(count(t, "\nextern shared fn @\"$lib::gen::first<int>\"(ptr, i64) "
                     "-> i64 module \"a\"\n"),
            2u)
      << t;
  for (const pir::Module &m : l.Program.Modules) {
    const pir::Class *box = m.findClass("Box<Str>");
    EXPECT_EQ(box && !box->IsExtern, m.Name == "a") << m.Name;
  }
  EXPECT_NE(t.find("extern fn @\"$lib::gen::_h\"() -> i64 module "
                   "\"lib::gen\" symbol @_h\n"),
            std::string::npos)
      << t;
}

// Before they are shared, every module that uses an instantiation of another
// module's template has a copy of its own, so its PIR is the same in every
// program (docs/design/pkm.md §4.8); shareInstantiations keeps the first
// (imports first, the main module last) and makes the others externs of it,
// a vcall on the class naming the slot's signature instead.
TEST(Lowering, EveryModuleLowersItsOwnCopiesUntilShared) {
  auto dir = std::filesystem::temp_directory_path() /
             ("lowering_generic_copies_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir / "lib");
  std::ofstream(dir / "lib" / "gen.pkn")
      << "class Box<T> { v: T; "
         "view fn n() -> int { return 1; } }\n"
         "impl Box<T> { fn __init__(v: T) { self.v = v; } }\n"
         "fn first<T>(xs: view T[]) -> T { return xs[0]; }\n";
  for (const char *m : {"a", "b"})
    std::ofstream(dir / (std::string(m) + ".pkn"))
        << "import lib::gen;\n"
           "fn "
        << m
        << "() -> int { box = gen::Box<Str>(\"s\"); "
           "return box.n() + gen::first<int>([1]); }\n";
  const std::string main =
      "import lib::gen; import a; import b;\n"
      "fn main() -> int { b = gen::Box<Str>(\"m\");\n"
      "  return b.n() + gen::first<int>([2]) + a::a() + b::b(); }\n";
  auto copies = lower(main, dir.string(), lowering::Copies::PerModule);
  auto shared = lower(main, dir.string());
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(copies.Ok) << copies.Error;
  ASSERT_TRUE(shared.Ok) << shared.Error;
  // `a` and `b` make their own; the main module may use one its imports
  // have, which every program it is in has too.
  for (const pir::Module &m : copies.Program.Modules) {
    if (m.Name == "lib::gen")
      continue;
    const pir::Class *box = m.findClass("Box<Str>");
    const pir::Function *first = m.findFunction("$lib::gen::first<int>");
    ASSERT_TRUE(box && first) << m.Name;
    if (m.Name == "main") {
      EXPECT_TRUE(!box->IsExtern || box->Module == "a" || box->Module == "b");
      EXPECT_TRUE(!first->IsExtern || first->Module == "a" ||
                  first->Module == "b");
    } else {
      EXPECT_FALSE(box->IsExtern) << m.Name;
      EXPECT_FALSE(first->IsExtern) << m.Name;
    }
  }
  lowering::shareInstantiations(copies.Program);
  std::ostringstream os;
  pir::print(copies.Program, os);
  EXPECT_EQ(os.str(), shared.Text);
  EXPECT_TRUE(pir::verify(copies.Program).empty());
  EXPECT_EQ(count(shared.Text, "\nshared class Box<Str> {"), 1u) << shared.Text;
  EXPECT_NE(shared.Text.find(": (obj) -> i64 ["), std::string::npos)
      << shared.Text;
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
                 "fn main() -> int { s = x::tag() + r::tag() + y::tag() "
                 "+ tag(); return s.len(); }\n",
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
// call, and bound to a variable it is boxed like any other temporary.
TEST(Lowering, StringLiteralUsedAsAnObjectIsAStr) {
  auto l = lower(R"(
    fn main() -> int {
      c = "ab"[1];
      n = "abc".len();
      v = "lit";
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
  // the bound one is boxed into `v` (released at scope exit).
  EXPECT_EQ(count(m, "call @$rt.Paykan_dispose(%str."), 2u) << m;
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
    fn Paykan_println(s: Str) { n = s.len(); }
    fn Paykan_panic_div_by_zero() { n = "mine".len(); }
    fn PaykanObject_None() -> int { return 4; }
    fn z() -> int { return 0; }
    fn main() -> int {
      Paykan_println("x");
      Paykan_panic_div_by_zero();
      a: Str? = None;
      s = a.toString();
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
  for (const char *name :
       {"$rt.PaykanString_new", "$rt.Paykan_panic_div_by_zero"}) {
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

TEST(Lowering, ParsesCallTheRuntimeAndReturnAnOptionalValue) {
  // The runtime parses into a local; the result is that value as the
  // present optional, or None (#96): no box.
  auto l = lower(R"(
    fn p(s: Str) -> int? { return int<Str>(s); }
    fn q(s: Str) -> float? { return float<Str>(s); }
    fn t(s: Str) -> bool? { return bool<Str>(s); }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  const char *expected[][3] = {
      {"p", "call @$rt.PaykanString_parse_int(", "none opt_i64"},
      {"q", "call @$rt.PaykanString_parse_float(", "none opt_f64"},
      {"t", "call @$rt.PaykanString_parse_bool(", "none opt_bool"},
  };
  for (const auto &e : expected) {
    std::string fn = function(l.Text, e[0]);
    EXPECT_NE(fn.find(e[1]), std::string::npos) << fn;
    EXPECT_NE(fn.find("local.addr"), std::string::npos) << fn;
    EXPECT_NE(fn.find(" = some "), std::string::npos) << fn;
    EXPECT_NE(fn.find(e[2]), std::string::npos) << fn;
    EXPECT_EQ(fn.find(" = box "), std::string::npos) << fn;
  }
}

// -- Moves at last use (docs/pir.md §12): a copy at a variable's last use is
// -- handed on, the retain and the variable's release gone

namespace {

const char *const kTake = R"(
    class Node { n: int; }
    fn take(n: Node) -> int { return n.n; }
)";

/// @p source (after kTake) lowered, through the moves-at-last-use pass and
/// verified after it; what the pass did is in @p stats.
Lowered lowerMoved(const std::string &source, pir::Statistics &stats) {
  Lowered l = lower(kTake + source);
  if (!l.Ok)
    return l;
  pir::PassManager passes(/*verifyEach=*/true);
  passes.add(pir::createMovesAtLastUsePass());
  if (Status s = passes.run(l.Program, stats); !s) {
    l.Ok = false;
    l.Error = s.message();
    return l;
  }
  l.Text = pir::toString(l.Program);
  return l;
}

Lowered lowerMoved(const std::string &source) {
  pir::Statistics stats;
  return lowerMoved(source, stats);
}

} // namespace

TEST(LoweringMoves, ALastUseInEachBranchOfAnIfMoves) {
  auto l = lowerMoved(R"(
    fn f(a: Node, c: bool) -> int {
      r = 0;
      if c { r = take(a); } else { r = take(a) + 1; }
      return r;
    }
    fn main() -> int { return f(Node(1), True); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "f");
  EXPECT_EQ(count(f, "retain"), 0u) << f;
  EXPECT_EQ(count(f, "release"), 0u) << f;
  EXPECT_EQ(count(f, "null box"), 0u) << f;
}

TEST(LoweringMoves, ALastUseInSomeArmsOfAMatchReleasesInTheOthers) {
  // Two arms hand `a` on; the third releases it where the match ends, as
  // the scope exit did.
  auto l = lowerMoved(R"(
    fn f(a: Node, k: int) -> int {
      r = 0;
      match k {
        0 { r = take(a); }
        1 { r = 2; }
        _ { r = take(a) + 1; }
      }
      return r;
    }
    fn main() -> int { return f(Node(1), 1); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "f");
  EXPECT_EQ(count(f, "retain"), 0u) << f;
  EXPECT_EQ(count(f, "release"), 1u) << f;
  size_t arm = f.find("case 1 {");
  ASSERT_NE(arm, std::string::npos) << f;
  EXPECT_GT(f.find("release"), arm) << f;
  EXPECT_LT(f.find("release"), f.find("default {")) << f;
}

TEST(LoweringMoves, ALastUseInEveryArmOfAMatchMovesInEach) {
  // Every arm hands `a` on.  A bool's two arms leave no other path; an
  // enum's switch has one where no case matches, which gets the release.
  auto l = lowerMoved(R"(
    enum Color { Red, Green, Blue }
    fn byBool(a: Node, b: bool) -> int {
      r = 0;
      match b {
        True { r = take(a); }
        False { r = take(a) + 1; }
      }
      return r;
    }
    fn byEnum(a: Node, c: Color) -> int {
      r = 0;
      match c {
        Red { r = take(a); }
        Green { r = take(a) + 1; }
        Blue { r = take(a) + 2; }
      }
      return r;
    }
    fn main() -> int { return byBool(Node(1), True) + byEnum(Node(2), Color::Red); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "byBool");
  EXPECT_EQ(count(f, "retain"), 0u) << f;
  EXPECT_EQ(count(f, "release"), 0u) << f;
  EXPECT_EQ(count(f, "null box"), 0u) << f;
  f = function(l.Text, "byEnum");
  EXPECT_EQ(count(f, "retain"), 0u) << f;
  EXPECT_EQ(count(f, "null box"), 0u) << f;
  EXPECT_EQ(count(f, "release"), 1u) << f;
  EXPECT_LT(f.find("default {"), f.find("release")) << f;
}

TEST(LoweringMoves, AUseInALoopBodyIsACopyAndTheUseAfterTheLoopMoves) {
  auto l = lowerMoved(R"(
    fn f(a: Node) -> int {
      s = 0;
      for i in 0..3 { s = s + take(a); }
      return s + take(a);
    }
    fn main() -> int { return f(Node(1)); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "f");
  // The retain left is the loop body's.
  EXPECT_EQ(count(f, "retain"), 1u) << f;
  size_t loop = f.find("while {");
  ASSERT_NE(loop, std::string::npos) << f;
  EXPECT_GT(f.find("retain"), loop) << f;
  EXPECT_EQ(count(f, "release"), 0u) << f;
}

TEST(LoweringMoves, AVariableReassignedAfterItsLastUseMovesBothValues) {
  auto l = lowerMoved(R"(
    fn f(a: Node) -> int {
      x = a;
      n = take(x);
      x = Node(5);
      return n + take(x);
    }
    fn main() -> int { return f(Node(1)); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "f");
  EXPECT_EQ(count(f, "retain"), 0u) << f;
  EXPECT_EQ(count(f, "release"), 0u) << f;
}

TEST(LoweringMoves, AParameterIsHandedOnAtItsLastUse) {
  pir::Statistics stats;
  auto l = lowerMoved(R"(
    fn f(a: Node) -> int { return take(a); }
    fn main() -> int { return f(Node(1)); }
  )",
                      stats);
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "f");
  EXPECT_EQ(count(f, "retain"), 0u) << f;
  EXPECT_EQ(count(f, "release"), 0u) << f;
  EXPECT_GE(stats.get("moves-at-last-use", "transfers"), 1u);
}

TEST(LoweringMoves, ReturningALocalHandsItsReferenceToTheCaller) {
  auto l = lowerMoved(R"(
    fn f() -> Node { x = Node(1); return x; }
    fn main() -> int { return take(f()); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "f");
  EXPECT_EQ(count(f, "retain"), 0u) << f;
  EXPECT_EQ(count(f, "release"), 0u) << f;
}

TEST(LoweringMoves, OnlyTheSecondOfTwoUsesInOneCallMoves) {
  auto l = lowerMoved(R"(
    fn two(a: Node, b: Node) -> int { return a.n + b.n; }
    fn f(a: Node) -> int { return two(a, a); }
    fn main() -> int { return f(Node(1)); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "f");
  EXPECT_EQ(count(f, "retain"), 1u) << f;
  EXPECT_EQ(count(f, "release"), 0u) << f;
}

TEST(LoweringMoves, SelfIsNeverHandedOn) {
  // `self` is a borrow the caller's reference keeps alive: a method that
  // passes it on or returns it takes a reference of its own.
  const std::string src = R"(
    impl Node {
      fn give() -> int { return take(self); }
      fn same() -> Node { return self; }
    }
    fn main() -> int { n = Node(1); return n.give() + take(n.same()); }
  )";
  auto before = lower(kTake + src);
  auto l = lowerMoved(src);
  ASSERT_TRUE(before.Ok) << before.Error;
  ASSERT_TRUE(l.Ok) << l.Error;
  for (const char *m : {"Node.give", "Node.same"}) {
    std::string after = function(l.Text, m);
    EXPECT_EQ(after, function(before.Text, m));
    EXPECT_NE(after.find(" = box %self"), std::string::npos) << after;
  }
}

TEST(LoweringMoves, AFieldReadHeldAcrossALaterCallKeepsTheCopy) {
  // `replace` may assign h.node while the name read before it is still to
  // be used, and x may be what keeps that node alive: x is handed to
  // takeName as a copy.  With no such read, it moves.
  auto l = lowerMoved(R"(
    class Named { name: Str; }
    class Holder { node: Named; }
    fn takeName(n: Named) -> Str { return n.name; }
    fn replace(h: Holder) -> Str { h.node = Named("other"); return "!"; }
    fn kept(h: Holder) -> Str {
      x = h.node;
      s = takeName(x);
      return h.node.name + replace(h) + s;
    }
    fn moved(h: Holder) -> Str {
      x = h.node;
      s = takeName(x);
      return replace(h) + s;
    }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "kept");
  EXPECT_EQ(count(f, "retain %x."), 1u) << f;
  f = function(l.Text, "moved");
  EXPECT_EQ(count(f, "retain %x."), 0u) << f;
}

TEST(LoweringMoves, AMethodKeepsACopyWhileItStillUsesSelf) {
  // The caller may hold no reference to the receiver (read from a field),
  // and the parameter may be what keeps it alive.  In `__init__` the
  // constructor holds the object, so each field takes its parameter.
  auto l = lowerMoved(R"(
    class Named { name: Str; }
    class Pair2 { a: Named; b: Named; }
    impl Pair2 {
      fn __init__(a: Named, b: Named) { self.a = a; self.b = b; }
      fn swap(n: Named) -> Str {
        old = self.a;
        self.a = n;
        return old.name + self.b.name;
      }
    }
    fn takeName(n: Named) -> Str { return n.name; }
    impl Named {
      fn give(n: Named) -> Str { return takeName(n) + self.name; }
    }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "Pair2.__init__");
  EXPECT_EQ(count(f, "retain"), 0u) << f;
  EXPECT_EQ(count(f, "release %"), 2u) << f; // the fields' old values
  f = function(l.Text, "Named.give");
  EXPECT_EQ(count(f, "retain %n."), 1u) << f;
}

TEST(LoweringMoves, AViewIsNeverHandedOn) {
  // A view names what another variable holds: the object lives as long as
  // the function that views it.  Passing a variable to a view parameter at
  // its last use is a move.
  auto l = lowerMoved(R"(
    class Named { name: Str; }
    fn look(n: view Named) -> Str { return n.name + ""; }
    fn outer(n: view Named) -> Str { return look(n); }
    fn local(n: Named) -> Str { v: view = n; s = look(v); return s; }
    fn plain(n: Named) -> Str { return look(n); }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "outer");
  EXPECT_NE(f.find(": box view"), std::string::npos) << f;
  EXPECT_EQ(count(f, "retain %n."), 1u) << f;
  f = function(l.Text, "local");
  EXPECT_EQ(count(f, "retain %v."), 1u) << f;
  f = function(l.Text, "plain");
  EXPECT_EQ(count(f, "retain"), 0u) << f;
}

TEST(LoweringMoves, HeldValuesCopiedWholeAtTheirLastUseMove) {
  // A tuple, a struct, an enum with a reference and a function value: each
  // copy into a new variable or out of the function is a move.
  auto l = lowerMoved(R"(
    struct P { node: Node; k: int; }
    enum E { A(node: Node), B }
    fn tuple(a: Node) -> (Node, int) { t = (a, 1); u = t; return u; }
    fn pair(a: Node) -> P { p = P(a, 1); q = p; return q; }
    fn variant(a: Node) -> E { e = E::A(a); g = e; return g; }
    fn function(a: Node) -> () -> int {
      f = fn() -> int { return a.n; };
      g = f;
      return g;
    }
    fn main() -> int {
      return tuple(Node(1)).1 + pair(Node(2)).k + function(Node(3))();
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  for (const char *fn : {"tuple", "pair", "variant"}) {
    std::string f = function(l.Text, fn);
    // In pair, `a` given to P's constructor stays a copy: the result is
    // written after the call through a pointer pair holds no reference to
    // (docs/pir.md §12, borrowed references).
    const size_t copies = std::string(fn) == "pair" ? 1 : 0;
    EXPECT_EQ(count(f, "retain"), copies) << f;
    EXPECT_EQ(count(f, "release"), copies) << f;
  }
  // The capture is a copy (the closure keeps a reference of its own); the
  // function value is moved.
  std::string f = function(l.Text, "function");
  EXPECT_EQ(count(f, "retain"), 1u) << f;
  EXPECT_EQ(count(f, "release"), 1u) << f;
}

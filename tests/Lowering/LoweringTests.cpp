// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// AST -> PIR lowering tests: every program is lowered, verified, printed, and
// the printed PIR is checked for the ownership shapes the lowering must
// produce (docs/pir.md §7).  The programs' behaviour is covered by the
// CodeGen suite, run on the mlir backend.

#include "Lowering/Holds.h"
#include "ModuleName.h"
#include "TestUtils.h"
#include "paykan/lowering/Lowering.h"
#include "paykan/pir/Lend.h"
#include "paykan/pir/MoveAtLastUse.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Pass.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
/// says, the main module knowing what @p world says; and check that the
/// code passes keep every hold of what it printed.
Lowered lower(const std::string &source, const std::string &projectRoot = "",
              lowering::Copies copies = lowering::Copies::Shared,
              lowering::World world = lowering::World::Open) {
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
                              l.Program, errs, nullptr, copies, world)) {
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
  // What the driver runs next, the code passes (docs/pir.md §12), on a
  // copy: what they make verifies, and every hold keeps a retain.
  pir::ParseError perr;
  auto copy = pir::parseProgram(l.Text, perr);
  if (!copy) {
    l.Error = "printed PIR does not parse: " + perr.str();
    return l;
  }
  if (std::string e = codePassesKeepHolds(*copy); !e.empty()) {
    l.Error = "code passes: " + e + "\n" + l.Text;
    return l;
  }
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

/// The printed function @p name ("fn @name(...)" up to the closing brace
/// at column 0), its parameters' prologue included.
std::string fullFunction(const std::string &text, const std::string &name) {
  size_t start = text.find("fn @" + name + "(");
  if (start == std::string::npos)
    return "";
  size_t end = text.find("\n}\n", start);
  return text.substr(start, end == std::string::npos ? std::string::npos
                                                     : end + 3 - start);
}

/// The printed function @p name without its parameters' prologue (`%o =
/// owned %p`, `%l = not %o`, `if %l { retain %p }`, docs/pir.md §3), which
/// the prologue tests check: what the body does with its parameters.
std::string function(const std::string &text, const std::string &name) {
  std::istringstream in(fullFunction(text, name));
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);)
    lines.push_back(line);
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    auto value = [](const std::string &line) {
      return line.substr(2, line.find(' ', 2) - 2);
    };
    const std::string &l = lines[i];
    const size_t own = l.find(" = owned ");
    if (own != std::string::npos && i + 4 < lines.size() &&
        l.starts_with("  %")) {
      const std::string o = value(l);
      const std::string n = value(lines[i + 1]);
      std::string rest;
      for (size_t k = i + 1; k <= i + 4; ++k)
        rest.append(lines[k]).append("\n");
      std::string prologue;
      prologue.append("  ").append(n).append(" = not ").append(o);
      prologue.append("\n  if ").append(n).append(" {\n    retain ");
      prologue.append(l.substr(own + 9)).append("\n  }\n");
      if (rest == prologue) {
        i += 4;
        continue;
      }
    }
    out.append(l).append("\n");
  }
  return out;
}

/// The value the first instruction of @p f containing @p what defines
/// (`%b.4` of `%b.4 = field.load %h.obj.3, H.b`), or "".
std::string defined(const std::string &f, const std::string &what) {
  const size_t at = f.find(what);
  if (at == std::string::npos)
    return "";
  const size_t line = f.find_first_not_of(' ', f.rfind('\n', at) + 1);
  const size_t eq = f.find(" = ", line);
  return eq < at ? f.substr(line, eq - line) : "";
}

/// Whether the first instruction of @p f containing @p what is a `stable`
/// load (docs/pir.md §6).
bool stableLoad(const std::string &f, const std::string &what) {
  const size_t at = f.find(what);
  if (at == std::string::npos)
    return false;
  const std::string line = f.substr(at, f.find('\n', at) - at);
  return line.ends_with(" stable");
}

/// Whether the box @p value is retained before the first @p call of @p f and
/// released after it: a reference of its own while the call runs.
bool heldAcross(const std::string &f, const std::string &value,
                const std::string &call) {
  const size_t c = f.find(call);
  return !value.empty() && c != std::string::npos &&
         f.find("retain " + value + "\n") < c &&
         f.find("release " + value + "\n", c) != std::string::npos;
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
  // One PaykanType per type: size, alignment, kind, the counts of its
  // references and of the words that tell storage never set, then those
  // (offsets): a Str is its reference, which is never NULL in a value.
  EXPECT_NE(l.Text.find("data @.type0 = [8, 8, 4, 1, 1, 0, 0]"),
            std::string::npos)
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
  EXPECT_NE(l.Text.find("data @.type0 = [1, 1, 2, 0, 0]"), std::string::npos)
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
  EXPECT_NE(l.Text.find("data @.type0 = [16, 8, 5, 0, 0]"), std::string::npos)
      << l.Text;
  EXPECT_NE(l.Text.find("data @.type1 = [2, 1, 5, 0, 0]"), std::string::npos)
      << l.Text;
}

// A PaykanType names the words of a value that are all zero only in storage
// never set (a buffer's new slot): a reference that may not be None, a
// function value's code word, and for a tagged union whose first variant
// holds one, the tag and then that variant's words.  A type whose zero
// bytes are a value names none: an int, an optional (None), an enum whose
// first variant holds only values and optionals.
TEST(Lowering, GenericNativeFunctionDescribesStorageNeverSet) {
  auto l = lower(R"(
    native fn keep<T>(v: view T) = "pk_keep";
    enum F { B(s: Str), A }
    enum G { A(n: int, s: Str?), B(s: Str) }
    enum H { X(s: Str), Y }
    enum E { A(h: H), B }
    enum Maybe<T> { Just(v: T), Nothing }
    struct S { n: int; f: () -> int; }
    struct P { f: F; s: Str; }
    fn main() -> int {
      keep<F>(F::A);
      keep<() -> int>(fn() => 1);
      o: (() -> int)? = None;
      keep<(() -> int)?>(o);
      keep<S>(S(1, fn() => 2));
      keep<G>(G::B("x"));
      keep<E>(E::B);
      keep<Maybe<Str>>(Maybe<Str>::Nothing);
      keep<Maybe<int>>(Maybe<int>::Nothing);
      keep<P>(P(F::A, "p"));
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  for (const char *descriptor : {
           // F: the tag at 0, B's Str at 8 (nullable: NULL in an A).
           "= [16, 8, 6, 1, 2, 17, 0, 8]\n",
           // () -> int: the code word at 0, the closure (nullable) at 8.
           "= [16, 8, 6, 1, 1, 17, 0]\n",
           // Its optional: the same 16 bytes, None all zeros.
           "= [16, 8, 6, 1, 0, 17]\n",
           // S: the function value at 8, its code word.
           "= [24, 8, 6, 1, 1, 33, 8]\n",
           // G: zeros are A(0, None).
           "= [32, 8, 6, 2, 0, 33, 49]\n",
           // E: its tag, A's H's tag, then H's X's Str.
           "= [24, 8, 6, 1, 3, 33, 0, 8, 16]\n",
           // Maybe<int>: zeros are Just(0).
           "= [16, 8, 6, 0, 0]\n",
           // P: its Str alone tells, the fewest words.
           "= [24, 8, 6, 2, 1, 17, 32, 16]\n",
       })
    EXPECT_NE(l.Text.find(descriptor), std::string::npos) << descriptor << "\n"
                                                          << l.Text;
  // Maybe<Str> is described as F is: one descriptor for both.
  EXPECT_EQ(count(l.Text, "= [16, 8, 6, 1, 2, 17, 0, 8]\n"), 1u) << l.Text;
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

// A read that a later call of its statement may replace the variable under
// (an `inout` argument, a struct's method that may change `self`) takes a
// reference of its own before the call and lets it go after its use: a
// variable's box retained, a struct copied into a temporary.  The place the
// call is given is no read, and a statement with no such call reads in
// place (docs/pir.md §7, "Reads a call may replace").
TEST(Lowering, AReadACallMayReplaceHasAReferenceOfItsOwn) {
  auto l = lower(R"(
    class Node { name: Str; view fn greet(s: Str) -> Str { return self.name + s; } }
    struct H { n: Node; }
    impl H { fn reset() -> Str { self.n = Node("r"); return "!"; } }
    fn repl(x: inout Str) -> Str { x = "b"; return "!"; }
    fn replNode(x: inout Node) -> Str { x = Node("c"); return "?"; }
    fn joined(s: Str) -> Str { return s + repl(s); }
    fn receiver(l: Node) -> Str { return l.greet(replNode(l)); }
    fn field(h: H) -> Str { return h.n.greet(h.reset()); }
    fn other(s: Str, l: Node) -> Str { return l.greet(repl(s)); }
    fn main() -> int {
      return joined("a").len() + receiver(Node("n")).len() +
             field(H(Node("m"))).len() + other("o", Node("p")).len();
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  for (const char *fn : {"joined", "receiver"}) {
    std::string f = function(l.Text, fn);
    const size_t call =
        f.find(std::string(fn) == "joined" ? "call @repl(" : "call @replNode(");
    ASSERT_NE(call, std::string::npos) << f;
    // The read's retain comes before the call, its release after it.
    EXPECT_EQ(count(f.substr(0, call), "retain "), 1u) << f;
    const size_t retain = f.find("retain ");
    const std::string read =
        f.substr(retain + 7, f.find('\n', retain) - retain - 7);
    const size_t release = f.find("release " + read + "\n");
    ASSERT_NE(release, std::string::npos) << f;
    EXPECT_GT(release, call) << f;
  }
  std::string f = function(l.Text, "field");
  const size_t reset = f.find("call @H.reset(");
  ASSERT_NE(reset, std::string::npos) << f;
  EXPECT_NE(f.find("local %held."), std::string::npos) << f;
  EXPECT_LT(f.find("mem.copy "), reset) << f;
  // `l` is not what repl changes: its receiver is read in place.
  std::string o = function(l.Text, "other");
  EXPECT_EQ(count(o, "retain "), 0u) << o;
}

// A box read from an object while a call that may replace it runs takes a
// reference of its own for as long as it is in use: a method's receiver
// (the method itself is such a call, given the receiver), the left operand
// of `+` and of `==`, the object a field assignment writes to (twice for
// `+=`).  A call given nothing that reaches an object replaces nothing, so
// the reads around it stay in place (docs/pir.md §7, "Reads a call may
// replace").
TEST(Lowering, AReadThroughAnObjectACallMayReplaceIsHeld) {
  auto l = lower(R"(
    class B { s: Str; a: int[3]; fn m(h: H) -> Str { return self.s; } }
    class H { b: B; fn drop() -> Str { return self.b.m(self); } }
    fn g(h: H) -> Str {
      h.b = B("y", [4, 5, 6]);
      return "!";
    }
    fn gi(h: H) -> int {
      h.b = B("y", [4, 5, 6]);
      return 1;
    }
    fn shout(s: Str) -> Str { return s + "!"; }
    fn receiver(h: H) -> Str { return h.b.m(h); }
    fn operand(h: H) -> Str { return h.b.s + g(h); }
    fn equal(h: H) -> bool { return h.b.s == g(h); }
    fn target(h: H) { h.b.s = g(h); }
    fn compound(h: H) { h.b.s += g(h); }
    fn inPlace(h: H) -> Str { return h.b.s + shout("x") + Str(h.b.a[0] + gi(h)); }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  struct Case {
    const char *Fn, *Read, *Call;
  };
  for (const Case &c : {
           Case{"receiver", ", H.b", "vcall "},
           Case{"H.drop", ", H.b", "vcall "},
           Case{"operand", ", B.s", "call @g("},
           Case{"equal", ", B.s", "call @g("},
           Case{"target", ", H.b", "call @g("},
       }) {
    std::string f = function(l.Text, c.Fn);
    EXPECT_TRUE(heldAcross(f, defined(f, c.Read), c.Call)) << c.Fn << "\n" << f;
  }
  // `h.b.s = h.b.s + g(h)`: the object written to and the string joined.
  std::string f = function(l.Text, "compound");
  EXPECT_TRUE(heldAcross(f, defined(f, ", H.b"), "call @g(")) << f;
  EXPECT_TRUE(heldAcross(f, defined(f, ", B.s"), "call @g(")) << f;
  // shout is given a string and gi is called after the element is read:
  // the only retain is gi's argument.
  f = function(l.Text, "inPlace");
  EXPECT_EQ(count(f, "retain "), 1u) << f;
}

// A call given an object, of a function whose body stores no reference
// outside its own variables and makes no call that may (a method returning
// a field, storing a number and calling another such method, or calling
// itself), cannot replace a read: the read stays in place.  One that stores
// a reference (into a field, through an `inout` parameter, into a struct's
// `self`), calls a function that may (also through a recursion), or runs a
// method a subclass may override (an `Obj`'s too) still holds it
// (docs/pir.md §7, "Reads a call may replace").  The reads here are rooted
// in a parameter the function may hand on, which keeps the rule without the
// types: `h.b` is held across `B.reset`, which lets go of a `Str` only.
TEST(Lowering, ACallOfAFunctionThatReleasesNothingHoldsNoRead) {
  auto l = lower(R"(
    class B {
      s: Str;
      n: int;
      fn virt() -> int { return self.n; }
    }
    impl B {
      fn count() -> int { return self.n; }
      fn bump() -> int {
        self.n = self.n + 1;
        return self.twice(self.n);
      }
      fn twice(k: int) -> int { return 2 * k; }
      fn down(k: int) -> int {
        if k == 0 {
          return self.n;
        }
        return self.down(k - 1);
      }
      fn reset() -> int {
        self.s = "r";
        return 0;
      }
      fn viaFree(h: H) -> int { return clear(h); }
      fn ping(k: int, h: H) -> int {
        if k == 0 {
          return clear(h);
        }
        return self.pong(k - 1, h);
      }
      fn pong(k: int, h: H) -> int { return self.ping(k, h); }
      fn setIt(x: inout Str) -> int {
        x = "x";
        return 0;
      }
      fn viaVirt() -> int { return self.virt(); }
      fn viaObj(o: Obj) -> int { return o.toString().len(); }
    }
    struct S { b: B; }
    impl S {
      fn reset() -> int {
        self.b = B("r", 0);
        return 0;
      }
    }
    class H {
      b: B;
      c: B;
      s: S;
      o: B?;
    }
    fn clear(h: H) -> int {
      h.b = B("c", 0);
      return 0;
    }
    fn drop(x: inout B?) -> int {
      x = None;
      return 0;
    }
    fn peek(h: H) -> Str { return "!" + Str(h.b.count()); }
    fn byCount(h: H) -> int { return h.b.count(); }
    fn byBump(h: H) -> int { return h.b.bump(); }
    fn byDown(h: H) -> int { return h.b.down(3); }
    fn byReset(h: H) -> int { return h.b.reset(); }
    fn byFree(h: H) -> int { return h.b.viaFree(h); }
    fn byPing(h: H) -> int { return h.b.ping(2, h); }
    fn byVirt(h: H) -> int { return h.b.viaVirt(); }
    fn byObj(h: H) -> int { return h.b.viaObj(h); }
    fn bySet(h: H) -> Str { return h.b.s + Str(h.c.setIt(h.b.s)); }
    fn byStruct(h: H) -> Str { return h.s.b.s + Str(h.s.reset()); }
    fn byDrop(h: H) -> Str { return h.b.s + Str(drop(h.o)); }
    fn operand(h: H) -> Str { return h.b.s + peek(h); }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  for (const char *fn : {"byCount", "byBump", "byDown"}) {
    std::string f = function(l.Text, fn);
    EXPECT_EQ(count(f, "retain "), 0u) << fn << "\n" << f;
  }
  struct Case {
    const char *Fn, *Read, *Call;
  };
  for (const Case &c : {
           Case{"byReset", ", H.b", "call @B.reset("},
           Case{"byFree", ", H.b", "call @B.viaFree("},
           Case{"byPing", ", H.b", "call @B.ping("},
           Case{"byVirt", ", H.b", "call @B.viaVirt("},
           Case{"byObj", ", H.b", "call @B.viaObj("},
           Case{"bySet", ", B.s", "call @B.setIt("},
           Case{"byStruct", ", B.s", "call @S.reset("},
           Case{"byDrop", ", B.s", "call @drop("},
       }) {
    std::string f = function(l.Text, c.Fn);
    EXPECT_TRUE(heldAcross(f, defined(f, c.Read), c.Call)) << c.Fn << "\n" << f;
  }
  std::string f = function(l.Text, "operand");
  EXPECT_FALSE(heldAcross(f, defined(f, ", B.s"), "call @peek(")) << f;
  EXPECT_EQ(count(f, "retain "), 1u) << f; // peek's argument
}

// A read of storage the function never hands on (a class method's `self`,
// a struct method's, an `inout`, `view` or slice parameter) is held across
// a call only when the call may let go of a reference of the read slot's
// class: one it stores into (a `Str` field), what dying objects of an exact
// class it lets go of hold (a `Cell<int>`'s `Str`), whatever it is when it
// lets go of an object of a class a subclass of which may hold anything (a
// `B?`) or calls a function value.  A weak store lets go of nothing, and a
// store of a number neither.  A constructor does what its `__init__` does,
// with the superclass's through `__super__`; the runtime's `==` of a tuple
// what its objects' `equals` do.  A read rooted in a parameter it may hand
// on keeps the rule without the types (docs/pir.md §7).  A read a call that
// may let go of references does not hold is read by a `stable` load; one
// in use at no such call (`self.s` across `setN`, which lets go of nothing)
// is read as before.
TEST(Lowering, ACallHoldsOnlyTheReadsOfTheClassesItMayLetGo) {
  auto l = lower(R"(
    class Box<T> { v: T; view fn at(k: int) -> T { return self.v; } }
    class Cell<T> { v: T; name: Str; }
    class Par { name: Str; }
    impl Par { fn __init__(n: Str) { self.name = n; } }
    class Kid<T> : Par { v: T; }
    impl Kid<T> { fn __init__(v: T) { __super__("p"); self.v = v; } }
    class Tag<T> {
      v: T;
      s: Str;
      view fn equals(o: Obj) -> bool {
        match o {
          t: Tag<T> { t.s = "seen"; }
          _ { }
        }
        return True;
      }
    }
    class B {
      s: Str;
      n: int;
      weak w: B?;
      g: Box<int>;
      c: Cell<int>;
      o: B?;
    }
    impl B {
      fn setS() -> int {
        self.s = "r";
        return 0;
      }
      fn setN() -> int {
        self.n = 1;
        return 0;
      }
      fn link(x: B) -> int {
        self.w = x;
        return 0;
      }
      fn setG() -> int {
        self.g = Box<int>(1);
        return 0;
      }
      fn setC() -> int {
        self.c = Cell<int>(1, "c");
        return 0;
      }
      fn setO() -> int {
        self.o = None;
        return 0;
      }
      view fn take(k: Kid<Box<int>>) -> int { return k.v.v; }
      view fn apply(f: () -> int) -> int { return f(); }
      view fn count(b: bool) -> int { return self.n; }
      view fn note(k: H) -> int {
        k.s = "n";
        return 0;
      }
    }
    class H {
      b: B;
      s: Str;
      g: Box<int>;
      fn receiver() -> int { return self.b.setS(); }
      fn string() -> Str { return self.s + Str(self.b.setS()); }
      fn number() -> Str { return self.s + Str(self.b.setN()); }
      fn weakStore() -> int { return self.b.link(self.b); }
      fn sameClass() -> int { return self.g.at(self.b.setG()); }
      fn otherClass() -> Str { return self.b.s + Str(self.b.setG()); }
      fn destroyed() -> Str { return self.s + Str(self.b.setC()); }
      fn anything() -> Str { return self.s + Str(self.b.setO()); }
      fn built() -> int { return self.b.take(Kid<Box<int>>(self.g)); }
      fn superInit() -> Str {
        return self.s + Str(Kid<Box<int>>(self.g).v.v);
      }
      fn closure(f: () -> int) -> int { return self.b.apply(f); }
      fn compared(a: (Tag<int>, int), x: (Tag<int>, int)) -> Str {
        return self.s + Str(a == x);
      }
      fn comparedB(a: (Tag<int>, int), x: (Tag<int>, int)) -> int {
        return self.b.count(a == x);
      }
    }
    struct S { b: B; }
    impl S { fn receiver() -> int { return self.b.setS(); } }
    fn byInout(x: inout B) -> int { return x.setS(); }
    fn byView(h: view H, k: H) -> int { return h.b.note(k); }
    fn bySlice(xs: inout B[]) -> int { return xs[0].setS(); }
    fn byLocal(h: H) -> int { return h.b.setS(); }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  struct Case {
    const char *Fn, *Read, *Call;
    bool Held, Stable;
  };
  for (const Case &c : {
           Case{"H.receiver", ", H.b", "call @B.setS(", false, true},
           Case{"H.string", ", H.s", "call @B.setS(", true, false},
           Case{"H.number", ", H.s", "call @B.setN(", false, false},
           Case{"H.weakStore", ", H.b", "call @B.link(", false, true},
           Case{"H.sameClass", ", H.g", "call @B.setG(", true, false},
           Case{"H.otherClass", ", B.s", "call @B.setG(", false, true},
           Case{"H.destroyed", ", H.s", "call @B.setC(", true, false},
           Case{"H.anything", ", H.s", "call @B.setO(", true, false},
           Case{"H.built", ", H.b", "call @Kid<Box<int>>(", false, true},
           Case{"H.superInit", ", H.s", "call @Kid<Box<int>>(", true, false},
           Case{"H.closure", ", H.b", "call @B.apply(", true, false},
           Case{"H.compared", ", H.s", "PaykanTuple_equals(", true, false},
           Case{"H.comparedB", ", H.b", "PaykanTuple_equals(", false, true},
           Case{"S.receiver", "ptr.load box", "call @B.setS(", false, true},
           Case{"byInout", "ptr.load box", "call @B.setS(", false, true},
           Case{"byView", ", H.b", "call @B.note(", false, true},
           Case{"bySlice", "ptr.load box", "call @B.setS(", false, true},
           Case{"byLocal", ", H.b", "call @B.setS(", true, false},
       }) {
    std::string f = function(l.Text, c.Fn);
    EXPECT_EQ(heldAcross(f, defined(f, c.Read), c.Call), c.Held) << c.Fn << "\n"
                                                                 << f;
    EXPECT_EQ(stableLoad(f, c.Read), c.Stable) << c.Fn << "\n" << f;
  }
}

// A call that replaces a slot through a narrowed `inout` arm (`d: inout
// Derived<int>` on a `Base` field) lets go of a reference of the field's
// class, not of the arm's: a read of the field is held across it, though
// what dies is an object of an exact class that holds no `Base`.  So is the
// read of an `inout` parameter, whose slot may be a field of a superclass
// of its type, across a call that replaces that field; and a read of a
// parameter the function may hand on across any call that may let go of a
// reference (the counterexamples of the analysis of these holds).  So they
// are in the main module of a program, which knows that a `Base` that dies
// may be a `Plain` or a `Derived<int>`.
TEST(Lowering, AReadACallMayReplaceThroughANarrowedArmIsHeld) {
  const std::string source = R"(
    class Base { n: int; }
    impl Base {
      fn __init__(n: int) { self.n = n; }
      view fn m(x: int) -> int { return self.n + x; }
    }
    class Derived<X> : Base { v: X; }
    impl Derived<X> {
      fn __init__(n: int, v: X) { __super__(n); self.v = v; }
    }
    class Plain : Base { tag: Str; }
    impl Plain {
      fn __init__(n: int, t: Str) { __super__(n); self.tag = t; }
    }
    class Holder { b: Base; }
    impl Holder {
      fn run() -> int { return self.b.m(reset(self)); }
      fn runPlain() -> int { return self.b.m(resetPlain(self)); }
    }
    fn reset(h: Holder) -> int {
      match h.b {
        d: inout Derived<int> { d = Derived<int>(1, 9); }
        _ { }
      }
      return 7;
    }
    fn resetPlain(h: Holder) -> int {
      match h.b {
        d: inout Plain { d = Plain(1, "new"); }
        _ { }
      }
      return 7;
    }
    class Wrap { h: Holder; }
    fn clobber(w: Wrap) -> int {
      w.h.b = Base(100);
      return 5;
    }
    fn use(x: inout Derived<int>, w: Wrap) -> int { return x.m(clobber(w)); }
    class Node { name: Str; }
    class Pair { node: Node; other: Node; }
    fn touchOther(p: Pair) -> Str {
      p.other = Node("o2");
      return "!";
    }
    fn local(p: Pair) -> Str { return p.node.name + touchOther(p); }
    fn main() -> int { return 0; }
  )";
  struct Case {
    const char *Fn, *Read, *Call;
  };
  for (lowering::World world :
       {lowering::World::Open, lowering::World::Closed}) {
    auto l = lower(source, "", lowering::Copies::Shared, world);
    ASSERT_TRUE(l.Ok) << l.Error;
    for (const Case &c : {
             Case{"Holder.run", ", Holder.b", "call @reset("},
             Case{"Holder.runPlain", ", Holder.b", "call @resetPlain("},
             Case{"use", "ptr.load box", "call @clobber("},
             Case{"local", ", Node.name", "call @touchOther("},
         }) {
      std::string f = function(l.Text, c.Fn);
      EXPECT_TRUE(heldAcross(f, defined(f, c.Read), c.Call)) << c.Fn << "\n"
                                                             << f;
      EXPECT_FALSE(stableLoad(f, c.Read)) << c.Fn << "\n" << f;
    }
  }
}

// The standard library's containers keep their state in a private object,
// read through `self` of a struct method: a call that lets go only of the
// elements' and the nodes' references (a LinkedList's pushFront, a map's
// put and clear over buffers of entries, a heap's pop) holds no read of it;
// one over elements a subclass may make hold anything (a class key's map,
// a stack of closures) holds it.
TEST(Lowering, ContainerMethodsHoldTheirStateOnlyForWhatTheyMayLetGo) {
  auto l = lower(R"(import ::collections as c;
    class Dog { name: Str; }
    fn main() -> int {
      s = c::Stack<int>();
      s.push(1);
      m = c::HashMap<Str, int>();
      m.put("a", 1);
      m.clear();
      d = c::HashMap<Str, Dog>();
      d.put("d", Dog("rex"));
      q = c::MinPriorityQueue<Str>();
      q.push("x", 1);
      f = c::Stack<() -> int>();
      f.push(fn() => 2);
      return s.pop() + int(u32(m.len())) + int(u32(d.len())) + q.pop().len() +
             f.pop()();
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  struct Case {
    const char *Fn, *Call;
    bool Held;
  };
  for (const Case &c : {
           Case{"Stack<int>.push", "call @LinkedList<int>.pushFront(", false},
           Case{"\"HashMap<Str, int>.put\"", "call @\"_Table<Str, int>.put\"(",
                false},
           Case{"\"HashMap<Str, int>.clear\"",
                "call @\"_Table<Str, int>.clear\"(", false},
           Case{"MinPriorityQueue<Str>.pop", "call @_Heap<Str>.pop(", false},
           Case{"\"HashMap<Str, Dog>.put\"", "call @\"_Table<Str, Dog>.put\"(",
                true},
           Case{"\"Stack<() -> int>.push\"",
                "call @\"LinkedList<() -> int>.pushFront\"(", true},
       }) {
    std::string f = function(l.Text, c.Fn);
    ASSERT_FALSE(f.empty()) << c.Fn << "\n" << l.Text;
    EXPECT_EQ(heldAcross(f, defined(f, "ptr.load box, %self.addr"), c.Call),
              c.Held)
        << c.Fn << "\n"
        << f;
    // What is not held is read by a stable load (docs/pir.md §6).
    EXPECT_EQ(stableLoad(f, "ptr.load box, %self.addr"), !c.Held)
        << c.Fn << "\n"
        << f;
  }
}

// The main module of a program (World::Closed) knows every class derived
// from its classes: a virtual call on one runs the implementation its
// vtable names or an override of a class derived from it (a pure one's
// trap stops the program), and an object of one that dies holds the fields
// of its class or of one of those.  So `Quiet.run`, overridden by `Hush`,
// and the pure `Shape.area` lets go of nothing a read is of; `Loud.run`
// may, through `Shout`'s override, which replaces `Holder.item`; and a
// `Dog` that dies holds a string, but a `Cat` may be a `Lion` holding the
// map's table.  A main module that may be imported (World::Open, as
// --emit-pkm lowers it) knows none of it, and holds every such read.
TEST(Lowering, TheMainModuleOfAProgramKnowsItsClassesOverrides) {
  const std::string source = R"(import ::collections as c;
    class Item { name: Str; }
    class Quiet { fn run(h: Holder) -> int { return 1; } }
    class Hush : Quiet { fn run(h: Holder) -> int { return 2; } }
    class Loud { fn run(h: Holder) -> int; }
    class Shout : Loud {
      fn run(h: Holder) -> int {
        h.item = Item("new");
        return 2;
      }
    }
    class Holder {
      item: Item;
      quiet: Quiet;
      loud: Loud;
      fn viaQuiet() -> Str { return self.item.name + Str(self.quiet.run(self)); }
      fn viaLoud() -> Str { return self.item.name + Str(self.loud.run(self)); }
    }
    class Shape { view fn area() -> int; }
    class Square : Shape {
      s: int;
      view fn area() -> int { return self.s * self.s; }
    }
    fn total(shapes: view Shape[]) -> int {
      t = 0;
      for i in 0..shapes.len() { t = t + shapes[i].area(); }
      return t;
    }
    class Dog { name: Str; }
    class Cat { name: Str; }
    class Lion : Cat { pride: c::HashMap<Str, Cat>; }
    fn main() -> int {
      d = c::HashMap<Str, Dog>();
      d.put("d", Dog("rex"));
      k = c::HashMap<Str, Cat>();
      k.put("k", Cat("tom"));
      return int(u32(d.len() + k.len()));
    }
  )";
  struct Case {
    const char *Fn, *Read, *Call;
    bool Held;
  };
  const Case cases[] = {
      {"Holder.viaQuiet", ", Item.name", "vcall ", false},
      {"Holder.viaLoud", ", Item.name", "vcall ", true},
      {"total", "ptr.load box", "vcall ", false},
      {"\"HashMap<Str, Dog>.put\"", "ptr.load box, %self.addr",
       "call @\"_Table<Str, Dog>.put\"(", false},
      {"\"HashMap<Str, Cat>.put\"", "ptr.load box, %self.addr",
       "call @\"_Table<Str, Cat>.put\"(", true},
  };
  auto closed =
      lower(source, "", lowering::Copies::Shared, lowering::World::Closed);
  ASSERT_TRUE(closed.Ok) << closed.Error;
  auto open = lower(source);
  ASSERT_TRUE(open.Ok) << open.Error;
  for (const Case &c : cases) {
    std::string f = function(closed.Text, c.Fn);
    ASSERT_FALSE(f.empty()) << c.Fn << "\n" << closed.Text;
    EXPECT_EQ(heldAcross(f, defined(f, c.Read), c.Call), c.Held) << c.Fn << "\n"
                                                                 << f;
    // What is not held is read by a stable load (docs/pir.md §6).
    EXPECT_EQ(stableLoad(f, c.Read), !c.Held) << c.Fn << "\n" << f;
    std::string g = function(open.Text, c.Fn);
    EXPECT_TRUE(heldAcross(g, defined(g, c.Read), c.Call)) << c.Fn << "\n" << g;
  }
}

// A field no code stores into once its object is made keeps the reference
// its object's `__init__` gave it (docs/pir.md §7): `self.fixed.name` needs
// no hold across a call through a function value, which may do anything,
// nor does a field a derived class's `__init__` stores.  Every other way to
// store into a field keeps its read held: an assignment, a compound one, an
// `inout` argument or local, a `match` arm's `n: inout T` (whose copy is
// written back), an `-> inout` result (either branch of one), a lambda's
// body (one an `__init__` makes too), a derived class's method, a copy of
// a generic function, which names its type argument's fields, and an
// `__init__`'s store into another object of the class.  So is a
// struct's part in an object (a copy of the struct replaces it), a read in
// an `__init__`, and one rooted in a parameter the function may hand on.
// Outside a program's main module, where an importer may name `Holder`,
// only a private field's stores are all known: one no template of the
// module stores into either, as `spoil`, which an importer's copy may run,
// does `_spoiled`.
TEST(Lowering, AFieldStoredOnlyAtConstructionNeedsNoHold) {
  const std::string source = R"(
    class Item { name: Str; }
    class Special : Item { }
    impl Special { fn __init__(n: Str) { __super__(n); } }
    struct P { name: Str; }
    fn bump(s: inout Str) { s = s + "!"; }
    fn setTag<T>(x: T) { x.generic = "g"; }
    class Holder {
      fixed: Item;
      assigned: Str;
      compound: Str;
      byArg: Str;
      byLocal: Str;
      byArm: Item;
      byResult: Str;
      byTernary: Str;
      byLambda: Str;
      inherited: Str;
      generic: Str;
      childInit: Str;
      pos: P;
      fn slot(first: bool) -> inout Str {
        return if first then self.byResult else self.byTernary;
      }
      fn grow() { self.compound += "x"; }
      view fn fixedRead(f: () -> int) -> Str { return self.fixed.name + Str(f()); }
      view fn assignedRead(f: () -> int) -> Str { return self.assigned + Str(f()); }
      view fn compoundRead(f: () -> int) -> Str { return self.compound + Str(f()); }
      view fn byArgRead(f: () -> int) -> Str { return self.byArg + Str(f()); }
      view fn byLocalRead(f: () -> int) -> Str { return self.byLocal + Str(f()); }
      view fn byArmRead(f: () -> int) -> Str { return self.byArm.name + Str(f()); }
      view fn byResultRead(f: () -> int) -> Str { return self.byResult + Str(f()); }
      view fn byTernaryRead(f: () -> int) -> Str { return self.byTernary + Str(f()); }
      view fn byLambdaRead(f: () -> int) -> Str { return self.byLambda + Str(f()); }
      view fn inheritedRead(f: () -> int) -> Str { return self.inherited + Str(f()); }
      view fn genericRead(f: () -> int) -> Str { return self.generic + Str(f()); }
      view fn childInitRead(f: () -> int) -> Str { return self.childInit + Str(f()); }
      view fn posRead(f: () -> int) -> Str { return self.pos.name + Str(f()); }
    }
    impl Holder {
      fn __init__(f: () -> int) {
        self.fixed = Item("f");
        self.assigned = "a";
        self.compound = "c";
        self.byArg = "b";
        self.byLocal = "l";
        self.byArm = Item("m");
        self.byResult = "r";
        self.byTernary = "t";
        self.byLambda = "z";
        self.inherited = "i";
        self.generic = "g";
        self.childInit = "c";
        self.pos = P("p");
        early = self.fixed.name + Str(f());
        k = fn() { self.byLambda = "k"; };
        k();
      }
    }
    class Child : Holder {
      fn poke() { self.inherited = "p"; }
    }
    impl Child {
      fn __init__(f: () -> int) { __super__(f); self.childInit = "child"; }
    }
    fn outside(h: Holder, f: () -> int) -> Str { return h.fixed.name + Str(f()); }
    fn stores(h: Holder) {
      h.assigned = "x";
      bump(h.byArg);
      x: inout = h.byLocal;
      x = "y";
      match h.byArm {
        s: inout Special { s = Special("s"); }
        _ { }
      }
      setTag(h);
    }
    class Vault {
      __secret: Str;
      _shared: Str;
      open: Str;
      _spoiled: Str;
      view fn secretRead(f: () -> int) -> Str { return self.__secret + Str(f()); }
      view fn sharedRead(f: () -> int) -> Str { return self._shared + Str(f()); }
      view fn openRead(f: () -> int) -> Str { return self.open + Str(f()); }
      view fn spoiledRead(f: () -> int) -> Str { return self._spoiled + Str(f()); }
    }
    impl Vault {
      fn __init__() {
        self.__secret = "s";
        self._shared = "m";
        self.open = "o";
        self._spoiled = "p";
      }
    }
    fn spoil<T>(v: T) { v._spoiled = "t"; }
    class Link {
      tag: Str;
      view fn tagRead(f: () -> int) -> Str { return self.tag + Str(f()); }
    }
    impl Link {
      fn __init__(prev: Link?) {
        self.tag = "t";
        match prev {
          o: Link { o.tag = "o"; }
          None { }
        }
      }
    }
    fn main() -> int { return 0; }
  )";
  struct Case {
    const char *Fn, *Read;
    bool Closed, Open; // fixed in that world
  };
  const Case cases[] = {
      {"Holder.fixedRead", ", Item.name", true, false},
      {"Holder.childInitRead", ", Holder.childInit", true, false},
      {"Holder.assignedRead", ", Holder.assigned", false, false},
      {"Holder.compoundRead", ", Holder.compound", false, false},
      {"Holder.byArgRead", ", Holder.byArg", false, false},
      {"Holder.byLocalRead", ", Holder.byLocal", false, false},
      {"Holder.byArmRead", ", Item.name", false, false},
      {"Holder.byResultRead", ", Holder.byResult", false, false},
      {"Holder.byTernaryRead", ", Holder.byTernary", false, false},
      {"Holder.byLambdaRead", ", Holder.byLambda", false, false},
      {"Holder.inheritedRead", ", Holder.inherited", false, false},
      {"Holder.genericRead", ", Holder.generic", false, false},
      {"Holder.posRead", "ptr.load box, %pos.addr", false, false},
      {"Holder.__init__", ", Item.name", false, false},
      {"outside", ", Item.name", false, false},
      {"Vault.secretRead", ", Vault.__secret", true, true},
      {"Vault.sharedRead", ", Vault._shared", true, true},
      {"Vault.openRead", ", Vault.open", true, false},
      {"Vault.spoiledRead", ", Vault._spoiled", true, false},
      {"Link.tagRead", ", Link.tag", false, false},
  };
  for (lowering::World world :
       {lowering::World::Open, lowering::World::Closed}) {
    auto l = lower(source, "", lowering::Copies::Shared, world);
    ASSERT_TRUE(l.Ok) << l.Error;
    for (const Case &c : cases) {
      const bool fixed = world == lowering::World::Closed ? c.Closed : c.Open;
      std::string f = function(l.Text, c.Fn);
      ASSERT_FALSE(f.empty()) << c.Fn << "\n" << l.Text;
      EXPECT_EQ(heldAcross(f, defined(f, c.Read), "icall "), !fixed)
          << c.Fn << "\n"
          << f;
      EXPECT_EQ(stableLoad(f, c.Read), fixed) << c.Fn << "\n" << f;
    }
  }
}

// The storage of a value held in an object's field, used where it is while
// a call that may replace the object runs, keeps the object alive: an
// array's while its index or the element assigned in it is computed, one
// passed to a `view` or slice parameter, a struct method's receiver, an
// `inout` argument in a struct field, the storage a call's `inout` result
// names.  The object is retained first and released after the use; for an
// `inout` local, by a hidden local as long as the local can be used.
TEST(Lowering, StorageInAnObjectACallMayReplaceKeepsTheObject) {
  auto l = lower(R"(
    struct S { v: Str; }
    impl S { fn take(h: H) { self.v = "t"; } }
    class A {
      s: S; arr: Str[2]; k: Str; n: int[2];
      fn slot(h: H) -> inout Str { return self.k; }
    }
    class H { a: A; }
    fn gi(h: H) -> int {
      h.a.k = "g";
      return 1;
    }
    fn word(h: H) -> Str {
      h.a.k = "w";
      return "w";
    }
    fn look(xs: view Str[], h: H) -> int {
      h.a.k = "l";
      return xs[0].len();
    }
    fn put(x: inout Str, h: H) { x = "p"; }
    fn pick(x: inout Str) -> inout Str { return x; }
    fn element(h: H) -> int { return h.a.n[gi(h)]; }
    fn store(h: H) { h.a.arr[1] = word(h); }
    fn storeAt(h: H) { h.a.n[gi(h)] = 2; }
    fn slice(h: H) -> int { return look(h.a.arr, h); }
    fn receiver(h: H) { h.a.s.take(h); }
    fn field(h: H) { put(h.a.s.v, h); }
    fn result(h: H) { put(h.a.slot(h), h); }
    fn local(h: H) { x: inout = h.a.s.v; x = "l"; }
    fn picked(h: H) { y: inout = pick(h.a.k); y = "r"; }
    fn bound(h: H) { z: inout = h.a.slot(h); z = "b"; }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  struct Case {
    const char *Fn, *Call;
  };
  for (const Case &c :
       {Case{"element", "call @gi("}, Case{"store", "call @word("},
        Case{"storeAt", "call @gi("}, Case{"slice", "call @look("},
        Case{"receiver", "call @S.take("}, Case{"field", "call @put("},
        Case{"result", "call @put("}}) {
    std::string f = function(l.Text, c.Fn);
    EXPECT_TRUE(heldAcross(f, defined(f, ", H.a"), c.Call)) << c.Fn << "\n"
                                                            << f;
  }
  struct Borrow {
    const char *Fn, *Local;
  };
  for (const Borrow &b :
       {Borrow{"local", "x"}, Borrow{"picked", "y"}, Borrow{"bound", "z"}}) {
    std::string f = function(l.Text, b.Fn);
    const std::string a = defined(f, ", H.a");
    const size_t hold = f.find(std::string("store %") + b.Local + ".obj.");
    ASSERT_NE(hold, std::string::npos) << f;
    const std::string store = f.substr(hold, f.find('\n', hold) - hold);
    EXPECT_LT(f.find("retain " + a + "\n"), hold) << f;
    EXPECT_EQ(store.substr(store.size() - a.size()), a) << f;
  }
}

// What an `inout` parameter, a value passed by address or a struct
// method's `self` names is its caller's, perhaps an object's field that a
// call given an object may replace: a box read from there is held across
// such a call as well.  An `inout` result read as a value is read before
// what holds its storage is let go.
TEST(Lowering, AReadOfTheCallersStorageACallMayReplaceIsHeld) {
  auto l = lower(R"(
    class H { k: Str; fn slot(h: H) -> inout Str { return self.k; } }
    class G { h: H; }
    struct S { v: Str; }
    impl S { fn both(h: H) -> Str { return self.v + g(h); } }
    fn g(h: H) -> Str {
      h.k = "g";
      return "!";
    }
    fn param(x: inout Str, h: H) -> Str { return x + g(h); }
    fn slice(xs: view Str[], h: H) -> Str { return xs[0] + g(h); }
    fn read(o: G) -> Str { return o.h.slot(o.h) + "!"; }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  for (const char *fn : {"param", "S.both", "slice"}) {
    std::string f = function(l.Text, fn);
    EXPECT_TRUE(heldAcross(f, defined(f, "ptr.load box"), "call @g("))
        << fn << "\n"
        << f;
  }
  std::string f = function(l.Text, "read");
  const size_t load = f.find(" = ptr.load box, %mcall");
  ASSERT_NE(load, std::string::npos) << f;
  EXPECT_GT(f.find("release " + defined(f, ", G.h") + "\n"), load) << f;
}

// `Str(s)` is `s`: a field's string returned through it is retained, as the
// field returned is, not handed out as the field's own reference.
TEST(Lowering, StrOfAFieldIsRetainedWhenHandedOn) {
  auto l = lower(R"(
    class Gen { v: Str; fn show() -> Str { return Str(self.v); } }
    fn main() -> int { return Gen("x").show().len(); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "Gen.show");
  const std::string v = defined(f, ", Gen.v");
  EXPECT_LT(f.find("retain " + v + "\n"), f.find("ret " + v)) << f;
}

// An `inout` result used after its call may be in the object an `inout`
// argument of a class holds (`return b.k;`), or in one held in a value held
// in memory there: once the call returns, the caller retains what the
// argument holds (for a value held in memory, a copy of it) for as long as
// the address is used, in a hidden local for an `inout` local.  A result
// read at once is read before anything can replace it.
TEST(Lowering, AnInoutResultHoldsWhatItsInoutArgumentsHold) {
  auto l = lower(R"(
    class B { k: Str; }
    struct S { b: B; }
    class H { b: B; s: S; }
    fn pick(b: inout B) -> inout Str { return b.k; }
    fn pickS(s: inout S) -> inout Str { return s.b.k; }
    fn put(x: inout Str, h: H) { x = "p"; }
    fn bound(h: H) { x: inout = pick(h.b); x = "b"; }
    fn passed(h: H) { put(pick(h.b), h); }
    fn read(h: H) -> Str { return pick(h.b) + "!"; }
    fn copied(h: H) { y: inout = pickS(h.s); y = "c"; }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "bound");
  std::string held = defined(f, "ptr.load box, %b.addr");
  ASSERT_FALSE(held.empty()) << f;
  EXPECT_LT(f.find("call @pick("), f.find("retain " + held + "\n")) << f;
  const size_t store = f.find("store %x.obj.");
  ASSERT_NE(store, std::string::npos) << f;
  const std::string line = f.substr(store, f.find('\n', store) - store);
  EXPECT_EQ(line.substr(line.rfind(' ') + 1), held) << f;
  f = function(l.Text, "passed");
  held = defined(f, "ptr.load box, %b.addr");
  EXPECT_TRUE(heldAcross(f, held, "call @put(")) << f;
  f = function(l.Text, "read");
  EXPECT_EQ(f.find("ptr.load box, %b.addr"), std::string::npos) << f;
  f = function(l.Text, "copied");
  const size_t call = f.find("call @pickS(");
  ASSERT_NE(call, std::string::npos) << f;
  EXPECT_LT(call, f.find("mem.copy %held.addr.")) << f;
  EXPECT_LT(call, f.find("mem.copy %y.obj.addr.")) << f;
}

// A string's character: the string is read before its index is computed,
// with a reference of its own across a call there that may replace it, and
// in place across one that cannot.
TEST(Lowering, AStringIsReadBeforeTheIndexOfItsCharacter) {
  auto l = lower(R"(
    class B { s: Str; }
    class H { b: B; }
    fn g(h: H) -> int {
      h.b = B("y");
      return 0;
    }
    fn n(k: int) -> int { return k; }
    fn held(h: H) -> char { return h.b.s[g(h)]; }
    fn inPlace(h: H) -> char { return h.b.s[n(1)]; }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "held");
  EXPECT_TRUE(heldAcross(f, defined(f, ", B.s"), "call @g(")) << f;
  f = function(l.Text, "inPlace");
  EXPECT_LT(f.find(", B.s"), f.find("call @n(")) << f;
  EXPECT_EQ(count(f, "retain "), 0u) << f;
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

// A pure virtual method's function is a trap, the slot's target in its
// class and in a subclass that leaves it unimplemented; an implementation
// replaces it in the subclass's vtable.  An abstract class has no
// constructor (nothing allocates it) but keeps its `__init__` and its
// destructor, which its subclasses' use.
TEST(Lowering, PureVirtualSlotsAreTrapsAndAbstractClassesHaveNoConstructor) {
  auto l = lower(R"(
    class Shape {
      name: Str;
      view fn area() -> float;
      view fn kind() -> Str;
    }
    impl Shape {
      fn __init__(name: Str) { self.name = name; }
      view fn report() -> float { return self.area(); }
    }
    class Polygon : Shape {
      view fn kind() -> Str { return "polygon"; }
    }
    class Square : Polygon {
      side: float;
      view fn area() -> float { return self.side * self.side; }
    }
    fn main() -> int { s: Shape = Square("s", 2.0); return int<float>(s.report()); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  const std::string &t = l.Text;
  std::string trap = function(t, "Shape.area");
  EXPECT_EQ(trap,
            "fn @Shape.area(%self.1: obj) -> f64 {\n"
            "  call @$rt.Paykan_panic_pure_virtual(@.str0, 5, @.str1, 4)\n"
            "  unreachable\n}\n")
      << t;
  EXPECT_NE(t.find("cstr @.str0 = \"Shape\" len 5\ncstr @.str1 = \"area\" "
                   "len 4\n"),
            std::string::npos)
      << t;
  EXPECT_NE(function(t, "Shape.kind").find("Paykan_panic_pure_virtual"),
            std::string::npos)
      << t;
  EXPECT_NE(t.find("extern fn @$rt.Paykan_panic_pure_virtual(ptr, i64, ptr, "
                   "i64) -> void"),
            std::string::npos)
      << t;
  // Shape's slots are its traps; Polygon inherits area's and implements
  // kind; Square implements area.
  EXPECT_EQ(count(t, "area = @Shape.area : (obj) -> f64"), 2u) << t;
  EXPECT_NE(t.find("kind = @Polygon.kind : (obj) -> box"), std::string::npos)
      << t;
  EXPECT_NE(t.find("area = @Square.area : (obj) -> f64"), std::string::npos)
      << t;
  // Only the concrete class is constructed.
  EXPECT_EQ(function(t, "Shape"), "") << t;
  EXPECT_EQ(function(t, "Polygon"), "") << t;
  EXPECT_NE(function(t, "Square").find(" = new Square"), std::string::npos)
      << t;
  EXPECT_EQ(count(t, " = new "), 1u) << t;
  EXPECT_NE(function(t, "Shape.__init__"), "") << t;
  EXPECT_NE(function(t, "Polygon.__init__").find("call @Shape.__init__("),
            std::string::npos)
      << t;
  EXPECT_NE(function(t, "Shape.destroy"), "") << t;
  // The impl method calls the pure one through the vtable.
  EXPECT_NE(function(t, "Shape.report").find("vcall %"), std::string::npos)
      << t;
}

// While a class's `__init__` runs the object is one of that class from the
// point every field of the class's body has a value: `vtable.store` follows
// the assignment of the last field on each path (both branches' here, and
// the one after them), or the `__super__(...)` call when no field needs
// assigning (an optional one starts as None).
TEST(Lowering, InitMakesTheObjectOneOfItsClassOnceItsFieldsAreAssigned) {
  auto l = lower(R"(
    class A { a: int; }
    impl A { fn __init__() { self.a = 1; } }
    class B : A { b: int; c: int; }
    impl B {
      fn __init__(f: bool) {
        __super__();
        if f { self.b = 1; self.c = 2; } else { self.c = 3; }
        self.b = 4;
      }
    }
    class C : B { d: C?; }
    fn main() -> int { c = C(True, None); return c.b; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  const std::string &t = l.Text;
  std::string a = function(t, "A.__init__");
  EXPECT_NE(a.find("  field.store %self.2, A.a, 1\n"
                   "  vtable.store %self.2, A\n"),
            std::string::npos)
      << a;
  std::string b = function(t, "B.__init__");
  EXPECT_EQ(count(b, "vtable.store"), 2u) << b;
  EXPECT_NE(b.find("    field.store %self.6, B.c, 2\n"
                   "    vtable.store %self.6, B\n"
                   "  } else {\n"),
            std::string::npos)
      << b;
  EXPECT_NE(b.find("  field.store %self.8, B.b, 4\n"
                   "  vtable.store %self.8, B\n"),
            std::string::npos)
      << b;
  EXPECT_LT(b.find("call @A.__init__("), b.find("vtable.store")) << b;
  std::string c = function(t, "C.__init__");
  EXPECT_EQ(count(c, "vtable.store"), 1u) << c;
  EXPECT_NE(c.find("  call @B.__init__(%self.6, %f.7)\n"
                   "  %self.8 = load %self.0\n"
                   "  vtable.store %self.8, C\n"),
            std::string::npos)
      << c;
  // The constructor installs the class's vtable with `new`, before any
  // `__init__` runs, and stores none itself.
  EXPECT_EQ(count(function(t, "C"), "vtable.store"), 0u) << t;
}

// A subclass that declares an inherited pure virtual method pure again has
// a trap of its own, which names the method by the class that declares it
// pure first, as Sema's "cannot instantiate" error does.
TEST(Lowering, RedeclaredPureVirtualTrapNamesTheDeclaringClass) {
  auto l = lower(R"(
    class Shape { view fn area() -> float; }
    class Mid : Shape { view fn area() -> float; }
    class Square : Mid {
      side: float;
      view fn area() -> float { return self.side * self.side; }
    }
    fn main() -> int { s: Shape = Square(2.0); return int<float>(s.area()); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  const std::string &t = l.Text;
  EXPECT_NE(t.find("area = @Mid.area : (obj) -> f64"), std::string::npos) << t;
  EXPECT_NE(function(t, "Mid.area").find("Paykan_panic_pure_virtual"),
            std::string::npos)
      << t;
  EXPECT_NE(t.find("\"Shape\" len 5"), std::string::npos) << t;
  EXPECT_EQ(t.find("\"Mid\""), std::string::npos) << t;
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

// An `__init__`'s leading assignments that give a field of its class's body
// its first value find it null, in the object `new` zeroed, and release no
// old value (docs/pir.md §7): each of a default `__init__`'s, strong or
// weak.  A field assigned again, a superclass's field (`__super__(...)`
// assigned it), and one whose value is computed once `self` can be given
// away (here to `fill`, which may assign the field) release the old value
// as every other store does.
TEST(Lowering, AnInitsFirstAssignmentOfAFieldReleasesNoOldValue) {
  auto l = lower(R"(
    class Node { name: Str; }
    class Pair { a: Node; b: Node?; weak c: Node?; }
    class Twice { n: Node; }
    impl Twice {
      fn __init__(first: Node, second: Node) {
        self.n = first;
        self.n = second;
      }
    }
    class Late { a: int; o: Node?; }
    fn fill(l: Late) -> Node { l.o = Node("f"); return Node("r"); }
    impl Late { fn __init__() { self.a = 1; self.o = fill(self); } }
    class Base { s: Node; }
    class Kid : Base { t: Node; }
    impl Kid {
      fn __init__(a: Node, b: Node) { __super__(a); self.t = b; self.s = b; }
    }
    fn main() -> int {
      p = Pair(Node("a"), None, None);
      t = Twice(Node("1"), Node("2"));
      l = Late();
      k = Kid(Node("s"), Node("t"));
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  const std::string &t = l.Text;
  const std::string pair = function(t, "Pair.__init__");
  EXPECT_EQ(count(pair, "old.field"), 0u) << pair;
  EXPECT_EQ(count(pair, "weak.release"), 0u) << pair;
  EXPECT_EQ(count(pair, "field.store %"), 3u) << pair;
  EXPECT_EQ(count(function(t, "Node.__init__"), "old.field"), 0u) << t;
  const std::string twice = function(t, "Twice.__init__");
  EXPECT_EQ(count(twice, " = field.load %"), 1u) << twice;
  EXPECT_LT(twice.find("field.store %"), twice.find("old.field")) << twice;
  EXPECT_NE(twice.find("    release %old.field."), std::string::npos) << twice;
  const std::string late = function(t, "Late.__init__");
  EXPECT_EQ(count(late, " = field.load %"), 1u) << late;
  EXPECT_LT(late.find("call @fill("), late.find("old.field")) << late;
  EXPECT_NE(late.find(", Late.o\n"), std::string::npos) << late;
  const std::string kid = function(t, "Kid.__init__");
  EXPECT_EQ(count(kid, " = field.load %"), 1u) << kid;
  EXPECT_NE(kid.find("field.load %self.12, Kid.s\n"), std::string::npos) << kid;
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
  // A float or Str match tests its arms in turn, then switches on the index
  // of the first that held.
  for (const char *chain : {"f", "s"}) {
    std::string fn = function(l.Text, chain);
    EXPECT_NE(fn.find("select %match.eq."), std::string::npos) << fn;
    EXPECT_NE(fn.find("switch %match.arm."), std::string::npos) << fn;
  }
}

// A match on strings or floats used to nest each arm's test in the else of
// the one before, a region an arm, which the passes and backends after the
// lowering recurse into: 3000 arms overflowed them at 2 MB.  Its depth is
// now the same however many arms it has.
TEST(Lowering, AStrOrFloatMatchIsAsDeepForAnyNumberOfArms) {
  auto depth = [](const std::string &text) {
    size_t deepest = 0;
    for (size_t at = 0; at < text.size();) {
      size_t end = text.find('\n', at);
      if (end == std::string::npos)
        end = text.size();
      size_t indent = text.find_first_not_of(' ', at);
      if (indent < end)
        deepest = std::max(deepest, indent - at);
      at = end + 1;
    }
    return deepest;
  };
  auto match = [](const char *type, size_t arms) {
    std::string src = "fn m(x: " + std::string(type) + ") -> int { match x {";
    const bool str = std::string(type) == "Str";
    for (size_t i = 0; i < arms; ++i) {
      std::string n = std::to_string(i);
      src.append(str ? " \"" : " ").append(n).append(str ? "\"" : ".5");
      src.append(" { return ").append(n).append("; }");
    }
    return src + " _ { return -1; } } }\nfn main() -> int { return 0; }\n";
  };
  for (const char *type : {"Str", "float"}) {
    auto two = lower(match(type, 2));
    auto forty = lower(match(type, 40));
    ASSERT_TRUE(two.Ok) << two.Error;
    ASSERT_TRUE(forty.Ok) << forty.Error;
    std::string m = function(forty.Text, "m");
    EXPECT_EQ(depth(m), depth(function(two.Text, "m"))) << m;
    EXPECT_EQ(count(m, "if %match.open."), 39u) << m;
    EXPECT_EQ(count(m, "switch %match.arm."), 1u) << m;
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
  // Each class is tested in turn, A only while B failed, then a switch on
  // the first that held, with the wildcard as its default.
  EXPECT_EQ(count(m, "select %is."), 2u) << m;
  EXPECT_EQ(count(m, "if %match.open."), 1u) << m;
  EXPECT_NE(m.find("switch %match.arm."), std::string::npos) << m;
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
  EXPECT_NE(l.Text.find("= [16, 8, 5, 0, 0]"), std::string::npos) << l.Text;
  EXPECT_NE(l.Text.find("= [2, 1, 5, 0, 0]"), std::string::npos) << l.Text;
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

// A method another module compiles is not read here, and may be
// overridden by a class of any module that imports it: what it does is not
// known, also in the main module of a program, which knows only its own
// classes' overrides.  So `self.origin` is held across `sum()`, though a
// Point reaches no object, and so is `self.mine`, a main class's object,
// across the `sum()` it inherits from the imported class: `move` may have
// replaced either (another module's code may run the program's, as a
// Point's `equals` override would).  A field the program stores only at
// construction keeps its Point whatever `sum()` does, and the main module
// of a program, the only one naming `Fixed`, knows it: `Fixed.origin` is
// read in place, by a stable load.
TEST(Lowering, AReadIsHeldAcrossAMethodAnotherModuleCompiles) {
  auto dir = std::filesystem::temp_directory_path() /
             ("lowering_foreign_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "point.pkn")
      << "class Point { x: int; y: int; "
         "view fn sum() -> int { return self.x + self.y; } }\n";
  const std::string source =
      "import point;\n"
      "class Mine : point::Point { tag: Str; }\n"
      "class Box {\n"
      "  origin: point::Point;\n"
      "  mine: Mine;\n"
      "  view fn total() -> int { return self.origin.sum(); }\n"
      "  view fn own() -> int { return self.mine.sum(); }\n"
      "  fn move(p: point::Point, m: Mine) { self.origin = p; self.mine = m; "
      "}\n"
      "}\n"
      "class Fixed {\n"
      "  origin: point::Point;\n"
      "  view fn total() -> int { return self.origin.sum(); }\n"
      "}\n"
      "fn main() -> int { return 0; }\n";
  for (lowering::World world :
       {lowering::World::Open, lowering::World::Closed}) {
    auto l = lower(source, dir.string(), lowering::Copies::Shared, world);
    ASSERT_TRUE(l.Ok) << l.Error;
    for (const auto &[fn, read] : {std::pair{"Box.total", ", Box.origin"},
                                   std::pair{"Box.own", ", Box.mine"}}) {
      std::string f = function(l.Text, fn);
      EXPECT_TRUE(heldAcross(f, defined(f, read), "vcall ")) << f;
      EXPECT_FALSE(stableLoad(f, read)) << f;
    }
    const bool closed = world == lowering::World::Closed;
    std::string f = function(l.Text, "Fixed.total");
    EXPECT_EQ(heldAcross(f, defined(f, ", Fixed.origin"), "vcall "), !closed)
        << f;
    EXPECT_EQ(stableLoad(f, ", Fixed.origin"), closed) << f;
  }
  std::filesystem::remove_all(dir);
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

// `f32<int>` and `f32<unsigned>` round once, in the runtime, and a constant
// folds to the same f32, as does an integer literal in an `f32` slot
// (new-F3c-2); a narrower source is exact in an f64, so its `itof` and
// `fround32` are one rounding too.
TEST(Lowering, F32OfAWideIntegerIsRoundedOnce) {
  auto l = lower(R"(
    fn a(n: int) -> f32 { return f32(n); }
    fn b(u: unsigned) -> f32 { return f32(u); }
    fn k() -> f32 { return f32(1152921573326323713); }
    fn d() -> f32 { x: f32 = 1152921573326323713; return x; }
    fn e(n: i32) -> f32 { return f32(n); }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string a = function(l.Text, "a");
  EXPECT_NE(a.find("call @$rt.Paykan_int_to_f32(%n."), std::string::npos) << a;
  EXPECT_EQ(a.find("itof"), std::string::npos) << a;
  EXPECT_EQ(a.find("fround32"), std::string::npos) << a;
  std::string b = function(l.Text, "b");
  EXPECT_NE(b.find("call @$rt.Paykan_uint_to_f32(%u."), std::string::npos) << b;
  EXPECT_EQ(b.find("utof"), std::string::npos) << b;
  // 2^60 + 2^37, the f32 nearest 2^60 + 2^36 + 1 (through the f64, 2^60).
  for (const char *fn : {"k", "d"}) {
    std::string f = function(l.Text, fn);
    EXPECT_NE(f.find("1.1529216420458004e+18"), std::string::npos) << f;
    EXPECT_EQ(f.find("call "), std::string::npos) << f;
  }
  std::string e = function(l.Text, "e");
  EXPECT_NE(e.find("itof"), std::string::npos) << e;
  EXPECT_NE(e.find("fround32"), std::string::npos) << e;
  EXPECT_EQ(e.find("call "), std::string::npos) << e;
}

// `char<S>` checks the byte range on the slot and names its source in the
// panic, printed unsigned for an unsigned one; a `u8` is always in range.
TEST(Lowering, CharOfASizedIntegerNamesItsSource) {
  auto l = lower(R"(
    fn a(n: i32) -> char { return char(n); }
    fn b(u: unsigned) -> char { return char(u); }
    fn d(x: u8) -> char { return char(x); }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  EXPECT_NE(l.Text.find("= \"i32\" len 3"), std::string::npos) << l.Text;
  EXPECT_NE(l.Text.find("= \"unsigned\" len 8"), std::string::npos) << l.Text;
  std::string a = function(l.Text, "a");
  EXPECT_NE(a.find("call @$rt.Paykan_panic_to_char(%n."), std::string::npos)
      << a;
  EXPECT_NE(a.find(", 0, @.str"), std::string::npos) << a;
  EXPECT_NE(a.find(", 3)"), std::string::npos) << a;
  std::string b = function(l.Text, "b");
  EXPECT_NE(b.find("call @$rt.Paykan_panic_to_char(%u."), std::string::npos)
      << b;
  EXPECT_NE(b.find(", 1, @.str"), std::string::npos) << b;
  EXPECT_NE(b.find(", 8)"), std::string::npos) << b;
  std::string d = function(l.Text, "d");
  EXPECT_EQ(d.find("call "), std::string::npos) << d;
  EXPECT_NE(d.find("to char"), std::string::npos) << d;
  EXPECT_EQ(count(l.Text, "Paykan_panic_int_to_char"), 0u) << l.Text;
}

// The pairs of the families with a `bool` or a `char` and identity are
// inline, `u.toInt()` is a compare and a `select`, and no conversion
// produces an `ftou` (an unsigned target takes no float).
TEST(Lowering, TheFamiliesPairsAreInline) {
  auto l = lower(R"(
    fn a(k: char) -> u8 { return u8(k); }
    fn b(k: char) -> i8 { return i8(k); }
    fn d(x: bool) -> u16 { return u16(x); }
    fn e(x: u8) -> bool { return bool(x); }
    fn f(n: int) -> int { return int(n); }
    fn g(u: unsigned) -> int? { return u.toInt(); }
    fn h(x: float) -> unsigned { return unsigned(int(x)); }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string a = function(l.Text, "a");
  EXPECT_NE(a.find("to i64"), std::string::npos) << a;
  EXPECT_NE(a.find("mask 8"), std::string::npos) << a;
  std::string b = function(l.Text, "b");
  EXPECT_NE(b.find("to i64"), std::string::npos) << b;
  EXPECT_NE(b.find("wrap 8"), std::string::npos) << b;
  EXPECT_NE(function(l.Text, "d").find("to i64"), std::string::npos) << l.Text;
  EXPECT_NE(function(l.Text, "e").find("cmp ne"), std::string::npos) << l.Text;
  std::string f = function(l.Text, "f");
  EXPECT_EQ(count(f, " = "), 1u) << f; // the load of `n`, returned
  std::string g = function(l.Text, "g");
  EXPECT_NE(g.find("cmp ge %u."), std::string::npos) << g;
  EXPECT_NE(g.find("some %u."), std::string::npos) << g;
  EXPECT_NE(g.find("select"), std::string::npos) << g;
  EXPECT_NE(function(l.Text, "h").find("ftoi"), std::string::npos) << l.Text;
  EXPECT_EQ(count(l.Text, "ftou"), 0u) << l.Text;
  EXPECT_EQ(count(l.Text, "call @$rt."), 1u) << l.Text; // h's panic
}

// `Str(x)` of a value no runtime conversion takes is `x.toString()` (the
// lowering's showCallOf): an object's `toString` slot, a tuple's runtime
// text, an optional's "None" or its value's, held as `h.p.toString()`
// holds `h.p`.  It is the code of the call it stands for, and no `Str<...>`
// callee reaches the PIR.  `Str<Str>(s)` is `s`.
TEST(Lowering, StrOfAValueIsItsToString) {
  const std::string decls = R"(
    class P { x: int; view fn toString() -> Str { return "P"; } }
    class H { p: P; }
    fn main() -> int { return 0; }
  )";
  auto l = lower(decls + R"(
    fn a(p: P) -> Str { return Str(p); }
    fn b(t: (int, Str)) -> Str { return Str(t); }
    fn c(o: int?) -> Str { return Str(o); }
    fn d(h: H) -> Str { return Str(h.p); }
    fn e(s: Str) -> Str { return Str<Str>(s); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  EXPECT_EQ(count(l.Text, "Str<"), 0u) << l.Text;
  std::string a = function(l.Text, "a");
  EXPECT_NE(a.find("= vcall %p.obj."), std::string::npos) << a;
  EXPECT_NE(a.find(" : P [1] ()"), std::string::npos) << a;
  EXPECT_NE(function(l.Text, "b").find("call @$rt.PaykanTuple_toString("),
            std::string::npos)
      << l.Text;
  std::string c = function(l.Text, "c");
  EXPECT_NE(c.find("is_some %o."), std::string::npos) << c;
  EXPECT_NE(c.find("call @$rt.PaykanString_from_int("), std::string::npos) << c;
  std::string d = function(l.Text, "d");
  EXPECT_NE(d.find("retain %p."), std::string::npos) << d;
  EXPECT_NE(d.find(" : P [1] ()"), std::string::npos) << d;
  EXPECT_EQ(count(function(l.Text, "e"), "call "), 0u) << l.Text;
  auto t = lower(decls + R"(
    fn a(p: P) -> Str { return p.toString(); }
    fn b(t: (int, Str)) -> Str { return t.toString(); }
    fn c(o: int?) -> Str { return o.toString(); }
    fn d(h: H) -> Str { return h.p.toString(); }
    fn e(s: Str) -> Str { return s; }
  )");
  ASSERT_TRUE(t.Ok) << t.Error;
  for (const char *fn : {"a", "b", "c", "d"})
    EXPECT_EQ(fullFunction(l.Text, fn), fullFunction(t.Text, fn)) << fn;
}

TEST(Lowering, ParsesCallTheRuntimeAndReturnAnOptionalValue) {
  // The runtime parses into a local; the result is that value as the
  // present optional, or None (#96): no box.
  auto l = lower(R"(
    fn p(s: Str) -> int? { return s.toInt(); }
    fn q(s: Str) -> float? { return s.toFloat(); }
    fn t(s: Str) -> bool? { return s.toBool(); }
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

TEST(Lowering, StrParseMethodsCallTheRuntimeParseOfTheirType) {
  // `s.toInt()` & co. are the same parse: an unsigned width reads no sign
  // (parse_uint) and a narrow one is range-checked; a sized float is the
  // runtime's value of the type, not rounded again; the receiver is
  // borrowed, a temporary one released after the parse.
  auto l = lower(R"(
    fn a(s: Str) -> int? { return s.toInt(); }
    fn b(s: Str) -> i8? { return s.toI8(); }
    fn c(s: Str) -> unsigned? { return s.toUnsigned(); }
    fn d(s: Str) -> u8? { return s.toU8(); }
    fn e(s: Str) -> float? { return s.toFloat(); }
    fn f(s: Str) -> f32? { return s.toF32(); }
    fn g(s: Str) -> f16? { return s.toF16(); }
    fn h(s: Str) -> bool? { return s.toBool(); }
    fn k(s: Str) -> u32? { return s.toU32(); }
    fn t(s: Str) -> u16? { return (s + "0").toU16(); }
    fn main() -> int { return 0; }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  const char *expected[][3] = {
      {"a", "PaykanString_parse_int(", "none opt_i64"},
      {"b", "PaykanString_parse_int(", "none opt_i64"},
      {"c", "PaykanString_parse_uint(", "none opt_i64"},
      {"d", "PaykanString_parse_uint(", "none opt_i64"},
      {"e", "PaykanString_parse_float(", "none opt_f64"},
      {"f", "PaykanString_parse_f32(", "none opt_f64"},
      {"g", "PaykanString_parse_f16(", "none opt_f64"},
      {"h", "PaykanString_parse_bool(", "none opt_bool"},
      {"k", "PaykanString_parse_uint(", "none opt_i64"},
      {"t", "PaykanString_parse_uint(", "none opt_i64"},
  };
  for (const auto &e : expected) {
    std::string fn = function(l.Text, e[0]);
    EXPECT_EQ(count(fn, "call @$rt.PaykanString_parse_"), 1u) << fn;
    EXPECT_NE(fn.find(std::string("call @$rt.") + e[1]), std::string::npos)
        << fn;
    EXPECT_NE(fn.find(" = some "), std::string::npos) << fn;
    EXPECT_NE(fn.find(e[2]), std::string::npos) << fn;
    EXPECT_EQ(fn.find(" = box "), std::string::npos) << fn;
    // No rounding after the parse, nor a range check of a 64-bit one.
    EXPECT_EQ(fn.find("fround32"), std::string::npos) << fn;
    EXPECT_EQ(fn.find("Paykan_round_f16"), std::string::npos) << fn;
  }
  for (const char *narrow : {"b", "d", "k", "t"}) {
    std::string fn = function(l.Text, narrow);
    EXPECT_NE(fn.find("cmp ge"), std::string::npos) << fn;
    EXPECT_NE(fn.find("cmp le"), std::string::npos) << fn;
  }
  for (const char *wide : {"a", "c", "e", "f", "g", "h"})
    EXPECT_EQ(function(l.Text, wide).find("cmp le"), std::string::npos)
        << function(l.Text, wide);
  // The concatenation the receiver is, disposed of after the parse.
  std::string t = function(l.Text, "t");
  const size_t parse = t.find("PaykanString_parse_uint(%concat");
  ASSERT_NE(parse, std::string::npos) << t;
  EXPECT_NE(t.find("call @$rt.Paykan_dispose(%concat", parse),
            std::string::npos)
      << t;
}

// -- Moves at last use (docs/pir.md §12): a copy at a variable's last use is
// -- handed on, the retain and the variable's release gone

namespace {

const char *const kTake = R"(
    class Node { n: int; }
    fn take(n: Node) -> int { return n.n; }
)";

/// @p l through the moves-at-last-use pass and verified after it; what the
/// pass did is in @p stats.
Lowered moved(Lowered l, pir::Statistics &stats) {
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

/// @p source (after kTake) lowered and moved (moved).
Lowered lowerMoved(const std::string &source, pir::Statistics &stats) {
  return moved(lower(kTake + source), stats);
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

TEST(LoweringMoves, AFieldReadInUseAcrossALaterCallKeepsTheCopy) {
  // The name read from h.node is in use while `label` runs, and x may be
  // what keeps that node alive (the pass does not ask what a call does): x
  // is handed to takeName as a copy (an argument's, `retain lend`,
  // docs/pir.md §6).  The read `replace` could free, as it
  // may assign h.node, has a reference of its own (docs/pir.md §7, "Reads a
  // call may replace"), so there x moves, as it does with no such read.
  auto l = lowerMoved(R"(
    class Named { name: Str; }
    class Holder { node: Named; }
    fn takeName(n: Named) -> Str { return n.name; }
    fn label(n: int) -> Str { return "#"; }
    fn replace(h: Holder) -> Str { h.node = Named("other"); return "!"; }
    fn kept(h: Holder) -> Str {
      x = h.node;
      s = takeName(x);
      return h.node.name + label(1) + s;
    }
    fn held(h: Holder) -> Str {
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
  EXPECT_EQ(count(f, "retain lend %x."), 1u) << f;
  for (const char *fn : {"held", "moved"}) {
    f = function(l.Text, fn);
    EXPECT_EQ(count(f, "retain %x."), 0u) << fn << "\n" << f;
    EXPECT_EQ(count(f, "retain lend %x."), 0u) << fn << "\n" << f;
  }
  f = function(l.Text, "held");
  EXPECT_TRUE(heldAcross(f, defined(f, ", Named.name"), "call @replace(")) << f;
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
  // The fields' first values replace nothing (docs/pir.md §7).
  EXPECT_EQ(count(f, "release %"), 0u) << f;
  f = function(l.Text, "Named.give");
  EXPECT_EQ(count(f, "retain lend %n."), 1u) << f;
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
  EXPECT_EQ(count(f, "retain lend %n."), 1u) << f;
  f = function(l.Text, "local");
  EXPECT_EQ(count(f, "retain lend %v."), 1u) << f;
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

// A read rooted in a parameter the function may hand on keeps its reference
// across any call that may let go of one, also one that lets go of no
// reference of the read's class (touchGen replaces a `Gen<int>`, the read
// is a `Str` of a `Lbl`): without it the read would be borrowed across the
// call, and `x` would stay a copy at take(), dying at the end of f instead
// of when take returns (docs/pir.md §7).
TEST(LoweringMoves, AReadRootedInAParameterKeepsTheHandOffBeforeIt) {
  auto l = lowerMoved(R"(
    class Gen<T> { v: T; }
    class Lbl { name: Str; }
    class Pair { lbl: Lbl; g: Gen<int>; }
    fn touchGen(p: Pair) -> Str {
      p.g = Gen<int>(2);
      return "!";
    }
    fn f(p: Pair) -> Str {
      x = Node(1);
      t = take(x);
      return p.lbl.name + touchGen(p) + Str(t);
    }
    fn main() -> int { return f(Pair(Lbl("l"), Gen<int>(1))).len(); }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string f = function(l.Text, "f");
  EXPECT_TRUE(heldAcross(f, defined(f, ", Lbl.name"), "call @touchGen(")) << f;
  EXPECT_FALSE(stableLoad(f, ", Lbl.name")) << f;
  // `x` is handed on: no retain before take().
  const size_t take = f.find("call @take(");
  ASSERT_NE(take, std::string::npos) << f;
  EXPECT_EQ(f.substr(0, take).find("retain "), std::string::npos) << f;
}

// The standard library's containers read their state by a stable load
// across the calls that store their arguments (a stack's push, a queue's
// enqueue, a map's put, a set's add of strings), so the argument is handed
// on to the call, as the hold of the state made it before: the functions
// retain nothing (docs/pir.md §7, §12).
TEST(LoweringMoves, AStableReadOfAContainersStateKeepsTheHandOff) {
  pir::Statistics stats;
  auto l = moved(lower(R"(import ::collections as c;
    fn main() -> int {
      s = c::Stack<Str>();
      s.push("a");
      q = c::Queue<Str>();
      q.enqueue("b");
      m = c::HashMap<Str, int>();
      m.put("c", 1);
      h = c::HashSet<Str>();
      h.add("d");
      return int(u32(s.len() + q.len() + m.len() + h.len()));
    }
  )"),
                 stats);
  ASSERT_TRUE(l.Ok) << l.Error;
  for (const char *fn : {"Stack<Str>.push", "Queue<Str>.enqueue",
                         "\"HashMap<Str, int>.put\"", "HashSet<Str>.add"}) {
    std::string f = function(l.Text, fn);
    ASSERT_FALSE(f.empty()) << fn << "\n" << l.Text;
    EXPECT_TRUE(stableLoad(f, "ptr.load box, %self.addr")) << fn << "\n" << f;
    EXPECT_EQ(count(f, "retain "), 0u) << fn << "\n" << f;
  }
}

// -- Box parameters (docs/pir.md §3) and the copies an argument may be lent
// instead (docs/pir.md §6, `retain lend`)

namespace {

/// The box parameters whose prologue (docs/pir.md §3) the printed function
/// @p name of @p text starts with, in order, and "misplaced" for one after
/// the start of its body.
std::vector<std::string> prologueOf(const std::string &text,
                                    const std::string &name) {
  const std::string full = fullFunction(text, name);
  // The body's first statement that is no `local`: the prologue is before.
  std::istringstream body(function(text, name));
  std::string first;
  for (std::string line; std::getline(body, line);)
    if (line.starts_with("  ") && !line.starts_with("  local ")) {
      first = line;
      break;
    }
  const size_t end =
      first.empty() ? full.size() : full.find("\n" + first + "\n");
  std::vector<std::string> params;
  for (size_t at = full.find(" = owned "); at != std::string::npos;
       at = full.find(" = owned ", at + 1))
    params.push_back(at < end
                         ? full.substr(at + 9, full.find('\n', at) - at - 9)
                         : "misplaced");
  return params;
}

/// How the argument @p arg of the first instruction of @p f containing
/// @p call (up to its argument list) is copied for it: "lend" by a `retain
/// lend` (docs/pir.md §6), "copy" by a plain `retain`, "" when nothing
/// before the call retains it.
std::string argCopy(const std::string &f, const std::string &call,
                    size_t arg = 0) {
  const size_t c = f.find(call);
  if (c == std::string::npos)
    return "no call";
  const size_t open = c + call.size();
  std::string args = f.substr(open, f.find(')', open) - open) + ", ";
  for (size_t i = 0; i < arg && args.find(", ") != std::string::npos; ++i)
    args = args.substr(args.find(", ") + 2);
  const std::string v = args.substr(0, args.find(", "));
  const std::string before = f.substr(0, c);
  if (before.find("retain lend " + v + "\n") != std::string::npos)
    return "lend";
  if (before.find("retain " + v + "\n") != std::string::npos)
    return "copy";
  return "";
}

} // namespace

// Every body the lowering makes of a function that lends (docs/pir.md §3)
// starts with its box parameters' prologue, before it stores them: a
// function, a native function's body, an instance of a generic one, a
// class's method, `impl` method, `__init__` and constructor, a struct's
// method, `__init__` and constructor, a lambda's code and the code of a
// function used as a value.  Each owns its parameters whether the caller
// handed them over or lent them.  A function without a box parameter
// (`fib`, `main`, a destructor) has none; a tagged union's variant is made
// in place, by no function.
TEST(LoweringLend, EveryBodyThatLendsStartsWithItsParametersPrologue) {
  auto l = lower(R"(
    class Node { name: Str; }
    class C {
      n: Node;
      view fn virt(m: Node) -> int { return 1; }
    }
    impl C {
      fn __init__(n: Node) { self.n = n; }
      view fn meth(m: Node) -> int { return 2; }
    }
    struct S { n: Node; }
    impl S {
      fn __init__(n: Node) { self.n = n; }
      view fn meth(m: Node) -> int { return 3; }
    }
    enum E { A(n: Node), B }
    native fn put(fd: int, s: view Str) -> int = "pk_test_put";
    fn f(n: Node) -> int { return 4; }
    fn id<T>(x: T) -> T { return x; }
    fn fib(n: int) -> int { return n; }
    fn main() -> int {
      n = Node("a");
      c = C(n);
      s = S(n);
      e = E::A(n);
      g = fn(m: Node) -> int { return 5; };
      h = f;
      return f(n) + c.virt(n) + c.meth(n) + s.meth(n) + g(n) + h(n) +
             id(n).name.len() + fib(1) + put(1, "p");
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  EXPECT_NE(fullFunction(l.Text, "f")
                .find("  local %n.0: box\n"
                      "  %n.own.2 = owned %n.1\n"
                      "  %n.lent.3 = not %n.own.2\n"
                      "  if %n.lent.3 {\n"
                      "    retain %n.1\n"
                      "  }\n"
                      "  store %n.0, %n.1\n"),
            std::string::npos)
      << l.Text;
  using Names = std::vector<std::string>;
  struct Case {
    const char *Fn;
    Names Params;
  };
  for (const Case &c : {
           Case{"f", {"%n.1"}},
           Case{"put", {"%s.2"}},
           Case{"id<Node>", {"%x.1"}},
           Case{"C.virt", {"%m.2"}},
           Case{"C.meth", {"%m.2"}},
           Case{"C.__init__", {"%n.2"}},
           Case{"C", {"%n.1"}},
           Case{"S.meth", {"%m.2"}},
           Case{"S.__init__", {"%n.2"}},
           Case{"S", {"%n.1"}},
           Case{"Node.__init__", {"%name.2"}},
           Case{"Node", {"%name.1"}},
           Case{"fn.main.0.call", {"%m.2"}},
           Case{"fn.ref.main.f.call", {"%2"}},
           Case{"fib", {}},
           Case{"main", {}},
           Case{"C.destroy", {}},
           Case{"Node.destroy", {}},
       }) {
    ASSERT_FALSE(fullFunction(l.Text, c.Fn).empty()) << c.Fn << "\n" << l.Text;
    EXPECT_EQ(prologueOf(l.Text, c.Fn), c.Params) << c.Fn << "\n"
                                                  << fullFunction(l.Text, c.Fn);
  }
  EXPECT_EQ(l.Text.find("fn @E"), std::string::npos) << l.Text;
}

// The 65th box parameter and those after it have no bit in the ownership
// mask (docs/pir.md §3): they are always handed over, so they have no
// prologue, and the copy of an argument for one is no `retain lend`.
TEST(LoweringLend, BoxParametersPastTheSixtyFourthAreAlwaysHandedOver) {
  std::string params, args;
  for (int i = 0; i < 65; ++i) {
    params += (i ? ", p" : "p") + std::to_string(i) + ": Str";
    args += i ? ", s" : "s";
  }
  auto l = lower("fn wide(" + params + ") -> int { return 0; }\n" +
                 "fn main() -> int { s: Str = \"s\"; return wide(" + args +
                 "); }\n");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::vector<std::string> expected;
  expected.reserve(64);
  for (int i = 0; i < 64; ++i)
    expected.push_back("%p" + std::to_string(i) + "." + std::to_string(i + 1));
  EXPECT_EQ(prologueOf(l.Text, "wide"), expected) << l.Text;
  std::string m = function(l.Text, "main");
  EXPECT_EQ(count(m, "retain lend %s."), 64u) << m;
  EXPECT_EQ(argCopy(m, "call @wide(", 63), "lend") << m;
  EXPECT_EQ(argCopy(m, "call @wide(", 64), "copy") << m;
}

// The copy the lowering makes only to hand a variable over to a lending
// callee is a `retain lend` (docs/pir.md §6): at each call given `a` here,
// the last too, which the moves at last use then make a move, the mark
// going with its retain.  A temporary (`Node("tmp")`) is handed over as it
// is.  `n.name` is a field read `println<Str>` may be lent: it is given
// nothing that reaches an object, so it cannot reach the field.  The copy
// for a store (`h.n = n`) is no argument's.  The lending pass lends the
// copies the moves leave, which main's `a` and show's `n` outlive, and
// clears their marks.
TEST(LoweringLend, ACopyMadeOnlyToHandAnArgumentOverIsMarked) {
  const char *const source = R"(import ::io;
    class Node {
      name: Str;
      next: Node?;
    }
    fn show(n: Node) {
      io::println(n.name);
    }
    fn keep(h: Holder, n: Node) {
      h.n = n;
    }
    fn pass(n: Node) {
      show(n);
    }
    class Holder {
      n: Node?;
    }
    fn main() -> int {
      a = Node("a", None);
      show(a);
      pass(a);
      h = Holder(None);
      keep(h, a);
      show(Node("tmp", None));
      show(a);
      return 0;
    }
  )";
  auto l = lower(source);
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_EQ(count(m, "retain lend %a."), 4u) << m;
  EXPECT_EQ(count(m, "retain lend %h."), 1u) << m;
  EXPECT_EQ(count(m, "retain "), 5u) << m;
  EXPECT_EQ(argCopy(m, "call @show(%call."), "") << m;
  std::string f = function(l.Text, "show");
  EXPECT_EQ(argCopy(f, "call @\"$::io::println<Str>\"("), "lend") << f;
  f = function(l.Text, "pass");
  EXPECT_EQ(argCopy(f, "call @show("), "lend") << f;
  f = function(l.Text, "keep");
  EXPECT_EQ(count(f, "retain lend "), 0u) << f;
  EXPECT_EQ(count(f, "retain %n."), 1u) << f;

  pir::Statistics stats;
  auto mv = moved(lower(source), stats);
  ASSERT_TRUE(mv.Ok) << mv.Error;
  m = function(mv.Text, "main");
  EXPECT_NE(m.find("  retain lend %a.4\n"
                   "  call @show(%a.4)\n"),
            std::string::npos)
      << m;
  EXPECT_NE(m.find("  retain lend %a.5\n"
                   "  call @pass(%a.5)\n"),
            std::string::npos)
      << m;
  EXPECT_NE(m.find("  retain lend %a.8\n"
                   "  call @keep(%h.7, %a.8)\n"),
            std::string::npos)
      << m;
  EXPECT_NE(m.find("  call @show(%call.11)\n"
                   "  %a.12 = load %a.0\n"
                   "  call @show(%a.12)\n"),
            std::string::npos)
      << m;
  pir::PassManager lending(/*verifyEach=*/true);
  lending.add(pir::createLendingPass());
  ASSERT_TRUE(lending.run(mv.Program, stats));
  EXPECT_EQ(stats.get("lending", "marks"), 4u); // main's three, show's
  EXPECT_EQ(stats.get("lending", "arguments lent"), 4u);
  EXPECT_EQ(stats.get("lending", "copies kept for the window"), 0u);
  const std::string text = pir::toString(mv.Program);
  EXPECT_EQ(text.find("retain lend "), std::string::npos) << text;
  m = function(text, "main");
  EXPECT_NE(m.find("  %a.4 = load %a.0\n"
                   "  call @show(lent %a.4)\n"
                   "  %a.5 = load %a.0\n"
                   "  call @pass(lent %a.5)\n"),
            std::string::npos)
      << m;
  EXPECT_NE(m.find("  call @keep(%h.7, lent %a.8)\n"), std::string::npos) << m;
  EXPECT_EQ(count(m, "retain "), 0u) << m;
  EXPECT_NE(function(text, "show").find("(lent %name."), std::string::npos)
      << text;
}

// A box read from storage the function does not own, given to a lending
// callee, is a copy the callee may be lent (`retain lend`) unless a call
// made while it is in use may let go of it (docs/pir.md §7, "Reads a call
// may replace"): the callee, or the call of a later argument, storing into
// a slot of its class or of what holds it, or doing what is not known.  A
// callee given nothing that reaches an object cannot reach the storage at
// all.  A read of a variable the statement gives to an `inout` parameter
// is not lent.  What a class's `toString` and `==` do is known in a closed
// world, where no importer derives a class from Node.
TEST(LoweringLend, AReadIsLentUnlessACallInUseMayLetItGo) {
  const char *const source = R"(import ::io;
    class Node { name: Str; }
    class H { n: Node; s: Str; }
    fn strs(m: Node, h: H) { h.s = "z"; }
    fn nodes(m: Node, h: H) { h.n = Node("z"); }
    fn clear(h: H) -> int { h.n = Node("c"); return 0; }
    fn later(m: Node, k: int) -> int { return k; }
    fn name(s: Str) -> int { return s.len(); }
    fn swap(m: Node, h: inout H) { h = H(Node("w"), "w"); }
    fn same(a: Node, h: H) -> bool { return a == h.n; }
    fn main() -> int {
      h = H(Node("a"), "s");
      strs(h.n, h);
      nodes(h.n, h);
      k = later(h.n, clear(h));
      k = k + name(h.n.name);
      io::println(h.n.name);
      io::println(h.n);
      swap(h.n, h);
      if same(h.n, h) { k = k + 1; }
      return k;
    }
  )";
  for (lowering::World world :
       {lowering::World::Open, lowering::World::Closed}) {
    const bool closed = world == lowering::World::Closed;
    auto l = lower(source, "", lowering::Copies::Shared, world);
    ASSERT_TRUE(l.Ok) << l.Error;
    std::string m = function(l.Text, "main");
    EXPECT_EQ(argCopy(m, "call @strs("), "lend") << m;
    EXPECT_EQ(argCopy(m, "call @nodes("), "copy") << m;
    EXPECT_EQ(argCopy(m, "call @later("), "copy") << m;
    EXPECT_EQ(argCopy(m, "call @name("), "lend") << m;
    EXPECT_EQ(argCopy(m, "call @\"$::io::println<Str>\"("), "lend") << m;
    EXPECT_EQ(argCopy(m, "call @\"$::io::println<Node>\"("),
              closed ? "lend" : "copy")
        << m;
    EXPECT_EQ(argCopy(m, "call @swap("), "copy") << m;
    EXPECT_EQ(argCopy(m, "call @same("), closed ? "lend" : "copy") << m;
    std::string f = function(l.Text, "same");
    EXPECT_EQ(argCopy(f, "[2] ("), closed ? "lend" : "copy") << f;
  }
}

// Each kind of call given the field its callee replaces (plan probe k1):
// a function, a method through the vtable, a constructor, a struct's
// method, a function value, an instance of a generic function and
// `__super__`; none of them is lent the read, while each is the variable
// holding the object.  A field read of a variable the statement gives to an
// `inout` parameter (`f(x.name, x)`), or to a struct's method that may
// change it (`st.set(st.n.name, st.n)`), is not lent either.
TEST(LoweringLend, NoKindOfCallIsLentAReadItMayLetGo) {
  auto l = lower(R"(import ::io;
    class Node { name: Str; }
    class Holder { n: Node; }
    fn direct(m: Node, h: Holder) {
      h.n = Node("z");
      io::println(m.name);
    }
    class Base { view fn take(m: Node, h: Holder) { io::println("base"); } }
    class Derived : Base {
      view fn take(m: Node, h: Holder) { h.n = Node("z"); }
    }
    class Keeper { m: Node; }
    impl Keeper {
      fn __init__(m: Node, h: Holder) { h.n = Node("z"); self.m = m; }
    }
    class Parent { m: Node; }
    impl Parent {
      fn __init__(m: Node, h: Holder) { h.n = Node("z"); self.m = m; }
    }
    class Child : Parent {}
    impl Child { fn __init__(h: Holder) { __super__(h.n, h); } }
    struct S { k: int; }
    impl S { view fn meth(m: Node, h: Holder) { h.n = Node("z"); } }
    fn gen<T>(m: T, h: Holder) -> T {
      h.n = Node("z");
      return m;
    }
    fn f(s: Str, n: inout Node) { n = Node("other"); }
    struct Q { n: Node; }
    impl Q { fn set(s: Str, v: Node) { self.n = Node("z"); } }
    fn main() -> int {
      h = Holder(Node("a"));
      direct(h.n, h);
      b: Base = Derived();
      b.take(h.n, h);
      k = Keeper(h.n, h);
      s = S(1);
      s.meth(h.n, h);
      g = fn(m: Node) -> Str { h.n = Node("z"); return m.name; };
      io::println(g(h.n));
      io::println(gen(h.n, h).name);
      c = Child(h);
      x = Node("x");
      f(x.name, x);
      st = Q(Node("n"));
      st.set(st.n.name, st.n);
      return 0;
    }
  )");
  ASSERT_TRUE(l.Ok) << l.Error;
  std::string m = function(l.Text, "main");
  EXPECT_EQ(count(m, "retain %n."), 6u) << m;
  EXPECT_EQ(count(m, "retain lend %n."), 0u) << m;
  EXPECT_EQ(count(m, "retain lend %h."), 6u) << m;
  std::string c = function(l.Text, "Child.__init__");
  EXPECT_EQ(count(c, "retain %n."), 1u) << c;
  EXPECT_EQ(count(c, "retain lend %n."), 0u) << c;
  EXPECT_EQ(count(c, "retain lend %h."), 1u) << c;
  EXPECT_NE(argCopy(m, "call @f("), "lend") << m;
  EXPECT_NE(argCopy(m, "call @Q.set(", 1), "lend") << m;
  EXPECT_NE(argCopy(m, "call @Q.set(", 2), "lend") << m;
}

// Another module's native function keeps nothing it is lent and changes
// only what its `inout` arguments name, as its interface says (an IFACE
// FUNC record's native flag, docs/design/pkm.md §3.5): a field read given
// to one is lent, where one given to another module's function, which
// may run anything, is copied, and so is one given to a native with an
// `inout` argument.
TEST(LoweringLend, AReadGivenToAnotherModulesNativeIsLent) {
  auto dir = std::filesystem::temp_directory_path() /
             ("lowering_natives_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "lib.pkn")
      << "native fn peek(o: view Obj) -> int = \"pk_test_peek\";\n"
         "native fn poke(o: view Obj, n: inout int) = \"pk_test_poke\";\n"
         "fn look(o: view Obj) -> int { return 0; }\n";
  auto l = lower(R"(import lib;
    class Node { name: Str; }
    class H { n: Node; }
    fn main() -> int {
      h = H(Node("a"));
      k = lib::peek(h.n);
      k = k + lib::look(h.n);
      lib::poke(h.n, k);
      return k;
    }
  )",
                 dir.string());
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(l.Ok) << l.Error;
  const std::string m = function(l.Text, "main");
  EXPECT_EQ(argCopy(m, "call @\"lib::peek\"("), "lend") << m;
  EXPECT_EQ(argCopy(m, "call @\"lib::look\"("), "copy") << m;
  EXPECT_EQ(argCopy(m, "call @\"lib::poke\"("), "copy") << m;
}

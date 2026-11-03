// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: the qualifiers of the concurrency prototype on places (unique,
// `const`, `shared`, `shared const`, `atomic const`, `locked`), the
// conversions between them, and the copy rule of --strict-ownership
// (docs/design/concurrency.md, "Ownership"; samples/concurrency/README.md).

#include "TestUtils.h"
#include <gtest/gtest.h>

#include <string>

using namespace paykan::test;

namespace {

/// Sema over @p source with the copy rule on, the way src/Driver/main.cpp
/// turns it on for --strict-ownership (Sema::setStrictOwnership); otherwise
/// as semaCheck does it.
SemaResult semaCheckStrict(const std::string &source) {
  auto [parseOk, driver] = parse(source);
  if (!parseOk)
    return {false, "parse error", 1};
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  diag.setSourceInfo(driver->getCurrentFile(), &driver->getSourceLines());
  paykan::sema::Sema sema(driver->getASTContext(), diag, "",
                          driver->getFrontendName());
  sema.setStrictOwnership(true);
  auto ctx = sema.run(driver->getRoot());
  return {ctx.Ok, os.str(), ctx.ErrorCount};
}

/// Expect @p r to have failed with each of @p diags (`line:col: error: ...`).
void expectErrors(const SemaResult &r,
                  std::initializer_list<const char *> diags) {
  EXPECT_FALSE(r.Ok);
  for (const char *diag : diags)
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
}

/// The one message of a refused conversion: `cannot store a 'FROM' in
/// WHERE, a 'INTO'` and this reason, whatever the slot.
std::string cannotStore(const std::string &from, const std::string &where,
                        const std::string &into) {
  return "error: cannot store a '" + from + "' in " + where + ", a '" + into +
         "': a reference converts only from unique, or from 'const' into the "
         "other 'const' forms";
}

/// The message of a qualifier written on a borrow: @p what is `parameter
/// 'n'`, `the result of 'h'` or `local 'v'`, @p written the modes and
/// qualifier as written (`view const`), @p mode the mode to keep.
std::string noQualifierOnBorrow(const std::string &what,
                                const std::string &written,
                                const std::string &mode) {
  return "error: " + what + " is '" + written +
         "': a borrow carries no qualifier; a 'view' takes every form of the "
         "object (read-only through it) and an 'inout' a unique or 'shared' "
         "one, with no retain or release; write '" +
         mode + " T'";
}

} // namespace

// The declared type converts, in every slot the same way: a unique value
// (new, or moved) flows into every form, a `const` one into the other
// `const` forms (`shared const`, `atomic const`), and a `shared` reference,
// a `shared const` one and a handle (`atomic const`, `locked`) only into
// their own form, as a copy.  Each of the 36 pairs, in a variable.
TEST(Qualifier, ConversionsBetweenForms) {
  struct From {
    const char *Qual; // "" is unique
    const char *Expr;
  };
  const From froms[] = {{"", "move a"},         {"const", "c"},
                        {"shared", "s"},        {"shared const", "sc"},
                        {"atomic const", "ac"}, {"locked", "l"}};
  const char *const intos[] = {
      "", "const", "shared", "shared const", "atomic const", "locked"};
  auto typeOf = [](const std::string &qual) {
    return qual.empty() ? std::string("Node") : qual + " Node";
  };
  for (const From &from : froms)
    for (const char *into : intos) {
      const std::string fromQ = from.Qual, intoQ = into;
      const bool flows = fromQ.empty() || fromQ == intoQ ||
                         (fromQ == "const" &&
                          (intoQ == "shared const" || intoQ == "atomic const"));
      const std::string decl =
          "let x: " + typeOf(intoQ) + " = " + from.Expr + ";";
      auto r = semaCheck(R"(class Node { id: int; }
fn main() -> int {
  let a = Node(1);
  let c: const Node = Node(2);
  let s: shared Node = Node(3);
  let sc: shared const Node = Node(4);
  let ac: atomic const Node = Node(5);
  let l: locked Node = Node(6);
  )" + decl + R"(
  return 0;
})");
      if (flows) {
        EXPECT_TRUE(r.Ok) << decl << "\n" << r.Diagnostics;
        EXPECT_EQ(r.ErrorCount, 0u) << decl << "\n" << r.Diagnostics;
        continue;
      }
      EXPECT_EQ(r.ErrorCount, 1u) << decl << "\n" << r.Diagnostics;
      expectErrors(r, {(":9:3: " + cannotStore(typeOf(fromQ), "variable 'x'",
                                               typeOf(intoQ)))
                           .c_str()});
    }

  // A declaration writes the qualifier with its type, or alone with the
  // type inferred (`x: shared = e`, `let y: const = e`); a `const` moves
  // into `atomic const` as it does into `shared const`.
  auto ok = semaCheck(R"(class Node { id: int; }
fn main() -> int {
  let a = Node(1);
  let c: const Node = move a;
  let sc: shared const Node = c;
  let ac: atomic const Node = c;
  let c2: const Node = c;
  let s: shared Node = Node(2);
  let s2: shared Node = s;
  let sc2: shared const Node = sc;
  let ac2: atomic const Node = ac;
  let l: locked Node = Node(3);
  let l2: locked Node = l;
  let b = Node(4);
  let bs: shared Node = move b;
  let d = Node(5);
  let da: atomic const Node = move d;
  let e = Node(6);
  let el: locked Node = move e;
  x: shared = Node(7);
  y: const = Node(8);
  let z: shared const = c;
  return c2.id + s2.id + sc2.id + ac2.id + bs.id + da.id + x.id + y.id + z.id;
})");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  EXPECT_EQ(ok.ErrorCount, 0u) << ok.Diagnostics;
}

// The same rule at a parameter, a field and a result, each named by the
// message; a temporary is unique and goes anywhere.
TEST(Qualifier, ConversionsAtParametersFieldsAndResults) {
  auto r = semaCheck(R"(class Node { id: int; }
class Holder { u: Node; c: const Node; s: shared Node; sc: shared const Node; ac: atomic const Node; l: locked Node; }
fn uniq(n: Node) -> int { return n.id; }
fn cst(n: const Node) -> int { return n.id; }
fn shr(n: shared Node) -> int { return n.id; }
fn shc(n: shared const Node) -> int { return n.id; }
fn atc(n: atomic const Node) -> int { return n.id; }
fn lck(n: locked Node) -> int { return 0; }
fn giveU(s: shared Node) -> Node { return s; }
fn giveC(s: shared Node) -> const Node { return s; }
fn giveS(c: const Node) -> shared Node { return c; }
fn giveSC(c: const Node) -> shared const Node { return c; }
fn giveAC(c: const Node) -> atomic const Node { return c; }
fn main() -> int {
  let c: const Node = Node(1);
  let s: shared Node = Node(2);
  let ac: atomic const Node = Node(3);
  let l: locked Node = Node(4);
  let h = Holder(Node(0), Node(0), Node(0), Node(0), Node(0), Node(0));
  n = uniq(Node(0)) + cst(Node(0)) + shr(Node(0)) + shc(Node(0)) + atc(Node(0)) + lck(Node(0));
  n = n + cst(c) + shc(c) + atc(c) + shr(s) + atc(ac) + lck(l);
  n = n + uniq(c) + shr(c) + lck(c);
  n = n + uniq(s) + cst(s) + atc(s);
  n = n + uniq(ac) + shr(ac) + uniq(l);
  h.u = s;
  h.c = s;
  h.s = c;
  h.l = c;
  h.sc = c;
  h.ac = c;
  h.s = s;
  return n;
})");
  EXPECT_EQ(r.ErrorCount, 16u) << r.Diagnostics;
  const std::string u = "Node", c = "const Node", s = "shared Node",
                    ac = "atomic const Node", l = "locked Node";
  expectErrors(
      r, {(":9:36: " + cannotStore(s, "the result", u)).c_str(),
          (":10:42: " + cannotStore(s, "the result", c)).c_str(),
          (":11:42: " + cannotStore(c, "the result", s)).c_str(),
          (":22:16: " + cannotStore(c, "parameter 1 of 'uniq'", u)).c_str(),
          (":22:25: " + cannotStore(c, "parameter 1 of 'shr'", s)).c_str(),
          (":22:34: " + cannotStore(c, "parameter 1 of 'lck'", l)).c_str(),
          (":23:16: " + cannotStore(s, "parameter 1 of 'uniq'", u)).c_str(),
          (":23:25: " + cannotStore(s, "parameter 1 of 'cst'", c)).c_str(),
          (":23:34: " + cannotStore(s, "parameter 1 of 'atc'", ac)).c_str(),
          (":24:16: " + cannotStore(ac, "parameter 1 of 'uniq'", u)).c_str(),
          (":24:26: " + cannotStore(ac, "parameter 1 of 'shr'", s)).c_str(),
          (":24:37: " + cannotStore(l, "parameter 1 of 'uniq'", u)).c_str(),
          (":25:3: " + cannotStore(s, "field 'u'", u)).c_str(),
          (":26:3: " + cannotStore(s, "field 'c'", c)).c_str(),
          (":27:3: " + cannotStore(c, "field 's'", s)).c_str(),
          (":28:3: " + cannotStore(c, "field 'l'", l)).c_str()});
  // A `const` into the other `const` forms, and a `shared` copy, in a
  // result (lines 12, 13) and a field (lines 29 to 31).
  for (const char *line : {":12:", ":13:", ":29:", ":30:", ":31:"})
    EXPECT_EQ(r.Diagnostics.find(std::string(".pkn") + line), std::string::npos)
        << line << "\n"
        << r.Diagnostics;
}

// A `const` place is frozen, deeply: no field assignment (compound or
// nested either), no call of a method that is not a `view fn`, no `inout`
// argument of anything it holds; a `view fn` and a `view` argument are
// fine.  What it holds reads as `const` (a unique field), as `shared const`
// (a `shared` field), and so cannot become unique again.  `shared const`
// and `atomic const` are `const` the same way, the message naming the form.
TEST(Qualifier, ConstPlaces) {
  auto r = semaCheck(R"(class Inner { x: int; }
class Node {
  id: int;
  inner: Inner;
  s: shared Inner;
  fn tick() { self.id += 1; }
  view fn get() -> int { return self.id; }
}
class Limits { lo: int; hi: int; fn widen() { self.hi += 1; } }
class Config { name: Str; limits: Limits; }
fn bump(n: inout int) { n += 1; }
fn grow(i: inout Inner) { }
fn keep(i: Inner) -> int { return i.x; }
fn look(i: view Inner) -> int { return i.x; }
fn main() -> int {
  let c: const Node = Node(1, Inner(2), Inner(3));
  c.id = 5;
  c.id += 1;
  c.inner.x = 1;
  c.tick();
  bump(c.id);
  grow(c.inner);
  let i = c.inner;
  i.x = 2;
  let i2: Inner = c.inner;
  let i3: const Inner = c.inner;
  n = c.get() + keep(c.inner) + look(c.inner) + c.inner.x + i3.x + i.x;
  let sh = c.s;
  sh.x = 9;
  c.s.x = 9;
  let sc: shared const Node = Node(1, Inner(2), Inner(3));
  sc.id = 1;
  sc.tick();
  sc.inner.x = 3;
  let cfg: atomic const Config = Config("demo", Limits(0, 100));
  cfg.limits.hi = 5;
  cfg.limits.widen();
  cfg.name = "x";
  let lim = cfg.limits;
  lim.hi = 1;
  let lc: const Limits = cfg.limits;
  let nm = cfg.name;
  return n + sc.get() + lim.hi + lc.lo + nm.len() + cfg.limits.lo;
})");
  EXPECT_EQ(r.ErrorCount, 18u) << r.Diagnostics;
  expectErrors(
      r,
      {":17:3: error: 'c' is 'const'; cannot assign to its field 'id'",
       ":18:3: error: 'c' is 'const'; cannot assign to its field 'id'",
       ":19:3: error: 'c' is 'const'; cannot assign to its field 'x'",
       ":20:3: error: 'c' is 'const'; 'tick' is not a 'view fn'",
       ":21:8: error: 'c' is 'const'; it cannot be passed to 'inout' "
       "parameter 'n'",
       ":22:8: error: 'c' is 'const'; it cannot be passed to 'inout' "
       "parameter 'i'",
       ":24:3: error: 'i' is 'const'; cannot assign to its field 'x'",
       (":25:3: " + cannotStore("const Inner", "variable 'i2'", "Inner"))
           .c_str(),
       (":27:22: " +
        cannotStore("const Inner", "parameter 1 of 'keep'", "Inner"))
           .c_str(),
       ":29:3: error: 'sh' is 'shared const'; cannot assign to its field 'x'",
       ":30:3: error: 'c' is 'const'; cannot assign to its field 'x'",
       ":32:3: error: 'sc' is 'shared const'; cannot assign to its field 'id'",
       ":33:3: error: 'sc' is 'shared const'; 'tick' is not a 'view fn'",
       ":34:3: error: 'sc' is 'shared const'; cannot assign to its field 'x'",
       ":36:3: error: 'cfg' is 'atomic const'; cannot assign to its field "
       "'hi'",
       ":37:3: error: 'cfg' is 'atomic const'; 'widen' is not a 'view fn'",
       ":38:3: error: 'cfg' is 'atomic const'; cannot assign to its field "
       "'name'",
       ":40:3: error: 'lim' is 'const'; cannot assign to its field 'hi'"});
  // A `const` copy of what it holds, a `view fn`, a `view` argument and a
  // read are fine (lines 26, 41, 42, 43; line 27's one error is `keep`'s,
  // by the count).
  for (const char *line : {":26:", ":41:", ":42:", ":43:"})
    EXPECT_EQ(r.Diagnostics.find(std::string(".pkn") + line), std::string::npos)
        << line << "\n"
        << r.Diagnostics;
}

// A value (`int`, `bool`, a struct, a tuple, a fixed array) takes no
// qualifier, wherever it is declared: a variable, a parameter, a field or
// a result.  `const` on a value is what `let` does; `shared` and `atomic
// const` qualify a reference; the prototype's `locked` holds an object.  A
// value that holds a reference gets the hint to box it.  A `Str`, an
// optional reference and a `mem::Box` are references and take every form.
TEST(Qualifier, ValuesTakeNoQualifier) {
  auto r = semaCheck(R"(class Node { id: int; }
struct P { x: int; }
struct Pair { left: Node; n: int; }
class H { x: const int; t: shared (int, int); }
fn f(p: shared P) -> int { return p.x; }
fn g(n: const int) -> int { return n; }
fn h() -> shared int { return 1; }
fn main() -> int {
  let k: const int = 1;
  let w: shared int = 2;
  let b: shared bool = True;
  let ac: atomic const int = 1;
  let p: atomic const P = P(1);
  let sp: shared P = P(1);
  let cp: const P = P(1);
  let t: shared (int, int) = (1, 2);
  let ta: atomic const (int, int) = (1, 2);
  let arr: const int[2] = [1, 2];
  let aa: atomic const int[2] = [1, 2];
  let l: locked int = 1;
  let lp: locked P = P(1);
  let lt: locked (int, int) = (1, 2);
  let bp: atomic const Pair = Pair(Node(4), 1);
  let na: shared Node[2] = [Node(1), Node(2)];
  let ct: const (int, Node) = (1, Node(2));
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 22u) << r.Diagnostics;
  const std::string kConst = "error: 'const' applies to objects; '";
  const std::string kLet = "' is a value, which 'let' fixes";
  const std::string kShared = "error: 'shared' qualifies a reference; '";
  const std::string kAtomic = "error: 'atomic const' qualifies a reference; '";
  const std::string kLocked =
      "error: 'locked' holds an object in the prototype, not a '";
  expectErrors(
      r, {(":4:11: " + kConst + "int" + kLet).c_str(),
          (":4:25: " + kShared + "(int, int)' is a value").c_str(),
          (":5:6: " + kShared + "P' is a value").c_str(),
          (":6:6: " + kConst + "int" + kLet).c_str(),
          (":7:1: " + kShared + "int' is a value").c_str(),
          (":9:3: " + kConst + "int" + kLet).c_str(),
          (":10:3: " + kShared + "int' is a value").c_str(),
          (":11:3: " + kShared + "bool' is a value").c_str(),
          (":12:3: " + kAtomic + "int' is a value").c_str(),
          (":13:3: " + kAtomic + "P' is a value").c_str(),
          (":14:3: " + kShared + "P' is a value").c_str(),
          (":15:3: " + kConst + "P" + kLet).c_str(),
          (":16:3: " + kShared + "(int, int)' is a value").c_str(),
          (":17:3: " + kAtomic + "(int, int)' is a value").c_str(),
          (":18:3: " + kConst + "int[2]" + kLet).c_str(),
          (":19:3: " + kAtomic + "int[2]' is a value").c_str(),
          (":20:3: " + kLocked + "int' (wrap the value in a class)").c_str(),
          (":21:3: " + kLocked + "P' (wrap the value in a class)").c_str(),
          (":22:3: " + kLocked + "(int, int)' (wrap the value in a class)")
              .c_str(),
          (":23:3: " + kAtomic +
           "Pair' is a value: box it ('mem::Box<Pair>') to share what it "
           "holds")
              .c_str(),
          (":24:3: " + kShared +
           "Node[2]' is a value: box it ('mem::Box<Node[2]>') to share what "
           "it holds")
              .c_str(),
          (":25:3: " + kConst +
           "(int, Node)' is a value: box it ('mem::Box<(int, Node)>') to "
           "share what it holds")
              .c_str()});

  auto ok = semaCheck(R"(import ::memory as mem;
class Node { id: int; }
struct Pair { left: Node; n: int; }
fn main() -> int {
  let st: atomic const Str = "x";
  let ss: shared Str = "y";
  let cs: const Str = "z";
  let ns: shared Node? = None;
  let cn: const Node? = Node(1);
  let an: atomic const Node? = Node(2);
  let bp: atomic const mem::Box<Pair> = mem::Box(Pair(Node(3), 4));
  let sb: shared mem::Box<Pair> = mem::Box(Pair(Node(5), 6));
  return st.len() + ss.len() + cs.len() + bp.value().n + sb.value().n;
})");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  EXPECT_EQ(ok.ErrorCount, 0u) << ok.Diagnostics;
}

// A borrow is written with no qualifier, as a parameter, a result or a
// local: a `view` is read-only already and takes every form of the object,
// an `inout` names a unique or `shared` one, neither counting it.  Every
// qualifier is refused the same way, the message keeping the mode.
TEST(Qualifier, BorrowsCarryNoQualifier) {
  auto r = semaCheck(R"(class Node { id: int; }
fn a(n: view const Node) -> int { return n.id; }
fn b(n: inout shared Node) { }
fn c(n: view shared Node) -> int { return n.id; }
fn d(n: inout const Node) { }
fn e(n: view atomic const Node) -> int { return n.id; }
fn f(n: inout locked Node) { }
fn g(n: view shared const Node) -> int { return n.id; }
fn h(n: inout Node) -> inout shared Node { return n; }
fn i(n: view Node) -> view const Node { return n; }
fn main() -> int {
  s: shared Node = Node(1);
  v: view const Node = s;
  w: inout shared Node = s;
  x: view shared Node = s;
  return v.id + x.id;
})");
  EXPECT_EQ(r.ErrorCount, 12u) << r.Diagnostics;
  const std::string param = "parameter 'n'";
  expectErrors(
      r,
      {(":2:6: " + noQualifierOnBorrow(param, "view const", "view")).c_str(),
       (":3:6: " + noQualifierOnBorrow(param, "inout shared", "inout")).c_str(),
       (":4:6: " + noQualifierOnBorrow(param, "view shared", "view")).c_str(),
       (":5:6: " + noQualifierOnBorrow(param, "inout const", "inout")).c_str(),
       (":6:6: " + noQualifierOnBorrow(param, "view atomic const", "view"))
           .c_str(),
       (":7:6: " + noQualifierOnBorrow(param, "inout locked", "inout")).c_str(),
       (":8:6: " + noQualifierOnBorrow(param, "view shared const", "view"))
           .c_str(),
       (":9:1: " +
        noQualifierOnBorrow("the result of 'h'", "inout shared", "inout"))
           .c_str(),
       (":10:1: " +
        noQualifierOnBorrow("the result of 'i'", "view const", "view"))
           .c_str(),
       (":13:3: " + noQualifierOnBorrow("local 'v'", "view const", "view"))
           .c_str(),
       (":14:3: " + noQualifierOnBorrow("local 'w'", "inout shared", "inout"))
           .c_str(),
       (":15:3: " + noQualifierOnBorrow("local 'x'", "view shared", "view"))
           .c_str()});
}

// A `shared` object passes to `inout T` and to `view T`, and is named by an
// `inout` or `view` local, like a unique one; every `const` form passes to
// `view T` (and is read through a `view` local) but not to `inout T`.  A
// `locked` cell is never borrowed: not by a `view` or `inout` parameter,
// and its contents are reached in a `lock` block only, not by a field read
// or a method call, through the variable or a `view` of it.
TEST(Qualifier, SharedPassesToBorrowsALockedCellDoesNot) {
  auto r = semaCheck(
      R"(class Node { id: int; fn tick() { self.id += 1; } view fn get() -> int { return self.id; } }
fn grow(n: inout Node) { n.id = 9; }
fn show(n: view Node) -> int { return n.id; }
fn main() -> int {
  s: shared Node = Node(1);
  c: const Node = Node(2);
  sc: shared const Node = Node(3);
  ac: atomic const Node = Node(4);
  l: locked Node = Node(5);
  u = Node(6);
  grow(s);
  grow(u);
  n = show(s) + show(c) + show(sc) + show(ac) + show(u) + show(Node(7));
  w: inout = s;
  w.id = 3;
  v: view = s;
  v2: view = c;
  v3: view = ac;
  n = n + v.id + v2.id + v3.id + v.get() + s.get() + c.get() + sc.get() + ac.get();
  s.tick();
  grow(c);
  grow(sc);
  grow(ac);
  show(l);
  grow(l);
  n = n + l.id;
  l.tick();
  v4: view = l;
  n = n + v4.id;
  lock l as held: view { n = n + held.id; }
  return n;
})");
  EXPECT_EQ(r.ErrorCount, 8u) << r.Diagnostics;
  expectErrors(
      r, {":21:8: error: 'c' is 'const'; it cannot be passed to 'inout' "
          "parameter 'n'",
          ":22:8: error: 'sc' is 'shared const'; it cannot be passed to "
          "'inout' parameter 'n'",
          ":23:8: error: 'ac' is 'atomic const'; it cannot be passed to "
          "'inout' parameter 'n'",
          ":24:8: error: 'l' is 'locked': a cell's contents are reached in a "
          "'lock' block, not borrowed by 'view' parameter 'n' of 'show'",
          ":25:8: error: 'l' is 'locked': a cell's contents are reached in a "
          "'lock' block, not borrowed by 'inout' parameter 'n' of 'grow'",
          ":26:11: error: 'l' is 'locked': reach its contents in 'lock l as "
          "c: view { }' (or 'inout')",
          ":27:3: error: 'l' is 'locked': reach its contents in 'lock l as c: "
          "view { }' (or 'inout')",
          ":29:11: error: 'v4' is 'locked': reach its contents in 'lock v4 "
          "as c: view { }' (or 'inout')"});
  // The `shared` object borrowed every way, and the `const` forms viewed
  // (lines 11 to 20), and the `lock` block (line 30).
  for (const char *line : {":11:", ":12:", ":13:", ":14:", ":15:", ":16:",
                           ":17:", ":18:", ":19:", ":20:", ":30:"})
    EXPECT_EQ(r.Diagnostics.find(std::string(".pkn") + line), std::string::npos)
        << line << "\n"
        << r.Diagnostics;
}

// A field's qualifier composes with its receiver's.  Through a unique
// receiver a field reads as declared: a `shared` field is `shared` (it
// cannot become unique), a `const` field is `const` (the message names the
// field).  Through a `const` receiver every field is a `const` form: a
// unique field `const`, a `shared` field `shared const` (not `shared`
// again).  Through a `shared` receiver a `const` field stays `const` and a
// `shared` one `shared`; through a `shared const` receiver a unique field
// is `const` and a `shared` field `shared const`.
TEST(Qualifier, AFieldsQualifierComposesWithTheReceivers) {
  auto r = semaCheck(R"(class Node { id: int; fn tick() { self.id += 1; } }
class Pair { left: Node; right: shared Node; c: const Node; }
fn main() -> int {
  let p = Pair(Node(1), Node(2), Node(3));
  let r = p.right;
  r.id = 1;
  let u: Node = p.right;
  let l = p.left;
  l.id = 2;
  let cl: const Node = p.left;
  let cc = p.c;
  cc.id = 3;
  let cu: Node = p.c;
  p.c.id = 4;
  p.c.tick();
  let pc: const Pair = Pair(Node(1), Node(2), Node(3));
  pc.left.id = 1;
  pc.left.tick();
  let l2 = pc.left;
  l2.id = 1;
  let l3: Node = pc.left;
  let l4: const Node = pc.left;
  let rr = pc.right;
  rr.id = 1;
  pc.right.id = 2;
  let rs: shared Node = pc.right;
  let rsc: shared const Node = pc.right;
  let ps: shared Pair = Pair(Node(1), Node(2), Node(3));
  let sr = ps.right;
  sr.id = 1;
  let sc = ps.c;
  sc.id = 1;
  ps.left.id = 5;
  let psc: shared const Pair = Pair(Node(1), Node(2), Node(3));
  psc.left.id = 1;
  let scl = psc.left;
  scl.id = 1;
  let scr = psc.right;
  scr.id = 1;
  return r.id + l.id + cl.id + l4.id + rsc.id + sr.id + scl.id;
})");
  EXPECT_EQ(r.ErrorCount, 16u) << r.Diagnostics;
  // NOLINTBEGIN(bugprone-suspicious-missing-comma)
  expectErrors(
      r,
      {(":7:3: " + cannotStore("shared Node", "variable 'u'", "Node")).c_str(),
       ":12:3: error: 'cc' is 'const'; cannot assign to its field "
       "'id'",
       (":13:3: " + cannotStore("const Node", "variable 'cu'", "Node")).c_str(),
       ":14:3: error: field 'c' is 'const'; cannot assign to its "
       "field 'id'",
       ":15:3: error: field 'c' is 'const'; 'tick' is not a 'view fn'",
       ":17:3: error: 'pc' is 'const'; cannot assign to its field "
       "'id'",
       ":18:3: error: 'pc' is 'const'; 'tick' is not a 'view fn'",
       ":20:3: error: 'l2' is 'const'; cannot assign to its field "
       "'id'",
       (":21:3: " + cannotStore("const Node", "variable 'l3'", "Node")).c_str(),
       ":24:3: error: 'rr' is 'shared const'; cannot assign to its "
       "field 'id'",
       ":25:3: error: 'pc' is 'const'; cannot assign to its field "
       "'id'",
       (":26:3: " +
        cannotStore("shared const Node", "variable 'rs'", "shared Node"))
           .c_str(),
       ":32:3: error: 'sc' is 'const'; cannot assign to its field "
       "'id'",
       ":35:3: error: 'psc' is 'shared const'; cannot assign to its "
       "field "
       "'id'",
       ":37:3: error: 'scl' is 'const'; cannot assign to its field "
       "'id'",
       ":39:3: error: 'scr' is 'shared const'; cannot assign to its "
       "field "
       "'id'"});
  // NOLINTEND(bugprone-suspicious-missing-comma)
  // The `shared` field written through a unique or `shared` receiver, the
  // unique field written through them, and the `const` copies.
  for (const char *line :
       {":6:", ":9:", ":10:", ":22:", ":27:", ":30:", ":33:"})
    EXPECT_EQ(r.Diagnostics.find(std::string(".pkn") + line), std::string::npos)
        << line << "\n"
        << r.Diagnostics;
}

// `let t = s` (and `t: shared = s`) takes its qualifier from the
// initializer: of a `shared` reference it is `shared`, so it is written
// through and copies on, but never becomes unique or `const`; of a `const`
// one it is `const`; of a handle another handle, used as the first.
TEST(Qualifier, TheInferredQualifierIsTheInitializers) {
  auto r = semaCheck(R"(class Node { id: int; fn tick() { self.id += 1; } }
fn main() -> int {
  let s: shared Node = Node(1);
  let t = s;
  t.id = 22;
  t.tick();
  let u: Node = t;
  let c: const Node = t;
  let t2: shared Node = t;
  t3: shared = s;
  t3.id = 1;
  let cc: const Node = Node(2);
  let d = cc;
  d.id = 3;
  let e: Node = d;
  let sd: shared const Node = d;
  let ac: atomic const Node = Node(3);
  let f = ac;
  f.id = 1;
  let g: Node = f;
  let l: locked Node = Node(4);
  let m = l;
  lock m as mm: inout { mm.id = 5; }
  let mu: Node = m;
  return t2.id + t3.id + sd.id;
})");
  EXPECT_EQ(r.ErrorCount, 7u) << r.Diagnostics;
  // NOLINTBEGIN(bugprone-suspicious-missing-comma)
  expectErrors(
      r,
      {(":7:3: " + cannotStore("shared Node", "variable 'u'", "Node")).c_str(),
       (":8:3: " + cannotStore("shared Node", "variable 'c'", "const Node"))
           .c_str(),
       ":14:3: error: 'd' is 'const'; cannot assign to its field 'id'",
       (":15:3: " + cannotStore("const Node", "variable 'e'", "Node")).c_str(),
       ":19:3: error: 'f' is 'atomic const'; cannot assign to its "
       "field "
       "'id'",
       (":20:3: " + cannotStore("atomic const Node", "variable 'g'", "Node"))
           .c_str(),
       (":24:3: " + cannotStore("locked Node", "variable 'mu'", "Node"))
           .c_str()});
  // NOLINTEND(bugprone-suspicious-missing-comma)
}

// Under --strict-ownership a move-only value read from a place (a unique
// reference, `const` included, or a struct holding one) flows into an
// owning slot (a variable, a parameter, the result) only by `move`, and
// `[v; n]` copies it; a field leaves by `mem::replace` or `mem::take`.  A
// `shared` reference, a handle, a value of Copyable parts, a temporary and
// a `view` argument are fine.  Without the flag the same program is
// accepted: the rule is off by default.
TEST(Qualifier, StrictOwnershipCopyRule) {
  auto ok = semaCheckStrict(R"(class Node { id: int; }
class Owned { left: Node; }
impl Owned { fn __init__(l: Node) { self.left = move l; } }
struct Point { x: int; y: int; }
fn make() -> Node { let n = Node(1); return move n; }
fn take(n: Node) -> int { return n.id; }
fn takeShared(n: shared Node) -> int { return n.id; }
fn takeAtomic(n: atomic const Node) -> int { return n.id; }
fn look(n: view Node) -> int { return n.id; }
fn main() -> int {
  let a = Node(2);
  let s: shared Node = Node(3);
  let t = s;
  let ac: atomic const Node = Node(4);
  let ac2 = ac;
  let l: locked Node = Node(5);
  let l2 = l;
  let p = Point(1, 2);
  let q = p;
  let o = Owned(Node(6));
  n = takeShared(s) + takeAtomic(ac) + look(a) + look(s) + a.id + o.left.id + q.x + t.id + ac2.id;
  n = n + take(move a) + take(Node(7)) + make().id;
  let arr = [Point(1, 2); 3];
  let cc: const Node = Node(8);
  let sc: shared const Node = move cc;
  let c2: const Node = Node(9);
  let ac3: atomic const Node = move c2;
  lock l2 as held: view { n = n + held.id; }
  return n + arr[0].x + sc.id + ac3.id;
})");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  EXPECT_EQ(ok.ErrorCount, 0u) << ok.Diagnostics;

  const std::string copies = R"(class Node { id: int; }
struct Holder { n: Node; k: int; }
impl Holder { fn __init__(n: Node, k: int) { self.n = move n; self.k = k; } }
fn make() -> Node { let n = Node(1); return n; }
fn take(n: Node) -> int { return n.id; }
fn main() -> int {
  let a = Node(2);
  let b = a;
  n = take(a);
  let h = Holder(Node(3), 1);
  let h2 = h;
  let k = h.n;
  let arr = [Node(4); 3];
  let cc: const Node = Node(5);
  let cd = cc;
  let sc: shared const Node = cc;
  return n + b.id + h2.k + k.id + arr[0].id + cd.id + sc.id + make().id;
})";
  auto r = semaCheckStrict(copies);
  EXPECT_EQ(r.ErrorCount, 8u) << r.Diagnostics;
  const char *const kUnique =
      "error: cannot copy 'a' (a unique 'Node'); write 'move a' to transfer "
      "it, or declare it 'shared Node'";
  const char *const kConst =
      "error: cannot copy 'cc' (a unique 'const Node'); write 'move cc' to "
      "transfer it, or declare it 'shared const Node'";
  expectErrors(
      r, {":4:38: error: cannot copy 'n' (a unique 'Node'); write 'move n' to "
          "transfer it, or declare it 'shared Node'",
          (":8:3: " + std::string(kUnique)).c_str(),
          (":9:12: " + std::string(kUnique)).c_str(),
          ":11:3: error: cannot copy 'h': 'Holder' holds unique field 'n' "
          "('Node'); write 'move h' to transfer it",
          ":12:3: error: cannot copy 'h.n' (a unique 'Node'); a field or "
          "element leaves by 'mem::replace' or 'mem::take', or is declared "
          "'shared Node'",
          ":13:13: error: '[v; n]' copies its value: 'Node' is not Copyable",
          (":15:3: " + std::string(kConst)).c_str(),
          (":16:3: " + std::string(kConst)).c_str()});

  auto lax = semaCheck(copies);
  EXPECT_TRUE(lax.Ok) << lax.Diagnostics;
  EXPECT_EQ(lax.ErrorCount, 0u) << lax.Diagnostics;
}

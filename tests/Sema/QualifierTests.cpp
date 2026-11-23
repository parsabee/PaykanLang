// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: the qualifiers of the concurrency prototype on places (the
// bare type, `mutable`, `shared`, `shared mutable`, `atomic shared`, `locked
// mutable`), the writes each one buys, the conversions between them, and the
// copy rule (docs/design/concurrency.md, "Ownership";
// samples/concurrency/README.md).  The bare type is immutable and costs
// nothing; every word added buys one thing: `mutable` the writes through the
// owner, `shared` several owners, `atomic shared` an object frozen for
// every thread, `locked mutable` an object written under a lock.

#include "TestUtils.h"
#include <gtest/gtest.h>

#include <string>

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

/// Expect no diagnostic at any of @p lines (`:N:`): the accepted statements
/// beside the rejected ones.
void expectCleanLines(const SemaResult &r,
                      std::initializer_list<const char *> lines) {
  for (const char *line : lines)
    EXPECT_EQ(r.Diagnostics.find(std::string(".pkn") + line), std::string::npos)
        << line << "\n"
        << r.Diagnostics;
}

/// Whether the written type @p ty (`Node`, `mutable Node`, `shared Node`, ...)
/// is a unique reference: the bare type or `mutable T`.
bool isUniqueForm(const std::string &ty) {
  return ty.rfind("shared ", 0) != 0 && ty.rfind("atomic ", 0) != 0 &&
         ty.rfind("locked ", 0) != 0;
}

/// `a 'NAME'`, or `an 'NAME'` for a name that starts with a vowel, as the
/// diagnostics print it (enough for the names of this file's programs).
std::string article(const std::string &s) {
  const bool vowel =
      !s.empty() && std::string("AEIOaeio").find(s[0]) != std::string::npos;
  return std::string(vowel ? "an '" : "a '") + s + "'";
}

/// The qualifier of the written type @p ty: `shared mutable` of `shared mutable
/// Node`, `` of `Node`.
std::string formOf(const std::string &ty) {
  std::string form;
  size_t at = 0;
  for (;;) {
    const size_t end = ty.find(' ', at);
    const std::string word = ty.substr(at, end - at);
    if (end == std::string::npos || (word != "shared" && word != "mutable" &&
                                     word != "atomic" && word != "locked"))
      return form;
    form += (form.empty() ? "" : " ") + word;
    at = end + 1;
  }
}

/// The one message of a refused conversion: `cannot store a 'FROM' in
/// WHERE`, the slot by its form (`a 'mutable' reference`, `a unique reference`)
/// and the reason, whatever the slot: a unique reference gains `mutable` only
/// by `move`; every other form is stored only as it is.
std::string cannotStore(const std::string &from, const std::string &where,
                        const std::string &into) {
  const std::string src = formOf(from), dst = formOf(into);
  // A `locked` form is a cell's.
  auto noun = [](const std::string &form) {
    return form.rfind("locked", 0) == 0 ? " cell" : " reference";
  };
  return "error: cannot store " + article(from) + " in " + where + ", " +
         (dst.empty() ? "a unique reference" : article(dst) + noun(dst)) +
         ": " +
         (isUniqueForm(from)
              ? "a reference gains 'mutable' only by 'move'"
              : article(src) + noun(src) + " is stored only as '" + src + "'");
}

/// The head of a refused write through @p what (`'x'`, `field 'f'`), an
/// immutable reference of type @p ty (`Node` or `shared Node`): the form
/// with `mutable` is what the message offers.
std::string immutable(const std::string &what, const std::string &ty) {
  const bool shared = ty.rfind("shared ", 0) == 0;
  const std::string bare = shared ? ty.substr(7) : ty;
  return "error: " + what + " is immutable (" + article(ty) + "; write '" +
         (shared ? "shared mutable " : "mutable ") + bare + "' to change it)";
}

/// The head of a refused write through @p what, a frozen object of type
/// @p ty (`atomic shared T`): nothing makes it writable.
std::string frozen(const std::string &what, const std::string &ty) {
  return "error: " + what + " is frozen (an 'atomic shared " + ty + "')";
}

/// The message of a qualifier written on a borrow: @p what is `parameter
/// 'n'`, `the result of 'h'` or `local 'v'`, @p written the modes and
/// qualifier as written (`view mutable`), @p mode the mode to keep.
std::string noQualifierOnBorrow(const std::string &what,
                                const std::string &written,
                                const std::string &mode) {
  return "error: " + what + " is '" + written +
         "': a borrow carries no qualifier; a 'view' takes every form of the "
         "object (read-only through it) and an 'inout' a 'mutable' or 'shared "
         "mutable' "
         "one; write it without '" +
         written.substr(mode.size() + 1) + "'";
}

} // namespace

// The declared type converts, in every slot the same way.  A unique
// reference flows into every form but gains `mutable` only by `move`, and
// read from a place it flows only as `move x` (the copy rule): a `T` read
// from a place is refused by `mutable T`, `shared mutable T` and `locked
// mutable T` as a conversion and by every other form as a copy, a `mutable T`
// read from a place by every form as a copy, while `move x` of either goes
// anywhere, `mutable` into the bare type included.  A `shared` reference
// (`shared T`, `shared mutable T`), a frozen object (`atomic shared T`) and a
// cell (`locked mutable T`) convert only as they are, as a copy.  Each of the
// 48 pairs, in a variable.
TEST(Qualifier, ConversionsBetweenForms) {
  struct From {
    const char *Qual; // "" is unique
    const char *Expr;
  };
  const From froms[] = {{"", "a"},
                        {"", "move a"},
                        {"mutable", "m"},
                        {"mutable", "move m"},
                        {"shared", "s"},
                        {"shared mutable", "sm"},
                        {"atomic shared", "ac"},
                        {"locked mutable", "lm"}};
  const char *const intos[] = {"",
                               "mutable",
                               "shared",
                               "shared mutable",
                               "atomic shared",
                               "locked mutable"};
  auto typeOf = [](const std::string &qual) {
    return qual.empty() ? std::string("Node") : qual + " Node";
  };
  auto hasMutable = [](const std::string &qual) {
    return qual == "mutable" || qual == "shared mutable" ||
           qual == "locked mutable";
  };
  for (const From &from : froms)
    for (const char *into : intos) {
      const std::string fromQ = from.Qual, intoQ = into;
      const bool unique = fromQ.empty() || fromQ == "mutable";
      const bool moved = std::string(from.Expr).rfind("move ", 0) == 0;
      const bool copied =
          unique && !moved && !(fromQ.empty() && hasMutable(intoQ));
      const bool flows = unique ? moved : fromQ == intoQ;
      const std::string decl =
          "let x: " + typeOf(intoQ) + " = " + from.Expr + ";";
      auto r = semaCheck(R"(class Node { id: int; }
fn main() -> int {
  let a = Node(1);
  let m: mutable Node = Node(2);
  let s: shared Node = Node(3);
  let sm: shared mutable Node = Node(4);
  let ac: atomic shared Node = Node(5);
  let lm: locked mutable Node = Node(6);
  )" + decl + R"(
  return 0;
})");
      if (flows) {
        EXPECT_TRUE(r.Ok) << decl << "\n" << r.Diagnostics;
        EXPECT_EQ(r.ErrorCount, 0u) << decl << "\n" << r.Diagnostics;
        continue;
      }
      EXPECT_EQ(r.ErrorCount, 1u) << decl << "\n" << r.Diagnostics;
      if (copied) {
        const std::string name = from.Expr;
        std::string msg = ":9:3: error: cannot copy '";
        msg += name;
        msg += "' (a unique '";
        msg += typeOf(fromQ);
        msg += "'); write 'move ";
        msg += name;
        msg += "' to transfer it, or declare it '";
        msg += typeOf(fromQ.empty() ? "shared" : "shared mutable");
        msg += "'";
        expectErrors(r, {msg.c_str()});
        continue;
      }
      expectErrors(r, {(":9:3: " + cannotStore(typeOf(fromQ), "variable 'x'",
                                               typeOf(intoQ)))
                           .c_str()});
    }

  // A declaration writes the qualifier with its type, or alone with the
  // type inferred (`x: shared mutable = e`, `y: mutable = e`, `let z: shared =
  // e`); a moved `mutable T` flows into every form, a moved `T` into every form
  // too, `mutable T`, `shared mutable T`, `locked mutable T` and `atomic shared
  // T` included, and a temporary into `shared T` or `atomic shared T` as it is.
  auto ok = semaCheck(R"(class Node { id: int; }
fn main() -> int {
  let a = Node(1);
  let m: mutable Node = move a;
  let u: Node = move m;
  let m2: mutable Node = Node(10);
  let s: shared Node = move m2;
  let m3: mutable Node = Node(11);
  let sm: shared mutable Node = move m3;
  let m4: mutable Node = Node(12);
  let ac: atomic shared Node = move m4;
  let m5: mutable Node = Node(13);
  let l: locked mutable Node = move m5;
  let b = Node(2);
  let bm: mutable Node = move b;
  let c = Node(3);
  let cs: shared mutable Node = move c;
  let d = Node(4);
  let da: atomic shared Node = move d;
  let e = Node(5);
  let el: locked mutable Node = move e;
  let f = Node(6);
  let fs: shared Node = move f;
  let f2 = Node(14);
  let fa: atomic shared Node = move f2;
  let fs2: shared Node = Node(15);
  let fa2: atomic shared Node = Node(16);
  let s2: shared Node = s;
  let sm2: shared mutable Node = sm;
  let ac2: atomic shared Node = ac;
  let l2: locked mutable Node = l;
  x: shared mutable = Node(7);
  y: mutable = Node(8);
  let z: shared = move u;
  let m6: mutable Node = Node(17);
  let w: mutable = move m6;
  let u2 = Node(18);
  let v: atomic shared = move u2;
  t: mutable Node = Node(9);
  return s2.id + sm2.id + ac2.id + bm.id + cs.id + da.id + fs.id + fa.id + fs2.id + fa2.id + x.id + y.id + z.id + w.id + v.id + t.id;
})");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  EXPECT_EQ(ok.ErrorCount, 0u) << ok.Diagnostics;
}

// The same rule at a parameter, a field and a result, each named by the
// message; a temporary is unique and goes anywhere, a moved `T` or `mutable T`
// too (a returned parameter is moved), and a `T` read from a place gains
// `mutable` only by `move` (`h.m = move u2`).
TEST(Qualifier, ConversionsAtParametersFieldsAndResults) {
  auto r = semaCheck(R"(class Node { id: int; }
class Holder { u: Node; m: mutable Node; s: shared Node; sm: shared mutable Node; ac: atomic shared Node; l: locked mutable Node; }
fn uniq(n: Node) -> int { return n.id; }
fn mt(n: mutable Node) -> int { return n.id; }
fn shr(n: shared Node) -> int { return n.id; }
fn shm(n: shared mutable Node) -> int { return n.id; }
fn atc(n: atomic shared Node) -> int { return n.id; }
fn lck(n: locked mutable Node) -> int { return 0; }
fn giveU(m: mutable Node) -> Node { return m; }
fn giveM(u: Node) -> mutable Node { return u; }
fn giveMF(h: Holder) -> mutable Node { return h.u; }
fn giveM2(u: Node) -> mutable Node { return move u; }
fn giveT() -> mutable Node { return Node(0); }
fn giveS(s: shared mutable Node) -> shared Node { return s; }
fn giveSM(s: shared Node) -> shared mutable Node { return s; }
fn giveAC(u: Node) -> atomic shared Node { return u; }
fn giveL(s: shared Node) -> locked mutable Node { return s; }
fn main() -> int {
  let u = Node(1);
  let m: mutable Node = Node(2);
  let s: shared Node = Node(3);
  let sm: shared mutable Node = Node(4);
  let ac: atomic shared Node = Node(5);
  let l: locked mutable Node = Node(6);
  let h: mutable Holder = Holder(Node(0), Node(0), Node(0), Node(0), Node(0), Node(0));
  n = uniq(Node(0)) + mt(Node(0)) + shr(Node(0)) + shm(Node(0)) + atc(Node(0)) + lck(Node(0));
  let a1 = Node(1); let a2 = Node(1); let a3 = Node(1);
  let b1: mutable Node = Node(2); let b2: mutable Node = Node(2); let b3: mutable Node = Node(2);
  let b4: mutable Node = Node(2); let b5: mutable Node = Node(2); let b6: mutable Node = Node(2);
  n = n + uniq(move a1) + shr(move a2) + atc(move a3) + uniq(move b1) + mt(move b2) + shr(move b3) + shm(move b4) + atc(move b5) + lck(move b6);
  n = n + shr(s) + shm(sm) + atc(ac) + lck(l);
  n = n + mt(u) + shm(u) + lck(u);
  n = n + uniq(s) + mt(s) + shm(s) + atc(s);
  n = n + uniq(sm) + shr(sm) + atc(sm) + lck(sm);
  n = n + uniq(ac) + shr(ac) + uniq(l) + lck(ac);
  let c1: mutable Node = Node(2); let c2 = Node(1); let c3 = Node(1);
  let c4: mutable Node = Node(2); let c5: mutable Node = Node(2); let c6: mutable Node = Node(2);
  h.u = move c1;
  h.s = move c2;
  h.ac = move c3;
  h.m = move c4;
  h.sm = move c5;
  h.l = move c6;
  h.u = s;
  h.m = u;
  h.sm = u;
  h.l = u;
  h.s = sm;
  h.sm = s;
  h.ac = s;
  h.u = ac;
  h.l = l;
  let u2 = Node(7);
  h.m = move u2;
  return n;
})");
  EXPECT_EQ(r.ErrorCount, 27u) << r.Diagnostics;
  const std::string u = "Node", m = "mutable Node", s = "shared Node",
                    sm = "shared mutable Node", ac = "atomic shared Node",
                    l = "locked mutable Node";
  expectErrors(
      r, {(":11:40: " + cannotStore(u, "the result", m)).c_str(),
          (":14:51: " + cannotStore(sm, "the result", s)).c_str(),
          (":15:52: " + cannotStore(s, "the result", sm)).c_str(),
          (":17:51: " + cannotStore(s, "the result", l)).c_str(),
          (":32:14: " + cannotStore(u, "parameter 1 of 'mt'", m)).c_str(),
          (":32:23: " + cannotStore(u, "parameter 1 of 'shm'", sm)).c_str(),
          (":32:32: " + cannotStore(u, "parameter 1 of 'lck'", l)).c_str(),
          (":33:16: " + cannotStore(s, "parameter 1 of 'uniq'", u)).c_str(),
          (":33:24: " + cannotStore(s, "parameter 1 of 'mt'", m)).c_str(),
          (":33:33: " + cannotStore(s, "parameter 1 of 'shm'", sm)).c_str(),
          (":33:42: " + cannotStore(s, "parameter 1 of 'atc'", ac)).c_str(),
          (":34:16: " + cannotStore(sm, "parameter 1 of 'uniq'", u)).c_str(),
          (":34:26: " + cannotStore(sm, "parameter 1 of 'shr'", s)).c_str(),
          (":34:36: " + cannotStore(sm, "parameter 1 of 'atc'", ac)).c_str(),
          (":34:46: " + cannotStore(sm, "parameter 1 of 'lck'", l)).c_str(),
          (":35:16: " + cannotStore(ac, "parameter 1 of 'uniq'", u)).c_str(),
          (":35:26: " + cannotStore(ac, "parameter 1 of 'shr'", s)).c_str(),
          (":35:37: " + cannotStore(l, "parameter 1 of 'uniq'", u)).c_str(),
          (":35:46: " + cannotStore(ac, "parameter 1 of 'lck'", l)).c_str(),
          (":44:3: " + cannotStore(s, "field 'u'", u)).c_str(),
          (":45:3: " + cannotStore(u, "field 'm'", m)).c_str(),
          (":46:3: " + cannotStore(u, "field 'sm'", sm)).c_str(),
          (":47:3: " + cannotStore(u, "field 'l'", l)).c_str(),
          (":48:3: " + cannotStore(sm, "field 's'", s)).c_str(),
          (":49:3: " + cannotStore(s, "field 'sm'", sm)).c_str(),
          (":50:3: " + cannotStore(s, "field 'ac'", ac)).c_str(),
          (":51:3: " + cannotStore(ac, "field 'u'", u)).c_str()});
  // A `mutable` into the bare type, and a returned (so moved), moved or new `T`
  // into `mutable`, in a result (lines 9, 10, 12, 13); the temporaries, the
  // moved `T` and `mutable T` and the copies of each other form, at a parameter
  // (26 to 31) and a field (36 to 43, 54).
  expectCleanLines(r, {":9:", ":10:", ":12:", ":13:", ":26:", ":27:", ":28:",
                       ":29:", ":30:", ":31:", ":36:", ":37:", ":38:", ":39:",
                       ":40:", ":41:", ":42:", ":43:", ":54:"});
}

// A bare reference is immutable, deeply: no field assignment (compound or
// nested either), no call of a method that is not a `view fn`, no `inout`
// argument of anything it holds; a `view fn` and a `view` argument are
// fine.  A unique field reads as its root does: a bare `T` (a clone of it
// is immutable again, and the field gains `mutable` only by `move`), while a
// `shared mutable` field is the object's own form and keeps its writes.  A
// `shared` reference and a frozen object are immutable the same way, the
// message naming the form.
TEST(Qualifier, ABareReferenceIsImmutable) {
  auto r = semaCheck(R"(class Inner { x: int; }
class Node {
  id: int;
  inner: Inner;
  s: shared mutable Inner;
  fn tick() { self.id += 1; }
  view fn get() -> int { return self.id; }
}
class Limits { lo: int; hi: int; fn widen() { self.hi += 1; } }
class Config { name: Str; limits: Limits; }
fn bump(n: inout int) { n += 1; }
fn grow(i: inout Inner) { }
fn keep(i: Inner) -> int { return i.x; }
fn change(i: mutable Inner) -> int { return i.x; }
fn look(i: view Inner) -> int { return i.x; }
fn main() -> int {
  let c = Node(1, Inner(2), Inner(3));
  c.id = 5;
  c.id += 1;
  c.inner.x = 1;
  c.tick();
  bump(c.id);
  grow(c.inner);
  let i = c.inner.clone();
  i.x = 2;
  let i2: Inner = c.inner.clone();
  let i3: mutable Inner = c.inner;
  n = c.get() + keep(c.inner.clone()) + look(c.inner) + c.inner.x + i2.x + i.x;
  n = n + change(c.inner);
  let sh = c.s;
  sh.x = 9;
  c.s.x = 9;
  let sc: shared Node = Node(1, Inner(2), Inner(3));
  sc.id = 1;
  sc.tick();
  sc.inner.x = 3;
  let cfg: atomic shared Config = Config("demo", Limits(0, 100));
  cfg.limits.hi = 5;
  cfg.limits.widen();
  cfg.name = "x";
  let lim = cfg.limits.clone();
  lim.hi = 1;
  let lc: Limits = cfg.limits.clone();
  let nm = cfg.name.clone();
  return n + sc.get() + lim.hi + lc.lo + nm.len() + cfg.limits.lo;
}
impl Cloneable for Inner {}
impl Cloneable for Limits {})");
  EXPECT_EQ(r.ErrorCount, 16u) << r.Diagnostics;
  const std::string c = immutable("'c'", "Node"),
                    sc = immutable("'sc'", "shared Node"),
                    cfg = frozen("'cfg'", "Config");
  const std::string field = "; cannot assign to its field '";
  expectErrors(
      r, {(":18:3: " + c + field + "id'").c_str(),
          (":19:3: " + c + field + "id'").c_str(),
          (":20:3: " + c + "; cannot assign to 'c.inner.x'").c_str(),
          (":21:3: " + c + "; 'tick' is not a 'view fn'").c_str(),
          (":22:8: " + c + "; it cannot be passed to 'inout' parameter 'n'")
              .c_str(),
          (":23:8: " + c + "; it cannot be passed to 'inout' parameter 'i'")
              .c_str(),
          (":25:3: " + immutable("'i'", "Inner") + field + "x'").c_str(),
          (":27:3: " + cannotStore("Inner", "variable 'i3'", "mutable Inner"))
              .c_str(),
          (":29:18: " +
           cannotStore("Inner", "parameter 1 of 'change'", "mutable Inner"))
              .c_str(),
          (":34:3: " + sc + field + "id'").c_str(),
          (":35:3: " + sc + "; 'tick' is not a 'view fn'").c_str(),
          (":36:3: " + sc + "; cannot assign to 'sc.inner.x'").c_str(),
          (":38:3: " + cfg + "; cannot assign to 'cfg.limits.hi'").c_str(),
          (":39:3: " + cfg + "; 'widen' is not a 'view fn'").c_str(),
          (":40:3: " + cfg + field + "name'").c_str(),
          (":42:3: " + immutable("'lim'", "Limits") + field + "hi'").c_str()});
  // A bare clone of what it holds, a `view fn`, a `view` argument, a `T`
  // argument (a clone) and a read are fine (lines 26, 28, 43, 44, 45; line
  // 29's one error is `change`'s, by the count), and so is the `shared mutable`
  // field, written through the immutable root and through its copy (30-32).
  expectCleanLines(
      r, {":26:", ":28:", ":30:", ":31:", ":32:", ":43:", ":44:", ":45:"});
}

// `mutable` buys the writes: through a `mutable T` variable (`let n: mutable
// Node`, `n: mutable = e`, `n: mutable Node = e`), a `mutable T` parameter and
// a `mutable T` result, every write the bare type refuses is fine, the unique
// fields (through an `inout` local too, and one taken out by `mem::replace`
// into a `mutable T` parameter) and the `shared mutable` fields of the object
// included.
TEST(Qualifier, MutableBuysTheWrites) {
  auto r = semaCheck(R"(import ::memory as mem;
class Inner { x: int; }
class Node {
  id: int;
  inner: Inner;
  s: shared mutable Inner;
  fn tick() { self.id += 1; }
  view fn get() -> int { return self.id; }
}
fn bump(n: inout int) { n += 1; }
fn grow(i: inout Inner) { i.x += 1; }
fn change(i: mutable Inner) -> int { i.x = 7; return i.x; }
fn fresh() -> mutable Node { return Node(9, Inner(9), Inner(9)); }
fn main() -> int {
  let c: mutable Node = Node(1, Inner(2), Inner(3));
  c.id = 5;
  c.id += 1;
  c.inner.x = 1;
  c.tick();
  bump(c.id);
  grow(c.inner);
  i: inout = c.inner;
  i.x = 2;
  c.s.x = 9;
  let sh = c.s;
  sh.x = 10;
  let j: mutable Inner = Inner(4);
  n = c.get() + change(mem::replace(c.inner, Inner(5))) + change(move j);
  d: mutable = Node(2, Inner(2), Inner(3));
  d.tick();
  e: mutable Node = Node(3, Inner(2), Inner(3));
  e.inner.x = 3;
  let f = fresh();
  f.id = 4;
  let g: mutable Node = fresh();
  g.tick();
  return n + d.id + e.id + f.id + g.id;
})");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

// A reference gains `mutable` only by `move`: a `T` read from a place is
// refused by a `mutable T` variable, parameter, field and result (a field: a
// `return` of a parameter moves it), with the one message; `move x` flows,
// a returned parameter flows, a temporary flows, and a `mutable T` moves with
// its `mutable` (`let f = move e`) or without it (`let g: Node = move f`), into
// a `mutable T` parameter and field too.
TEST(Qualifier, MutableIsGainedByMove) {
  auto r = semaCheck(R"(class Node { id: int; }
class Holder { m: mutable Node; }
class Wrap { n: Node; }
fn change(n: mutable Node) { n.id = 1; }
fn give(w: Wrap) -> mutable Node { return w.n; }
fn giveParam(n: Node) -> mutable Node { return n; }
fn giveMoved(n: Node) -> mutable Node { return move n; }
fn giveNew() -> mutable Node { return Node(0); }
fn main() -> int {
  let a = Node(1);
  let m: mutable Node = a;
  change(a);
  let h: mutable Holder = Holder(Node(2));
  h.m = a;
  let b = Node(3);
  let bm: mutable Node = move b;
  let c = Node(4);
  change(move c);
  let d = Node(5);
  h.m = move d;
  let e: mutable Node = Node(6);
  let f = move e;
  f.id = 7;
  let g: Node = move f;
  let e2: mutable Node = Node(7);
  change(move e2);
  let e3: mutable Node = Node(8);
  h.m = move e3;
  let k: mutable Node = Node(8);
  let l: mutable = giveNew();
  return bm.id + g.id + k.id + l.id + giveMoved(Node(9)).id + giveParam(Node(10)).id;
})");
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
  expectErrors(
      r,
      {(":5:36: " + cannotStore("Node", "the result", "mutable Node")).c_str(),
       (":11:3: " + cannotStore("Node", "variable 'm'", "mutable Node"))
           .c_str(),
       (":12:10: " +
        cannotStore("Node", "parameter 1 of 'change'", "mutable Node"))
           .c_str(),
       (":14:3: " + cannotStore("Node", "field 'm'", "mutable Node")).c_str()});
  expectCleanLines(
      r, {":6:", ":7:", ":8:", ":16:", ":18:", ":20:", ":22:", ":23:", ":24:",
          ":25:", ":26:", ":27:", ":28:", ":29:", ":30:", ":31:"});
}

// `shared T` and `shared mutable T` are both several owners on one thread (a
// copy is another owner): a `shared mutable` reference is written through every
// copy, a `shared` one is read only, and neither converts into the other,
// at a variable or a parameter.
TEST(Qualifier, SharedAndSharedMutable) {
  auto r = semaCheck(
      R"(class Node { id: int; fn tick() { self.id += 1; } view fn get() -> int { return self.id; } }
fn read(n: shared Node) -> int { return n.get(); }
fn write(n: shared mutable Node) { n.tick(); }
fn main() -> int {
  let s: shared mutable Node = Node(1);
  let t = s;
  t.id = 2;
  t.tick();
  write(s);
  write(t);
  n = read(s);
  let r: shared Node = Node(3);
  let q = r;
  r.id = 4;
  q.tick();
  write(r);
  n = n + read(r) + read(q) + r.get() + q.get() + s.get();
  let rs: shared Node = s;
  let sr: shared mutable Node = r;
  return n + t.id;
})");
  EXPECT_EQ(r.ErrorCount, 6u) << r.Diagnostics;
  const std::string s = "shared Node", sm = "shared mutable Node";
  expectErrors(
      r,
      {(":11:12: " + cannotStore(sm, "parameter 1 of 'read'", s)).c_str(),
       (":14:3: " + immutable("'r'", s) + "; cannot assign to its field 'id'")
           .c_str(),
       (":15:3: " + immutable("'q'", s) + "; 'tick' is not a 'view fn'")
           .c_str(),
       (":16:9: " + cannotStore(s, "parameter 1 of 'write'", sm)).c_str(),
       (":18:3: " + cannotStore(sm, "variable 'rs'", s)).c_str(),
       (":19:3: " + cannotStore(s, "variable 'sr'", sm)).c_str()});
  expectCleanLines(r, {":6:", ":7:", ":8:", ":9:", ":10:", ":13:", ":17:"});
}

// A value (`int`, `bool`, a struct, a tuple, a fixed array) takes no
// qualifier, wherever it is declared: a variable, a parameter, a field or
// a result.  `mutable` on a value is what `let` already decides (its storage
// is rewritten by assignment); `shared`, `shared mutable` and `atomic shared`
// qualify a reference; the prototype's `locked` holds an object.  A value
// that holds a reference gets the hint to box it.  (`shared Node[2]` is an
// array of two shared references, #368: `shared` binds to the elements;
// `atomic shared` covers the whole type.)  A `Str`, an optional
// reference and a `mem::Box` are references and take every form; a
// `mem::Box` is written through `get()` under `mutable`, read by `value()`.
TEST(Qualifier, ValuesTakeNoQualifier) {
  auto r = semaCheck(R"(class Node { id: int; }
struct P { x: int; }
struct Pair { left: Node; n: int; }
class H { x: mutable int; t: shared (int, int); }
fn f(p: shared P) -> int { return p.x; }
fn g(n: mutable int) -> int { return n; }
fn h() -> shared int { return 1; }
fn main() -> int {
  let k: mutable int = 1;
  let w: shared int = 2;
  let b: shared bool = True;
  let ac: atomic shared int = 1;
  let p: atomic shared P = P(1);
  let sp: shared P = P(1);
  let cp: mutable P = P(1);
  let t: shared (int, int) = (1, 2);
  let ta: atomic shared (int, int) = (1, 2);
  let arr: mutable int[2] = [1, 2];
  let aa: atomic shared int[2] = [1, 2];
  let l: locked int = 1;
  let lp: locked mutable P = P(1);
  let lt: locked (int, int) = (1, 2);
  let bp: atomic shared Pair = Pair(Node(4), 1);
  let ct: mutable (int, Node) = (1, Node(2));
  let sm: shared mutable P = P(1);
  let san: atomic shared Node[2] = [Node(1), Node(2)];
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 23u) << r.Diagnostics;
  const std::string kMutable = "error: 'mutable' applies to objects; '";
  const std::string kLet =
      "' is a value: its storage is rewritten by assignment, which 'let' "
      "fixes";
  const std::string kShared = "error: 'shared' qualifies a reference; '";
  const std::string kSharedMutable =
      "error: 'shared mutable' qualifies a reference; '";
  const std::string kAtomic = "error: 'atomic shared' qualifies a reference; '";
  const std::string kLocked = "error: 'locked' holds an object for now, not '";
  const std::string kLockedMutable =
      "error: 'locked mutable' holds an object for now, not '";
  expectErrors(
      r,
      {(":4:11: " + kMutable + "int" + kLet).c_str(),
       (":4:27: " + kShared + "(int, int)' is a value").c_str(),
       (":5:6: " + kShared + "P' is a value").c_str(),
       (":6:6: " + kMutable + "int" + kLet).c_str(),
       (":7:1: " + kShared + "int' is a value").c_str(),
       (":9:3: " + kMutable + "int" + kLet).c_str(),
       (":10:3: " + kShared + "int' is a value").c_str(),
       (":11:3: " + kShared + "bool' is a value").c_str(),
       (":12:3: " + kAtomic + "int' is a value").c_str(),
       (":13:3: " + kAtomic + "P' is a value").c_str(),
       (":14:3: " + kShared + "P' is a value").c_str(),
       (":15:3: " + kMutable + "P" + kLet).c_str(),
       (":16:3: " + kShared + "(int, int)' is a value").c_str(),
       (":17:3: " + kAtomic + "(int, int)' is a value").c_str(),
       (":18:3: " + kMutable + "int[2]" + kLet).c_str(),
       (":19:3: " + kAtomic + "int[2]' is a value").c_str(),
       (":20:3: " + kLocked + "int': wrap the value in a class").c_str(),
       (":21:3: " + kLockedMutable + "P': wrap the value in a class").c_str(),
       (":22:3: " + kLocked + "(int, int)': wrap the value in a class").c_str(),
       (":23:3: " + kAtomic +
        "Pair' is a value: box it ('mem::Box<Pair>') to share what it "
        "holds")
           .c_str(),
       (":24:3: " + kMutable + "(int, Node)" + kLet).c_str(),
       (":25:3: " + kSharedMutable + "P' is a value").c_str(),
       (":26:3: " + kAtomic +
        "Node[2]' is a value: hold it in a class to share what it holds")
           .c_str()});

  auto ok = semaCheck(R"(import ::memory as mem;
class Node { id: int; }
struct Pair { left: Node; n: int; }
fn main() -> int {
  let st: atomic shared Str = "x";
  let ss: shared Str = "y";
  let cs: Str = "z";
  let ms: mutable Str = "w";
  ms.concat("v");
  let ns: shared Node? = None;
  let cn: Node? = Node(1);
  let mn: mutable Node? = Node(2);
  let an: atomic shared Node? = Node(3);
  let bp: atomic shared mem::Box<Pair> = mem::Box(Pair(Node(3), 4));
  let sb: shared mem::Box<Pair> = mem::Box(Pair(Node(5), 6));
  let mb: mutable mem::Box<Pair> = mem::Box(Pair(Node(7), 8));
  mb.get().n = 9;
  let smb: shared mutable mem::Box<Pair> = mem::Box(Pair(Node(7), 8));
  smb.get().n = 10;
  return st.len() + ss.len() + cs.len() + ms.len() + bp.value().n + sb.value().n + mb.value().n + smb.value().n;
})");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  EXPECT_EQ(ok.ErrorCount, 0u) << ok.Diagnostics;
}

// `atomic T` is the cell of a number, a `bool` or a `char`, wherever it is
// declared: `atomic int` stays the cell.  A class is not a cell's value: an
// object crosses threads frozen (`atomic shared N`) or under a lock
// (`locked mutable N`), and the message names both forms, with the class.
TEST(Qualifier, AtomicHoldsAValueNotAnObject) {
  auto ok = semaCheck(R"(class Box { n: atomic int; done: atomic bool; }
fn f(c: atomic int) -> int { return c; }
fn main() -> int {
  hits: atomic int = 0;
  let b = Box(0, False);
  hits += 1;
  b.n += 2;
  let was = b.done.exchange(True);
  let plain: int = hits;
  return f(b.n) + plain + (if was then 1 else 0);
})");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  EXPECT_EQ(ok.ErrorCount, 0u) << ok.Diagnostics;

  auto r = semaCheck(R"(class Node { id: int; }
class Holder { n: atomic Node; }
fn f(n: atomic Node) -> int { return 1; }
fn g() -> atomic Node { return Node(1); }
fn main() -> int {
  let a: atomic Node = Node(2);
  let s: atomic Str = "s";
  k: atomic int = 3;
  k += 1;
  return k;
})");
  EXPECT_EQ(r.ErrorCount, 5u) << r.Diagnostics;
  const std::string node =
      ": error: 'atomic' holds a number, a 'bool', a 'char' or a fixed array "
      "of "
      "them, not 'Node': an "
      "object crosses threads as 'atomic shared Node' (frozen) or "
      "'locked mutable Node'";
  const std::string str =
      ": error: 'atomic' holds a number, a 'bool', a 'char' or a fixed array "
      "of "
      "them, not 'Str': an "
      "object crosses threads as 'atomic shared Str' (frozen) or "
      "'locked mutable Str'";
  expectErrors(r, {(":2:16" + node).c_str(), (":3:6" + node).c_str(),
                   (":4:1" + node).c_str(), (":6:3" + node).c_str(),
                   (":7:3" + str).c_str()});
}

// A borrow is written with no qualifier, as a parameter, a result or a
// local: a `view` is read-only already and takes every form of the object,
// an `inout` names a `mutable` or `shared mutable` one and writes through it,
// neither counting it.  Every qualifier is refused the same way, `mutable`
// included, the message keeping the mode.
TEST(Qualifier, BorrowsCarryNoQualifier) {
  auto r = semaCheck(R"(class Node { id: int; }
fn a(n: view mutable Node) -> int { return n.id; }
fn b(n: inout shared mutable Node) { }
fn c(n: view shared Node) -> int { return n.id; }
fn d(n: inout mutable Node) { }
fn e(n: view atomic shared Node) -> int { return n.id; }
fn f(n: inout locked mutable Node) { }
fn g(n: view shared mutable Node) -> int { return n.id; }
fn h(n: inout Node) -> inout shared mutable Node { return n; }
fn i(n: view Node) -> view mutable Node { return n; }
fn main() -> int {
  s: shared mutable Node = Node(1);
  v: view mutable Node = s;
  w: inout shared mutable Node = s;
  x: view shared Node = s;
  return v.id + x.id;
})");
  EXPECT_EQ(r.ErrorCount, 12u) << r.Diagnostics;
  const std::string param = "parameter 'n'";
  expectErrors(
      r,
      {(":2:6: " + noQualifierOnBorrow(param, "view mutable", "view")).c_str(),
       (":3:6: " + noQualifierOnBorrow(param, "inout shared mutable", "inout"))
           .c_str(),
       (":4:6: " + noQualifierOnBorrow(param, "view shared", "view")).c_str(),
       (":5:6: " + noQualifierOnBorrow(param, "inout mutable", "inout"))
           .c_str(),
       (":6:6: " + noQualifierOnBorrow(param, "view atomic shared", "view"))
           .c_str(),
       (":7:6: " + noQualifierOnBorrow(param, "inout locked mutable", "inout"))
           .c_str(),
       (":8:6: " + noQualifierOnBorrow(param, "view shared mutable", "view"))
           .c_str(),
       (":9:1: " + noQualifierOnBorrow("the result of 'h'",
                                       "inout shared mutable", "inout"))
           .c_str(),
       (":10:1: " +
        noQualifierOnBorrow("the result of 'i'", "view mutable", "view"))
           .c_str(),
       (":13:3: " + noQualifierOnBorrow("local 'v'", "view mutable", "view"))
           .c_str(),
       (":14:3: " +
        noQualifierOnBorrow("local 'w'", "inout shared mutable", "inout"))
           .c_str(),
       (":15:3: " + noQualifierOnBorrow("local 'x'", "view shared", "view"))
           .c_str()});
}

// A `mutable` or `shared mutable` object passes to `inout T` and to `view T`,
// and is named by an `inout` or `view` local; the bare type, `shared T` and a
// frozen object pass to `view T` (and are read through a `view` local) but
// not to `inout T`, each refused with its own message.  A `locked` cell is
// never borrowed: not by a `view` or `inout` parameter, and its contents
// are reached in a `lock` block only, not by a field read or a method
// call, through the variable or a `view` of it.
TEST(Qualifier, MutablePassesToInoutEveryFormToViewALockedCellToNeither) {
  auto r = semaCheck(
      R"(class Node { id: int; fn tick() { self.id += 1; } view fn get() -> int { return self.id; } }
fn grow(n: inout Node) { n.id = 9; }
fn show(n: view Node) -> int { return n.id; }
fn main() -> int {
  u: mutable = Node(1);
  s: shared mutable Node = Node(2);
  c = Node(3);
  sc: shared Node = Node(4);
  ac: atomic shared Node = Node(5);
  l: locked mutable Node = Node(6);
  grow(u);
  grow(s);
  n = show(u) + show(s) + show(c) + show(sc) + show(ac) + show(Node(7));
  w: inout = s;
  w.id = 3;
  w2: inout = u;
  w2.id = 4;
  v: view = s;
  v2: view = c;
  v3: view = ac;
  n = n + v.id + v2.id + v3.id + v.get() + u.get() + s.get() + c.get() + sc.get() + ac.get();
  u.tick();
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
  EXPECT_EQ(r.ErrorCount, 9u) << r.Diagnostics;
  const std::string inout = "; it cannot be passed to 'inout' parameter 'n'";
  expectErrors(
      r, {(":24:8: " + immutable("'c'", "Node") + inout).c_str(),
          (":25:8: " + immutable("'sc'", "shared Node") + inout).c_str(),
          (":26:8: " + frozen("'ac'", "Node") + inout).c_str(),
          ":27:8: error: 'l' is 'locked': a cell's contents are reached in a "
          "'lock' block, not borrowed by 'view' parameter 'n' of 'show'",
          ":28:8: error: 'l' is 'locked': a cell's contents are reached in a "
          "'lock' block, not borrowed by 'inout' parameter 'n' of 'grow'",
          ":29:11: error: 'l' is 'locked': reach its contents in 'lock l as "
          "c: view { }' (or 'inout')",
          ":30:3: error: 'l' is 'locked': reach its contents in 'lock l as c: "
          "view { }' (or 'inout')",
          ":31:3: error: 'l' is 'locked': reach its contents in 'lock l as "
          "c: view { }' (or 'inout')"});
  // The `mutable` and `shared mutable` objects borrowed every way, the other
  // forms viewed (lines 11 to 23), and the `lock` block (line 33).
  expectCleanLines(r, {":11:", ":12:", ":13:", ":14:", ":15:", ":16:", ":17:",
                       ":18:", ":19:", ":20:", ":21:", ":22:", ":23:", ":33:"});
}

// A field follows its root.  A unique field is never written `mutable`: it is
// written under a `mutable` (or `shared mutable`) root and immutable under an
// immutable one, the message naming the root; it reads with the root's
// `mutable` or none, so the copy rule's refusal of a copy of it names that
// form, and so does the local the copy would initialize.  A `shared mutable`
// field keeps its writes under a `mutable` root and reads as `shared T` under
// an immutable one (a copy of it is `shared T` then, and no `shared mutable
// T`); a `shared` field is immutable under every root, the message naming
// the field.  Under a frozen root every field is frozen, and a unique one
// reads as a bare `T`.
TEST(Qualifier, AFieldFollowsItsRoot) {
  auto r = semaCheck(R"(class Node { id: int; fn tick() { self.id += 1; } }
class Pair { left: Node; right: shared mutable Node; s: shared Node; }
class Deep { left: Node; n: int; }
fn main() -> int {
  let p: mutable Pair = Pair(Node(1), Node(2), Node(3));
  p.left.id = 1;
  p.left.tick();
  p.right.id = 2;
  p.right.tick();
  p.s.id = 3;
  p.s.tick();
  let l = p.left;
  l.id = 4;
  let r = p.right;
  r.id = 5;
  let rs: shared Node = p.right;
  let s = p.s;
  s.id = 6;
  let q = Pair(Node(1), Node(2), Node(3));
  q.left.id = 1;
  q.left.tick();
  q.right.id = 2;
  q.right.tick();
  q.s.id = 3;
  let ql = q.left;
  ql.id = 1;
  let qm: mutable Node = q.left;
  let qu: Node = q.left;
  let qr = q.right;
  qr.id = 2;
  let qrs: shared Node = q.right;
  let qrm: shared mutable Node = q.right;
  let sm: shared mutable Pair = Pair(Node(1), Node(2), Node(3));
  sm.left.id = 1;
  sm.right.id = 2;
  sm.s.id = 3;
  let sh: shared Pair = Pair(Node(1), Node(2), Node(3));
  sh.left.id = 1;
  sh.right.id = 2;
  sh.s.id = 3;
  let ac: atomic shared Deep = Deep(Node(1), 2);
  ac.left.id = 1;
  ac.left.tick();
  let al = ac.left;
  al.id = 1;
  let am: mutable Node = ac.left;
  let au: Node = ac.left;
  return l.id + r.id + rs.id + s.id + ql.id + qu.id + qr.id + qrs.id + al.id + au.id;
})");
  EXPECT_EQ(r.ErrorCount, 22u) << r.Diagnostics;
  const std::string field = "; cannot assign to its field 'id'";
  const std::string tick = "; 'tick' is not a 'view fn'";
  const std::string fieldS = immutable("field 's'", "shared Node");
  // A field of a variable that owns its object leaves by `move`; of a
  // handle, in exchange.
  auto copied = [](const std::string &place, const std::string &ty) {
    const std::string root = place.substr(0, place.find('.'));
    // (Nothing is written through the frozen `ac`.)
    return "error: cannot copy '" + place + "' (a unique '" + ty + "'); " +
           (root == "ac"
                ? std::string()
                : "write 'move " + place + "' to take it (which ends '" + root +
                      "'), or ") +
           "declare the field 'shared " + ty + "'\n";
  };
  expectErrors(
      r,
      {(":10:3: " + fieldS + "; cannot assign to 'p.s.id'").c_str(),
       (":11:3: " + fieldS + tick).c_str(),
       (":12:3: " + copied("p.left", "mutable Node")).c_str(),
       (":25:3: " + copied("q.left", "Node")).c_str(),
       (":28:3: " + copied("q.left", "Node")).c_str(),
       (":44:3: " + copied("ac.left", "Node")).c_str(),
       (":47:3: " + copied("ac.left", "Node")).c_str(),
       (":16:3: " +
        cannotStore("shared mutable Node", "variable 'rs'", "shared Node"))
           .c_str(),
       (":18:3: " + immutable("'s'", "shared Node") + field).c_str(),
       (":20:3: " + immutable("'q'", "Pair") + "; cannot assign to 'q.left.id'")
           .c_str(),
       (":21:3: " + immutable("'q'", "Pair") + tick).c_str(),
       (":24:3: " + fieldS + "; cannot assign to 'q.s.id'").c_str(),
       (":26:3: " + immutable("'ql'", "Node") + field).c_str(),
       (":27:3: " + cannotStore("Node", "variable 'qm'", "mutable Node"))
           .c_str(),
       (":31:3: " +
        cannotStore("shared mutable Node", "variable 'qrs'", "shared Node"))
           .c_str(),
       (":36:3: " + fieldS + "; cannot assign to 'sm.s.id'").c_str(),
       (":38:3: " + immutable("'sh'", "shared Pair") +
        "; cannot assign to 'sh.left.id'")
           .c_str(),
       (":40:3: " + fieldS + "; cannot assign to 'sh.s.id'").c_str(),
       (":42:3: " + frozen("'ac'", "Deep") + "; cannot assign to 'ac.left.id'")
           .c_str(),
       (":43:3: " + frozen("'ac'", "Deep") + tick).c_str(),
       (":45:3: " + immutable("'al'", "Node") + field).c_str(),
       (":46:3: " + cannotStore("Node", "variable 'am'", "mutable Node"))
           .c_str()});
  // The unique and `shared mutable` fields written through a `mutable` or
  // `shared mutable` root, a `shared mutable` field through any root (lines 22,
  // 23, 30, 32, 39), the `mutable` local of a refused copy (line 13) and the
  // copies of a `shared mutable` field (15, 30).
  expectCleanLines(r, {":6:", ":7:", ":8:", ":9:", ":13:", ":15:", ":22:",
                       ":23:", ":30:", ":32:", ":34:", ":35:", ":39:"});
}

// `let t = s` (and `t: shared mutable = s`) takes its qualifier from the
// initializer: of a `shared mutable` reference it is `shared mutable`, so it is
// written through and copies on, but never becomes unique or `shared`; of
// a `shared` one it is `shared`; of a bare reference (moved, as a unique
// one is) a bare one, immutable and gaining `mutable` only by `move`; of a
// `mutable` one a `mutable` one; of a
// frozen object another frozen reference; of a handle another handle, used
// as the first.
TEST(Qualifier, TheInferredQualifierIsTheInitializers) {
  auto r = semaCheck(R"(class Node { id: int; fn tick() { self.id += 1; } }
fn main() -> int {
  let s: shared mutable Node = Node(1);
  let t = s;
  t.id = 22;
  t.tick();
  let u: Node = t;
  let m: mutable Node = t;
  let t2: shared mutable Node = t;
  t3: shared mutable = s;
  t3.id = 1;
  let r: shared Node = Node(2);
  let d = r;
  d.id = 3;
  let e: Node = d;
  let rr: shared Node = d;
  let cc = Node(4);
  let f = move cc;
  f.id = 5;
  let g: mutable Node = f;
  let h: Node = move f;
  let sf: shared Node = move h;
  let mm: mutable Node = Node(6);
  let i = move mm;
  i.id = 7;
  let j: Node = move i;
  let ac: atomic shared Node = Node(8);
  let k = ac;
  k.id = 1;
  let kk: Node = k;
  let l: locked mutable Node = Node(9);
  let n = l;
  lock n as nn: inout { nn.id = 5; }
  let nu: Node = n;
  return t2.id + t3.id + rr.id + sf.id + j.id;
})");
  EXPECT_EQ(r.ErrorCount, 9u) << r.Diagnostics;
  const std::string field = "; cannot assign to its field 'id'";
  expectErrors(
      r,
      {(":7:3: " + cannotStore("shared mutable Node", "variable 'u'", "Node"))
           .c_str(),
       (":8:3: " +
        cannotStore("shared mutable Node", "variable 'm'", "mutable Node"))
           .c_str(),
       (":14:3: " + immutable("'d'", "shared Node") + field).c_str(),
       (":15:3: " + cannotStore("shared Node", "variable 'e'", "Node")).c_str(),
       (":19:3: " + immutable("'f'", "Node") + field).c_str(),
       (":20:3: " + cannotStore("Node", "variable 'g'", "mutable Node"))
           .c_str(),
       (":29:3: " + frozen("'k'", "Node") + field).c_str(),
       (":30:3: " + cannotStore("atomic shared Node", "variable 'kk'", "Node"))
           .c_str(),
       (":34:3: " + cannotStore("locked mutable Node", "variable 'nu'", "Node"))
           .c_str()});
  expectCleanLines(r, {":5:", ":6:", ":9:", ":10:", ":11:", ":16:", ":18:",
                       ":21:", ":22:", ":24:", ":25:", ":26:", ":33:"});
}

// `self` is mutable in every method but a `view fn`: a `fn` assigns its
// fields (nested too), passes them to `inout` and calls any method, and
// `__init__` builds it the same way.  In a `view fn` each of those is
// refused, the message naming the method; a `view fn` reads and calls
// other `view fn`s.
TEST(Qualifier, SelfIsMutableOutsideAViewFn) {
  auto r = semaCheck(R"(class Inner { x: int; }
class Node {
  id: int;
  inner: Inner;
  fn tick() { self.id += 1; self.inner.x = 1; bump(self.id); grow(self.inner); self.tock(); }
  fn tock() { }
  view fn get() -> int { return self.id + self.peek(); }
  view fn peek() -> int { return self.inner.x; }
  view fn bad() -> int { self.id = 1; self.inner.x = 2; self.tick(); bump(self.id); grow(self.inner); return self.get(); }
}
impl Node { fn __init__(id: int) { self.id = id; self.inner = Inner(0); self.inner.x = id; self.tick(); } }
fn bump(n: inout int) { n += 1; }
fn grow(i: inout Inner) { i.x += 1; }
fn main() -> int {
  let n = Node(1);
  let m: mutable Node = Node(2);
  m.tick();
  return n.get() + n.bad() + m.get();
})");
  EXPECT_EQ(r.ErrorCount, 5u) << r.Diagnostics;
  const std::string self = ": error: 'self' is read-only in 'view fn bad'; ";
  expectErrors(
      r, {(":9:26" + self + "cannot assign to its field 'id'").c_str(),
          (":9:39" + self + "cannot assign to 'self.inner.x'").c_str(),
          (":9:57" + self + "'tick' is not a 'view fn'").c_str(),
          (":9:75" + self + "it cannot be passed to 'inout' parameter 'n'")
              .c_str(),
          (":9:90" + self + "it cannot be passed to 'inout' parameter 'i'")
              .c_str()});
  expectCleanLines(r, {":5:", ":7:", ":8:", ":11:", ":17:", ":18:"});
}

// A `match move` binding takes no qualifier: it owns the subject in the
// subject's form, so one of a bare subject reads and calls `view fn`s and
// one of a `mutable` subject writes.  A plain binding of a place is a borrow,
// a `view`: it reads and writes nothing, whatever the subject's form.  An
// `inout` arm borrows a `mutable` place that is not `let`, and a `view` arm
// reads without a copy.
TEST(Qualifier, AMatchBindingFollowsItsSubject) {
  auto r = semaCheck(
      R"(class Node { id: int; fn tick() { self.id += 1; } view fn get() -> int { return self.id; } }
fn main() -> int {
  let n = Node(1);
  k = 0;
  match move n { x: Node { x.id = 1; x.tick(); k = x.get(); } }
  m: mutable Node = Node(2);
  match move m { x: Node { x.id = 2; x.tick(); } }
  m = Node(3);
  match m { x: inout Node { x.id = 3; x.tick(); } }
  match m { x: view Node { k = k + x.get(); } }
  match m { x: Node { x.id = 6; k = k + x.get(); } }
  let o: Node? = Node(3);
  match move o { x: Node { x.id = 4; k = k + x.get(); } None { } }
  let lm: mutable Node = Node(4);
  match lm { x: inout Node { x.id = 5; } }
  return m.id + k;
})");
  EXPECT_EQ(r.ErrorCount, 5u) << r.Diagnostics;
  const std::string x = immutable("'x'", "Node");
  expectErrors(
      r, {(":5:28: " + x + "; cannot assign to its field 'id'").c_str(),
          (":5:38: " + x + "; 'tick' is not a 'view fn'").c_str(),
          ":11:23: error: 'x' is a 'match' binding, a borrow of 'm' ('match "
          "move m' binds what it owns); cannot assign to its field 'id'",
          (":13:28: " + x + "; cannot assign to its field 'id'").c_str(),
          ":15:9: error: 'lm' is declared with 'let' and cannot be bound by "
          "'inout' arm 'x'"});
  expectCleanLines(r, {":7:", ":8:", ":9:", ":10:"});
}

// An object held in a value (a struct field, an array element, a tuple
// element) follows the variable's `let`, not `mutable`, which a value never
// takes: under a `let` variable the value's own storage (its field, its
// element), what `inout` may take of it and the objects it holds are all
// fixed, and without `let` every one of them is written through the value.
TEST(Qualifier, ObjectsHeldInAValueFollowTheVariable) {
  auto r = semaCheck(R"(class Node { id: int; fn tick() { self.id += 1; } }
struct Pair { left: Node; n: int; }
fn grow(n: inout Node) { n.id = 2; }
fn main() -> int {
  let p = Pair(Node(1), 2);
  p.left.id = 2;
  p.left.tick();
  p.n = 3;
  grow(p.left);
  q = Pair(Node(1), 2);
  q.left.id = 3;
  q.n = 3;
  grow(q.left);
  let arr = [Node(1), Node(2)];
  arr[0].id = 4;
  arr[1].tick();
  arr[0] = Node(5);
  let t = (Node(1), 2);
  t.0.id = 5;
  t.0.tick();
  brr = [Node(1), Node(2)];
  brr[0].id = 6;
  brr[1].tick();
  u = (Node(1), 2);
  u.0.id = 7;
  u.0.tick();
  return p.left.id + q.n + arr[0].id + t.0.id + brr[0].id + u.0.id;
})");
  EXPECT_EQ(r.ErrorCount, 9u) << r.Diagnostics;
  const std::string held =
      ", and so are the objects it holds; drop 'let' to change them; ";
  expectErrors(
      r, {(":6:3: error: 'p' is declared with 'let'" + held +
           "cannot assign to 'p.left.id'")
              .c_str(),
          (":7:3: error: 'p' is declared with 'let'" + held +
           "'tick' is not a 'view fn'")
              .c_str(),
          ":8:3: error: 'p' is declared with 'let'; its fields are part of "
          "its value and cannot be assigned",
          ":9:8: error: 'p' is declared with 'let' and cannot be passed to "
          "'inout' parameter 'n'",
          (":15:3: error: 'arr' is declared with 'let'" + held +
           "cannot assign to 'arr[0].id'")
              .c_str(),
          (":16:3: error: 'arr' is declared with 'let'" + held +
           "'tick' is not a 'view fn'")
              .c_str(),
          ":17:3: error: 'arr' is declared with 'let'; its elements are part "
          "of its value and cannot be assigned",
          (":19:3: error: 't' is declared with 'let'" + held +
           "cannot assign to 't.0.id'")
              .c_str(),
          (":20:3: error: 't' is declared with 'let'" + held +
           "'tick' is not a 'view fn'")
              .c_str()});
  expectCleanLines(r, {":11:", ":12:", ":13:", ":21:", ":22:", ":23:", ":24:",
                       ":25:", ":26:"});
}

// The copy rule: a move-only value read from a place (a unique
// reference, `mutable` or not, or a struct holding one) flows into an owning
// slot (a variable, a parameter, the result) only by `move`, and `[v; n]`
// copies it; a field leaves by `mem::replace` (or `mem::take`, where it
// is optional).  A `shared`
// reference, a handle, a value of Copyable parts, a temporary, a `view`
// argument and a returned local (a `return` moves it) are fine.
TEST(Qualifier, CopyRule) {
  auto ok = semaCheck(R"(class Node { id: int; }
class Owned { left: Node; }
impl Owned { fn __init__(l: Node) { self.left = move l; } }
struct Point { x: int; y: int; }
fn make() -> Node { let n = Node(1); return move n; }
fn make2() -> mutable Node { let n = Node(1); return n; }
fn byValue(n: Node) -> Node { return n; }
fn take(n: Node) -> int { return n.id; }
fn takeMutable(n: mutable Node) -> int { return n.id; }
fn takeShared(n: shared Node) -> int { return n.id; }
fn takeAtomic(n: atomic shared Node) -> int { return n.id; }
fn look(n: view Node) -> int { return n.id; }
fn main() -> int {
  let a = Node(2);
  let m: mutable Node = Node(3);
  let s: shared Node = Node(4);
  let t = s;
  let sm: shared mutable Node = Node(5);
  let sm2 = sm;
  let ac: atomic shared Node = Node(6);
  let ac2 = ac;
  let l: locked mutable Node = Node(7);
  let l2 = l;
  let p = Point(1, 2);
  let q = p;
  let o = Owned(Node(8));
  n = takeShared(s) + takeAtomic(ac) + look(a) + look(m) + look(s) + a.id + m.id + o.left.id + q.x + t.id + sm2.id + ac2.id;
  n = n + take(move a) + takeMutable(move m) + take(Node(9)) + takeMutable(Node(10)) + make().id + make2().id + byValue(Node(14)).id;
  let arr = [Point(1, 2); 3];
  let b = Node(11);
  let bs: shared Node = move b;
  let c = Node(12);
  let ca: atomic shared Node = move c;
  let d: mutable Node = Node(13);
  let dm: shared mutable Node = move d;
  lock l2 as held: view { n = n + held.id; }
  return n + arr[0].x + bs.id + ca.id + dm.id;
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
  let m: mutable Node = Node(5);
  let md = m;
  let sm: shared mutable Node = m;
  let ac: atomic shared Node = a;
  return n + b.id + h2.k + k.id + arr[0].id + md.id + sm.id + ac.id + make().id;
})";
  auto r = semaCheck(copies);
  // `[Node(4); 3]` makes a node for each element, no copy.
  EXPECT_EQ(r.ErrorCount, 7u) << r.Diagnostics;
  const char *const kUnique =
      "error: cannot copy 'a' (a unique 'Node'); write 'move a' to transfer "
      "it, or declare it 'shared Node'";
  const char *const kMutable =
      "error: cannot copy 'm' (a unique 'mutable Node'); write 'move m' to "
      "transfer it, or declare it 'shared mutable Node'";
  expectErrors(
      r, {(":8:3: " + std::string(kUnique)).c_str(),
          (":9:12: " + std::string(kUnique)).c_str(),
          ":11:3: error: cannot copy 'h': 'Holder' holds unique field 'n' "
          "('Node'); write 'move h' to transfer it",
          ":12:3: error: cannot copy 'h.n' (a unique 'Node'); write 'move "
          "h.n' to take it (which ends 'h'), or declare the field 'shared "
          "Node'",
          (":15:3: " + std::string(kMutable)).c_str(),
          (":16:3: " + std::string(kMutable)).c_str(),
          (":17:3: " + std::string(kUnique)).c_str()});
}

// `mut` is no word of the language: a field, a parameter and a local take
// the name, and where a type is written it is a name like any other.
TEST(Qualifier, MutIsAnOrdinaryName) {
  auto ok = semaCheck(R"(class Node { mut: int; }
fn twice(mut: int) -> int { return mut * 2; }
fn main() -> int {
  let mut = 2;
  let n = Node(mut);
  return twice(n.mut);
}
)");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;

  auto type = semaCheck(R"(class Node { n: int; }
fn main() -> int {
  x: mut = Node(1);
  return 0;
}
)");
  EXPECT_EQ(type.ErrorCount, 1u) << type.Diagnostics;
  expectErrors(type,
               {":3:3: error: variable 'x' has unknown class type 'mut'\n"});
}

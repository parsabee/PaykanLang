// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Codegen tests: a string literal is one static, immortal `Str` object for
// all its evaluations (#363).  Nothing may tell it from a string of its
// own: the object is only ever in a place no string is changed through (a
// bare `Str` variable or parameter, the middle of an expression), and a
// string stored anywhere else is made a string of its own there.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// The object itself, in PIR: its methods and the runtime's string functions
// take it as any `Str`, and retains and releases leave it alone.
TEST(StringLiterals, AStaticObjectIsAStrThatCountsNothing) {
  LeakGuard g;
  auto r = compileAndRunPIR(R"(module "t"
cstr @.c0 = "hello" len 5
strobj @.s0 = "hello" len 5
extern fn @$rt.PaykanString_new(ptr, i64) -> obj
extern fn @$rt.PaykanString_owned(box) -> box
extern fn @$rt.PaykanString_length(obj) -> i64
extern fn @$rt.PaykanString_equals(obj, box, i64) -> i64
extern fn @$rt.PaykanString_concat(obj, obj) -> obj
fn @main() -> i64 {
  %n = call @$rt.PaykanString_length(@.s0)
  %b = box @.s0
  release %b
  release %b
  release %b
  %fresh = call @$rt.PaykanString_new(@.c0, 5)
  %fb = box %fresh
  %same = call @$rt.PaykanString_equals(@.s0, %fb, 0)
  release %fb
  %both = call @$rt.PaykanString_concat(@.s0, @.s0)
  %bb = box %both
  %m = call @$rt.PaykanString_length(%both)
  release %bb
  %own = call @$rt.PaykanString_owned(%b)
  %o = unbox %own
  %k = call @$rt.PaykanString_length(%o)
  release %own
  %again = call @$rt.PaykanString_length(@.s0)
  %x = mul %same, 100
  %y = add %x, %n
  %z = add %y, %m
  %w = add %z, %k
  %v = add %w, %again
  ret %v
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 100 + 5 + 10 + 5 + 5);
  g.expectNoLeaks("AStaticObjectIsAStrThatCountsNothing");
}

// A change in place is its owner's alone, through every kind of place a
// `Str` is held in: the literal reads the same at its next evaluation (each
// is evaluated twice here), because a string that gains `mutable` by `move`, or
// is stored in a field or an element, is one of its own from there on.
TEST(StringLiterals, AChangeInPlaceIsItsOwnersAlone) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
struct P { name: Str; n: int; }
class C { name: Str; }
class D { p: P; t: (Str, int); }
fn lit() -> Str { return "lit"; }
fn litMutable() -> mutable Str { let a = "tmp"; return move a; }
fn grow(s: inout Str) { s.concat("+"); }
fn own(s: mutable Str) -> Str { s.concat("~"); return move s; }
fn main() -> int {
  for round in 0..2 {
    let a = "abc";
    b: mutable = move a;              // gains mutable by move
    b.concat("!");
    u = ("tup", 1);               // an element is written as its variable is
    u.0.concat("?");
    p = P("pp", 1);
    p.name.concat("#");
    c: mutable = C(lit());            // a unique field, as its object is
    c.name.concat("$");
    d: mutable = D(P("dp", 2), ("dt", 3));
    d.p.name.concat("%");
    d.t.0.concat("^");
    x: mutable Str = lit();
    x.concat("&");
    grow(x);
    arr = ["e1", "e2"];
    arr[round].concat("*");
    let y = own("own");
    z: mutable = litMutable();
    z.concat("@");
    fresh: mutable Str = "new";       // a literal in a `mutable` slot is a new string
    fresh.concat(".");
    io::println(b + " " + u.0 + " " + p.name + " " + c.name + " " + d.p.name + " " + d.t.0 + " " + x + " " + arr[0] + arr[1] + " " + y + " " + z + " " + fresh);
  }
  io::println(lit() + " " + litMutable() + " " + Str(litMutable().len()));
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "abc! tup? pp# lit$ dp% dt^ lit&+ e1*e2 own~ tmp@ new.\n"
                      "abc! tup? pp# lit$ dp% dt^ lit&+ e1e2* own~ tmp@ new.\n"
                      "lit tmp 3\n");
  g.expectNoLeaks("AChangeInPlaceIsItsOwnersAlone");
}

// A string several names hold is theirs, not the literal's object: a change
// through one of them shows through the others, in a tuple, an array, a
// field, an optional, a generic class's field, a result and a parameter,
// and never in the literal.
TEST(StringLiterals, ASharedStringIsItsHoldersOwn) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
class Box<T> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = move v; } }
class K { name: shared mutable Str; }
struct S { name: shared mutable Str; n: int; }
fn lit() -> Str { return "lit"; }
fn made() -> shared mutable Str { return "ret"; }
fn madeVia() -> shared mutable Str { let a = "via"; return move a; }
fn take(s: shared mutable Str) -> shared mutable Str { s.concat("+"); return s; }
fn pick(c: bool) -> Str { return if c then "yes" else "no"; }
fn main() -> int {
  for round in 0..2 {
    let t1: (shared mutable Str, int) = ("tup", 1);
    let u1 = t1; t1.0.concat("!");
    let a1: shared mutable Str[2] = ["e0", "e1"];
    let b1 = a1; a1[0].concat("!");
    let k = K("field"); let kn = k.name; k.name.concat("!");
    let s = S("sfield", 1); let s2 = s; s.name.concat("!");
    let o: shared mutable Str? = "opt";
    let o2 = o;
    match o { x: Str { x.concat("!"); } _ {} }
    let g = Box<shared mutable Str>("gen"); let gv = g.v; g.v.concat("!");
    let r = made(); let r2 = r; r.concat("!");
    let v = madeVia(); let v2 = v; v.concat("!");
    let p = take("arg");
    let q: shared mutable Str = pick(round == 0); let q2 = q; q.concat("!");
    w: shared mutable Str = "first"; let w2 = w; w = "second"; let w3 = w; w.concat("!");
    io::println(u1.0 + " " + b1[0] + " " + kn + " " + s2.name + " " + o2.toString() + " " + gv + " " + r2 + " " + v2 + " " + p + " " + q2 + " " + w2 + " " + w3);
  }
  io::println(lit() + " " + pick(True) + " " + pick(False));
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "tup! e0! field! sfield! opt! gen! ret! via! arg+ yes! first "
            "second!\n"
            "tup! e0! field! sfield! opt! gen! ret! via! arg+ no! first "
            "second!\n"
            "lit yes no\n");
  g.expectNoLeaks("ASharedStringIsItsHoldersOwn");
}

// A `shared` string that came from a literal is an object with a life of
// its own: a `weak` reference to it reads None once its holders are gone,
// as it did when every literal made a string.
TEST(StringLiterals, AWeakReferenceSeesASharedStringGo) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
class Holder { s: shared Str?; }
fn name() -> Str { return "named"; }
fn main() -> int {
  h: mutable = Holder("held");
  w: weak = h.s;
  let before = if w == None then "gone" else "there";
  h.s = None;
  let after = if w == None then "gone" else "there";
  let moved: shared Str = name();
  v: weak = moved;
  io::println(before + " " + after + " " + (if v == None then "gone" else "there"));
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "there gone there\n");
  g.expectNoLeaks("AWeakReferenceSeesASharedStringGo");
}

// What a program prints does not depend on whether a string came from a
// literal: this program's output is the one it had when every literal made
// a string.  An earlier read of a place names the string a later change is
// made to; an argument may replace the receiver's place; a receiver's chain
// is evaluated once and leaks nothing; a temporary receiver, and a literal
// in a conditional one, is changed as a string of its own; and a string
// that a native function, an `inout` binding or a payload stores is one.
TEST(StringLiterals, NothingTellsALiteralsStringFromAnother) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
import ::memory as mem;
enum Maybe { Just(text: Str), Nothing }
class C { name: Str; }
impl C {
  fn bang() -> Str { self.name.concat("!"); return "|"; }
  fn reset() -> Str { self.name = "a-new-and-rather-long-name-" + Str(1); return "+x"; }
}
class D { c: C; }
fn lit() -> Str { return "lit"; }
fn grow(s: inout Str) -> Str { s.concat("+"); return "|"; }
fn repl(s: inout Str) -> Str { s = "replaced-by-the-argument-" + Str(2); return "+arg"; }
fn set(s: inout Str, v: Str) { s = move v; }
fn keep(t: inout (Str, int)) -> Str { return "+arg"; }
fn make(n: int) -> C { return C(lit()); }
fn pair() -> (Str, int) { return (lit(), 1); }
fn next(n: inout int) -> int { n = n + 1; return 0; }
fn main() -> int {
  for round in 0..2 {
    // an earlier read of a place names the string a later change is made to
    s: mutable Str = lit();
    io::println(s + grow(s));
    c: mutable = C(lit());
    io::println(c.name + c.bang());
    // an argument that replaces the receiver's place
    c.name.concat(c.reset());
    io::println(c.name);
    r: mutable Str = lit();
    r.concat(repl(r));
    io::println(r);
    // through chains, and a tuple lent to an argument
    d: mutable = D(C(lit()));
    d.c.name.concat("+a");
    objs = [C(lit()), C(lit())];
    n = 0;
    objs[next(n)].name.concat("+b");
    make(1).name.concat("+c");
    pair().0.concat("+d");
    t = (lit(), 1);
    t.0.concat(keep(t));
    io::println(d.c.name + " " + objs[0].name + " " + t.0 + " " + Str(n));
    // temporaries, and a conditional of a place and a literal
    lit().concat("!");
    let b = lit();
    (move b).concat("?");
    (if round == 0 then objs[next(n)].name else "other").concat("!");
    (if round == 0 then s else r).concat("~");
    io::println(objs[0].name + " " + s + " " + r + " " + Str(n));
    // through natives, an `inout` binding and a payload
    x: mutable Str = lit();
    let old = mem::replace(x, "new");
    x.concat("!");
    y: mutable Str = lit();
    mem::swap(x, y);
    y.concat("#");
    set(x, "set");
    x.concat("$");
    m = Maybe::Just("just");
    match m { j: inout Just { j.text.concat("%"); } _ {} }
    shown: mutable Str = "nothing";
    match m { j: Just { shown = j.text.clone(); } _ {} }
    io::println(old + " " + x + " " + y + " " + shown);
  }
  io::println(lit() + " " + "new" + " " + "set" + " " + "just" + " " + "other");
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "lit+|\nlit!|\na-new-and-rather-long-name-1\nreplaced-by-"
                      "the-argument-2\nlit+a lit+b lit+arg 1\nlit+b! lit+~ "
                      "replaced-by-the-argument-2 2\nlit set$ new!# "
                      "just%\nlit+|\nlit!|\na-new-and-rather-long-name-"
                      "1\nreplaced-by-the-argument-2\nlit+a lit+b lit+arg "
                      "1\nlit+b lit+ replaced-by-the-argument-2~ 1\nlit set$ "
                      "new!# just%\nlit new set just other\n");
  g.expectNoLeaks("NothingTellsALiteralsStringFromAnother");
}

// A `mutable` parameter is the function's own string whoever calls it (#363):
// a function value's type and an overridden method's signature do not say
// `mutable`, and a method named `equals` is lent its argument, so the callers
// cannot all make it one.  The second review's findings.
TEST(StringLiterals, AMutableParameterIsItsFunctionsOwn) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
struct S { n: int; }
impl S {
  view fn equals(s: mutable Str) -> bool { s.concat("!"); io::println(s); return True; }
  view fn same(s: shared mutable Str) -> bool { s.concat("="); io::println(s); return True; }
}
class K {
  n: int;
  view fn equals(other: mutable Obj) -> bool {
    match other { s: inout Str { s.concat("!"); io::println(s); } _ {} }
    return True;
  }
}
class B { view fn take(s: Str) -> Str { return s; } }
class D : B { view fn take(s: mutable Str) -> Str { s.concat("!"); return move s; } }
class C { name: Str; }
impl C { static fn make(s: mutable Str) -> Str { s.concat("s"); return move s; } }
struct Ops { f: (Str) -> Str; }
fn lit() -> Str { return "lit"; }
fn app(s: mutable Str) -> Str { s.concat("!"); return move s; }
fn apply(f: (mutable Str) -> Str, s: Str) -> Str { return f(move s); }
fn twice<T>(f: (T) -> T, x: T) -> T { return f(f(move x)); }
fn opt(o: mutable Obj?) -> Str {
  match o { s: inout Str { s.concat("?"); return s.clone(); } _ {} }
  return "none";
}
async fn later(s: mutable Str) -> Str { s.concat("@"); return move s; }
fn main() -> int {
  for round in 0..2 {
    // a function value: a named function's, a lambda, a parameter, a field,
    // an element, a static fn's, a generic function's parameter
    let f = app;
    let g = fn(s: mutable Str) -> Str { s.concat("~"); return move s; };
    let o = Ops(app);
    let fs = [app, app];
    let m = C::make;
    io::println(f("f") + f(lit()) + g("g") + g(lit()) + apply(app, "p") +
                apply(g, lit()) + o.f("o") + fs[round]("e") + m("m") +
                twice(app, "t"));
    // an override that adds `mutable`, called through its base
    b: mutable B = D();
    io::println(b.take("b") + b.take(lit()));
    // a method named `equals` is lent its argument, as `==` lends it
    let v = S(1);
    v.equals("v");
    v.equals(lit());
    v.same("w");
    v.same(lit());
    let k: Obj = K(1);
    io::println(Str(k == "k") + Str(K(2).equals(lit())));
    // an optional Obj: a string, None, another object
    io::println(opt("x") + opt(None) + opt(K(3)) + opt(lit()));
    // a task's
    let t = later("a");
    let u = later(lit());
    io::println((await t) + (await u));
  }
  io::println(lit() + "f" + "g" + "p" + "o" + "e" + "m" + "t" + "b" + "v" +
              "w" + "k" + "x" + "a");
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "f!lit!g~lit~p!lit~o!e!mst!!\n"
                      "b!lit!\n"
                      "v!\n"
                      "lit!\n"
                      "w=\n"
                      "lit=\n"
                      "k!\n"
                      "lit!\n"
                      "TrueTrue\n"
                      "x?nonenonelit?\n"
                      "a@lit@\n"
                      "f!lit!g~lit~p!lit~o!e!mst!!\n"
                      "b!lit!\n"
                      "v!\n"
                      "lit!\n"
                      "w=\n"
                      "lit=\n"
                      "k!\n"
                      "lit!\n"
                      "TrueTrue\n"
                      "x?nonenonelit?\n"
                      "a@lit@\n"
                      "litfgpoemtbvwkxa\n");
  g.expectNoLeaks("AMutableParameterIsItsFunctionsOwn");
}

// A `Str` held as an `Obj` is stored as a `Str` is (#363): a `match` gives
// it back to change in place, and a `weak` reference sees it go.
TEST(StringLiterals, AStringHeldAsAnObjIsUnsharedToo) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
import ::collections as cl;
class H { o: Obj; }
fn lit() -> Str { return "lit"; }
fn objOf() -> Obj { return "obj"; }
fn st(w: bool) -> Str { return if w then "gone" else "there"; }
fn g(o: Obj) -> Str {
  p: mutable Obj = move o;
  match p { s: inout Str { s.concat("?"); return s.clone(); } _ {} }
  return "d";
}
fn main() -> int {
  for round in 0..2 {
    let o: Obj = "lit";
    p: mutable Obj = move o;
    match p { s: inout Str { s.concat("!"); } _ {} }
    q: mutable Obj = objOf();
    match q { s: inout Str { s.concat("1"); } _ {} }
    let o2: Obj = lit();
    h: mutable = H(move o2);
    match h.o { s: inout Str { s.concat("2"); } _ {} }
    let o3: Obj = lit();
    let a: shared mutable Obj = move o3;
    let b = a;
    match a { s: Str { s.concat("3"); } _ {} }
    l = cl::List<Obj>();
    l.push("lo");
    e: mutable = l.pop();
    match e { s: inout Str { s.concat("4"); } _ {} }
    io::println(p.toString() + " " + q.toString() + " " + h.o.toString() + " " +
                b.toString() + " " + e.toString() + " " + g("arg") + g(lit()));
    // a weak reference sees it go
    let o4: Obj = "weak";
    sh: shared Obj? = move o4;
    w: weak = sh;
    let before = st(w == None);
    sh = None;
    io::println(before + " " + st(w == None));
  }
  io::println(lit() + " " + "obj" + " " + "lo" + " " + "arg" + " " + "weak");
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "lit! obj1 lit2 lit3 lo4 arg?lit?\n"
                      "there gone\n"
                      "lit! obj1 lit2 lit3 lo4 arg?lit?\n"
                      "there gone\n"
                      "lit obj lo arg weak\n");
  g.expectNoLeaks("AStringHeldAsAnObjIsUnsharedToo");
}

// A variable returned into a result of another form, `shared`, `shared
// mutable`, `locked mutable` or `atomic shared`, is a string of its own from
// there
// (#363): its box is taken without the dispatch's hook (emitAsSharedRaw).
TEST(StringLiterals, AReturnedVariableIsUnsharedForItsResult) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
fn lit() -> Str { return "lit"; }
fn st(w: bool) -> Str { return if w then "gone" else "there"; }
fn sh(s: Str) -> shared mutable Str { return s; }
fn arrow(s: Str) -> shared mutable Str => s;
fn loc() -> shared mutable Str { let t = lit(); return t; }
fn pick(c: bool, s: Str, t: Str) -> shared mutable Str { return if c then s else t; }
fn un(o: Str?) -> shared mutable Str {
  match move o { s: Str { return s; } _ {} }
  return "none";
}
fn opt(o: Str?) -> shared mutable Str? { return o; }
fn lk(s: Str) -> locked mutable Str { return s; }
fn fr(s: Str) -> atomic shared Str { return s; }
fn weakly(s: Str) -> shared Str? { return s; }
fn mutableOf(s: Str) -> mutable Str { return s; }
fn main() -> int {
  for round in 0..2 {
    let a = sh("a");
    let a2 = a;
    a.concat("1");
    let b = arrow("b");
    let b2 = b;
    b.concat("2");
    let c = loc();
    let c2 = c;
    c.concat("3");
    let d = pick(round == 0, "d", "D");
    let d2 = d;
    d.concat("4");
    let e = un("e");
    let e2 = e;
    e.concat("5");
    let f = opt("f");
    let f2 = f;
    match f { z: Str { z.concat("6"); } _ {} }
    let h = fn(s: Str) -> shared mutable Str => s;
    let i = h("i");
    let i2 = i;
    i.concat("7");
    io::println(a2 + b2 + c2 + d2 + e2 + f2.toString() + i2);
    let cell = lk("cell");
    lock cell as v: inout { v.concat("!"); io::println(v); }
    let fz = fr("frozen");
    let fz2 = fz;
    mutableOf("m").concat("8");
    m: mutable = mutableOf("m");
    m.concat("9");
    io::println(fz + fz2 + m);
    x: shared Str? = weakly("w");
    w: weak = x;
    x = None;
    io::println(st(w == None));
  }
  io::println(lit() + "a" + "b" + "d" + "D" + "e" + "f" + "i" + "cell" +
              "frozen" + "m" + "w");
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "a1b2lit3d4e5f6i7\n"
                      "cell!\n"
                      "frozenfrozenm9\n"
                      "gone\n"
                      "a1b2lit3D4e5f6i7\n"
                      "cell!\n"
                      "frozenfrozenm9\n"
                      "gone\n"
                      "litabdDeficellfrozenmw\n");
  g.expectNoLeaks("AReturnedVariableIsUnsharedForItsResult");
}

// `move` before a string temporary is the temporary, boxed (the lowering
// gave its owner the raw string: a PIR verification failure).
TEST(StringLiterals, AStringTemporaryIsMoved) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
fn lit() -> Str { return "lit"; }
fn main() -> int {
  for r in 0..2 {
    x: mutable Str = move "lit";
    x.concat("!");
    y: mutable Str = move ("a" + Str(r));
    y.concat("?");
    let z: Str = move "bare";
    w: mutable = move lit();
    w.concat("#");
    io::println(x + y + z + w + " " + lit() + "lit" + "bare");
  }
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "lit!a0?barelit# litlitbare\n"
                      "lit!a1?barelit# litlitbare\n");
  g.expectNoLeaks("AStringTemporaryIsMoved");
}

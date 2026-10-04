// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: optional types `T?` (issue #5) — assignability,
// operations that require unwrapping, equality against None, `match` as the
// unwrap, exhaustiveness, optional fields, `T?[]`, and module round-trips.

#include "TestUtils.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace paykan::test;

// ─── helpers ────────────────────────────────────────────────────────────────

static const char *const kNodeClass =
    "class Node {\n"
    "  v: int;\n"
    "  next: Node?;\n"
    "  fn __init__(x: int) { self.v = x; }\n"
    "  fn val() -> int { return self.v; }\n"
    "}\n"
    "class Leaf : Node {\n"
    "  fn __init__(x: int) { __super__(x); }\n"
    "}\n";

static std::string withNode(const std::string &body) {
  return std::string(kNodeClass) + "fn main() -> int {\n" + body +
         "\n  return 0;\n}\n";
}

static void expectOk(const std::string &src) {
  auto r = semaCheck(src);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

static void expectError(const std::string &src, const std::string &needle) {
  auto r = semaCheck(src);
  EXPECT_FALSE(r.Ok) << "expected an error containing: " << needle;
  EXPECT_NE(r.Diagnostics.find(needle), std::string::npos)
      << "diagnostics were:\n"
      << r.Diagnostics;
}

static const char *const kUnwrapNode =
    "cannot use optional 'Node?' as 'Node' without unwrapping (use match)";

// ─── declarations and assignability ─────────────────────────────────────────

TEST(Optional, DeclWithNoneAndWithValue) {
  expectOk(withNode(R"(
    a: Node? = None;
    b: Node? = Node(1);
    s: Str? = "hi";
    t: Str? = None;
  )"));
}

TEST(Optional, WideningTToOptionalT) {
  expectOk(withNode(R"(
    n: Node = Node(1);
    m: Node? = n;
    l: Node? = Leaf(2);   // subclass widens too
  )"));
}

TEST(Optional, OptionalSubclassToOptionalBase) {
  expectOk(withNode(R"(
    l: Leaf? = Leaf(1);
    n: Node? = l;
  )"));
}

TEST(Optional, OptionalToObjAllowed) {
  expectOk(withNode(R"(
    n: Node? = None;
    o: Obj = n;
    println(n);          // print takes Obj
  )"));
}

TEST(Optional, ReassignOptionalVar) {
  expectOk(withNode(R"(
    n: Node? = None;
    n = Node(1);
    n = None;
    m: Node = Node(2);
    n = m;
  )"));
}

TEST(Optional, InferredFromOptionalCallResult) {
  expectOk(std::string(kNodeClass) + R"(
    fn find(k: int) -> Node? { return None; }
    fn main() -> int {
      r = find(1);         // inferred Node?
      r = None;
      return 0;
    }
  )");
}

TEST(Optional, ParamAndReturn) {
  expectOk(std::string(kNodeClass) + R"(
    fn wrap(n: Node) -> Node? { return n; }
    fn pass(n: Node?) -> Node? { return n; }
    fn none() -> Node? { return None; }
    fn main() -> int {
      a: Node? = wrap(Node(1));
      b: Node? = pass(None);
      c: Node? = pass(Node(2));
      d: Node? = none();
      return 0;
    }
  )");
}

TEST(Optional, MovOfOptional) {
  expectOk(withNode(R"(
    a: Node? = Node(1);
    b = mov a;
    c: Node? = mov b;
  )"));
}

TEST(Optional, NarrowingToNonOptionalIsError) {
  expectError(withNode("a: Node? = None; b: Node = a;"), kUnwrapNode);
}

TEST(Optional, NarrowingOnAssignmentIsError) {
  expectError(withNode("a: Node? = None; b: Node = Node(1); b = a;"),
              kUnwrapNode);
}

TEST(Optional, NarrowingOnReturnIsError) {
  expectError(std::string(kNodeClass) + R"(
    fn f(n: Node?) -> Node { return n; }
    fn main() -> int { return 0; }
  )",
              kUnwrapNode);
}

TEST(Optional, NarrowingOnArgumentIsError) {
  expectError(std::string(kNodeClass) + R"(
    fn take(n: Node) -> int { return n.v; }
    fn main() -> int { a: Node? = None; return take(a); }
  )",
              kUnwrapNode);
}

TEST(Optional, NarrowingOnFieldAssignIsError) {
  expectError(withNode("a: Node? = None; n: Node = Node(1); n.next = a; "
                       "h: Node = Node(2); c: Node = h.next;"),
              kUnwrapNode);
}

TEST(Optional, UnrelatedTypeIsPlainMismatch) {
  // Unwrapping would not help here, so the ordinary diagnostic is used.
  expectError(withNode("s: Str? = None; n: Node = s;"),
              "initializer of type 'Str?' does not match declared type "
              "'Node'");
}

TEST(Optional, ObjIsNotAssignableToOptional) {
  expectError(withNode("o: Obj = Node(1); n: Node? = o;"),
              "initializer of type 'Obj' does not match declared type "
              "'Node?'");
}

// ─── operations on an optional value require unwrapping ─────────────────────

TEST(Optional, MethodCallOnOptionalIsError) {
  expectError(withNode("a: Node? = Node(1); x: int = a.val();"), kUnwrapNode);
}

TEST(Optional, MemberAccessOnOptionalIsError) {
  expectError(withNode("a: Node? = Node(1); x: int = a.v;"), kUnwrapNode);
}

TEST(Optional, MemberAssignOnOptionalIsError) {
  expectError(withNode("a: Node? = Node(1); a.v = 3;"), kUnwrapNode);
}

TEST(Optional, SubscriptOnOptionalArrayIsError) {
  expectError(withNode("xs: int[]? = [1, 2]; x: int = xs[0];"),
              "cannot use optional 'int[]?' as 'int[]' without unwrapping "
              "(use match)");
}

TEST(Optional, SubscriptAssignOnOptionalArrayIsError) {
  expectError(withNode("xs: int[]? = [1, 2]; xs[0] = 5;"),
              "cannot use optional 'int[]?' as 'int[]'");
}

TEST(Optional, ConcatOnOptionalStrIsError) {
  expectError(withNode(R"(s: Str? = "a"; t: Str = s + "b";)"),
              "cannot use optional 'Str?' as 'Str' without unwrapping "
              "(use match)");
}

TEST(Optional, ToStringOnOptionalIsError) {
  expectError(withNode(R"(s: Str? = "a"; t: Str = s.toString();)"),
              "cannot use optional 'Str?' as 'Str'");
}

TEST(Optional, UnaryOnOptionalIsError) {
  expectError(withNode(R"(s: Str? = "a"; b: bool = !s;)"),
              "cannot use optional 'Str?' as 'Str'");
}

// ─── equality ───────────────────────────────────────────────────────────────

TEST(Optional, CompareAgainstNone) {
  expectOk(withNode(R"(
    a: Node? = None;
    if (a == None) { println("none"); }
    if (a != None) { println("some"); }
    if (None == a) { println("none"); }
    s: Str? = "x";
    b: bool = s != None;
  )"));
}

TEST(Optional, CompareTwoOptionals) {
  expectOk(withNode(R"(
    a: Node? = None;
    b: Node? = Node(1);
    l: Leaf? = Leaf(2);
    if (a == b) { println("eq"); }
    if (a != b) { println("ne"); }
    if (b == l) { println("subtype ok"); }
    s: Str? = "x";
    t: Str? = "x";
    if (s == t) { println("str eq"); }
  )"));
}

TEST(Optional, CompareOptionalWithNonOptionalIsError) {
  expectError(withNode("a: Node? = None; n: Node = Node(1); b: bool = a == n;"),
              "cannot compare optional 'Node?' with non-optional 'Node'; "
              "compare against None or unwrap it with match");
}

TEST(Optional, CompareMismatchedOptionalsIsError) {
  expectError(withNode("a: Node? = None; s: Str? = None; b: bool = a == s;"),
              "operands of '==' have mismatched types 'Node?' and 'Str?'");
}

// ─── match is the unwrap ────────────────────────────────────────────────────

TEST(Optional, MatchWithBindingAndWildcard) {
  expectOk(withNode(R"(
    a: Node? = Node(1);
    match a {
      n: Node { println(Str<int>(n.val())); }
      _       { println("none"); }
    }
  )"));
}

TEST(Optional, MatchWithNoneArm) {
  expectOk(withNode(R"(
    a: Node? = None;
    match a {
      n: Node { println(Str<int>(n.v)); }
      None    { println("none"); }
    }
  )"));
}

TEST(Optional, MatchSubclassArmThenInner) {
  expectOk(withNode(R"(
    a: Node? = Leaf(1);
    match a {
      l: Leaf { println("leaf"); }
      n: Node { println("node"); }
      None    { println("none"); }
    }
  )"));
}

TEST(Optional, MatchOnOptionalStr) {
  expectOk(withNode(R"(
    s: Str? = "x";
    match s {
      v: Str { println(v + "!"); }
      None   { println("none"); }
    }
  )"));
}

TEST(Optional, MatchOnOptionalArray) {
  expectOk(withNode(R"(
    xs: int[]? = [1, 2, 3];
    match xs {
      a: int[] { println(Str<int>(a.len())); }
      None     { println("none"); }
    }
  )"));
}

TEST(Optional, MatchOnOptionalFieldAndCallResult) {
  expectOk(std::string(kNodeClass) + R"(
    fn find(k: int) -> Node? { return Node(k); }
    fn main() -> int {
      h: Node = Node(1);
      match h.next {
        n: Node { println("has next"); }
        _       { println("tail"); }
      }
      match find(2) {
        n: Node { println(Str<int>(n.v)); }
        None    { }
      }
      return 0;
    }
  )");
}

TEST(Optional, MatchTypeArmPlusNoneIsExhaustiveForReturn) {
  expectOk(std::string(kNodeClass) + R"(
    fn value(a: Node?) -> int {
      match a {
        n: Node { return n.v; }
        None    { return -1; }
      }
    }
    fn main() -> int { return value(None); }
  )");
}

TEST(Optional, MatchTypeArmAloneIsNotExhaustive) {
  expectError(std::string(kNodeClass) + R"(
    fn value(a: Node?) -> int {
      match a {
        n: Node { return n.v; }
      }
    }
    fn main() -> int { return value(None); }
  )",
              "non-void function 'value' does not always return a value");
}

TEST(Optional, MatchSubclassArmPlusNoneIsNotExhaustive) {
  expectError(std::string(kNodeClass) + R"(
    fn value(a: Node?) -> int {
      match a {
        l: Leaf { return l.v; }
        None    { return -1; }
      }
    }
    fn main() -> int { return value(None); }
  )",
              "does not always return a value");
}

TEST(Optional, MatchLiteralArmOnOptionalIsError) {
  expectError(withNode(R"(s: Str? = "x"; match s { "x" { } _ { } })"),
              "match on optional 'Str?' requires type-name arms or 'None', "
              "not literal patterns");
}

TEST(Optional, MatchUnrelatedArmTypeIsError) {
  expectError(withNode("a: Node? = None; match a { s: Str { } _ { } }"),
              "type 'Str' is not a subclass of 'Node' (the match subject has "
              "type 'Node?')");
}

TEST(Optional, MatchOptionalArmTypeIsError) {
  expectError(withNode("a: Node? = None; match a { n: Node? { } _ { } }"),
              "match arm type 'Node?' cannot be optional");
}

TEST(Optional, MatchArmAfterInnerArmIsUnreachable) {
  expectError(withNode("a: Node? = None; match a { Node { } Leaf { } _ { } }"),
              "unreachable arm: the 'Node' arm above already matches every "
              "non-None value");
}

TEST(Optional, MatchDuplicateNoneArmIsError) {
  expectError(withNode("a: Node? = None; match a { None { } None { } _ { } }"),
              "duplicate 'None' arm in match on 'Node?'");
}

TEST(Optional, MatchWrongArrayArmIsError) {
  expectError(withNode("xs: int[]? = [1]; match xs { a: Str[] { } _ { } }"),
              "match arm type 'Str[]' does not match the optional subject "
              "type 'int[]?'");
}

TEST(Optional, MatchBindingIsNarrowedToArmType) {
  // Binding has the arm type — a subclass arm binds the subclass.
  expectOk(withNode(R"(
    a: Node? = Leaf(1);
    match a {
      l: Leaf { x: Leaf = l; }
      n: Node { y: Node = n; }
      _ { }
    }
  )"));
}

// ─── the existing Obj-subject match is unchanged ────────────────────────────

TEST(Optional, ObjSubjectMatchStillWorks) {
  expectOk(R"(
    fn main() -> int {
      match Stdin.readln() {
        line: Str { println(line); }
        _         { println("eof"); }
      }
      return 0;
    }
  )");
}

TEST(Optional, NoneArmOnObjSubjectIsStillAnError) {
  expectError(R"(
    fn main() -> int {
      o: Obj = None;
      match o {
        None { }
        _    { }
      }
      return 0;
    }
  )",
              "requires type-name arms, not literal patterns");
}

// ─── optional fields ────────────────────────────────────────────────────────

TEST(Optional, OptionalFieldNeedNotBeAssignedInInit) {
  // `next: Node?` is implicitly None; `v: int` still must be assigned.
  expectOk(withNode("n: Node = Node(1);"));
}

TEST(Optional, NonOptionalFieldStillMustBeAssigned) {
  expectError(R"(
    class Pair {
      a: Str;
      b: Str?;
      fn __init__() { self.b = None; }
    }
    fn main() -> int { return 0; }
  )",
              "field 'a' of class 'Pair' is not assigned on every path");
}

TEST(Optional, OptionalFieldAssignedViaMatch) {
  expectOk(std::string(kNodeClass) + R"(
    fn main() -> int {
      a: Node = Node(1);
      b: Node = Node(2);
      a.next = b;
      a.next = None;
      match a.next {
        n: Node { a.next = n.next; }
        None    { }
      }
      return 0;
    }
  )");
}

TEST(Optional, LinkedListTraversal) {
  expectOk(std::string(kNodeClass) + R"(
    fn sum(head: Node?) -> int {
      total: int = 0;
      cur: Node? = head;
      while (cur != None) {
        match cur {
          n: Node { total = total + n.v; cur = n.next; }
          None    { }
        }
      }
      return total;
    }
    fn main() -> int {
      a: Node = Node(1);
      b: Node = Node(2);
      a.next = b;
      return sum(a);
    }
  )");
}

// ─── arrays of optionals and optional arrays ────────────────────────────────

TEST(Optional, ArrayOfOptionals) {
  expectOk(withNode(R"(
    xs: Node?[] = [];
    xs.push(None);
    xs.push(Node(1));
    xs[0] = Node(2);
    xs[1] = None;
    e: Node? = xs[0];
    p: Node? = xs.pop();
    match xs[0] {
      n: Node { println(Str<int>(n.v)); }
      None    { }
    }
  )"));
}

TEST(Optional, OptionalArrayWithEmptyLiteral) {
  expectOk(withNode(R"(
    xs: Str[]? = [];
    ys: Str[]? = None;
    ys = [];
  )"));
}

TEST(Optional, ArrayOfOptionalsElementNarrowingIsError) {
  expectError(withNode("xs: Node?[] = []; xs.push(Node(1)); n: Node = xs[0];"),
              kUnwrapNode);
}

// ─── ternary ────────────────────────────────────────────────────────────────

TEST(Optional, TernaryWithNoneBranchIsOptional) {
  expectOk(withNode(R"(
    c: bool = True;
    a: Node? = if c then Node(1) else None;
    b: Node? = if c then None else Node(2);
    s: Str? = if c then "x" else None;
    d: Node? = if c then a else b;
    l: Leaf? = None;
    e: Node? = if c then a else l;
  )"));
}

TEST(Optional, TernaryOptionalResultCannotNarrow) {
  expectError(
      withNode("c: bool = True; a: Node = if c then Node(1) else None;"),
      kUnwrapNode);
}

// ─── rejected optional inner types ──────────────────────────────────────────

TEST(Optional, OptionalEnumIsRejected) {
  expectError(R"(
    enum Color { Red, Green }
    fn main() -> int {
      c: Color? = None;
      return 0;
    }
  )",
              "optional enum types are not supported yet");
}

TEST(Optional, OptionalEnumFieldIsRejected) {
  expectError(R"(
    enum Color { Red, Green }
    class P { c: Color?; fn __init__() {} }
    fn main() -> int { return 0; }
  )",
              "optional enum types are not supported yet");
}

TEST(Optional, UnknownInnerTypeIsRejected) {
  expectError("fn main() -> int { x: Missing? = None; return 0; }",
              "has unknown class type 'Missing'");
}

// ─── modules: T? round-trips through exported signatures ────────────────────

static std::string writeFile(const std::string &dir, const std::string &relPath,
                             const std::string &content) {
  auto full = std::filesystem::path(dir) / relPath;
  std::filesystem::create_directories(full.parent_path());
  std::ofstream ofs(full);
  ofs << content;
  return full.string();
}

struct SemaFileResult {
  bool Ok;
  std::string Diagnostics;
};

static SemaFileResult semaCheckFile(const std::string &filePath,
                                    const std::string &projectRoot) {
  paykan::parser::ParserDriver drv(paykan::test::testFrontend());
  if (drv.parseFile(filePath) != 0)
    return {false, "parse error"};
  std::ostringstream os;
  paykan::sema::DiagEngine diagEngine(os);
  diagEngine.setSourceInfo(drv.getCurrentFile(), &drv.getSourceLines());
  paykan::sema::Sema sema(drv.getASTContext(), diagEngine, projectRoot,
                          drv.getFrontendName());
  bool ok = sema.run(drv.getRoot()).Ok;
  return {ok, os.str()};
}

static const char *const kListModule =
    "class Node {\n"
    "  v: int;\n"
    "  next: Node?;\n"
    "  fn __init__(x: int) { self.v = x; }\n"
    "  fn tail() -> Node? { return self.next; }\n"
    "  fn link(n: Node?) { self.next = n; }\n"
    "}\n"
    "fn find(head: Node?, key: int) -> Node? { return head; }\n"
    "fn first(xs: Str?[]) -> Str? { return xs[0]; }\n"
    "fn maybeArr(xs: int[]?) -> int[]? { return xs; }\n";

TEST(OptionalModule, ExportedOptionalSignaturesRoundTrip) {
  auto tmp = (paykan::test::tempDir() / "pkn_opt_mod").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "list.pkn", kListModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import list;
fn main() -> int {
  n: list::Node = list::Node(1);
  n.link(None);
  n.link(list::Node(2));
  t: list::Node? = n.tail();
  f: list::Node? = list::find(n, 2);
  g: list::Node? = list::find(None, 2);
  ss: Str?[] = [];
  ss.push(None);
  s: Str? = list::first(ss);
  a: int[]? = list::maybeArr([1, 2]);
  b: int[]? = list::maybeArr(None);
  match t {
    x: list::Node { println(Str<int>(x.v)); }
    None { println("none"); }
  }
  return 0;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(OptionalModule, ExportedOptionalReturnIsStillOptional) {
  // The imported return type must be `Node?` (not degraded to Node/Obj):
  // narrowing it without a match is the optional-unwrap error.
  auto tmp = (paykan::test::tempDir() / "pkn_opt_mod2").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "list.pkn", kListModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import list;
fn main() -> int {
  n: list::Node = list::find(None, 1);
  return 0;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("cannot use optional 'Node?' as 'Node' "
                               "without unwrapping (use match)"),
            std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

TEST(OptionalModule, ExportedOptionalFieldIsStillOptional) {
  auto tmp = (paykan::test::tempDir() / "pkn_opt_mod3").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "list.pkn", kListModule);
  auto main = writeFile(tmp, "main.pkn", R"(
import list;
fn main() -> int {
  n: list::Node = list::Node(1);
  m: list::Node = n.next;
  return 0;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("cannot use optional 'Node?' as 'Node'"),
            std::string::npos)
      << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// ─── interaction with tuples (#4) ───────────────────────────────────────────

TEST(OptionalTuple, TupleWithOptionalElementsOk) {
  expectOk(std::string(kNodeClass) + R"(
    fn pick(n: Node?, k: int) -> (Node?, int) { return (n, k); }
    fn main() -> int {
      a: Node? = Node(7);
      t: (Node?, int) = pick(a, 1);
      u = pick(None, 2);
      x, k = t;
      y: Node?, j: int = u;
      same: bool = t == pick(a, 1);
      isNone: bool = y == None;
      ts: (Node?, int)[] = [];
      ts.push(t);
      match x {
        n: Node { k = n.val(); }
        None    { }
      }
      return 0;
    }
  )");
}

TEST(OptionalTuple, DestructuredOptionalElementStillNeedsUnwrap) {
  expectError(std::string(kNodeClass) + R"(
    fn pick(n: Node?, k: int) -> (Node?, int) { return (n, k); }
    fn main() -> int {
      x, k = pick(None, 1);
      n: Node = x;
      return 0;
    }
  )",
              kUnwrapNode);
}

// #71: a `None` element takes the destination's `T?`, in every typed sink
// (declaration, assignment, argument, method argument, return, field and
// subscript store) and in nested literals.
TEST(OptionalTuple, NoneInTupleLiteralTakesTheSlotsOptional) {
  expectOk(std::string(kNodeClass) + R"(
    class Box { p: (Node?, int); fn __init__() { self.p = (None, 0); }
                fn set(p: (int?, Str)) -> Str { return p.1; } }
    fn take(p: (int?, Str)) -> Str { return p.1; }
    fn mk() -> (Node?, int) { return (None, 2); }
    fn main() -> int {
      a: (int?, Str) = (None, "a");
      b: (Node?, int) = (None, 1);
      b = (None, 3);
      take((None, "x"));
      bx = Box();
      bx.set((None, "y"));
      bx.p = (None, 4);
      ps: (Node?, int)[] = [];
      ps.push((None, 5));
      ps[0] = (None, 6);
      n: (Str, (Node?, int?)) = ("n", (None, None));
      c: (Obj, int) = (None, 1);
      return 0;
    }
  )");
}

TEST(OptionalTuple, NoneInTupleLiteralStillNeedsAnOptionalSlot) {
  expectError("fn main() -> int { a: (int, Str) = (None, \"a\"); return 0; }",
              "initializer of type '(Obj, Str)' does not match declared type "
              "'(int, Str)'");
  expectError(std::string(kNodeClass) +
                  "fn main() -> int { b: (Node?, int) = (None, None); "
                  "return 0; }",
              "initializer of type '(Obj, Obj)' does not match declared type "
              "'(Node?, int)'");
  // Without a destination a bare `None` is still `Obj`.
  expectError(std::string(kNodeClass) +
                  "fn main() -> int { t = (None, 1); n: Node? = t.0; "
                  "return 0; }",
              "does not match declared type 'Node?'");
}

// #71: an array literal with a declared element type takes it when its
// elements disagree but every one fits (`int` and `int?`, `("a", 1)` and
// `("b", n)`, `None` among class instances).
TEST(OptionalTuple, ArrayLiteralElementsTakeTheDeclaredElementType) {
  expectOk(std::string(kNodeClass) + R"(
    fn count(xs: (Str, int?)[]) -> int { return xs.len(); }
    fn mk(nb: int?) -> (Str, int?)[] { return [("r", nb), ("s", 1)]; }
    fn main() -> int {
      nb: int? = None;
      n: Node? = None;
      tbl: (Str, int?)[] = [("a", 1), ("b", nb), ("c", 3)];
      tbl = [("x", nb), ("y", None)];
      count([("p", 1), ("q", nb)]);
      tbl.push(("w", None));
      ints: int?[] = [1, nb, None];
      nodes: Node?[] = [None, n, Node(1)];
      grid: int?[][] = [[1], [nb], []];
      nested: (Str, (int?, Node?))[] = [("a", (1, None)), ("b", (nb, n))];
      inTuple: (int?[], Str) = ([1, nb], "s");
      return 0;
    }
  )");
}

TEST(OptionalTuple, ArrayLiteralElementThatDoesNotFitIsReported) {
  expectError("fn main() -> int { nb: int? = None; "
              "t: (Str, int?)[] = [(\"a\", 1), (\"b\", nb), (1, 2)]; "
              "return 0; }",
              "array literal element of type '(int, int)' does not match the "
              "expected element type '(Str, int?)'");
  expectError("fn main() -> int { nb: int? = None; "
              "t: (Str, int)[] = [(\"a\", 1), (\"b\", nb)]; return 0; }",
              "array literal element of type '(Str, int?)' does not match "
              "the expected element type '(Str, int)'");
  // No destination: the elements must agree on their own.
  expectError("fn main() -> int { nb: int? = None; "
              "x = [(1, \"a\"), (nb, \"b\")]; return 0; }",
              "array literal has inconsistent element types");
  // No promotion inside a tuple literal, contextual or not.
  expectError("fn main() -> int { t: (float, int)[] = [(1, 2)]; return 0; }",
              "does not match declared type '(float, int)[]'");
}

TEST(OptionalTuple, OptionalTupleTypeRejected) {
  expectError(R"(
    fn find(k: int) -> (int, Str)? { return None; }
    fn main() -> int { return 0; }
  )",
              "optional tuple types are not supported yet");
  expectError("fn main() -> int { x: (int, Str)?[] = []; return 0; }",
              "optional tuple types are not supported yet");
}

TEST(OptionalTupleModule, TupleAndOptionalSignaturesRoundTrip) {
  // Serialised spellings mix tuple parentheses with `?` and `[]` suffixes:
  // "(Node?, int)[]?", "(Node?, Str?)[]", "(Node?, int)".
  auto tmp = (paykan::test::tempDir() / "pkn_opt_tuple_mod").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib.pkn",
            "class Node { v: int; fn __init__(x: int) { self.v = x; } }\n"
            "fn wrap(xs: (Node?, int)[]?) -> (Node?, int)[]? { return xs; }\n"
            "fn pairs() -> (Node?, Str?)[] { return []; }\n"
            "fn mk(n: Node?) -> (Node?, int) { return (n, 5); }\n");
  auto ok = writeFile(tmp, "main.pkn", R"(
import lib;
fn main() -> int {
  a: (lib::Node?, int)[]? = lib::wrap(None);
  b: (lib::Node?, int)[]? = lib::wrap([]);
  p: (lib::Node?, Str?)[] = lib::pairs();
  x, k = lib::mk(None);
  return k;
}
)");
  auto r = semaCheckFile(ok, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  // The element must still be optional after the round trip.
  auto bad = writeFile(tmp, "bad.pkn", R"(
import lib;
fn main() -> int {
  x, k = lib::mk(None);
  n: lib::Node = x;
  return 0;
}
)");
  auto r2 = semaCheckFile(bad, tmp);
  EXPECT_FALSE(r2.Ok);
  EXPECT_NE(r2.Diagnostics.find("cannot use optional 'Node?' as 'Node'"),
            std::string::npos)
      << r2.Diagnostics;
  std::filesystem::remove_all(tmp);
}

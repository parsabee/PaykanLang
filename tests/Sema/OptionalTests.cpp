// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: optional types `T?` (issue #5) — assignability,
// operations that require unwrapping, equality against None, `match` as the
// unwrap, exhaustiveness, optional fields, `T?[n]`, and module round-trips.

#include "TestUtils.h"
#include <gtest/gtest.h>
#include <sstream>

using namespace paykan::test;

// ─── helpers ────────────────────────────────────────────────────────────────

static const char *const kNodeClass =
    "class Node {\n"
    "  v: int;\n"
    "  next: Node?;\n"
    "  view fn val() -> int { return self.v; }\n"
    "}\n"
    "impl Node {\n"
    "  fn __init__(x: int) { self.v = x; }\n"
    "}\n"
    "class Leaf : Node {}\n"
    "impl Leaf {\n"
    "  fn __init__(x: int) { __super__(x); }\n"
    "}\n";

static std::string withNode(const std::string &body) {
  return std::string(kNodeClass) + "import ::io; fn main() -> int {\n" + body +
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
    m: Node? = move n;
    l: Node? = Leaf(2);   // subclass widens too
  )"));
}

TEST(Optional, OptionalSubclassToOptionalBase) {
  expectOk(withNode(R"(
    l: Leaf? = Leaf(1);
    n: Node? = move l;
  )"));
}

TEST(Optional, OptionalIsPrinted) {
  expectOk(withNode(R"(
    n: Node? = None;
    io::println(n);          // `None` or the node
    s: Str? = "a";
    io::print(s);
    io::printerrln(s);
  )"));
}

// An optional is not an object (#96): it does not become an `Obj`, in any
// position.
TEST(Optional, OptionalIsNotAnObj) {
  const char *msg = "cannot use optional 'Node?' as 'Obj': an optional is not "
                    "an object; unwrap it with match";
  expectError(withNode("n: Node? = None; o: Obj = n;"), msg);
  expectError(withNode("n: Node? = None; o: Obj = \"o\"; o = n;"), msg);
  expectError(std::string(kNodeClass) +
                  "fn take(o: Obj) { }\n"
                  "fn main() -> int { n: Node? = None; take(n); return 0; }\n",
              msg);
  expectError(std::string(kNodeClass) +
                  "fn give(n: Node?) -> Obj { return n; }\n"
                  "fn main() -> int { return 0; }\n",
              msg);
  expectError(
      std::string(kNodeClass) +
          "class H { o: Obj; }\n"
          "impl H { fn __init__() { self.o = \"o\"; } }\n"
          "fn main() -> int { h: mutable = H(); n: Node? = None; h.o = n; "
          "return 0; }\n",
      msg);
  expectError(withNode("n: Node? = None; xs: Obj[1] = [Node(1)]; xs[0] = n;"),
              "an optional is not an object");
  expectError(withNode("s: Str? = None; t: (Obj, int) = (move s, 1);"),
              "does not match");
}

TEST(Optional, ReassignOptionalVar) {
  expectOk(withNode(R"(
    n: Node? = None;
    n = Node(1);
    n = None;
    m: Node = Node(2);
    n = move m;
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

// An optional of a unique reference is move-only: it moves on, inferred as
// the optional.
TEST(Optional, CopyOfOptional) {
  expectOk(withNode(R"(
    a: Node? = Node(1);
    b = move a;
    c: Node? = move b;
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

TEST(Optional, ConcatOnOptionalStrIsError) {
  expectError(withNode(R"(s: Str? = "a"; t: Str = s + "b";)"),
              "cannot use optional 'Str?' as 'Str' without unwrapping "
              "(use match)");
}

// `toString()` is the one method an optional has (the builtin `Showable`):
// "None", or its value's text.  Any other needs the value unwrapped.
TEST(Optional, ToStringOnOptionalIsItsText) {
  auto r = semaCheck(withNode(
      R"(s: Str? = "a"; n: int? = None; t: Str = s.toString() + n.toString();)"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  expectError(withNode(R"(s: Str? = "a"; n = s.len();)"),
              "cannot use optional 'Str?' as 'Str'");
}

TEST(Optional, UnaryOnOptionalIsError) {
  expectError(withNode(R"(s: Str? = "a"; b: bool = !s;)"),
              "cannot use optional 'Str?' as 'Str'");
}

// An optional tuple's elements are read once it is unwrapped: a tuple index
// on it, or destructuring it, says so (it said "non-tuple type"), once.  An
// optional of anything else is still no tuple.
TEST(Optional, TupleIndexOnOptionalTupleIsError) {
  const char *unwrap = "cannot use optional '(int, int)?' as '(int, int)' "
                       "without unwrapping (use match)";
  for (const char *body : {"t: (int, int)? = (1, 2); x = t.0;",
                           "t: (int, int)? = (1, 2); x = t.5;",
                           "u: ((int, int)?, int) = ((1, 2), 3); x = u.0.1;",
                           "t: (int, int)? = (1, 2); a, b = t;"}) {
    auto r = semaCheck(withNode(body));
    EXPECT_FALSE(r.Ok) << body;
    EXPECT_NE(r.Diagnostics.find(unwrap), std::string::npos) << body << "\n"
                                                             << r.Diagnostics;
    EXPECT_EQ(r.ErrorCount, 1u) << body << "\n" << r.Diagnostics;
  }
  expectError(withNode(R"(s: Str? = "a"; x = s.0;)"),
              "tuple index '.0' applied to non-tuple type 'Str?'");
  expectOk(withNode("t: (int, int)? = (1, 2);\n"
                    "match t { p: (int, int) { a, b = p; x = p.1 + a; } "
                    "None { } }"));
}

// ─── equality ───────────────────────────────────────────────────────────────

TEST(Optional, CompareAgainstNone) {
  expectOk(withNode(R"(
    a: Node? = None;
    if a == None { io::println("none"); }
    if a != None { io::println("some"); }
    if None == a { io::println("none"); }
    s: Str? = "x";
    b: bool = s != None;
  )"));
}

TEST(Optional, CompareTwoOptionals) {
  expectOk(withNode(R"(
    a: Node? = None;
    b: Node? = Node(1);
    l: Leaf? = Leaf(2);
    if a == b { io::println("eq"); }
    if a != b { io::println("ne"); }
    if b == l { io::println("subtype ok"); }
    s: Str? = "x";
    t: Str? = "x";
    if s == t { io::println("str eq"); }
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
      n: Node { io::println(Str<int>(n.val())); }
      _       { io::println("none"); }
    }
  )"));
}

TEST(Optional, MatchWithNoneArm) {
  expectOk(withNode(R"(
    a: Node? = None;
    match a {
      n: Node { io::println(Str<int>(n.v)); }
      None    { io::println("none"); }
    }
  )"));
}

TEST(Optional, MatchSubclassArmThenInner) {
  expectOk(withNode(R"(
    a: Node? = Leaf(1);
    match a {
      l: Leaf { io::println("leaf"); }
      n: Node { io::println("node"); }
      None    { io::println("none"); }
    }
  )"));
}

TEST(Optional, MatchOnOptionalStr) {
  expectOk(withNode(R"(
    s: Str? = "x";
    match s {
      v: Str { io::println(v + "!"); }
      None   { io::println("none"); }
    }
  )"));
}

TEST(Optional, MatchOnOptionalFieldAndCallResult) {
  expectOk(std::string(kNodeClass) + R"(import ::io;
    fn find(k: int) -> Node? { return Node(k); }
    fn main() -> int {
      h: Node = Node(1);
      match h.next {
        n: Node { io::println("has next"); }
        _       { io::println("tail"); }
      }
      match find(2) {
        n: Node { io::println(Str<int>(n.v)); }
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

TEST(Optional, MatchBindingIsNarrowedToArmType) {
  // Binding has the arm type — a subclass arm binds the subclass.
  expectOk(withNode(R"(
    a: Node? = Leaf(1);
    match move a {
      l: Leaf { x: Leaf = move l; }
      n: Node { y: Node = move n; }
      _ { }
    }
  )"));
}

// ─── `None` has no type of its own (#394) ───────────────────────────────────

// With no optional to be the absent value of, `None` is an error, and the
// diagnostic says to write the type.
TEST(OptionalNone, WithoutADestinationIsAnError) {
  expectError(withNode("x = None;"),
              "cannot infer the type of 'x' from None: write the optional it "
              "is, as in 'x: T? = None'");
  expectError(withNode("let x = None;"),
              "cannot infer the type of 'x' from None");
  expectError(withNode("c = True; y = if c then None else None;"),
              "cannot infer the type of 'y' from None");
  expectError(withNode("t = (None, 1);"),
              "cannot infer the type of 't' from '(None, int)': a None takes "
              "the type of the optional it flows into, so write the "
              "variable's type");
  const char *destructured =
      "cannot destructure a value of type '(None, int)': a None has no type "
      "until a slot of an optional type takes it, and a destructuring stores "
      "each element as it is; assign the None on its own";
  expectError(withNode("a, b = (None, 1);"), destructured);
  expectError(withNode("a: Node?, b: int = (None, 1);"), destructured);
  expectError(withNode("xs = [None, None];"),
              "an array literal of None alone has no element type: write the "
              "array's type, as in 'xs: T?[2] = ...'");
  expectError(withNode("xs = [None; 3];"),
              "an array literal of None alone has no element type");
  // Nor does a sibling give an inner literal its type.
  expectError(withNode("xs = [[None, None], [None, Node(1)]];"),
              "an array literal of None alone has no element type");
  expectError(withNode("xs = [(None, 1), (None, 2)];"),
              "cannot infer the array's element type from '(None, int)': a "
              "None takes the type of the optional it flows into, so write "
              "the array's type");
  expectError(withNode("None;"),
              "None has no type here: it is the absent value of an optional, "
              "and nothing says which");
  expectError(withNode("io::println(None);"),
              "cannot infer type parameter 'T' of 'io::println' from the call "
              "arguments: None is the absent value of an optional, and says "
              "nothing of its type");
  expectError(withNode("s = Str(None);"),
              "'Str' cannot convert None: it is the absent value of an "
              "optional, and nothing here says which");
  // No receiver, and nothing for a `move` to take (a temporary is moved
  // as it is; a None is no value yet).
  const std::string alone =
      " None: it is the absent value of an optional, and no value until a "
      "slot of an optional type takes it";
  expectError(withNode("s = None.toString();"),
              "cannot call 'toString' on" + alone);
  expectError(withNode("x = None.clone();"), "cannot call 'clone' on" + alone);
  expectError(withNode("c = True; x = (if c then None else None).clone();"),
              "cannot call 'clone' on" + alone);
  expectError(withNode("x: Node? = move None;"), "cannot move" + alone);
  expectError(std::string(kNodeClass) +
                  "fn take(n: Node?) -> bool { return n == None; }\n"
                  "fn main() -> int { take(move None); return 0; }\n",
              "cannot move" + alone);
  expectError(withNode("match None { _ { } }"), "'None'");
  expectError("fn f() => None;\nfn main() -> int { return 0; }\n",
              "the result of 'f' has no type: None is the absent value of an "
              "optional, and an inferred result does not say which; write "
              "the result type ('-> T?')");
  expectError("fn main() -> int { f = fn() => None; return 0; }\n",
              "the lambda's result has no type");
}

// `None` is no object: no slot of a class type takes it, `Obj` included, and
// nothing but an optional compares with it.
TEST(OptionalNone, IsNoObj) {
  const char *noObj = "None is not a value of type 'Obj': it is the absent "
                      "value of an optional ('Obj?')";
  expectError(withNode("o: Obj = None;"), noObj);
  expectError(withNode("o: Obj = \"o\"; o = None;"), noObj);
  expectError("fn take(o: Obj) { }\nfn main() -> int { take(None); return 0; "
              "}\n",
              noObj);
  expectError("fn give() -> Obj { return None; }\n"
              "fn main() -> int { return 0; }\n",
              noObj);
  expectError("class H { o: Obj; }\n"
              "impl H { fn __init__() { self.o = None; } }\n"
              "fn main() -> int { return 0; }\n",
              noObj);
  expectError(withNode("xs: Obj[2] = [None, Node(1)];"), noObj);
  expectError(std::string(kNodeClass) +
                  "fn count(xs: view Obj[]) -> int { return xs.len(); }\n"
                  "fn main() -> int { return count([None, Node(1)]); }\n",
              noObj);
  expectError(withNode("xs: Node[3] = [None; 3];"),
              "None is not a value of type 'Node'");
  expectError(withNode("n: Node = None;"),
              "None is not a value of type 'Node': it is the absent value of "
              "an optional ('Node?')");
  expectError(withNode("i: int = None;"),
              "None is not a value of type 'int': it is the absent value of "
              "an optional ('int?')");
  expectError(withNode("o: Obj = Node(1); b = o == None;"),
              "cannot compare 'Obj' with None: only an optional is ever None");
  expectError(withNode("n = Node(1); b = None != n;"),
              "cannot compare 'Node' with None: only an optional is ever "
              "None");
  expectError(withNode("i = 1; b = i == None;"),
              "cannot compare 'int' with None: only an optional is ever None");
  expectError(withNode("b = None == None;"),
              "cannot compare None with None: it is the absent value of an "
              "optional, and neither operand is one");
  // The root has no object of its own.
  expectError(withNode("o: Obj = Obj();"),
              "cannot make an 'Obj': it is the root of the class hierarchy, "
              "and every object is one of a class");
}

// A `None` element of an array literal with no declared type is the absent
// value of the other elements' common type.
TEST(OptionalNone, ArrayLiteralTakesTheOthersOptional) {
  expectOk(std::string(kNodeClass) + R"(
    fn two(xs: Node?[2]) -> int { return xs.len(); }
    fn three(xs: view Node?[]) -> int { return xs.len(); }
    fn ints(xs: int?[3]) -> int { return xs.len(); }
    fn main() -> int {
      two([None, Node(1)]);
      xs = [None, Node(1)];
      two(move xs);
      n: Node? = Node(2);
      ys = [None, move n];
      two(move ys);
      zs = [Node(1), None, Leaf(2)];
      three(zs);
      is = [1, None, 3];
      ints(is);
      ss = ["a", None];
      match ss[1] { s: Str { } None { } }
      ts = [(1, "a"), None];
      match ts[0] { t: (int, Str) { } None { } }
      return 0;
    }
  )");
  // The literal's type, as a mismatch names it.
  expectError(withNode("xs = [None, Node(1)]; bad: int = xs;"),
              "initializer of type 'Node?[2]' does not match declared type "
              "'int'");
  expectError(withNode("xs = [1, None]; bad: Str = xs;"),
              "initializer of type 'int?[2]' does not match declared type "
              "'Str'");
  // Elements that are a `T` and a `T?` still disagree, a None aside.
  expectError(withNode("n: Node? = None; xs = [None, move n, Node(1)];"),
              "array literal has inconsistent element types: 'Node?' and "
              "'Node'");
  // A type with no optional has no None element.
  expectError(withNode("xs = [None, [1, 2]];"), "'int[2]'");
}

// `Obj?` is an optional like any other: an `Obj` converts to a present one,
// `None` is its absent value, and a match on it dispatches on the class.
TEST(OptionalNone, ObjOptionalIsAnOrdinaryOptional) {
  expectOk(std::string(kNodeClass) + R"(
    class Box { any: Obj?; w: weak Obj?; }
    impl Box { fn __init__() { self.any = None; } }
    fn pick(c: bool) -> Obj? { return if c then None else Node(1); }
    fn what(o: view Obj?) -> int {
      match o {
        l: Leaf { return 1; }
        n: Node { return 2; }
        s: Str  { return 3; }
        x: Obj  { return 4; }
        None    { return 0; }
      }
    }
    fn present(o: Obj?) -> bool {
      match o {
        x: Obj { return True; }
        None   { return False; }
      }
    }
    fn main() -> int {
      o: Obj = Node(1);
      y: Obj? = move o;
      z: Obj? = None;
      z = Leaf(2);
      z = "text";
      z = None;
      b: mutable = Box();
      b.any = Node(3);
      b.any = None;
      eq = y == None || z != None || b.any == None || b.w == None;
      n = what(y) + what(z) + what(None) + what(pick(True));
      ok = present(move y);
      xs: Obj?[2] = [None, "s"];
      t: (Obj?, int) = (None, 1);
      return 0;
    }
  )");
  // An `Obj?` is no `Node?`, and the `Obj` arm covers every present value.
  expectError(withNode("y: Obj? = None; n: Node? = move y;"),
              "initializer of type 'Obj?' does not match declared type "
              "'Node?'");
  expectError(std::string(kNodeClass) +
                  "fn f(o: Obj?) -> int { match o { x: Obj { return 1; } } }\n"
                  "fn main() -> int { return 0; }\n",
              "does not always return a value");
  expectError(std::string(kNodeClass) +
                  "fn f(o: Obj?) -> int {\n"
                  "  match o { x: Obj { return 1; } n: Node { return 2; } "
                  "None { return 0; } }\n"
                  "}\n"
                  "fn main() -> int { return 0; }\n",
              "unreachable arm: the 'Obj' arm above already matches every "
              "non-None value");
}

// A conditional of two Nones is the absent value of the optional it flows
// into, as a None is.
TEST(OptionalNone, ConditionalOfNonesTakesTheSlotsType) {
  expectOk(std::string(kNodeClass) + R"(
    class Kid { key: int; parent: weak Kid?; up: weak mutable Kid?; }
    fn take(n: Node?) -> bool { return n == None; }
    fn give(c: bool) -> Node? { return if c then None else None; }
    fn main() -> int {
      c = True;
      // A weak field's parameter of a generated `__init__`, and its store.
      k: mutable = Kid(1, if c then None else None, if c then None else None);
      k.parent = if c then None else None;
      xs: Node?[3] = [if c then None else None; 3];
      v: int? = if c then None else None;
      w: Node? = if c then None else (if c then None else None);
      t: (int, Str)? = if c then None else None;
      u: (int?, Node?) = (if c then None else None, None);
      take(if c then None else None);
      b = v == (if c then None else None);
      e = if c then (if c then None else None) else Node(1);
      match e { n: Node { } None { } }
      return 0;
    }
  )");
}

// The explicit conversion writes the optional its `None` is the absent
// value of; the inferred one has nothing to go by.
TEST(OptionalNone, AnExplicitConversionTypesItsNone) {
  expectOk(withNode(R"(
    c = True;
    a: Str = Str<Node?>(None);
    b: Str = Str<int?>(None);
    d: Str = Str<(int, Str)?>(None);
    e: Str = Str<Node?>(if c then None else None);
  )"));
  expectError(withNode("a = Str<Node>(None);"),
              "argument of 'Str<Node>' has type 'None', expected 'Node'");
  expectError(withNode("a = Str(None);"), "'Str' cannot convert None");
}

// A type parameter is never inferred from a `None` argument; an `Obj` given
// for a `T?` binds `T` to `Obj`.
TEST(OptionalNone, TypeArgumentsAreNotInferredFromNone) {
  const std::string fns = std::string(kNodeClass) +
                          "fn wrap<T>(x: T?) -> T? { return move x; }\n"
                          "fn same<T>(x: T) -> T { return move x; }\n"
                          "fn orElse<T>(a: T?, b: T) -> T { return move b; }\n";
  expectOk(fns + R"(
    fn main() -> int {
      a = wrap<Node>(None);
      b = orElse(None, Node(3));
      c: int? = same<int?>(None);
      o: Obj = Node(1);
      d: Obj? = wrap(move o);
      return 0;
    }
  )");
  for (const char *call : {"wrap(None)", "same(None)", "same((None, 1))"})
    expectError(fns + "fn main() -> int { a = " + call + "; return 0; }\n",
                "from the call arguments: None is the absent value of an "
                "optional, and says nothing of its type");
}

// A template is checked once more with a class of its own for each type
// parameter, and what that check finds is dropped: a `None` it leaves
// untyped (behind `x + x`, which no such class has) is dropped with it.
TEST(OptionalNone, ATemplatesArchetypeCheckReportsNoNone) {
  expectOk(R"(import ::io;
    fn pad<T>(x: T, fill: T?) -> T { return move x; }
    fn twice<T>(x: T) -> T { return pad(x + x, None); }
    fn show<T>(x: T, fill: int?) -> int { return 1; }
    fn calls<T>(x: T) -> int { return show(x.size(), None); }
    class Sized { n: int; view fn size() -> int { return self.n; } }
    impl Sized { fn __init__(n: int) { self.n = n; } }
    fn main() -> int {
      io::println(twice(2));
      io::println(calls(Sized(3)));
      return 0;
    }
  )");
  // In an instance the program uses, an untyped None is still reported.
  expectError(R"(
    fn drop<T>(x: T) { None; }
    fn main() -> int { drop(1); return 0; }
  )",
              "None has no type here");
}

// ─── the existing Obj-subject match is unchanged ────────────────────────────

TEST(Optional, ObjSubjectMatchStillWorks) {
  expectOk(R"(import ::io;
    fn main() -> int {
      o: Obj = "text";
      match o {
        line: Str { io::println(line); }
        _         { io::println("other"); }
      }
      return 0;
    }
  )");
}

TEST(Optional, NoneArmOnObjSubjectIsStillAnError) {
  expectError(R"(
    fn main() -> int {
      o: Obj = "o";
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
    }

    impl Pair {
      fn __init__() { self.b = None; }
    }
    fn main() -> int { return 0; }
  )",
              "field 'a' of class 'Pair' is not assigned on every path");
}

TEST(Optional, OptionalFieldAssignedViaMatch) {
  expectOk(std::string(kNodeClass) + R"(
    import ::memory as mem;
    fn main() -> int {
      a: mutable Node = Node(1);
      b: Node = Node(2);
      a.next = move b;
      a.next = None;
      match mem::take(a.next) {
        n: Node { a.next = move n.next; }
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
      cur: Node? = move head;
      while cur != None {
        match move cur {
          n: Node { total = total + n.v; cur = move n.next; }
          None    { cur = None; }
        }
      }
      return total;
    }
    fn main() -> int {
      a: mutable Node = Node(1);
      b: Node = Node(2);
      a.next = move b;
      return sum(move a);
    }
  )");
}

// ─── arrays of optionals ────────────────────────────────────────────────────

TEST(Optional, ArrayOfOptionals) {
  expectOk(withNode(R"(
    xs: Node?[2] = [None, Node(1)];
    xs[0] = Node(2);
    xs[1] = None;
    e: view Node? = xs[0];
    match xs[0] {
      n: Node { io::println(Str<int>(n.v)); }
      None    { }
    }
  )"));
}

TEST(Optional, ArrayOfOptionalsElementNarrowingIsError) {
  expectError(withNode("xs: Node?[1] = [Node(1)]; n: Node = xs[0];"),
              kUnwrapNode);
}

// ─── ternary ────────────────────────────────────────────────────────────────

TEST(Optional, TernaryWithNoneBranchIsOptional) {
  expectOk(withNode(R"(
    c: bool = True;
    a: Node? = if c then Node(1) else None;
    b: Node? = if c then None else Node(2);
    s: Str? = if c then "x" else None;
    d: Node? = if c then move a else move b;
    l: Leaf? = None;
    e: Node? = if c then move d else move l;
  )"));
}

TEST(Optional, TernaryOptionalResultCannotNarrow) {
  expectError(
      withNode("c: bool = True; a: Node = if c then Node(1) else None;"),
      kUnwrapNode);
}

// ─── rejected optional inner types ──────────────────────────────────────────

// Optionals of enums, tuples and structs (docs/language/12-optionals.md).
TEST(Optional, OptionalEnumTupleAndStructAreAccepted) {
  auto r = semaCheck(R"(
    enum Color { Red, Green }
    struct S { x: int; }
    class P { c: Color?; t: (int, Str)?; s: S?; }
    impl P { fn __init__() {} }
    fn find(ok: bool) -> S? { return if ok then S(1) else None; }
    fn main() -> int {
      c: Color? = None;
      c = Color::Green;
      p: mutable = P();
      p.s = S(2);
      t: (int, Str)? = (1, "a");
      match t { v: (int, Str) { n = v.0; } None { } }
      match c { k: Color { b = k == Color::Red; } None { } }
      match find(True) { s: S { n = s.x; } None { } }
      e = p.s == None || t != None;
      txt = t.toString() + p.s.toString();
      return 0;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
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

namespace {
// This file's own: several files of sema_tests define a SemaFileResult,
// not all alike (ModuleTests.cpp's has a count), and one name shared by
// them would break the one-definition rule.
struct SemaFileResult {
  bool Ok;
  std::string Diagnostics;
};
} // namespace

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
    "import ::memory as mem;\n"
    "class Node {\n"
    "  v: int;\n"
    "  next: Node?;\n"
    "  fn tail() -> Node? { return mem::take(self.next); }\n"
    "  fn link(n: Node?) { self.next = move n; }\n"
    "}\n"
    "impl Node {\n"
    "  fn __init__(x: int) { self.v = x; }\n"
    "}\n"
    "fn find(head: Node?, key: int) -> Node? { return head; }\n"
    "fn first(xs: Str?[2]) -> Str? { return xs[0].clone(); }\n"
    "fn count(xs: view Str?[]) -> int { return xs.len(); }\n";

TEST(OptionalModule, ExportedOptionalSignaturesRoundTrip) {
  auto tmp = (paykan::test::tempDir() / "pkn_opt_mod").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "list.pkn", kListModule);
  auto main = writeFile(tmp, "main.pkn", R"(import ::io;
import list;
fn main() -> int {
  n: mutable list::Node = list::Node(1);
  n.link(None);
  n.link(list::Node(2));
  t: list::Node? = n.tail();
  f: list::Node? = list::find(move n, 2);
  g: list::Node? = list::find(None, 2);
  ss: Str?[2] = [None, "s"];
  c: int = list::count(ss);
  s: Str? = list::first(move ss);
  match t {
    x: list::Node { io::println(Str<int>(x.v)); }
    None { io::println("none"); }
  }
  return 0;
}
)");
  auto r = semaCheckFile(main, tmp);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  std::filesystem::remove_all(tmp);
}

// A `None` no slot types in an instance of another module's template is
// reported in that module's file, under the instantiation that made it
// (#394).  The module itself compiles: its template is checked per instance.
TEST(OptionalModule, AnUntypedNoneInAnotherModulesTemplateIsReportedThere) {
  auto tmp = (paykan::test::tempDir() / "pkn_none_mod").string();
  std::filesystem::remove_all(tmp);
  auto lib = writeFile(tmp, "lib.pkn",
                       "fn idle<T>(x: T) {\n  None;\n}\n"
                       "fn fine<T>(x: T?) -> bool {\n"
                       "  return x == None;\n}\n");
  EXPECT_TRUE(semaCheckFile(lib, tmp).Ok);
  auto ok = writeFile(tmp, "ok.pkn",
                      "import lib;\n"
                      "fn main() -> int { b = lib::fine<int>(None); return 0; "
                      "}\n");
  auto good = semaCheckFile(ok, tmp);
  EXPECT_TRUE(good.Ok) << good.Diagnostics;
  auto main = writeFile(tmp, "main.pkn",
                        "import lib;\n"
                        "fn main() -> int { lib::idle(1); return 0; }\n");
  auto r = semaCheckFile(main, tmp);
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("lib.pkn:2:3: error: None has no type here"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("main.pkn:2:20: note: in instantiation of "
                               "'lib::idle<int>' requested here"),
            std::string::npos)
      << r.Diagnostics;
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
    fn pick(n: Node?, k: int) -> (Node?, int) { return (move n, k); }
    fn main() -> int {
      a: Node? = Node(7);
      t: (Node?, int) = pick(move a, 1);
      u = pick(None, 2);
      same: bool = t == pick(Node(7), 1);
      ts: (Node?, int)[2] = [pick(None, 3), pick(None, 4)];
      x, k = move t;
      y: Node?, j: int = move u;
      isNone: bool = y == None;
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
    class Box { p: (Node?, int);
                view fn set(p: (int?, Str)) -> Str { return p.1.clone(); } }
    impl Box { fn __init__() { self.p = (None, 0); } }
    fn take(p: (int?, Str)) -> Str { return p.1.clone(); }
    fn mk() -> (Node?, int) { return (None, 2); }
    fn main() -> int {
      a: (int?, Str) = (None, "a");
      b: (Node?, int) = (None, 1);
      b = (None, 3);
      take((None, "x"));
      bx: mutable = Box();
      bx.set((None, "y"));
      bx.p = (None, 4);
      ps: (Node?, int)[1] = [(None, 5)];
      ps[0] = (None, 6);
      n: (Str, (Node?, int?)) = ("n", (None, None));
      c: (Obj?, int) = (None, 1);
      return 0;
    }
  )");
}

TEST(OptionalTuple, NoneInTupleLiteralStillNeedsAnOptionalSlot) {
  expectError("fn main() -> int { a: (int, Str) = (None, \"a\"); return 0; }",
              "initializer of type '(None, Str)' does not match declared type "
              "'(int, Str)'");
  expectError(std::string(kNodeClass) +
                  "fn main() -> int { b: (Node?, int) = (None, None); "
                  "return 0; }",
              "initializer of type '(None, None)' does not match declared "
              "type '(Node?, int)'");
  // No `Obj` slot takes one either (#394).
  expectError("fn main() -> int { c: (Obj, int) = (None, 1); return 0; }",
              "initializer of type '(None, int)' does not match declared type "
              "'(Obj, int)'");
  // Without a destination a `None` has no type, and nor has the tuple.
  expectError(std::string(kNodeClass) +
                  "fn main() -> int { t = (None, 1); n: Node? = t.0; "
                  "return 0; }",
              "cannot infer the type of 't' from '(None, int)': a None takes "
              "the type of the optional it flows into, so write the "
              "variable's type");
}

// #71: an array literal with a declared element type takes it when its
// elements disagree but every one fits (`int` and `int?`, `("a", 1)` and
// `("b", n)`, `None` among class instances).
TEST(OptionalTuple, ArrayLiteralElementsTakeTheDeclaredElementType) {
  expectOk(std::string(kNodeClass) + R"(
    fn count(xs: view (Str, int?)[]) -> int { return xs.len(); }
    fn mk(nb: int?) -> (Str, int?)[2] { return [("r", nb), ("s", 1)]; }
    fn main() -> int {
      nb: int? = None;
      n: Node? = None;
      m: Node? = None;
      tbl: (Str, int?)[3] = [("a", 1), ("b", nb), ("c", 3)];
      tbl = [("x", nb), ("y", None), ("z", 2)];
      count([("p", 1), ("q", nb)]);
      tbl[2] = ("w", None);
      ints: int?[3] = [1, nb, None];
      nodes: Node?[3] = [None, move n, Node(1)];
      grid: int?[2][2] = [[1, nb], [None, 2]];
      nested: (Str, (int?, Node?))[2] = [("a", (1, None)), ("b", (nb, move m))];
      return 0;
    }
  )");
}

TEST(OptionalTuple, ArrayLiteralElementThatDoesNotFitIsReported) {
  expectError("fn main() -> int { nb: int? = None; "
              "t: (Str, int?)[3] = [(\"a\", 1), (\"b\", nb), (1, 2)]; "
              "return 0; }",
              "array literal element of type '(int, int)' does not match the "
              "element type '(Str, int?)' of '(Str, int?)[3]'");
  expectError("fn main() -> int { nb: int? = None; "
              "t: (Str, int)[2] = [(\"a\", 1), (\"b\", nb)]; return 0; }",
              "array literal element of type '(Str, int?)' does not match "
              "the element type '(Str, int)' of '(Str, int)[2]'");
  // No destination: the elements must agree on their own.
  expectError("fn main() -> int { nb: int? = None; "
              "x = [(1, \"a\"), (nb, \"b\")]; return 0; }",
              "array literal has inconsistent element types");
  // No promotion inside a tuple literal, contextual or not.
  expectError("fn main() -> int { t: (float, int)[1] = [(1, 2)]; return 0; }",
              "array literal element of type '(int, int)' does not match the "
              "element type '(float, int)' of '(float, int)[1]'");
}

TEST(OptionalTuple, OptionalTupleTypeAccepted) {
  auto r = semaCheck(R"(
    fn find(k: int) -> (int, Str)? { return if k > 0 then (k, "s") else None; }
    fn main() -> int { x: (int, Str)?[1] = [None]; y = find(1); return 0; }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(OptionalTupleModule, TupleAndOptionalSignaturesRoundTrip) {
  // Serialised spellings mix tuple parentheses with `?` and array suffixes:
  // "(Node?, int)[2]", "(Node?, Str?)[]", "(Node?, Str?)[1]", "(Node?, int)".
  auto tmp = (paykan::test::tempDir() / "pkn_opt_tuple_mod").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib.pkn",
            "class Node { v: int; }\n"
            "impl Node { fn __init__(x: int) { self.v = x; } }\n"
            "fn wrap(xs: (Node?, int)[2]) -> (Node?, int)[2] { return xs; }\n"
            "fn count(xs: view (Node?, Str?)[]) -> int { return xs.len(); }\n"
            "fn pairs() -> (Node?, Str?)[1] { return [(None, None)]; }\n"
            "fn mk(n: Node?) -> (Node?, int) { return (move n, 5); }\n");
  auto ok = writeFile(tmp, "main.pkn", R"(
import lib;
fn main() -> int {
  a: (lib::Node?, int)[2] = lib::wrap([(None, 1), (lib::Node(2), 3)]);
  p: (lib::Node?, Str?)[1] = lib::pairs();
  c: int = lib::count(p);
  x, k = lib::mk(None);
  return k + c;
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

// ─── the value of an optional tuple or struct is at most 64 bytes ───────────

static const char *const kEightNine =
    "struct Eight { a: int; b: int; c: int; d: int; e: int; f: int; g: int; "
    "h: int; }\n"
    "struct Nine { a: int; b: int; c: int; d: int; e: int; f: int; g: int; "
    "h: int; i: int; }\n";

static const char *const kNineLimit =
    "an optional holds a value of up to 64 bytes in place, and 'Nine' is 72 "
    "bytes; use 'Box<Nine>?' (memory::Box) instead";

TEST(OptionalLimit, UpTo64BytesInPlace) {
  expectOk(std::string(kEightNine) + R"(
fn pick(x: Eight?) -> int { match x { e: Eight { return e.a; } None { return -1; } } }
fn eight() -> (int, int, int, int, int, int, int, int)? { return None; }
fn main() -> int {
  t: (int, int, int, int, int, int, int, int)? = (1, 2, 3, 4, 5, 6, 7, 8);
  s: (Str, Str, Str, Str, Str, Str, Str, Str)? = None;
  f: ((int) -> int, int, int, int, int, int, int)? = None;
  b: (bool, bool, bool, bool, bool, bool, bool, bool, int, int, int, int, int, int, int)? = None;
  n: Nine = Nine(1, 2, 3, 4, 5, 6, 7, 8, 9);
  big: (int, int, int, int, int, int, int, int, int) = (1, 2, 3, 4, 5, 6, 7, 8, 9);
  return pick(Eight(1, 2, 3, 4, 5, 6, 7, 8)) + n.i + big.8;
}
)");
}

TEST(OptionalLimit, VariableParameterAndResult) {
  expectError(std::string(kEightNine) +
                  "fn main() -> int { t: Nine? = None; return 0; }",
              std::string("variable 't' has type 'Nine?': ") + kNineLimit);
  expectError(std::string(kEightNine) + "fn f(x: Nine?) -> int { return 0; }\n"
                                        "fn main() -> int { return 0; }",
              std::string("parameter 'x' has type 'Nine?': ") + kNineLimit);
  expectError(std::string(kEightNine) + "fn g() -> Nine? { return None; }\n"
                                        "fn main() -> int { return 0; }",
              std::string("function 'g' return type has type 'Nine?': ") +
                  kNineLimit);
  expectError("fn main() -> int { t: (int, int, int, int, int, int, int, int, "
              "int)? = None; return 0; }",
              "variable 't' has type '(int, int, int, int, int, int, int, int, "
              "int)?': an optional holds a value of up to 64 bytes in place, "
              "and '(int, int, int, int, int, int, int, int, int)' is 72 "
              "bytes; use 'Box<(int, int, int, int, int, int, int, int, "
              "int)>?' (memory::Box) instead");
  expectError("fn main() -> int { t: (int, bool, int, int, int, int, int, int, "
              "int)? = None; return 0; }",
              "'(int, bool, int, int, int, int, int, int, int)' is 72 bytes");
  expectError(std::string(kEightNine) +
                  "fn main() -> int { t: (int, Nine?) = (1, None); return 0; }",
              "'Nine' is 72 bytes");
}

TEST(OptionalLimit, FieldsAndSignaturesOfClassesAndStructs) {
  // A class's fields and method signatures are resolved before the structs'
  // sizes are known: they are checked once they are.
  expectError(
      std::string(kEightNine) + "class C { n: Nine?; }\n"
                                "impl C { fn __init__() { self.n = None; } }\n"
                                "fn main() -> int { return 0; }",
      std::string("field 'n' in class 'C' has type 'Nine?': ") + kNineLimit);
  expectError(std::string(kEightNine) + "class C { fn f(x: Nine?) {} }\n"
                                        "fn main() -> int { return 0; }",
              std::string("parameter 'x' of method 'f' in 'C' has type "
                          "'Nine?': ") +
                  kNineLimit);
  expectError(std::string(kEightNine) +
                  "class C { fn g() -> Nine? { return None; } }\n"
                  "fn main() -> int { return 0; }",
              "'Nine' is 72 bytes");
  expectError(std::string(kEightNine) + "struct S { n: Nine?; }\n"
                                        "fn main() -> int { return 0; }",
              std::string("field 'n' in struct 'S' has type 'Nine?': ") +
                  kNineLimit);
  expectError(std::string(kEightNine) +
                  "class Holder<T> { v: T?; }\n"
                  "fn main() -> int { h = Holder<Nine>(None); return 0; }",
              std::string("field 'v' in class 'Holder<Nine>' has type "
                          "'Nine?': ") +
                  kNineLimit);
  // One report per type: a class with the default initializer names the
  // field once.
  auto r =
      semaCheck(std::string(kEightNine) + "class C { n: Nine?; }\n"
                                          "fn main() -> int { return 0; }");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}

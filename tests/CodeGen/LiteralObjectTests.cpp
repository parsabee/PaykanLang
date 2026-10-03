// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// String literals used as objects (#116): a literal in any value position (a
// subscript or method receiver, under `mov`, as a match subject, a
// conversion argument, a tuple / array element, an optional) is a Str object
// built from the literal, never its raw C string, and is released like any
// other string temporary.  Each program runs with every frontend on the
// backend under test, and must print the expected output and leave zero live
// heap blocks.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan;
using namespace paykan::test;

namespace {

void expectRunsOnEveryFrontend(const char *label, const std::string &source,
                               const std::string &expected) {
  for (const std::string &fe : frontend::Registry::get().names()) {
    LeakGuard g;
    auto r = compileAndRunWithFrontend(source, fe);
    ASSERT_TRUE(r.CompileOk) << label << " [" << fe << "]: " << r.StdErr;
    EXPECT_EQ(r.ExitCode, 0) << label << " [" << fe << "]: " << r.StdErr;
    EXPECT_EQ(r.StdOut, expected) << label << " [" << fe << "]";
    g.expectNoLeaks((std::string(label) + " [" + fe + "]").c_str());
  }
}

} // namespace

TEST(LiteralObject, Subscript) {
  expectRunsOnEveryFrontend("Subscript", R"(
    fn main() -> int {
      c = "ab"[1];
      println(Str(c));
      i = 0;
      while (i < 3) { println(Str<char>("xyz"[i])); i = i + 1; }
      return 0;
    }
  )",
                            "b\nx\ny\nz\n");
}

TEST(LiteralObject, SubscriptAsMatchSubjectAndConversionArgument) {
  expectRunsOnEveryFrontend("SubscriptMatch", R"(
    fn main() -> int {
      match "a\nb"[1] {
        '\n' { println("newline"); }
        _ { println("other"); }
      }
      println(Str<char>("xy"[1]));
      match "lit" {
        "lit" { println("lit"); }
        _ { println("no"); }
      }
      println(int<Str>("42"));
      return 0;
    }
  )",
                            "newline\ny\nlit\n42\n");
}

TEST(LiteralObject, MovOfALiteral) {
  expectRunsOnEveryFrontend("Mov", R"(
    fn consume(s: Str) -> int { return s.len(); }
    fn give() -> Str { return mov "ret"; }
    fn main() -> int {
      v = mov "lit";
      println(v);
      println(mov "lit");
      println(Str<int>(consume(mov "lit")));
      s: Str = mov "typed";
      println(s);
      println(mov "lit" + "x");
      println(give());
      return 0;
    }
  )",
                            "lit\nlit\n3\ntyped\nlitx\nret\n");
}

TEST(LiteralObject, MethodReceiver) {
  expectRunsOnEveryFrontend("Receiver", R"(
    fn main() -> int {
      println(Str<int>("abc".len()));
      println("str".toString());
      println(Str<bool>("x" == "x") + Str<bool>("x" != "y"));
      return 0;
    }
  )",
                            "3\nstr\nTrueTrue\n");
}

TEST(LiteralObject, TupleArrayAndOptionalElements) {
  expectRunsOnEveryFrontend("Elements", R"(
    fn first(t: (Str, int)) -> Str { return t.0; }
    fn opt(b: bool) -> Str? {
      if (b) { return "some"; }
      return None;
    }
    fn main() -> int {
      t = ("tup", 1);
      println(t.0);
      println(first(("arg", 2)));
      m: (Str, Str) = (mov "m1", "m2");
      println(m.0 + m.1);
      a: Str[] = ["x", mov "y"];
      a[0] = "z";
      a.push(mov "w");
      println(a[0] + a[1] + a[2]);
      println(["p", "q"][1]);
      o: Str? = "opt";
      println(o);
      o2: Str? = mov "opt2";
      println(o2);
      match opt(True) { v: Str { println(v); } None { println("none"); } }
      match opt(False) { v: Str { println(v); } None { println("none"); } }
      return 0;
    }
  )",
                            "tup\narg\nm1m2\nzyw\nq\nopt\nopt2\nsome\nnone\n");
}

TEST(LiteralObject, ExpressionStatementAndFields) {
  expectRunsOnEveryFrontend("Fields", R"(
    class K {
      name: Str;
      tag: Str?;
      fn __init__() { self.name = "k"; self.tag = mov "t"; }
    }
    fn main() -> int {
      "unused";
      "ab"[0];
      k = K();
      k.name = mov "k2";
      println(k.name);
      println(k.tag);
      return 0;
    }
  )",
                            "k2\nt\n");
}

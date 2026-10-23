// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// String literals used as objects (#116): a literal in any value position (a
// subscript or method receiver, as a match subject, a
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
  expectRunsOnEveryFrontend("Subscript", R"(import ::io;
    fn main() -> int {
      c = "ab"[1];
      io::println(Str(c));
      i = 0;
      while i < 3 { io::println(Str<char>("xyz"[i])); i = i + 1; }
      return 0;
    }
  )",
                            "b\nx\ny\nz\n");
}

TEST(LiteralObject, SubscriptAsMatchSubjectAndConversionArgument) {
  expectRunsOnEveryFrontend("SubscriptMatch", R"(import ::io;
    fn main() -> int {
      match "a\nb"[1] {
        '\n' { io::println("newline"); }
        _ { io::println("other"); }
      }
      io::println(Str<char>("xy"[1]));
      match "lit" {
        "lit" { io::println("lit"); }
        _ { io::println("no"); }
      }
      io::println(int<Str>("42"));
      return 0;
    }
  )",
                            "newline\ny\nlit\n42\n");
}

TEST(LiteralObject, LiteralInEveryValuePosition) {
  expectRunsOnEveryFrontend("Positions", R"(import ::io;
    fn consume(s: Str) -> int { return s.len(); }
    fn give() -> Str { return "ret"; }
    fn main() -> int {
      v = "lit";
      io::println(v);
      io::println("lit");
      io::println(Str<int>(consume("lit")));
      s: Str = "typed";
      io::println(s);
      io::println("lit" + "x");
      io::println(give());
      return 0;
    }
  )",
                            "lit\nlit\n3\ntyped\nlitx\nret\n");
}

TEST(LiteralObject, MethodReceiver) {
  expectRunsOnEveryFrontend("Receiver", R"(import ::io;
    fn main() -> int {
      io::println(Str<int>("abc".len()));
      io::println("str".toString());
      io::println(Str<bool>("x" == "x") + Str<bool>("x" != "y"));
      return 0;
    }
  )",
                            "3\nstr\nTrueTrue\n");
}

TEST(LiteralObject, TupleArrayAndOptionalElements) {
  expectRunsOnEveryFrontend("Elements", R"(import ::io;
    fn first(t: (Str, int)) -> Str { return t.0; }
    fn opt(b: bool) -> Str? {
      if b { return "some"; }
      return None;
    }
    fn main() -> int {
      t = ("tup", 1);
      io::println(t.0);
      io::println(first(("arg", 2)));
      m: (Str, Str) = ("m1", "m2");
      io::println(m.0 + m.1);
      a: Str[3] = ["x", "y", ""];
      a[0] = "z";
      a[2] = "w";
      io::println(a[0] + a[1] + a[2]);
      io::println(["p", "q"][1]);
      o: Str? = "opt";
      io::println(o);
      o2: Str? = "opt2";
      io::println(o2);
      match opt(True) { v: Str { io::println(v); } None { io::println("none"); } }
      match opt(False) { v: Str { io::println(v); } None { io::println("none"); } }
      return 0;
    }
  )",
                            "tup\narg\nm1m2\nzyw\nq\nopt\nopt2\nsome\nnone\n");
}

TEST(LiteralObject, ExpressionStatementAndFields) {
  expectRunsOnEveryFrontend("Fields", R"(import ::io;
    class K {
      name: Str;
      tag: Str?;
    }

    impl K {
      fn __init__() { self.name = "k"; self.tag = "t"; }
    }
    fn main() -> int {
      "unused";
      "ab"[0];
      k = K();
      k.name = "k2";
      io::println(k.name);
      io::println(k.tag);
      return 0;
    }
  )",
                            "k2\nt\n");
}

// Adjacent string literals (#197) are one literal, joined at compile time:
// it prints, compares, matches and is used as an object exactly as the
// single literal it spells.
TEST(LiteralObject, AdjacentPiecesAreOneLiteral) {
  expectRunsOnEveryFrontend("AdjacentPiecesAreOneLiteral", R"(import ::io;
    fn main() -> int {
      io::println("this message is far too long "
                  "to fit on one line");
      s = "ab" "c";
      io::println(Str(s == "abc") + " " + Str("ab" "c".len()) + " "
          + Str("ab" "c"[2]));
      match s {
        "a" "bc" { io::println("matched"); }
        _ { io::println("no"); }
      }
      usage = "usage: tool <file>\n"
              "  -o <file>   output\n";   // a comment between is fine
      io::print(usage);
      return 0;
    }
  )",
                            "this message is far too long to fit on one "
                            "line\nTrue 3 c\nmatched\nusage: tool <file>\n"
                            "  -o <file>   output\n");
}

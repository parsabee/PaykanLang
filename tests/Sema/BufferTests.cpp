// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: the builtin Buffer<T> (docs/language/05-arrays.md, "Buffer").

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

void expectError(const std::string &source, const std::string &message) {
  auto r = semaCheck(source);
  EXPECT_FALSE(r.Ok) << source;
  EXPECT_NE(r.Diagnostics.find(message), std::string::npos) << source << "\n"
                                                            << r.Diagnostics;
}

void expectBodyError(const std::string &body, const std::string &message) {
  expectError("fn main() -> int {\n" + body + "\n  return 0;\n}\n", message);
}

} // namespace

// A buffer of any element type, as a local, a field, a parameter (its
// element type inferred) and a result; `len` and `get` read through a
// `view`.
TEST(Buffer, ElementsFieldsAndInference) {
  auto r = semaCheck(R"(
    class P { x: int; }
    class Stack<T> { items: Buffer<T>; }
    fn count<T>(b: view Buffer<T>) -> int { return b.len(); }
    fn first(b: view Buffer<Str>) -> view Str { return b.get(0); }
    fn make() -> Buffer<P?> { return Buffer<P?>(); }
    fn main() -> int {
      s = Buffer<Str>();
      s.set(0, "a");
      t: Str = s.copy(0);
      s.reserve(5);
      s.grow(1);
      s.shrink(2);
      o: Obj = s;
      os = make();
      os.set(1, None);
      ts = Buffer<(int, Str)>();
      fs = Buffer<float>();
      fs.set(0, 1);
      nested = Buffer<Buffer<int>>();
      st = Stack<bool>(Buffer<bool>());
      return count(s) + count(nested) + first(s).len() + st.items.len();
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Buffer, Errors) {
  expectBodyError("  b = Buffer();", "'Buffer' takes one type argument, its "
                                     "element type: 'Buffer<int>()'");
  expectBodyError("  b = Buffer<int>(3);",
                  "'Buffer<int>()' takes no arguments: it starts with 3 "
                  "slots, and grow(by) adds more");
  expectBodyError("  b: Buffer<int, int> = Buffer<int>();",
                  "variable 'b': 'Buffer' takes one type argument");
  expectBodyError("  b: Buffer<void> = Buffer<int>();",
                  "a 'Buffer<void>' could hold no value");
  expectBodyError("  b = Buffer<int[2]>();",
                  "type argument 1 of 'Buffer': a fixed-size array");
  expectBodyError("  b = Buffer<int>();\n  b.set(0, \"a\");",
                  "argument 2 of 'set' has type 'Str', expected 'int'");
  expectBodyError("  b = Buffer<int>();\n  x: Buffer<float> = b;",
                  "initializer of type 'Buffer<int>' does not match declared "
                  "type 'Buffer<float>'");
  expectBodyError("  b = Buffer<int>();\n  b.push(1);",
                  "no method 'push' on type 'Buffer<int>'");
  expectError("class Buffer<T> { x: T; }\nfn main() -> int { return 0; }",
              "'Buffer' is a builtin class and cannot be redeclared");
  expectError("fn f(b: view Buffer<int>) { b.set(0, 1); }\n"
              "fn main() -> int { return 0; }",
              "'b' is a 'view' parameter; 'set' is not a 'view fn'");
  // At run time a buffer's class does not say its element type.
  expectBodyError("  o: Obj = Buffer<int>();\n"
                  "  match o { b: Buffer<int> { return 1; } }",
                  "match arm type 'Buffer<int>' cannot be tested");
  expectBodyError("  o: Obj? = Buffer<int>();\n"
                  "  match o { b: Buffer<int> { return 1; } None {} }",
                  "match arm type 'Buffer<int>' cannot be tested");
}

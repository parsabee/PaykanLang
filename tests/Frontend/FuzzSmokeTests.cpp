// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Parser fuzz smoke test: random and mutated inputs must never crash, hang or
// leak any enabled frontend (the ASan/LSan CI job checks the leaks).
// Deterministic (fixed seeds) so a failure reproduces.

#include "paykan/Frontend.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef PAYKAN_SAMPLES_DIR
#error "PAYKAN_SAMPLES_DIR must be defined via CMake compile definition"
#endif

namespace {

// xorshift64*: small, fast, deterministic.
class Rng {
  uint64_t S;

public:
  explicit Rng(uint64_t seed) : S(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  uint64_t next() {
    S ^= S >> 12;
    S ^= S << 25;
    S ^= S >> 27;
    return S * 0x2545F4914F6CDD1Dull;
  }
  size_t below(size_t n) { return n ? static_cast<size_t>(next() % n) : 0; }
};

/// Parse once with every enabled frontend, discarding diagnostics.  A
/// frontend may fail, but must report why.
void parseQuietly(const std::string &src) {
  for (const std::string &name : paykan::frontend::Registry::get().names()) {
    auto fe = paykan::frontend::Registry::get().create(name);
    ASSERT_NE(fe, nullptr) << name;
    paykan::ast::ASTContext ctx;
    std::ostringstream os;
    paykan::sema::DiagEngine diag(os);
    auto out = fe->parse("fuzz.pkn", src, ctx, diag, {});
    if (!out.Root) {
      EXPECT_GT(out.ErrorCount, 0u) << name;
    }
  }
}

std::vector<std::string> loadCorpus() {
  std::vector<std::string> corpus;
  std::filesystem::path root(PAYKAN_SAMPLES_DIR);
  if (std::filesystem::is_directory(root)) {
    for (const auto &e : std::filesystem::recursive_directory_iterator(root)) {
      if (!e.is_regular_file() || e.path().extension() != ".pkn")
        continue;
      std::ifstream in(e.path());
      std::stringstream ss;
      ss << in.rdbuf();
      corpus.push_back(ss.str());
    }
  }
  // A few seeds that cover every construct, in case the samples directory
  // is missing (the test still runs, just over less variety).
  corpus.push_back(R"(
    import ::io; import lib::{a as b, c};
    enum Color { Red, Green, Blue, }
    class Box<T> : Base { v: T; fn get() -> T { return self.v; } }
    fn first<T>(xs: T[]) -> T { return xs[0]; }
    fn main() -> int {
      p: (int, Str) = (1, "one"); a, b = p; x: Box<int>? = None;
      match x { Box<int> { } y: Str { } 1 { } "s" { } None { } _ { } }
      if (a < 2 && !b) { a = if a then 1 else 2; } else if (a == 3) { } else { }
      while (True) { break; continue; }
      xs: int[] = [1, 2, 3]; xs[0] = a; p.0.len(); c: Color = Color::Red;
      return first<int>(xs) + Box<Str>("v").get().len();
    }
  )");
  return corpus;
}

const char *const kFragments[] = {
    "fn",    "class", "enum",   "import",
    "match", "mov",   "if",     "then",
    "else",  "while", "return", "break",
    "::",    "->",    "<",      ">",
    "(",     ")",     "{",      "}",
    "[",     "]",     ";",      ",",
    ".",     ".0",    "?",      "=",
    "==",    "&&",    "||",     "!",
    "-",     "x",     "T",      "int",
    "Str",   "1",     "1.5",    "\"s\"",
    "'c'",   "None",  "True",   "_",
    " ",     "\n",    "\"",     "'",
    "//",    "\\",    "@",      "9223372036854775808",
};

std::string mutate(const std::string &base, Rng &rng) {
  std::string s = base;
  int rounds = 1 + static_cast<int>(rng.below(8));
  for (int r = 0; r < rounds; ++r) {
    switch (rng.below(6)) {
    case 0: // delete a span
      if (!s.empty()) {
        size_t at = rng.below(s.size());
        s.erase(at, rng.below(8) + 1);
      }
      break;
    case 1: // insert a grammar fragment
    {
      const char *frag =
          kFragments[rng.below(sizeof(kFragments) / sizeof(kFragments[0]))];
      s.insert(rng.below(s.size() + 1), frag);
      break;
    }
    case 2: // flip a byte
      if (!s.empty())
        s[rng.below(s.size())] = static_cast<char>(rng.next() & 0xFF);
      break;
    case 3: // duplicate a span
      if (!s.empty()) {
        size_t at = rng.below(s.size());
        size_t len = rng.below(32) + 1;
        s.insert(rng.below(s.size() + 1), s.substr(at, len));
      }
      break;
    case 4: // truncate
      s.resize(rng.below(s.size() + 1));
      break;
    default: // swap two bytes
      if (s.size() > 1) {
        size_t a = rng.below(s.size()), b = rng.below(s.size());
        std::swap(s[a], s[b]);
      }
      break;
    }
  }
  return s;
}

} // namespace

TEST(FuzzSmoke, CorpusParses) {
  // Sanity: the seeds are valid (except the error samples, which must still
  // parse without crashing).
  for (const auto &src : loadCorpus())
    parseQuietly(src);
}

TEST(FuzzSmoke, MutatedCorpus) {
  auto corpus = loadCorpus();
  Rng rng(0x5EEDu);
  for (int i = 0; i < 4000; ++i) {
    const std::string &base = corpus[rng.below(corpus.size())];
    parseQuietly(mutate(base, rng));
  }
}

TEST(FuzzSmoke, RandomBytes) {
  Rng rng(42);
  for (int i = 0; i < 1000; ++i) {
    std::string s(rng.below(256), '\0');
    for (auto &c : s)
      c = static_cast<char>(rng.next() & 0xFF);
    parseQuietly(s);
  }
}

TEST(FuzzSmoke, RandomTokens) {
  Rng rng(7);
  for (int i = 0; i < 2000; ++i) {
    std::string s;
    size_t n = rng.below(64);
    for (size_t k = 0; k < n; ++k) {
      s += kFragments[rng.below(sizeof(kFragments) / sizeof(kFragments[0]))];
      s += ' ';
    }
    parseQuietly(s);
  }
}

TEST(FuzzSmoke, PathologicalNesting) {
  for (const char *open : {"(", "[", "{", "if (", "-", "!", "mov ", "Box<"}) {
    std::string s = "fn main() -> int { x = ";
    for (int i = 0; i < 20000; ++i)
      s += open;
    parseQuietly(s);
  }
  std::string types = "fn main() -> int { x: ";
  for (int i = 0; i < 20000; ++i)
    types += "(int, ";
  parseQuietly(types);
  std::string arrays = "fn main() -> int { x: int";
  for (int i = 0; i < 100000; ++i)
    arrays += "[]";
  parseQuietly(arrays + " = []; return 0; }");
}

TEST(FuzzSmoke, LongTokens) {
  std::string s = "fn main() -> int { s = \"";
  s.append(1 << 20, 'a');
  parseQuietly(s); // unterminated, 1 MiB
  parseQuietly(s + "\"; return 0; }");
  std::string ident = "fn main() -> int { ";
  ident.append(1 << 20, 'x');
  parseQuietly(ident + " = 1; return 0; }");
  std::string digits = "fn main() -> int { x = ";
  digits.append(1 << 16, '9');
  parseQuietly(digits + "; y = t." + std::string(1 << 16, '9') +
               "; return 0; }");
}

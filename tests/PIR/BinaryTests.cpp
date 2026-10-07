// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Binary PIR codec and symbol index (docs/design/pkm.md §5, §6.1): the two
// round-trip invariants of §5.8 on hand-written PIR, per-function decoding
// through the index, and hostile input (truncation at every offset, byte
// flips, reserved codes, nesting bombs) that must fail cleanly.

#include "Programs.h"
#include "paykan/pir/Binary.h"
#include "paykan/pir/Codes.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/Verifier.h"
#include "paykan/pir/Version.h"
#include "paykan/plugin_api.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <vector>

using namespace paykan;
using namespace paykan::pir;
using paykan::pir::test::kProgram;

static_assert(PAYKAN_PIR_TEXT_VERSION == kPIRVersion,
              "the text and binary PIR versions are one number");

namespace {

// Every opcode (ftoi is not in kProgram), NaN, -0.0 and inf constants,
// quoted names, empty blocks, an abstract slot, an extern class, both vcall
// and vtable.addr forms, extreme integers and chars.
const char *const kEdgeProgram = R"(module "bin"
cstr @"quoted name" = "a\0b" len 3
data @.d = [-9223372036854775807, 9223372036854775807, 0, -1, 64, -65]
bytes @.b = []
extern obj @$rt.PaykanObject_None
extern vtable @$rt.PaykanArray_vtable

class "ns::Shape" {
  field "w.h": f64
  vtable {
    destroy = @"ns::Shape.destroy" : (obj) -> void
    area = null : (obj) -> f64
  }
}

extern class Other module "lib" {
  field n: i64
}

extern fn @$rt.Paykan_println(obj) -> void
extern fn @"lib::f"(i64) -> i64 module "lib" symbol @f

fn @"ns::Shape.destroy"(%self.1: obj) -> void {
  free %self.1
  ret
}

fn @main() -> i64 {
  local %"quoted local".0: f64
  local %1: box
  %s.1 = new "ns::Shape"
  %f.2 = ftoi 2.5
  %g.3 = ftoi nan
  %h.4 = ftoi -0.0
  %i.5 = ftoi -inf
  %nz.6 = cmp ne -0.0, 0.0
  store %"quoted local".0, 1.5
  %l.7 = load %"quoted local".0
  %a.8 = vcall %s.1 : "ns::Shape" [1] ()
  %b.9 = vcall %s.1 : (obj) -> f64 [1] ()
  %vt.10 = vtable.addr "ns::Shape"
  %vt2.11 = vtable.addr @$rt.PaykanArray_vtable
  field.store %s.1, "ns::Shape"."w.h", %a.8
  %fl.12 = field.load %s.1, "ns::Shape"."w.h"
  if %nz.6 {
  }
  if %nz.6 {
  } else {
  }
  while {
    cond false
  } {
  }
  %sub.13 = sub %f.2, %g.3
  %r.14 = call @"lib::f"(%sub.13)
  %ch.15 = cast '\0' to i64
  %ch2.16 = cast '\xff' to i64
  %nb.17 = select true, null box, null box
  store %1, %nb.17
  %nb2.18 = load %1
  release %nb2.18
  ret %r.14
}
)";

Program parse(const char *text) {
  ParseError err;
  auto p = parseProgram(text, err);
  EXPECT_TRUE(p.has_value()) << err.str();
  return p ? std::move(*p) : Program{};
}

/// The two invariants of §5.8 plus a clean verify of the decoded module.
void expectRoundTrip(const Module &m, const binary::EncodeOptions &opts = {}) {
  std::vector<uint8_t> b = binary::encode(m, opts);
  auto decoded = binary::decode(b);
  ASSERT_TRUE(decoded.isOk()) << decoded.status().message();
  std::vector<VerifyError> errors = verify(*decoded);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors);
  if (!opts.StripNames) {
    EXPECT_EQ(toString(*decoded), toString(m));
  }
  EXPECT_EQ(binary::encode(*decoded, opts), b);
}

/// A module holding only @p f, for printing it (Module is move-only: the
/// If statement owns its blocks).
Module oneFunction(Function f) {
  Module m;
  m.Name = "f";
  m.Functions.push_back(std::move(f));
  return m;
}

uint64_t bits(double d) {
  uint64_t u;
  std::memcpy(&u, &d, sizeof u);
  return u;
}

} // namespace

TEST(PIRBinary, EveryEnumeratorHasACodeAndAName) {
  // The switch is exhaustive (-Wswitch): a new enumerator fails to compile
  // here until the table in Codes.h has its row.
  auto check = [](Opcode op) {
    const OpcodeEntry &e = opcodeEntry(op);
    EXPECT_EQ(e.Id, op);
    EXPECT_STREQ(e.Name, opcodeName(op));
    Opcode back;
    EXPECT_TRUE(opcodeFromCode(e.Code, back));
    EXPECT_EQ(back, op);
    EXPECT_TRUE(opcodeFromName(e.Name, back));
    EXPECT_EQ(back, op);
  };
  for (const OpcodeEntry &e : kOpcodeTable) {
    switch (e.Id) {
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Rem:
    case Opcode::Neg:
    case Opcode::Not:
    case Opcode::Cmp:
    case Opcode::Select:
    case Opcode::IToF:
    case Opcode::FToI:
    case Opcode::Cast:
    case Opcode::Call:
    case Opcode::VCall:
    case Opcode::Retain:
    case Opcode::Release:
    case Opcode::Box:
    case Opcode::Unbox:
    case Opcode::New:
    case Opcode::Free:
    case Opcode::FieldLoad:
    case Opcode::FieldStore:
    case Opcode::VTableLoad:
    case Opcode::VTableAddr:
    case Opcode::Load:
    case Opcode::Store:
      check(e.Id);
    }
  }
  for (const TypeEntry &e : kTypeTable) {
    switch (e.Id) {
    case Type::Void:
    case Type::I64:
    case Type::F64:
    case Type::Bool:
    case Type::Char:
    case Type::Box:
    case Type::Obj:
    case Type::Ptr:
      EXPECT_STREQ(e.Name, typeName(e.Id));
      Type back;
      EXPECT_TRUE(typeFromCode(e.Code, back));
      EXPECT_EQ(back, e.Id);
    }
  }
  // Reserved codes decode to nothing.
  Opcode op;
  Type ty;
  for (unsigned c = 26; c < 256; ++c)
    EXPECT_FALSE(opcodeFromCode(static_cast<uint8_t>(c), op)) << c;
  for (unsigned c = 8; c < 256; ++c)
    EXPECT_FALSE(typeFromCode(static_cast<uint8_t>(c), ty)) << c;
}

TEST(PIRBinary, RoundTrip) {
  for (const Module &m : parse(kProgram).Modules)
    expectRoundTrip(m);
  for (const Module &m : parse(kEdgeProgram).Modules)
    expectRoundTrip(m);
}

TEST(PIRBinary, HeaderAndFlags) {
  Program p = parse(kProgram);
  std::vector<uint8_t> b = binary::encode(p.Modules[0]);
  ASSERT_GE(b.size(), 16u);
  EXPECT_EQ(std::string(b.begin(), b.begin() + 4), "PIRB");
  EXPECT_EQ(std::string(b.end() - 4, b.end()), "BRIP");
  EXPECT_EQ(b[4], binary::kCodecMajor);
  EXPECT_EQ(b[6], binary::kCodecMinor);
  EXPECT_EQ(b[8], kPIRVersion);
  auto info = binary::inspect(b);
  ASSERT_TRUE(info.isOk()) << info.status().message();
  EXPECT_EQ(info.value().ModuleName, "main");
  EXPECT_EQ(info.value().Flags, binary::kFlagDefinesMain);
  EXPECT_EQ(info.value().TrailerOffset + 4, b.size());
  EXPECT_EQ(info.value().Strings[0], "");
  EXPECT_EQ(info.value().Strings[1], "main"); // first use: the module name
  // "lib" defines no main.
  auto lib = binary::inspect(binary::encode(p.Modules[1]));
  ASSERT_TRUE(lib.isOk());
  EXPECT_EQ(lib.value().Flags, 0u);
}

TEST(PIRBinary, F64BitsAndExtremeIntegersSurvive) {
  using L = std::numeric_limits<double>;
  double payloadNaN;
  uint64_t nanBits = 0x7ff80000deadbeefull;
  std::memcpy(&payloadNaN, &nanBits, sizeof payloadNaN);
  const double doubles[] = {payloadNaN,    -L::quiet_NaN(), -0.0,
                            L::infinity(), L::denorm_min(), L::max()};
  const int64_t ints[] = {std::numeric_limits<int64_t>::min(),
                          std::numeric_limits<int64_t>::max(),
                          -1,
                          0,
                          63,
                          64,
                          -64,
                          -65,
                          127,
                          128};
  Module m;
  m.Name = "num";
  m.Datas.push_back(
      {"d", std::vector<int64_t>(std::begin(ints), std::end(ints))});
  Function f;
  f.Name = "f";
  f.Sig.Ret = Type::F64;
  for (double d : doubles) {
    Instr in;
    in.Op = Opcode::Neg;
    in.Result = {f.NextValueId++, Type::F64, ""};
    in.Args.push_back(Operand::f64(d));
    f.Body.Stmts.push_back(std::move(in));
  }
  f.Body.Stmts.push_back(Return{Operand::f64(payloadNaN)});
  m.Functions.push_back(std::move(f));
  expectRoundTrip(m);

  auto back = binary::decode(binary::encode(m));
  ASSERT_TRUE(back.isOk());
  EXPECT_EQ(back.value().Datas[0].Words, m.Datas[0].Words);
  const Function &g = back.value().Functions[0];
  for (size_t i = 0; i < std::size(doubles); ++i) {
    const Instr &in = std::get<Instr>(g.Body.Stmts[i]);
    EXPECT_EQ(bits(std::get<double>(in.Args[0].V)), bits(doubles[i])) << i;
  }
  EXPECT_EQ(
      bits(std::get<double>(std::get<Return>(g.Body.Stmts.back()).Value->V)),
      nanBits);
}

TEST(PIRBinary, StripNames) {
  Program p = parse(kProgram);
  const Module &m = p.Modules[0];
  binary::EncodeOptions strip{true};
  expectRoundTrip(m, strip);
  std::vector<uint8_t> b = binary::encode(m, strip);
  EXPECT_LT(b.size(), binary::encode(m).size());
  auto info = binary::inspect(b);
  ASSERT_TRUE(info.isOk());
  EXPECT_EQ(info.value().Flags & binary::kFlagNamesStripped,
            binary::kFlagNamesStripped);
  auto d = binary::decode(b);
  ASSERT_TRUE(d.isOk());
  for (const Function &f : d.value().Functions) {
    for (const Value &v : f.Params)
      EXPECT_TRUE(v.Name.empty());
    for (const Local &l : f.Locals)
      EXPECT_TRUE(l.Name.empty());
  }
  // Symbol names are not stripped.
  EXPECT_NE(d.value().findFunction("Point_destroy"), nullptr);
}

TEST(PIRBinary, IndexAndDecodeFunction) {
  Program p = parse(kProgram);
  const Module &m = p.Modules[0];
  std::vector<uint8_t> b = binary::encode(m);
  auto info = binary::inspect(b);
  ASSERT_TRUE(info.isOk()) << info.status().message();
  auto whole = binary::decode(b);
  ASSERT_TRUE(whole.isOk());
  std::vector<binary::SymbolEntry> idx = binary::index(b);
  ASSERT_EQ(idx.size(), info.value().Items.size());
  ASSERT_EQ(idx.size(), m.Externs.size() + m.CStrs.size() + m.Datas.size() +
                            m.Bytes.size() + m.Classes.size() +
                            m.Functions.size());
  size_t functions = 0;
  for (size_t i = 0; i < idx.size(); ++i) {
    const binary::SymbolEntry &e = idx[i];
    if (i) {
      EXPECT_TRUE(std::tie(idx[i - 1].Kind, idx[i - 1].Name) <=
                  std::tie(e.Kind, e.Name));
    }
    EXPECT_EQ(e.Linkage, 0);
    EXPECT_EQ(e.Hash, support::sha256(std::span<const uint8_t>(b).subspan(
                          e.Offset, e.Length)));
    using K = binary::SymbolKind;
    if (e.Kind != uint8_t(K::Function) && e.Kind != uint8_t(K::ExternFunction))
      continue;
    ++functions;
    auto f = binary::decodeFunction(b, *info, e.Offset, e.Length);
    ASSERT_TRUE(f.isOk()) << e.Name << ": " << f.status().message();
    EXPECT_EQ(f.value().Name, e.Name);
    EXPECT_EQ(f.value().IsExtern, e.Kind == uint8_t(K::ExternFunction));
    auto same = std::find_if(
        whole.value().Functions.begin(), whole.value().Functions.end(),
        [&](const Function &g) { return g.Name == e.Name; });
    ASSERT_NE(same, whole.value().Functions.end());
    EXPECT_EQ(toString(oneFunction(std::move(*f))),
              toString(oneFunction(std::move(*same))));
  }
  EXPECT_EQ(functions, m.Functions.size());
  // A slice that is not a function record is refused, never misread.
  for (const binary::ItemRef &item : info.value().Items) {
    if (item.Kind == binary::SymbolKind::Class) {
      EXPECT_FALSE(
          binary::decodeFunction(b, *info, item.Offset, item.Length).isOk());
    }
  }
  EXPECT_FALSE(binary::decodeFunction(b, *info, b.size() - 2, 8).isOk());
  EXPECT_FALSE(
      binary::decodeFunction(b, *info, idx[0].Offset, idx[0].Length + 1)
          .isOk());
}

TEST(PIRBinary, SymbolIndexRoundTrip) {
  Program p = parse(kEdgeProgram);
  std::vector<uint8_t> b = binary::encode(p.Modules[0]);
  std::vector<binary::SymbolEntry> idx = binary::index(b);
  ASSERT_FALSE(idx.empty());
  std::vector<uint8_t> s = binary::encodeSymbolIndex(idx);
  EXPECT_EQ(std::string(s.begin(), s.begin() + 4), "PKSY");
  EXPECT_EQ(std::string(s.end() - 4, s.end()), "YSKP");
  auto back = binary::decodeSymbolIndex(s);
  ASSERT_TRUE(back.isOk()) << back.status().message();
  ASSERT_EQ(back.value().size(), idx.size());
  for (size_t i = 0; i < idx.size(); ++i) {
    EXPECT_EQ(back.value().at(i).Name, idx[i].Name);
    EXPECT_EQ(back.value().at(i).Kind, idx[i].Kind);
    EXPECT_EQ(back.value().at(i).Linkage, idx[i].Linkage);
    EXPECT_EQ(back.value().at(i).Offset, idx[i].Offset);
    EXPECT_EQ(back.value().at(i).Length, idx[i].Length);
    EXPECT_EQ(back.value().at(i).Hash, idx[i].Hash);
  }
  EXPECT_EQ(binary::encodeSymbolIndex(*back), s);
  // Unsorted input is sorted on the way in.
  std::vector<binary::SymbolEntry> shuffled(idx.rbegin(), idx.rend());
  EXPECT_EQ(binary::encodeSymbolIndex(shuffled), s);
  // Every truncation fails; a reserved kind fails.
  for (size_t n = 0; n < s.size(); ++n)
    EXPECT_FALSE(binary::decodeSymbolIndex(std::span(s).first(n)).isOk()) << n;
  std::vector<binary::SymbolEntry> bad = idx;
  bad[0].Kind = 10;
  EXPECT_FALSE(
      binary::decodeSymbolIndex(binary::encodeSymbolIndex(bad)).isOk());
  EXPECT_TRUE(binary::index(std::span(b).first(b.size() - 1)).empty());
}

TEST(PIRBinary, Determinism) {
  Program p = parse(kProgram);
  std::vector<uint8_t> once = binary::encode(p.Modules[0]);
  EXPECT_EQ(binary::encode(p.Modules[0]), once);
  std::vector<uint8_t> copy = once;
  EXPECT_EQ(binary::encode(*binary::decode(copy)), once);
  auto decoded = binary::decode(once);
  ASSERT_TRUE(decoded.isOk());
  EXPECT_EQ(binary::encode(*decoded), once);
  // The text parser drops NaN payloads, so encode(parse(print(m))) is the
  // same only when there is no NaN; kProgram has none.
  EXPECT_EQ(binary::encode(parse(toString(p.Modules[0]).c_str()).Modules[0]),
            once);
}

TEST(PIRBinary, RejectsBadHeaders) {
  Program p = parse(kProgram);
  std::vector<uint8_t> good = binary::encode(p.Modules[0]);
  ASSERT_TRUE(binary::decode(good).isOk());
  auto patched = [&](size_t at, uint8_t value) {
    std::vector<uint8_t> b = good;
    b[at] = value;
    return b;
  };
  EXPECT_FALSE(binary::decode(patched(0, 'X')).isOk());   // magic
  EXPECT_FALSE(binary::decode(patched(4, 2)).isOk());     // major
  EXPECT_FALSE(binary::decode(patched(8, 2)).isOk());     // PIR version
  EXPECT_FALSE(binary::decode(patched(13, 0x80)).isOk()); // reserved flag
  EXPECT_FALSE(binary::decode(patched(good.size() - 1, 'X')).isOk()); // trailer
  EXPECT_TRUE(binary::decode(patched(6, 7)).isOk()); // a later minor is fine
  std::vector<uint8_t> tail = good;
  tail.push_back(0);
  EXPECT_FALSE(binary::decode(tail).isOk()); // bytes after the trailer
  EXPECT_FALSE(binary::decode({}).isOk());
  EXPECT_FALSE(binary::inspect(patched(0, 'X')).isOk());
}

TEST(PIRBinary, RejectsReservedCodesAndSlack) {
  // A one-instruction function whose record layout is known: length, name,
  // flags (4), sig (count 0, ret), conventions, params, locals, next id,
  // block count, statement tag, opcode.
  Module m;
  m.Name = "r";
  Function f;
  f.Name = "f";
  f.Sig.Ret = Type::I64;
  Instr in;
  in.Op = Opcode::Neg;
  in.Result = {f.NextValueId++, Type::I64, ""};
  in.Args.push_back(Operand::i64(1));
  f.Body.Stmts.push_back(std::move(in));
  f.Body.Stmts.push_back(Return{Operand::value(1)});
  m.Functions.push_back(std::move(f));
  std::vector<uint8_t> good = binary::encode(m);
  ASSERT_TRUE(binary::decode(good).isOk());
  std::vector<binary::SymbolEntry> idx = binary::index(good);
  ASSERT_EQ(idx.size(), 1u);
  size_t rec = idx[0].Offset;
  size_t retType = rec + 1 + 1 + 4 + 1;
  size_t opcode = retType + 1 + 1 + 1 + 1 + 1 + 1 + 1;
  ASSERT_EQ(good[retType], 1u); // i64
  ASSERT_EQ(good[opcode], 5u);  // neg
  for (uint8_t code : {8, 16, 17, 255}) {
    std::vector<uint8_t> b = good;
    b[retType] = code;
    EXPECT_FALSE(binary::decode(b).isOk()) << "type code " << int(code);
  }
  for (uint8_t code : {26, 30, 32, 34, 255}) {
    std::vector<uint8_t> b = good;
    b[opcode] = code;
    EXPECT_FALSE(binary::decode(b).isOk()) << "opcode " << int(code);
  }
  // A reserved function flag bit.
  {
    std::vector<uint8_t> b = good;
    b[rec + 2] |= 0x08; // flags bit 3: linkage hidden
    EXPECT_FALSE(binary::decode(b).isOk());
  }
  // Slack: one extra byte inside the record, length adjusted.
  {
    std::vector<uint8_t> b = good;
    b.insert(b.begin() + static_cast<long>(rec + idx[0].Length), 0);
    b[rec] += 1;
    auto d = binary::decode(b);
    EXPECT_FALSE(d.isOk());
    EXPECT_NE(d.status().message().find("slack"), std::string::npos)
        << d.status().message();
    auto info = binary::inspect(b);
    ASSERT_TRUE(info.isOk()); // inspect skips bodies
    EXPECT_FALSE(
        binary::decodeFunction(b, *info, rec, idx[0].Length + 1).isOk());
  }
  // A non-minimal LEB (the block count 2 written as 0x82 0x00).
  {
    std::vector<uint8_t> b = good;
    size_t count = opcode - 2;
    ASSERT_EQ(b[count], 2u);
    b[count] = 0x82;
    b.insert(b.begin() + static_cast<long>(count + 1), 0);
    b[rec] += 1;
    EXPECT_FALSE(binary::decode(b).isOk());
  }
}

TEST(PIRBinary, NestingAndStatementLimits) {
  Module m;
  m.Name = "deep";
  Function f;
  f.Name = "f";
  Block *cur = &f.Body;
  for (int i = 0; i < 600; ++i) {
    If s;
    s.Cond = Operand::boolean(true);
    s.Then = std::make_unique<Block>();
    Block *next = s.Then.get();
    cur->Stmts.push_back(std::move(s));
    cur = next;
  }
  cur->Stmts.emplace_back(std::in_place_type<Return>);
  f.Body.Stmts.emplace_back(std::in_place_type<Return>);
  m.Functions.push_back(std::move(f));
  std::vector<uint8_t> b = binary::encode(m);
  EXPECT_FALSE(binary::decode(b).isOk()); // 601 > 512
  binary::DecodeOptions deep;
  deep.MaxNesting = 1000;
  auto d = binary::decode(b, deep);
  ASSERT_TRUE(d.isOk()) << d.status().message();
  EXPECT_EQ(toString(*d), toString(m));
  binary::DecodeOptions few = deep;
  few.MaxStatements = 100;
  EXPECT_FALSE(binary::decode(b, few).isOk());
  binary::DecodeOptions none;
  none.MaxFunctions = 0;
  EXPECT_FALSE(binary::decode(b, none).isOk());
}

namespace {

// xorshift64*, as in tests/Frontend/FuzzSmokeTests.cpp.
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

/// Decoding must either fail or give a module that prints and re-encodes
/// to the same bytes (the encoding is unique) and survives the verifier
/// without crashing.
void mustNotCrash(std::span<const uint8_t> b) {
  auto d = binary::decode(b);
  binary::index(b);
  if (!d.isOk())
    return;
  std::string text = toString(*d);
  std::vector<uint8_t> again = binary::encode(*d);
  auto d2 = binary::decode(again);
  ASSERT_TRUE(d2.isOk()) << d2.status().message();
  EXPECT_EQ(toString(*d2), text);
  verify(*d);
}

} // namespace

TEST(PIRBinary, TruncationAtEveryOffsetFailsCleanly) {
  for (const char *text : {kProgram, kEdgeProgram})
    for (const Module &m : parse(text).Modules) {
      std::vector<uint8_t> b = binary::encode(m);
      for (size_t n = 0; n < b.size(); ++n) {
        std::span<const uint8_t> cut(b.data(), n);
        EXPECT_FALSE(binary::decode(cut).isOk()) << n;
        EXPECT_FALSE(binary::inspect(cut).isOk()) << n;
        EXPECT_TRUE(binary::index(cut).empty()) << n;
      }
    }
}

TEST(PIRBinary, ByteFlipFuzz) {
  Rng rng(20261006);
  for (const char *text : {kProgram, kEdgeProgram})
    for (const Module &m : parse(text).Modules) {
      std::vector<uint8_t> good = binary::encode(m);
      for (int iter = 0; iter < 1500; ++iter) {
        std::vector<uint8_t> b = good;
        size_t flips = 1 + rng.below(4);
        for (size_t i = 0; i < flips; ++i) {
          size_t at = rng.below(b.size());
          switch (rng.below(4)) {
          case 0:
            b[at] ^= static_cast<uint8_t>(1u << rng.below(8));
            break;
          case 1:
            b[at] = static_cast<uint8_t>(rng.next());
            break;
          case 2:
            b[at] = 0xFF; // inflate a count or LEB continuation
            break;
          default:
            b.insert(b.begin() + static_cast<long>(at),
                     static_cast<uint8_t>(rng.next()));
            break;
          }
        }
        mustNotCrash(b);
      }
    }
}

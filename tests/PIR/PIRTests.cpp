// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// PIR text round trip (parser <-> printer) and verifier tests.

#include "Programs.h"
#include "paykan/pir/PIR.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace paykan::pir;
using paykan::pir::test::kProgram;

namespace {

std::string reprint(const std::string &text) {
  ParseError err;
  auto p = parseProgram(text, err);
  EXPECT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return "";
  return toString(*p);
}

} // namespace

TEST(PIR, ProgramRoundTripsThroughPrinterAndParser) {
  std::string once = reprint(kProgram);
  ASSERT_FALSE(once.empty());
  std::string twice = reprint(once);
  EXPECT_EQ(once, twice) << once;
}

TEST(PIR, PrintedProgramIsTheSource) {
  // The sample is written exactly the way the printer prints.
  EXPECT_EQ(reprint(kProgram), kProgram);
}

// `ftoi` (f64 -> i64, toward zero) prints, parses and verifies like `itof`.
TEST(PIR, FToIRoundTripsAndVerifies) {
  const char *src = R"(module "m"

fn @main() -> i64 {
  %f.1 = itof 7
  %h.2 = mul %f.1, 0.5
  %i.3 = ftoi %h.2
  ret %i.3
}
)";
  EXPECT_EQ(reprint(src), src);
  ParseError err;
  auto p = parseProgram(src, err);
  ASSERT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return;
  auto errors = verify(*p);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors);
}

// #86: the lowering names methods `<Class>.<method>`.  Such symbols print
// bare (or quoted, when the class name needs it), parse back to the same
// names, stay distinct from a function spelled `<Class>_<method>`, and do not
// confuse the `Class.field` operand of field.load.
TEST(PIR, MethodSymbolsWithDotsRoundTripAndVerify) {
  const char *src = R"(module "m"

class K {
  field n: i64
  vtable {
    destroy = @K.destroy : (obj) -> void
    w = @K.w : (obj) -> i64
  }
}

class "Pair<Str, int>" {
  vtable {
    destroy = @"Pair<Str, int>.destroy" : (obj) -> void
  }
}

fn @K.w(%self.1: obj) -> i64 {
  %n.2 = field.load %self.1, K.n
  ret %n.2
}

fn @K_w() -> i64 {
  ret 2
}

fn @K.destroy(%self.1: obj) -> void {
  ret
}

fn @K_destroy(%self.1: obj) -> void {
  ret
}

fn @"Pair<Str, int>.destroy"(%self.1: obj) -> void {
  ret
}

fn @main() -> i64 {
  %o.1 = new K
  %a.2 = call @K.w(%o.1)
  %b.3 = call @K_w()
  call @K_destroy(%o.1)
  call @K.destroy(%o.1)
  %s.4 = add %a.2, %b.3
  ret %s.4
}
)";
  EXPECT_EQ(reprint(src), src);
  ParseError err;
  auto p = parseProgram(src, err);
  ASSERT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return;
  auto errors = verify(*p);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors);
  const Module &m = p->Modules[0];
  for (const char *name :
       {"K.w", "K_w", "K.destroy", "K_destroy", "Pair<Str, int>.destroy"})
    EXPECT_NE(m.findFunction(name), nullptr) << name;
  ASSERT_EQ(m.Classes.size(), 2u);
  EXPECT_EQ(m.Classes[0].VTable[1].Target, "K.w");
}

// Every f64 constant prints in a form the parser reads back to the same bits:
// subnormals (strtod flags them with ERANGE), the extremes, -0.0 and the
// infinities.  NaN prints as `nan` and reads back as a NaN (the payload and
// sign are not part of the format).  Out-of-range text is still rejected.
TEST(PIR, F64ConstantsRoundTripExactly) {
  using L = std::numeric_limits<double>;
  const double values[] = {0.0,
                           -0.0,
                           1.0,
                           0.1,
                           -2.5,
                           L::denorm_min(),
                           -L::denorm_min(),
                           1e-310,
                           L::min() / 2,
                           std::nextafter(L::min(), 0.0),
                           L::min(),
                           L::max(),
                           -L::max(),
                           L::epsilon(),
                           L::infinity(),
                           -L::infinity(),
                           L::quiet_NaN()};
  for (double v : values) {
    std::ostringstream op;
    print(Operand::f64(v), op);
    std::string text =
        "module \"m\"\n\nfn @f() -> f64 {\n  ret " + op.str() + "\n}\n";
    ParseError err;
    auto p = parseProgram(text, err);
    ASSERT_TRUE(p.has_value()) << op.str() << ": " << err.str();
    if (!p.has_value())
      continue;
    EXPECT_EQ(toString(*p), text);
    const auto &ret =
        std::get<Return>(p->Modules[0].Functions[0].Body.Stmts.at(0));
    ASSERT_TRUE(ret.Value.has_value());
    double back = std::get<double>(ret.Value->V);
    if (std::isnan(v)) {
      EXPECT_TRUE(std::isnan(back)) << op.str();
      continue;
    }
    uint64_t a, b;
    std::memcpy(&a, &v, sizeof a);
    std::memcpy(&b, &back, sizeof b);
    EXPECT_EQ(a, b) << op.str();
  }
  // Text that is not a finite double (overflow, or a nonzero value that
  // underflows to zero) stays an error.
  for (const char *bad : {"1e999", "-1e999", "1e-400"}) {
    ParseError err;
    std::string text =
        std::string("module \"m\"\n\nfn @f() -> f64 {\n  ret ") + bad + "\n}\n";
    EXPECT_FALSE(parseProgram(text, err)) << bad;
    EXPECT_NE(err.Message.find("float out of range"), std::string::npos)
        << bad << ": " << err.str();
  }
}

TEST(PIR, SampleProgramVerifies) {
  ParseError err;
  auto p = parseProgram(kProgram, err);
  ASSERT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return;
  auto errors = verify(*p);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors);
}

TEST(PIR, ParserKeepsNamesAndIds) {
  ParseError err;
  auto p = parseProgram(kProgram, err);
  ASSERT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return;
  const Function *main = p->Modules[0].findFunction("main");
  ASSERT_NE(main, nullptr);
  ASSERT_EQ(main->Locals.size(), 3u);
  EXPECT_EQ(main->Locals[0].Name, "i");
  EXPECT_EQ(main->Locals[1].Name, "acc");
  EXPECT_EQ(main->Locals[2].Name, "acc");
  const auto *first = std::get_if<Instr>(&main->Body.Stmts[0]);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->Op, Opcode::Call);
  EXPECT_EQ(first->Result.Name, "s");
  EXPECT_EQ(first->Result.Id, 1u);
  EXPECT_EQ(first->Result.Ty, Type::Obj); // from the callee's declaration
  EXPECT_EQ(main->NextValueId, 30u);
}

TEST(PIR, ParserAcceptsHandWrittenNames) {
  // Values and locals without numbers; a bare null; a quoted class name.
  const char *text = R"(module "m"
class "Box<int>" {
  field v: i64
  vtable { destroy = @d : (obj) -> void }
}
fn @d(%self: obj) -> void { ret }
fn @main() -> i64 {
  local %x: box
  store %x, null
  %o = new "Box<int>"
  %v = field.load %o, "Box<int>".v
  %b = box %o
  store %x, %b
  ret %v
}
)";
  ParseError err;
  auto p = parseProgram(text, err);
  ASSERT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return;
  auto errors = verify(*p);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors);
  std::string printed = toString(*p);
  // '<' and '>' need no quoting (docs/pir.md §10).
  EXPECT_NE(printed.find("%o.1 = new Box<int>"), std::string::npos) << printed;
  EXPECT_NE(printed.find("field.load %o.1, Box<int>.v"), std::string::npos)
      << printed;
  EXPECT_NE(printed.find("store %x.0, null box"), std::string::npos) << printed;
  // And the printed form parses back to the same text.
  EXPECT_EQ(reprint(printed), printed);
}

TEST(PIR, ParserReadsWhatThePrinterWritesForLoweredPrograms) {
  // Issue #48: a quoted value name with its `.N` index outside the quotes,
  // a `select` on an extern object singleton (an `obj`), and a runtime
  // extern (no `module` clause) followed by the next module's header.
  const char *text = R"(module "main"
class "Pair<Str, int>" {
  field k: i64
  vtable { destroy = @"Pair<Str, int>_destroy" : (obj) -> void }
}

fn @"Pair<Str, int>_destroy"(%self.1: obj) -> void {
  ret
}

fn @main() -> i64 {
  %obj.1 = new "Pair<Str, int>"
  %"Pair<Str, int>.shared".2 = box %obj.1
  %"x y".3 = unbox %"Pair<Str, int>.shared".2
  %c.4 = cmp eq %"x y".3, null obj
  %o.5 = select %c.4, @$rt.PaykanObject_None, %"x y".3
  call @$rt.Paykan_println(%o.5)
  release %"Pair<Str, int>.shared".2
  %r.6 = call @helper(1)
  ret %r.6
}

extern obj @$rt.PaykanObject_None
extern fn @helper(i64) -> i64 module "lib"
extern fn @$rt.Paykan_println(obj) -> void
module "lib"
fn @helper(%a.1: i64) -> i64 {
  ret %a.1
}
)";
  ParseError err;
  auto p = parseProgram(text, err);
  ASSERT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return;
  ASSERT_EQ(p->Modules.size(), 2u);
  auto errors = verify(*p);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors);
  std::string printed = toString(*p);
  EXPECT_NE(printed.find("%\"Pair<Str, int>.shared\".2 = box %obj.1"),
            std::string::npos)
      << printed;
  EXPECT_EQ(reprint(printed), printed);
}

TEST(PIR, ParseErrorsCarryPositions) {
  ParseError err;
  EXPECT_FALSE(
      parseProgram("module \"m\"\nfn @f() -> i64 {\n  ret 1 2\n}\n", err));
  EXPECT_EQ(err.Line, 3u);
  EXPECT_FALSE(err.Message.empty());

  EXPECT_FALSE(
      parseProgram("module \"m\"\nfn @f() -> i64 {\n  %x = frob 1\n}\n", err));
  EXPECT_EQ(err.Line, 3u);
  EXPECT_NE(err.Message.find("instruction"), std::string::npos) << err.str();

  EXPECT_FALSE(parseProgram("fn @f() -> i64 { ret 1 }", err));
  EXPECT_NE(err.Message.find("module"), std::string::npos) << err.str();

  EXPECT_FALSE(parseProgram("module \"m\"\ncstr @s = \"ab\" len 3\n", err));
  EXPECT_NE(err.Message.find("length"), std::string::npos) << err.str();
}

namespace {

/// Verifies a one-module program and returns the joined error text.
std::string verifyText(const std::string &text) {
  ParseError err;
  auto p = parseProgram(text, err);
  if (!p.has_value())
    return "PARSE ERROR: " + err.str();
  return formatErrors(verify(*p));
}

} // namespace

TEST(PIRVerifier, RejectsUseBeforeDefinitionAndOutsideBlock) {
  std::string errs = verifyText(R"(module "m"
fn @main() -> i64 {
  if true {
    %a = add 1, 2
  }
  %b = add %a, 1
  ret %b
}
)");
  EXPECT_NE(errs.find("outside its block"), std::string::npos) << errs;
}

// A value is visible from its definition to the end of its block, and in the
// blocks nested there: not in a sibling branch or case, not after its if,
// switch or while.  A while's condition values reach its body only.
TEST(PIRVerifier, AValueIsVisibleOnlyInItsBlock) {
  auto errs = [](const std::string &body) {
    return verifyText("module \"m\"\nfn @main() -> i64 {\n" + body + "}\n");
  };
  auto outside = [](const std::string &value) {
    return "m:@main: use of value '%" + value +
           "' before its definition or outside its block\n";
  };
  EXPECT_EQ(errs("  %a = add 1, 2\n"
                 "  if true {\n"
                 "    %b = add %a, 1\n"
                 "    while {\n"
                 "      %c = add %b, 1\n"
                 "      %d = cmp lt %c, %a\n"
                 "      cond %d\n"
                 "    } {\n"
                 "      %e = add %c, %b\n"
                 "    }\n"
                 "  }\n"
                 "  ret %a\n"),
            "");
  EXPECT_EQ(errs("  if true {\n"
                 "    %a = add 1, 2\n"
                 "  } else {\n"
                 "    %b = add %a, 1\n"
                 "  }\n"
                 "  ret 0\n"),
            outside("1"));
  EXPECT_EQ(errs("  switch 1 {\n"
                 "  case 1 {\n"
                 "    %a = add 1, 2\n"
                 "  }\n"
                 "  default {\n"
                 "    %b = add %a, 1\n"
                 "  }\n"
                 "  }\n"
                 "  ret 0\n"),
            outside("1"));
  EXPECT_EQ(errs("  while {\n"
                 "    %a = cmp lt 1, 2\n"
                 "    cond %a\n"
                 "  } {\n"
                 "    %b = add 1, 2\n"
                 "  }\n"
                 "  %c = not %a\n"
                 "  ret 0\n"),
            outside("1"));
  EXPECT_EQ(errs("  while {\n"
                 "    %a = cmp lt 1, 2\n"
                 "    cond %a\n"
                 "  } {\n"
                 "    %b = add 1, 2\n"
                 "  }\n"
                 "  ret %b\n"),
            outside("2"));
  // A value defined again in a nested block is reported, and after the
  // block the first definition is visible again, with its own type.
  EXPECT_EQ(errs("  %a.5 = add 1, 2\n"
                 "  if true {\n"
                 "    %b.5 = cmp eq 1, 2\n"
                 "  }\n"
                 "  ret %a.5\n"),
            "m:@main: value '%5' is defined twice\n");
}

namespace {

/// A function whose body is an elif chain of n arms, written the way the
/// lowering writes one: each arm's test is in the else block of the arm
/// before it, so the blocks nest n deep and every earlier test's value is
/// visible in an arm.
std::string elifChain(size_t n) {
  std::ostringstream os;
  os << "module \"m\"\nfn @main() -> i64 {\n  local %r: i64\n"
     << "  local %x: i64\n  store %x, 7\n";
  for (size_t i = 0; i < n; ++i)
    os << "  %x" << i << " = load %x\n  %c" << i << " = cmp eq %x" << i << ", "
       << i << "\n  if %c" << i << " {\n  %v" << i << " = add %x" << i << ", "
       << i << "\n  store %r, %v" << i << "\n  } else {\n";
  os << "  store %r, -1\n";
  for (size_t i = 0; i < n; ++i)
    os << "  }\n";
  os << "  %result = load %r\n  ret %result\n}\n";
  return os.str();
}

} // namespace

TEST(PIRVerifier, ALongElifChainVerifiesInTimeInProportion) {
  // Closing a block undoes what the block defined, so one chain of 4000 arms
  // takes about as long as sixteen chains of 250, where copying the visible
  // values at every block made it take thirteen times as long (seconds, for
  // 4000 arms).  The time measured is the processor time the verifier
  // spends, which a parallel ctest preempting it does not stretch.
  auto seconds = [](size_t arms, int times) {
    ParseError err;
    auto p = parseProgram(elifChain(arms), err);
    EXPECT_TRUE(p) << err.str();
    if (!p)
      return 0.0;
    double best = 0;
    for (int run = 0; run < 3; ++run) {
      const std::clock_t start = std::clock();
      for (int i = 0; i < times; ++i) {
        const std::vector<VerifyError> errors = verify(*p);
        EXPECT_TRUE(errors.empty()) << formatErrors(errors);
      }
      const double took =
          static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
      best = run == 0 ? took : std::min(best, took);
    }
    return best;
  };
  const double small = seconds(250, 16);
  const double large = seconds(4000, 1);
  EXPECT_LT(large, 4 * small)
      << small << " s for 16 x 250 arms, then " << large << " s for 4000";
  EXPECT_LT(large, 1.0) << large << " s for 4000 arms";
}

TEST(PIRVerifier, RejectsTypeMismatches) {
  std::string errs = verifyText(R"(module "m"
fn @main() -> i64 {
  %a = add 1, 2.0
  %b = cmp eq 1, true
  %c = cast 1.5 to bool
  %d = not 1
  %e = ftoi 3
  ret 1
}
)");
  EXPECT_NE(errs.find("operand of ftoi"), std::string::npos) << errs;
  EXPECT_NE(errs.find("different types"), std::string::npos) << errs;
  EXPECT_NE(errs.find("cmp operands"), std::string::npos) << errs;
  EXPECT_NE(errs.find("cannot cast f64 to bool"), std::string::npos) << errs;
  EXPECT_NE(errs.find("operand of not"), std::string::npos) << errs;
}

// A switch on an i64, a char or a bool, with or without a default, prints
// as it is written and verifies; one whose default and cases all return
// ends a non-void function.
TEST(PIR, SwitchRoundTripsAndVerifies) {
  const char *src = R"(module "m"

fn @main() -> i64 {
  local %r.0: i64
  store %r.0, 0
  switch 'b' {
    case 'a' {
      store %r.0, 1
    }
    case 'b' {
    }
  }
  switch true {
    case false {
      store %r.0, 2
    }
    default {
      store %r.0, 3
    }
  }
  %v.1 = load %r.0
  switch %v.1 {
    case 3 {
      ret 0
    }
    case -9 {
      ret 1
    }
    default {
      ret 2
    }
  }
}
)";
  EXPECT_EQ(reprint(src), src);
  EXPECT_EQ(verifyText(src), "");
}

TEST(PIRVerifier, ChecksSwitches) {
  std::string errs = verifyText(R"(module "m"
fn @main() -> i64 {
  %f.1 = add 1.5, 2.0
  switch %f.1 {
    case 1.5 {
    }
  }
  %n.2 = add 1, 2
  switch %n.2 {
    case %n.2 {
    }
    case true {
    }
    case 4 {
    }
    case 4 {
    }
  }
  switch %n.2 {
    case 1 {
      ret 1
    }
    default {
      ret 2
    }
  }
  ret 3
}
)");
  EXPECT_NE(errs.find("of type f64, expected i64, char or bool"),
            std::string::npos)
      << errs;
  EXPECT_NE(errs.find("is not a constant"), std::string::npos) << errs;
  EXPECT_NE(errs.find("switch case true has type bool, expected i64"),
            std::string::npos)
      << errs;
  EXPECT_NE(errs.find("switch case 4 listed twice"), std::string::npos) << errs;
  EXPECT_NE(errs.find("statement follows a terminator"), std::string::npos)
      << errs;
}

// A non-void function cannot end in a switch without a default, or one
// with a case that falls through.
TEST(PIRVerifier, ASwitchEndsAFunctionOnlyWhenEveryBranchDoes) {
  for (const char *rest : {"    default {\n      ret 2\n    }\n", ""}) {
    std::string src = std::string(R"(module "m"
fn @main() -> i64 {
  switch 1 {
    case 1 {
    }
)") + rest + "  }\n}\n";
    std::string errs = verifyText(src);
    EXPECT_NE(errs.find("fall"), std::string::npos) << src << "\n" << errs;
  }
}

// Scalar and box slots have addresses (an `inout` parameter is the address
// of the caller's slot), and an address is read and written as one of those.
TEST(PIRVerifier, ChecksAddressOps) {
  std::string errs = verifyText(R"(module "m"
class C {
  field n: i64
  field b: box
  vtable {
    destroy = @C.destroy : (obj) -> void
  }
}
fn @C.destroy(%self: obj) -> void {
  ret
}
fn @main() -> i64 {
  local %i: i64
  local %o: box
  local %d: ptr
  %o2 = new C
  %p = local.addr %o
  %q = local.addr %i
  %a = local.addr %d
  %r = field.addr %o2, C.b
  %s = field.addr %o2, C.m
  %t = field.addr %q, C.n
  %u = field.addr %o2, C.n
  %v = ptr.load obj, %q
  %w = ptr.load i64, %o2
  ptr.store 1, 2
  ptr.store %q, %o2
  %x = ptr.load f64, %u
  ptr.store %u, %x
  %y = ptr.load box, %p
  ptr.store %r, null box
  ret 0
}
)");
  for (const char *e :
       {"local.addr of a local of type ptr", "class 'C' has no field 'm'",
        "receiver of field.addr %3 has type ptr", "ptr.load of obj",
        "address of ptr.load %1 has type obj", "address of ptr.store 1",
        "ptr.store of obj"})
    EXPECT_NE(errs.find(e), std::string::npos) << e << "\n" << errs;
  // Box slots have addresses.
  EXPECT_EQ(errs.find("of type box"), std::string::npos) << errs;
  EXPECT_EQ(errs.find("ptr.load of box"), std::string::npos) << errs;
  EXPECT_EQ(errs.find("ptr.store of box"), std::string::npos) << errs;
  // The verifier does not know what an address points to: a well-typed
  // ptr.load / ptr.store through it is accepted.
  EXPECT_EQ(errs.find("ptr.load of f64"), std::string::npos) << errs;
  EXPECT_EQ(errs.find("ptr.store of f64"), std::string::npos) << errs;

  ParseError err;
  EXPECT_FALSE(parseProgram("module \"m\"\nfn @f(%p: ptr) -> void {\n"
                            "  ptr.load i64, %p\n  ret\n}\n",
                            err));
  EXPECT_NE(err.Message.find("no '%result ='"), std::string::npos) << err.str();
}

// A `mem` local or field holds an aggregate's bytes: `mem(<size>, <align>)`,
// reached only through its address.
TEST(PIR, MemSlotsRoundTripAndVerify) {
  const char *src = R"(module "m"
class C {
  field tag: char
  field buf: mem(3, 1)
  field words: mem(16, 8)
  vtable {
    destroy = @C.destroy : (obj) -> void
  }
}
fn @C.destroy(%self: obj) -> void {
  ret
}
fn @main() -> i64 {
  local %m: mem(24, 8)
  %p = local.addr %m
  mem.zero %p, 24
  %q = ptr.add %p, 8
  mem.copy %q, %p, 16
  %o = new C
  %f = field.addr %o, C.words
  mem.copy %f, %p, 16
  free %o
  ret 0
}
)";
  std::string once = reprint(src);
  ASSERT_FALSE(once.empty());
  EXPECT_EQ(reprint(once), once);
  for (const char *e : {"field buf: mem(3, 1)", "local %m.0: mem(24, 8)",
                        "= ptr.add %p.", "mem.copy %q.", "mem.zero %p."})
    EXPECT_NE(once.find(e), std::string::npos) << e << "\n" << once;
  ParseError err;
  auto p = parseProgram(src, err);
  ASSERT_TRUE(p) << err.str();
  EXPECT_TRUE(verify(*p).empty()) << formatErrors(verify(*p));

  // `mem` is no value's type, so not a parameter's.
  EXPECT_FALSE(
      parseProgram("module \"m\"\nfn @f(%p: mem) -> void {\n  ret\n}\n", err));
  EXPECT_NE(err.Message.find("'mem' is only the type of a local or a field"),
            std::string::npos)
      << err.str();
}

TEST(PIRVerifier, ChecksMemSlots) {
  std::string errs = verifyText(R"(module "m"
class C {
  field ok: mem(16, 8)
  field bad: mem(3, 2)
  vtable {
    destroy = @C.destroy : (obj) -> void
  }
}
fn @C.destroy(%self: obj) -> void {
  ret
}
fn @main() -> i64 {
  local %m: mem(24, 8)
  local %z: mem(0, 1)
  local %a: mem(8, 3)
  %v = load %m
  store %m, 1
  %o = new C
  %f = field.load %o, C.ok
  %p = local.addr %m
  %q = ptr.add %p, 1.5
  %r = ptr.add %o, 1
  mem.copy %p, %o, 8
  mem.zero %p, %p
  ret 0
}
)");
  for (const char *e :
       {"field 'C.bad': a mem slot's size must be a multiple of its alignment",
        "local '%z': a mem slot's size must be positive",
        "local '%a': a mem slot's alignment must be 1, 2, 4 or 8",
        "load of a mem local", "store to a mem local",
        "field.load of mem field 'ok'",
        "offset of ptr.add 1.5 has type f64, expected i64",
        "address of ptr.add %", "source of mem.copy %",
        "byte count of mem.zero %"})
    EXPECT_NE(errs.find(e), std::string::npos) << e << "\n" << errs;
  // A mem local and field have addresses.
  EXPECT_EQ(errs.find("local.addr of"), std::string::npos) << errs;
}

// Inline optionals: `some` takes a payload type, `is_some` / `unwrap` an
// optional, and an optional is neither compared, cast, computed with nor
// passed to a runtime or native function.
TEST(PIR, WeakReferencesRoundTripAndVerify) {
  const char *src = R"(module "m"
class Node {
  field next: box
  field parent: weak
  vtable {
    destroy = @Node.destroy : (obj) -> void
  }
}
fn @Node.destroy(%self: obj) -> void {
  %n = field.load %self, Node.next
  release %n
  %p = field.load %self, Node.parent
  weak.release %p
  ret
}
fn @main() -> i64 {
  local %w: weak
  %o = new Node
  %b = box %o
  %wk = weak.new %b
  store %w, %wk
  %w2 = load %w
  %l = weak.load %w2
  release %l
  field.store %o, Node.parent, %wk
  release %b
  ret 0
}
)";
  EXPECT_EQ(verifyText(src), "");
  ParseError err;
  auto p = parseProgram(src, err);
  ASSERT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return;
  std::string printed = toString(*p);
  for (const char *piece :
       {"field parent: weak", "local %w.0: weak", " = weak.new %b.",
        " = weak.load %w2.", "weak.release %p."})
    EXPECT_NE(printed.find(piece), std::string::npos) << piece << "\n"
                                                      << printed;
  auto again = parseProgram(printed, err);
  ASSERT_TRUE(again.has_value()) << err.str();
  EXPECT_EQ(toString(*again), printed);
}

TEST(PIRVerifier, ChecksWeakReferences) {
  std::string errs = verifyText(R"(module "m"
fn @main() -> i64 {
  %a = weak.new 1
  %b = weak.load null box
  %c = weak.new null box
  weak.release %c
  weak.release null box
  %d = cast %c to i64
  %e = cmp eq %c, %c
  ret 0
}
)");
  for (const char *e : {"operand of weak.new 1 has type i64, expected box",
                        "operand of weak.load null box has type box, expected "
                        "weak",
                        "operand of weak.release null box has type box, "
                        "expected weak",
                        "cannot cast weak to i64"})
    EXPECT_NE(errs.find(e), std::string::npos) << e << "\n" << errs;
  EXPECT_EQ(errs.find("%c = weak.new"), std::string::npos) << errs;
}

// `stable` after a `field.load` or a `ptr.load` of a box: the slot keeps
// its reference while the value is used (docs/pir.md §6).  It prints,
// parses and verifies; without it the load is borrowed as before.
TEST(PIR, StableLoadsRoundTripAndVerify) {
  const char *src = R"(module "m"
class Node {
  field next: box
  field n: i64
  vtable {
    destroy = @Node.destroy : (obj) -> void
  }
}
fn @Node.destroy(%self: obj) -> void {
  %n = field.load %self, Node.next
  release %n
  free %self
  ret
}
fn @Node.peek(%self: obj, %p: ptr) -> void {
  %a = field.load %self, Node.next stable
  %b = ptr.load box, %p stable
  %c = field.load %self, Node.next
  %d = ptr.load box, %p
  ret
}
fn @main() -> i64 {
  ret 0
}
)";
  EXPECT_EQ(verifyText(src), "");
  ParseError err;
  auto p = parseProgram(src, err);
  ASSERT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return;
  std::string printed = toString(*p);
  for (const char *piece :
       {" = field.load %self.1, Node.next stable\n",
        " = ptr.load box, %p.2 stable\n", " = field.load %self.1, Node.next\n",
        " = ptr.load box, %p.2\n"})
    EXPECT_NE(printed.find(piece), std::string::npos) << piece << "\n"
                                                      << printed;
  auto again = parseProgram(printed, err);
  ASSERT_TRUE(again.has_value()) << err.str();
  EXPECT_EQ(toString(*again), printed);
}

TEST(PIRVerifier, OnlyABoxLoadIsStable) {
  std::string errs = verifyText(R"(module "m"
class Node {
  field n: i64
  vtable {
    destroy = @Node.destroy : (obj) -> void
  }
}
fn @Node.destroy(%self: obj) -> void {
  free %self
  ret
}
fn @Node.peek(%self: obj, %p: ptr) -> void {
  %a = field.load %self, Node.n stable
  %b = ptr.load i64, %p stable
  ret
}
fn @main() -> i64 {
  ret 0
}
)");
  for (const char *e :
       {"a stable 'field.load' (only a field.load or a ptr.load of a box is "
        "stable)",
        "a stable 'ptr.load' (only a field.load or a ptr.load of a box is "
        "stable)"})
    EXPECT_NE(errs.find(e), std::string::npos) << e << "\n" << errs;
  // Nor is any other instruction, which the text cannot mark.
  ParseError err;
  auto p = parseProgram(R"(module "m"
fn @main() -> i64 {
  %a = add 1, 2
  ret %a
}
)",
                        err);
  ASSERT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return;
  std::get<Instr>(p->Modules[0].Functions[0].Body.Stmts[0]).Stable = true;
  EXPECT_NE(formatErrors(verify(*p)).find("a stable 'add'"), std::string::npos)
      << formatErrors(verify(*p));
}

// A call's arguments may be `lent` or `owned(%o)`, `owned %p` reads the
// ownership mask, and a retain may be `retain lend` (docs/pir.md §3, §6):
// each parses into its own field, and kProgram prints them back.
TEST(PIR, ArgumentOwnershipParses) {
  ParseError err;
  auto p = parseProgram(kProgram, err);
  ASSERT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return;
  const Function *f = p->Modules[0].findFunction("lend");
  ASSERT_NE(f, nullptr);
  const auto &body = f->Body.Stmts;
  const auto &oa = std::get<Instr>(body[0]);
  EXPECT_EQ(oa.Op, Opcode::Owned);
  EXPECT_EQ(oa.Result.Ty, Type::Bool);
  const auto &retain = std::get<Instr>(body[2]);
  EXPECT_EQ(retain.Op, Opcode::Retain);
  EXPECT_TRUE(retain.Lend);
  const auto &call = std::get<Instr>(body[3]);
  ASSERT_EQ(call.Own.size(), call.Args.size());
  EXPECT_EQ(call.ownership(0), ArgOwnership::Lent);
  EXPECT_EQ(call.ownership(1), ArgOwnership::Passed);
  EXPECT_EQ(call.Own[1].Bit.valueId(), 6u);
  EXPECT_EQ(call.ownership(2), ArgOwnership::Owned);
  EXPECT_EQ(call.ownership(3), ArgOwnership::Owned);
  const auto &vcall = std::get<Instr>(body[4]);
  EXPECT_EQ(vcall.ownership(0), ArgOwnership::Owned); // the receiver
  EXPECT_EQ(vcall.ownership(1), ArgOwnership::Lent);
  const auto &icall = std::get<Instr>(body[5]);
  EXPECT_EQ(icall.ownership(0), ArgOwnership::Owned); // the callee
  EXPECT_EQ(icall.ownership(1), ArgOwnership::Passed);
  EXPECT_EQ(icall.ownership(2), ArgOwnership::Owned);
  // A call with no mark has none.
  for (const Stmt &st : p->Modules[0].findFunction("main")->Body.Stmts) {
    if (const auto *in = std::get_if<Instr>(&st)) {
      EXPECT_TRUE(in->Own.empty() && !in->Lend);
    }
  }
  EXPECT_FALSE(parseProgram(R"(module "m"
fn @f(%a: box) -> void {
  call @f(owned(true %a)
  ret
}
)",
                            err));
}

TEST(PIRVerifier, ChecksArgumentOwnership) {
  std::string errs = verifyText(R"(module "m"
extern fn @$rt.Paykan_retain(box) -> void
fn @g(%n: i64, %b: box, %fp: ptr) -> void {
  %n2 = owned %n
  %k = add %n, 1
  %k2 = owned %k
  %ob = owned %b
  %x = add %n, 2
  call @g(lent %n, owned(%x) %b, %fp)
  call @g(%n, owned(true) %b, %fp)
  call @$rt.Paykan_retain(lent %b)
  icall %fp : (i64) -> void (lent 3)
  ret
}
fn @main() -> i64 {
  ret 0
}
)");
  for (const char *e :
       {"'owned' of %1, which is no box parameter of the function",
        "'owned' of %5, which is no box parameter of the function",
        "argument 0 of '@g' is lent or passed, but is no box parameter",
        "passed bit of argument 1 of '@g' %8 has type i64, expected bool",
        "the passed bit of argument 1 of '@g' is not a value",
        "argument 0 of icall is lent or passed, but icall does not lend"})
    EXPECT_NE(errs.find(e), std::string::npos) << e << "\n" << errs;
  EXPECT_NE(errs.find("argument 0 of '@$rt.Paykan_retain' is lent or passed, "
                      "but '@$rt.Paykan_retain' does not lend"),
            std::string::npos)
      << errs;
  EXPECT_EQ(errs.find("of %2,"), std::string::npos) << errs; // %ob is good

  // The ownership mask has 64 bits: the 65th box parameter has none.
  std::string params, args;
  for (int i = 0; i < 65; ++i) {
    params += (i ? ", %b" : "%b") + std::to_string(i) + ": box";
    args += (i ? ", %b" : "%b") + std::to_string(i);
  }
  errs = verifyText("module \"m\"\nfn @wide(" + params +
                    ") -> void {\n  %o63 = owned %b63\n  %o64 = owned %b64\n"
                    "  call @wide(lent " +
                    args.substr(0, args.rfind(", %b64")) + ", lent %b64)\n" +
                    "  ret\n}\nfn @main() -> i64 {\n  ret 0\n}\n");
  for (const char *e :
       {"'owned' of box parameter 64, which the ownership mask has no bit "
        "for (it is always owned)",
        "argument 64 of '@wide' is lent or passed, but is box parameter 64, "
        "which the ownership mask has no bit for"})
    EXPECT_NE(errs.find(e), std::string::npos) << e << "\n" << errs;
  EXPECT_EQ(errs.find("argument 0 of"), std::string::npos) << errs;
  EXPECT_EQ(errs.find("box parameter 63"), std::string::npos) << errs;

  // Only a call's arguments have an ownership, as many as it has
  // arguments, and only a retain lends: what the text cannot write.
  ParseError err;
  auto p = parseProgram(R"(module "m"
fn @f(%a: box) -> void {
  %x = add 1, 2
  release %a
  call @f(%a)
  ret
}
fn @main() -> i64 {
  ret 0
}
)",
                        err);
  ASSERT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return;
  auto &body = p->Modules[0].Functions[0].Body.Stmts;
  std::get<Instr>(body[0]).Own.resize(2);
  std::get<Instr>(body[1]).Lend = true;
  std::get<Instr>(body[2]).Own.resize(2);
  errs = formatErrors(verify(*p));
  for (const char *e :
       {"'add' gives its operands an ownership (only a call's arguments have "
        "one)",
        "a lend 'release' (only a retain lends)",
        "call to '@f' gives the ownership of 2 argument(s), but passes 1"})
    EXPECT_NE(errs.find(e), std::string::npos) << e << "\n" << errs;
}

// A vtable slot's target is the same machine function: a lending slot type
// takes the ownership mask, which a runtime target spells out.
TEST(PIRVerifier, AVTableTargetHasTheSlotsMachineSignature) {
  auto program = [](const char *equalsDecl) {
    return std::string(R"(module "m"
class C {
  vtable {
    destroy = @C.destroy : (obj) -> void
    equals = @$rt.PaykanObject_equals : (obj, box) -> i64
    same = @C.same : (obj, box) -> i64
  }
}
)") + equalsDecl +
           R"(
fn @C.destroy(%self: obj) -> void {
  free %self
  ret
}
fn @C.same(%self: obj, %o: box) -> i64 {
  ret 0
}
fn @main() -> i64 {
  ret 0
}
)";
  };
  EXPECT_EQ(
      verifyText(program("extern fn @$rt.PaykanObject_equals(obj, box, i64) "
                         "-> i64")),
      "");
  std::string errs = verifyText(
      program("extern fn @$rt.PaykanObject_equals(obj, box) -> i64"));
  EXPECT_NE(errs.find("vtable slot 1 ('equals') has signature (obj, box) -> "
                      "i64 (a machine function (obj, box, i64) -> i64) but "
                      "'@$rt.PaykanObject_equals' is declared (obj, box) -> "
                      "i64 (a machine function (obj, box) -> i64)"),
            std::string::npos)
      << errs;
  EXPECT_EQ(errs.find("'same'"), std::string::npos) << errs;
}

TEST(PIRVerifier, ChecksOptionals) {
  std::string errs = verifyText(R"(module "m"
extern fn @$rt.Paykan_take(opt_i64) -> void
extern fn @$c.native() -> opt_f64
fn @main() -> i64 {
  %a = some 1
  %b = none opt_i64
  %c = some null box
  %d = is_some 1
  %e = unwrap 2.5
  %f = cmp eq %a, %b
  %g = cast %a to i64
  %h = add %a, %b
  %i = is_some %a
  %j = unwrap %b
  %k = add %j, 1
  ret %k
}
)");
  for (const char *e :
       {"some of box", "operand of is_some has type i64",
        "operand of unwrap has type f64", "cmp on opt_i64",
        "cannot cast opt_i64 to i64", "arithmetic on opt_i64",
        "extern function '@$rt.Paykan_take' takes or returns an optional",
        "extern function '@$c.native' takes or returns an optional"})
    EXPECT_NE(errs.find(e), std::string::npos) << e << "\n" << errs;
  // is_some and unwrap of a well-typed optional are fine: the unwrapped
  // payload is an i64.
  EXPECT_EQ(errs.find("%k"), std::string::npos) << errs;
  EXPECT_EQ(errs.find("returned value"), std::string::npos) << errs;

  ParseError err;
  EXPECT_FALSE(parseProgram("module \"m\"\nfn @f() -> void {\n"
                            "  %n = none i64\n  ret\n}\n",
                            err));
  EXPECT_NE(err.Message.find("not an optional"), std::string::npos)
      << err.str();
}

TEST(PIRVerifier, RejectsBadControlFlow) {
  std::string errs = verifyText(R"(module "m"
fn @f() -> i64 {
  break
  ret 1
  ret 2
}
fn @g() -> i64 {
  if true {
    ret 1
  }
}
fn @main() -> i64 {
  ret 0
}
)");
  EXPECT_NE(errs.find("outside a while"), std::string::npos) << errs;
  EXPECT_NE(errs.find("follows a terminator"), std::string::npos) << errs;
  EXPECT_NE(errs.find("fall off the end"), std::string::npos) << errs;
}

TEST(PIRVerifier, RejectsBadCallsAndClasses) {
  std::string errs = verifyText(R"(module "m"
extern fn @k(i64) -> void
class C {
  field f: i64
  vtable {
    destroy = @k : (obj) -> void
  }
}
fn @main(%args.1: ptr, %args.len.2: i64) -> i64 {
  call @nope(1)
  call @k(1, 2)
  call @k(true)
  %o.3 = new D
  %v.4 = field.load %o.3, C.g
  %w.5 = vcall %o.3 : C [5] ()
  ret 0
}
fn @main2() -> void {
  ret
}
fn @main3() -> i64 {
  local %x.0: i64
  store %x.0, true
  ret 0
}
fn @init(%self.1: obj) -> void {
  vtable.store %self.1, E
  vtable.store 1, C
  ret
}
)");
  EXPECT_NE(errs.find("undeclared function '@nope'"), std::string::npos)
      << errs;
  EXPECT_NE(errs.find("passes 2 argument(s), expected 1"), std::string::npos)
      << errs;
  EXPECT_NE(errs.find("argument 0 of '@k'"), std::string::npos) << errs;
  EXPECT_NE(errs.find("unknown class 'D'"), std::string::npos) << errs;
  EXPECT_NE(errs.find("has no field 'g'"), std::string::npos) << errs;
  EXPECT_NE(errs.find("out of range"), std::string::npos) << errs;
  EXPECT_NE(errs.find("vtable slot 0 ('destroy') has signature"),
            std::string::npos)
      << errs;
  EXPECT_NE(errs.find("stored value"), std::string::npos) << errs;
  EXPECT_NE(errs.find("vtable.store of unknown class 'E'"), std::string::npos)
      << errs;
  EXPECT_NE(errs.find("operand of vtable.store"), std::string::npos) << errs;
}

TEST(PIRVerifier, ChecksMainAndDuplicates) {
  std::string errs = verifyText(R"(module "m"
cstr @a = "x" len 1
fn @a() -> void {
  ret
}
fn @main() -> void {
  ret
}
)");
  EXPECT_NE(errs.find("'@a' is defined twice"), std::string::npos) << errs;
  EXPECT_NE(errs.find("'@main' must have signature"), std::string::npos)
      << errs;
}

TEST(PIRVerifier, ChecksCrossModuleReferences) {
  std::string errs = verifyText(R"(module "main"
extern fn @helper(i64) -> i64 module "lib"
extern fn @missing() -> void module "lib"
extern fn @elsewhere() -> void module "nowhere"
fn @main() -> i64 {
  %r.1 = call @helper(1)
  ret %r.1
}
module "lib"
fn @helper(%a.1: i64) -> void {
  ret
}
)");
  EXPECT_NE(errs.find("'@helper' is declared (i64) -> i64 but module 'lib' "
                      "defines it (i64) -> void"),
            std::string::npos)
      << errs;
  EXPECT_NE(errs.find("'@missing' is not defined in module 'lib'"),
            std::string::npos)
      << errs;
  EXPECT_NE(errs.find("module 'nowhere', which is not in the program"),
            std::string::npos)
      << errs;
}

// #70: an extern's `symbol` is its name in the defining module, so the
// importer can declare two modules' `@tag` (and define its own) side by side.
TEST(PIRVerifier, ExternSymbolLinksSameNamedFunctionsOfTwoModules) {
  const char *text = R"(module "main"

extern fn @"x::tag"() -> i64 module "x" symbol @tag
extern fn @"y::tag"() -> i64 module "y" symbol @tag

fn @tag() -> i64 {
  ret 1
}

fn @main() -> i64 {
  %a.1 = call @"x::tag"()
  %b.2 = call @"y::tag"()
  %c.3 = call @tag()
  %s.4 = add %a.1, %b.2
  %t.5 = add %s.4, %c.3
  ret %t.5
}

module "x"

fn @tag() -> i64 {
  ret 2
}

module "y"

fn @tag() -> i64 {
  ret 3
}
)";
  ParseError err;
  auto parsed = parseProgram(text, err);
  ASSERT_TRUE(parsed.has_value()) << err.str();
  const Program prog = std::move(parsed).value_or(Program{});
  EXPECT_EQ(formatErrors(verify(prog)), "");
  const Function *fx = prog.Modules[0].findFunction("x::tag");
  const Function *own = prog.Modules[0].findFunction("tag");
  ASSERT_NE(fx, nullptr);
  ASSERT_NE(own, nullptr);
  EXPECT_EQ(fx->Symbol, "tag");
  EXPECT_EQ(fx->linkName(), "tag");
  EXPECT_EQ(own->linkName(), "tag");
  EXPECT_EQ(toString(prog), text);
}

TEST(PIRVerifier, ChecksExternSymbols) {
  std::string errs = verifyText(R"(module "main"
extern fn @"x::f"() -> i64 module "x" symbol @nope
extern fn @$rt.Paykan_println(obj) -> void symbol @puts
fn @main() -> i64 {
  ret 0
}
module "x"
fn @f() -> i64 {
  ret 1
}
)");
  EXPECT_NE(errs.find("'@nope' is not defined in module 'x'"),
            std::string::npos)
      << errs;
  EXPECT_NE(errs.find("runtime extern function '@$rt.Paykan_println' has a "
                      "'symbol'"),
            std::string::npos)
      << errs;
}

// #117: runtime externs, and only they, are named `@$rt.<C symbol>`; a
// program function may be spelled like a runtime symbol.
TEST(PIRVerifier, RuntimeExternsHaveRuntimeNames) {
  std::string errs = verifyText(R"(module "main"
extern fn @Paykan_println(obj) -> void
extern obj @PaykanObject_None
cstr @$rt.s = "x" len 1
fn @$rt.f() -> i64 {
  ret 1
}
fn @main() -> i64 {
  ret 0
}
)");
  EXPECT_NE(errs.find("runtime extern function '@Paykan_println' is not named "
                      "'@$rt.<symbol>'"),
            std::string::npos)
      << errs;
  EXPECT_NE(errs.find("extern global '@PaykanObject_None' is not named "
                      "'@$rt.<symbol>'"),
            std::string::npos)
      << errs;
  EXPECT_NE(errs.find("cstr global '@$rt.s' has a runtime name"),
            std::string::npos)
      << errs;
  EXPECT_NE(errs.find("function '@$rt.f' has a runtime name"),
            std::string::npos)
      << errs;

  // A program function and the runtime extern of the same C symbol live
  // side by side.
  std::string text = R"(module "main"
cstr @.str0 = "hi" len 2

extern fn @$rt.PaykanString_new(ptr, i64) -> obj
extern fn @$rt.Paykan_println(obj) -> void
extern fn @$rt.PaykanString_destroy(obj) -> void

fn @PaykanString_new(%x.1: i64) -> i64 {
  ret %x.1
}

fn @main() -> i64 {
  %s.1 = call @$rt.PaykanString_new(@.str0, 2)
  call @$rt.Paykan_println(%s.1)
  call @$rt.PaykanString_destroy(%s.1)
  %r.2 = call @PaykanString_new(0)
  ret %r.2
}
)";
  EXPECT_EQ(verifyText(text), "");
  ParseError err;
  auto p = parseProgram(text, err);
  ASSERT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return;
  EXPECT_EQ(toString(*p), text);
  EXPECT_TRUE(isRuntimeName("$rt.Paykan_println"));
  EXPECT_FALSE(isRuntimeName("Paykan_println"));
  EXPECT_FALSE(isRuntimeName("$rt."));
  EXPECT_EQ(runtimeSymbol("$rt.Paykan_println"), "Paykan_println");
  EXPECT_EQ(runtimeName("Paykan_println"), "$rt.Paykan_println");
}

// A C symbol names one C function, with one signature: a native extern
// (`$c.X`) and the runtime's (`$rt.X`) of one module must agree, and so must
// any two of a program's modules (findings 39, new-F3-1).  Before, the
// verifier let the pair through and the MLIR pipeline failed on the call.
TEST(PIRVerifier, OneCSymbolHasOneSignature) {
  const char *clash = R"(module "main"
extern fn @$rt.PaykanString_concat(obj, obj) -> obj
extern fn @$c.PaykanString_concat(i64) -> i64
fn @main() -> i64 {
  %r = call @$c.PaykanString_concat(1)
  ret %r
}
)";
  EXPECT_EQ(verifyText(clash),
            "main: native extern '@$c.PaykanString_concat' is declared (i64) "
            "-> i64 but runtime extern '@$rt.PaykanString_concat' of the "
            "same C symbol (obj, obj) -> obj\n");
  // The runtime's own signature is fine.
  EXPECT_EQ(verifyText(R"(module "main"
extern fn @$rt.PaykanString_length(obj) -> i64
extern fn @$c.PaykanString_length(obj) -> i64
fn @main() -> i64 {
  ret 0
}
)"),
            "");

  // Across modules: each module's extern is valid on its own.
  const char *twoModules = R"(module "main"
extern fn @$c.llabs(f64) -> f64
fn @main() -> i64 {
  %r = call @$c.llabs(-2.5)
  ret 0
}
module "na"
extern fn @$c.llabs(i64) -> i64
extern fn @$rt.fmod(f64, f64) -> f64
fn @ca(%x: i64) -> i64 {
  %r = call @$c.llabs(%x)
  ret %r
}
module "nb"
extern fn @$c.fmod(i64, i64) -> i64
extern fn @$c.llabs(i64) -> i64
fn @cb(%x: i64) -> i64 {
  %r = call @$c.fmod(%x, %x)
  ret %r
}
)";
  EXPECT_EQ(verifyText(twoModules),
            "na: extern fn '@$c.llabs' is declared (i64) -> i64 but module "
            "'main' declares '@$c.llabs' of the same C symbol (f64) -> f64\n"
            "nb: extern fn '@$c.fmod' is declared (i64, i64) -> i64 but "
            "module 'na' declares '@$rt.fmod' of the same C symbol (f64, "
            "f64) -> f64\n"
            "nb: extern fn '@$c.llabs' is declared (i64) -> i64 but module "
            "'main' declares '@$c.llabs' of the same C symbol (f64) -> "
            "f64\n");
  ParseError err;
  auto p = parseProgram(twoModules, err);
  ASSERT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return;
  std::vector<CSymbolClash> clashes = cSymbolClashes(*p);
  ASSERT_EQ(clashes.size(), 3u);
  EXPECT_EQ(clashes[0].Symbol, "llabs");
  EXPECT_EQ(clashes[0].FirstModule->Name, "main");
  EXPECT_EQ(clashes[0].OtherModule->Name, "na");
  EXPECT_EQ(clashes[1].Symbol, "fmod");
  EXPECT_EQ(clashes[1].First->Name, "$rt.fmod");
  EXPECT_EQ(clashes[1].Other->Name, "$c.fmod");
  EXPECT_EQ(clashes[2].OtherModule->Name, "nb");
}

// -- Sized numeric types (docs/design/sized-types.md)

// `wrap`, `mask`, `udiv`, `urem`, `ucmp`, `utof`, `ftou` and `fround32`
// print, parse and verify.
TEST(PIR, SizedTypeInstructionsRoundTripAndVerify) {
  const char *src = R"(module "m"

fn @main() -> i64 {
  %a.1 = wrap 8 300
  %b.2 = mask 16 %a.1
  %c.3 = udiv %b.2, 3
  %d.4 = urem %c.3, 2
  %e.5 = ucmp lt %c.3, %d.4
  %f.6 = utof %d.4
  %g.7 = fround32 %f.6
  %h.8 = ftou %g.7
  %i.9 = select %e.5, %h.8, %a.1
  %j.10 = wrap 32 %i.9
  ret %j.10
}
)";
  EXPECT_EQ(reprint(src), src);
  ParseError err;
  auto p = parseProgram(src, err);
  ASSERT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return;
  auto errors = verify(*p);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors);
}

TEST(PIR, SizedTypeInstructionsAreTyped) {
  auto fn = [](const std::string &body) {
    return "module \"m\"\nfn @main() -> i64 {\n" + body + "\n  ret 0\n}\n";
  };
  EXPECT_NE(verifyText(fn("  %a = wrap 8 1.5")).find("operand of wrap / mask"),
            std::string::npos);
  EXPECT_NE(verifyText(fn("  %a = mask 8 true")).find("operand of wrap / mask"),
            std::string::npos);
  EXPECT_NE(verifyText(fn("  %a = udiv 1.5, 2.5")).find("operand of udiv"),
            std::string::npos);
  EXPECT_NE(verifyText(fn("  %a = ucmp lt 1.5, 2.5")).find("operand of ucmp"),
            std::string::npos);
  EXPECT_NE(verifyText(fn("  %a = utof 1.5")).find("operand of utof"),
            std::string::npos);
  EXPECT_NE(verifyText(fn("  %a = ftou 1")).find("operand of ftou"),
            std::string::npos);
  EXPECT_NE(verifyText(fn("  %a = fround32 1")).find("operand of fround32"),
            std::string::npos);
  // The width is 8, 16 or 32: the parser refuses another.
  EXPECT_NE(verifyText(fn("  %a = wrap 64 1")).find("PARSE ERROR"),
            std::string::npos);
  EXPECT_NE(verifyText(fn("  %a = mask 7 1")).find("PARSE ERROR"),
            std::string::npos);
  EXPECT_EQ(
      verifyText(fn("  %a = wrap 32 1\n  %b = mask 8 %a\n  %c = fround32 1.5\n"
                    "  %d = ftou %c\n  %e = utof %d\n  %f = ucmp ge %d, %b")),
      "");
}

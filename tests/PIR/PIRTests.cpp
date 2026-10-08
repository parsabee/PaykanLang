// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR text round trip (parser <-> printer) and verifier tests.

#include "Programs.h"
#include "paykan/pir/PIR.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>

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
  free %self.1
  ret
}

fn @K_destroy(%self.1: obj) -> void {
  ret
}

fn @"Pair<Str, int>.destroy"(%self.1: obj) -> void {
  free %self.1
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
  EXPECT_EQ(main->NextValueId, 29u);
}

TEST(PIR, ParserAcceptsHandWrittenNames) {
  // Values and locals without numbers; a bare null; a quoted class name.
  const char *text = R"(module "m"
class "Box<int>" {
  field v: i64
  vtable { destroy = @d : (obj) -> void }
}
fn @d(%self: obj) -> void { free %self  ret }
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
  free %self.1
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

TEST(PIRVerifier, ChecksAddressOps) {
  std::string errs = verifyText(R"(module "m"
fn @main() -> i64 {
  local %o.0: obj
  local %i.1: i64
  local %b.2: box
  %p = local.addr %o.0
  %q = local.addr %i.1
  %v = ptr.load obj, %q
  ptr.store 1, 2
  ptr.store %q, null obj
  %w = ptr.load i64, %q
  ptr.store %q, %w
  %r = local.addr %b.2
  %x = ptr.load box, %r
  ptr.store %r, %x
  ret %w
}
)");
  EXPECT_NE(errs.find("local.addr of a local of type obj"), std::string::npos)
      << errs;
  EXPECT_NE(errs.find("ptr.load of obj"), std::string::npos) << errs;
  EXPECT_NE(errs.find("address of ptr.store"), std::string::npos) << errs;
  EXPECT_NE(errs.find("ptr.store of obj"), std::string::npos) << errs;
  // Scalars and a box (an optional's slot) are addressable.
  EXPECT_EQ(errs.find("ptr.load of i64"), std::string::npos) << errs;
  EXPECT_EQ(errs.find("of type box"), std::string::npos) << errs;
  EXPECT_EQ(errs.find("ptr.load of box"), std::string::npos) << errs;
  EXPECT_EQ(errs.find("ptr.store of box"), std::string::npos) << errs;
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
fn @main(%args.1: box) -> i64 {
  call @nope(1)
  call @k(1, 2)
  call @k(true)
  %o.2 = new D
  %v.3 = field.load %o.2, C.g
  %w.4 = vcall %o.2 : C [5] ()
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

// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR text round trip (parser <-> printer) and verifier tests.

#include "paykan/pir/PIR.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <string>

using namespace paykan::pir;

namespace {

// A program exercising every item and instruction of docs/pir.md.
const char *const kProgram = R"(module "main"
cstr @.str0 = "Hello\n" len 6
data @.d0 = [1, -2, 3]
bytes @.b0 = [0, 4]
extern obj @PaykanObject_None
extern vtable @PaykanArray_vtable

class Point {
  field x: i64
  field name: box
  vtable {
    destroy = @Point_destroy : (obj) -> void
    toString = @PaykanObject_toString : (obj) -> box
    equals = @PaykanObject_equals : (obj, box) -> i64
    area = null : (obj) -> i64
  }
}

class Point3 : Point {
  field x: i64
  field name: box
  field z: f64
  vtable {
    destroy = @Point_destroy : (obj) -> void
  }
}

extern class Adder module "lib" {
  field n: i64
}

extern fn @PaykanString_new(ptr, i64) -> obj
extern fn @Paykan_println(obj) -> void
extern fn @PaykanString_destroy(obj) -> void
extern fn @PaykanObject_toString(obj) -> box
extern fn @PaykanObject_equals(obj, box) -> i64
extern fn @helper(i64) -> i64 module "lib"

fn @Point_destroy(%self.1: obj) -> void {
  %n.2 = field.load %self.1, Point.name
  release %n.2
  free %self.1
  ret
}

fn @main() -> i64 {
  local %i.0: i64
  local %acc.1: box
  local %acc.2: box
  %s.1 = call @PaykanString_new(@.str0, 6)
  call @Paykan_println(%s.1)
  call @PaykanString_destroy(%s.1)
  %p.2 = new Point
  field.store %p.2, Point.x, 7
  field.store %p.2, Point.name, null box
  %vt.3 = vtable.addr Point
  %vt2.4 = vtable.addr @PaykanArray_vtable
  %vt3.5 = vtable.load %p.2
  %same.6 = cmp eq %vt.3, %vt3.5
  %b.7 = box %p.2
  retain %b.7
  store %acc.1, %b.7
  store %i.0, 0
  while {
    %c.8 = load %i.0
    %lt.9 = cmp lt %c.8, 10
    cond %lt.9
  } {
    %c2.10 = load %i.0
    %n.11 = add %c2.10, 1
    %m.12 = mul %n.11, 2
    %d.13 = div %m.12, 2
    %r.14 = rem %d.13, 3
    %neg.15 = neg %r.14
    store %i.0, %neg.15
    if %same.6 {
      continue
    } else {
      break
    }
  }
  %x.16 = field.load %p.2, Point.x
  %f.17 = itof %x.16
  %g.18 = cast %f.17 to i64
  %fl.19 = sub 1.5, 2000.0
  %nb.20 = not true
  %ch.21 = cast 'a' to i64
  %nl.22 = cast '\n' to i64
  %area.23 = vcall %p.2 : Point [3] ()
  %eq.24 = vcall %p.2 : (obj, box) -> i64 [2] (null box)
  %h.25 = call @helper(%g.18)
  %sel.26 = select %nb.20, %h.25, %area.23
  %o.27 = unbox %b.7
  release %b.7
  if %nb.20 {
    unreachable
  }
  ret %sel.26
}

module "lib"

class Adder {
  field n: i64
  vtable {
    destroy = @Adder_destroy : (obj) -> void
  }
}

fn @Adder_destroy(%self.1: obj) -> void {
  free %self.1
  ret
}

fn @helper(%a.1: i64) -> i64 {
  ret %a.1
}
)";

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
  EXPECT_EQ(main->NextValueId, 28u);
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
  %o.5 = select %c.4, @PaykanObject_None, %"x y".3
  call @Paykan_println(%o.5)
  release %"Pair<Str, int>.shared".2
  %r.6 = call @helper(1)
  ret %r.6
}

extern obj @PaykanObject_None
extern fn @helper(i64) -> i64 module "lib"
extern fn @Paykan_println(obj) -> void
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

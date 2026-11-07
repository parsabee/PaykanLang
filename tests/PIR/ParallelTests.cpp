// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The concurrency prototype's PIR (docs/design/concurrency.md; the "PIR"
// item of samples/concurrency/README.md): an `async fn`, `async_call` and
// `await`, and the `parallel` annotation on a `while`, through the text
// round trip, the binary round trip and the verifier.  The programs are
// what the lowering prints for samples/concurrency/01_parallel_for.pkn and
// 02_async_await.pkn (`paykan --emit-pir`), cut down to what the tests look
// at.

#include "paykan/pir/Binary.h"
#include "paykan/pir/Codes.h"
#include "paykan/pir/PIR.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace paykan;
using namespace paykan::pir;

namespace {

Program parse(const std::string &text) {
  ParseError err;
  auto p = parseProgram(text, err);
  EXPECT_TRUE(p.has_value()) << err.str();
  return p ? std::move(*p) : Program{};
}

/// The text a parse of @p text prints back ("" when it does not parse).
std::string reprint(const std::string &text) {
  ParseError err;
  auto p = parseProgram(text, err);
  EXPECT_TRUE(p.has_value()) << err.str();
  if (!p.has_value())
    return "";
  return toString(*p);
}

/// Verifies a program and returns the joined error text.
std::string verifyText(const std::string &text) {
  ParseError err;
  auto p = parseProgram(text, err);
  if (!p.has_value())
    return "PARSE ERROR: " + err.str();
  return formatErrors(verify(*p));
}

/// The message of a parse of @p text, which must fail.
std::string parseError(const std::string &text) {
  ParseError err;
  auto p = parseProgram(text, err);
  EXPECT_FALSE(p.has_value()) << text;
  return err.Message;
}

/// The two invariants of docs/design/pkm.md §5.8 plus a clean verify of
/// the decoded module, as tests/PIR/BinaryTests.cpp checks them.
void expectRoundTrip(const Module &m) {
  std::vector<uint8_t> b = binary::encode(m);
  auto decoded = binary::decode(b);
  ASSERT_TRUE(decoded.isOk()) << decoded.status().message();
  std::vector<VerifyError> errors = verify(*decoded);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors);
  EXPECT_EQ(toString(*decoded), toString(m));
  EXPECT_EQ(binary::encode(*decoded), b);
}

/// A module holding only @p f.
Module oneFunction(Function f) {
  Module m;
  m.Name = "f";
  m.Functions.push_back(std::move(f));
  return m;
}

/// The function of @p m named @p name, to change it in place.
Function &functionNamed(Module &m, const std::string &name) {
  for (Function &f : m.Functions)
    if (f.Name == name)
      return f;
  ADD_FAILURE() << "no function " << name;
  static Function none;
  return none;
}

/// The offset of the length byte of @p name's function record in @p b.
size_t recordOf(const std::vector<uint8_t> &b, const std::string &name) {
  for (const binary::SymbolEntry &e : binary::index(b))
    if (e.Name == name)
      return static_cast<size_t>(e.Offset);
  ADD_FAILURE() << "no record of " << name;
  return 0;
}

// The lowering of samples/concurrency/02_async_await.pkn, cut down.  An
// `async fn` is called with `async_call`, which yields a task (a `ptr`)
// whatever the function returns, and `await <type>, <task>` is the
// function's result; an async fn of another module is declared `extern
// async fn`.  An async_call's arguments carry an ownership as a call's do
// (`lent %b.1` in @start).  It is written the way the printer prints.
const char *const kAsyncProgram = R"(module "m"

extern async fn @"lib::slow"(i64) -> i64 module "lib" symbol @slow

async fn @twice(%n.1: i64) -> i64 {
  %d.2 = add %n.1, %n.1
  ret %d.2
}

async fn @hold(%b.1: box) -> i64 {
  %o.2 = owned %b.1
  if %o.2 {
    release %b.1
  }
  ret 1
}

fn @start(%b.1: box) -> i64 {
  %t.2 = async_call @hold(lent %b.1)
  %r.3 = await i64, %t.2
  release %b.1
  ret %r.3
}

fn @main() -> i64 {
  %t.1 = async_call @twice(20)
  %a.2 = await i64, %t.1
  %u.3 = async_call @"lib::slow"(1)
  %b.4 = await i64, %u.3
  %c.5 = call @start(null box)
  %s.6 = add %a.2, %b.4
  %r.7 = add %s.6, %c.5
  ret %r.7
}

module "lib"

async fn @slow(%n.1: i64) -> i64 {
  ret %n.1
}
)";

// The lowering of samples/concurrency/01_parallel_for.pkn, cut down: a
// `parallel for` is a `while` counting a local from 0 up to a count, with
// the annotation after its body (docs/design/concurrency.md "Data
// parallelism").  @sum reduces into a partial with a private loop
// variable; @sumprod has two reductions, `add` and `mul`, and no private;
// @main's first loop has two privates and a constant count, its second is
// a plain while.  It is written the way the printer prints.
const char *const kParallelProgram = R"(module "m"

fn @sum(%n.1: i64) -> i64 {
  local %s.0: i64
  local %part.1: i64
  local %k.2: i64
  local %i.3: i64
  store %s.0, 0
  store %part.1, 0
  store %k.2, 0
  while {
    %kv.2 = load %k.2
    %go.3 = ucmp lt %kv.2, %n.1
    cond %go.3
  } {
    %idx.4 = load %k.2
    %next.5 = add %idx.4, 1
    store %k.2, %next.5
    store %i.3, %idx.4
    %p.6 = load %part.1
    %i.7 = load %i.3
    %q.8 = add %p.6, %i.7
    store %part.1, %q.8
  } parallel %k.2, %n.1 privates(%i.3) reduce(%part.1 add)
  %s.9 = load %s.0
  %p.10 = load %part.1
  %f.11 = add %s.9, %p.10
  ret %f.11
}

fn @sumprod(%n.1: i64) -> i64 {
  local %k.0: i64
  local %a.1: i64
  local %b.2: i64
  store %k.0, 0
  store %a.1, 0
  store %b.2, 1
  while {
    %kv.2 = load %k.0
    %go.3 = ucmp lt %kv.2, %n.1
    cond %go.3
  } {
    %idx.4 = load %k.0
    %next.5 = add %idx.4, 1
    store %k.0, %next.5
    %a.6 = load %a.1
    %a2.7 = add %a.6, %idx.4
    store %a.1, %a2.7
    %b.8 = load %b.2
    %b2.9 = mul %b.8, 2
    store %b.2, %b2.9
  } parallel %k.0, %n.1 reduce(%a.1 add, %b.2 mul)
  %a.10 = load %a.1
  %b.11 = load %b.2
  %r.12 = add %a.10, %b.11
  ret %r.12
}

fn @main() -> i64 {
  local %k.0: i64
  local %x.1: i64
  local %y.2: f64
  store %k.0, 0
  while {
    %kv.1 = load %k.0
    %go.2 = ucmp lt %kv.1, 8
    cond %go.2
  } {
    %idx.3 = load %k.0
    %next.4 = add %idx.3, 1
    store %k.0, %next.4
    store %x.1, %idx.3
    %f.5 = itof %idx.3
    store %y.2, %f.5
  } parallel %k.0, 8 privates(%x.1, %y.2)
  store %k.0, 0
  while {
    %kv.6 = load %k.0
    %go.7 = cmp lt %kv.6, 2
    cond %go.7
  } {
    %idx.8 = load %k.0
    %next.9 = add %idx.8, 1
    store %k.0, %next.9
  }
  %s.10 = call @sum(100)
  %p.11 = call @sumprod(4)
  %t.12 = add %s.10, %p.11
  ret %t.12
}
)";

// A counted loop over %k.0 (local 0) with a private %i.1 (local 1) and a
// partial %p.2 (local 2), whose annotation the tests choose: loop("") is
// the plain while.
const char *const kLoopHead = R"(module "m"
fn @f(%n.1: i64) -> i64 {
  local %k.0: i64
  local %i.1: i64
  local %p.2: i64
  store %k.0, 0
  store %p.2, 0
  while {
    %kv.2 = load %k.0
    %go.3 = ucmp lt %kv.2, %n.1
    cond %go.3
  } {
    %idx.4 = load %k.0
    %next.5 = add %idx.4, 1
    store %k.0, %next.5
    store %i.1, %idx.4
    %iv.6 = load %i.1
    %pv.7 = load %p.2
    %q.8 = add %pv.7, %iv.6
    store %p.2, %q.8
  })";

const char *const kLoopTail = R"(
  %r.9 = load %p.2
  ret %r.9
}
fn @main() -> i64 {
  ret 0
}
)";

std::string loop(const std::string &annotation) {
  return kLoopHead + annotation + kLoopTail;
}

} // namespace

// -- async fn, async_call, await

// `async fn` (defined or extern), `async_call` and `await` print as they
// are written, verify, and parse into their fields: the flag on the
// function, a task of type ptr, the awaited type from the text.
TEST(PIR, AsyncFnAsyncCallAndAwaitRoundTripAndVerify) {
  EXPECT_EQ(reprint(kAsyncProgram), kAsyncProgram);
  EXPECT_EQ(verifyText(kAsyncProgram), "");
  Program p = parse(kAsyncProgram);
  ASSERT_EQ(p.Modules.size(), 2u);
  const Module &m = p.Modules[0];
  const Function *twice = m.findFunction("twice");
  const Function *slow = m.findFunction("lib::slow");
  const Function *start = m.findFunction("start");
  const Function *main = m.findFunction("main");
  const Function *def = p.Modules[1].findFunction("slow");
  ASSERT_NE(twice, nullptr);
  ASSERT_NE(slow, nullptr);
  ASSERT_NE(start, nullptr);
  ASSERT_NE(main, nullptr);
  ASSERT_NE(def, nullptr);
  EXPECT_TRUE(twice->IsAsync);
  EXPECT_FALSE(twice->IsExtern);
  EXPECT_TRUE(slow->IsAsync);
  EXPECT_TRUE(slow->IsExtern);
  EXPECT_EQ(slow->Module, "lib");
  EXPECT_EQ(slow->Symbol, "slow");
  EXPECT_TRUE(def->IsAsync);
  EXPECT_FALSE(start->IsAsync);
  EXPECT_FALSE(main->IsAsync);

  // `%t.1 = async_call @twice(20)`: the task is a ptr, whatever the callee
  // returns; `%a.2 = await i64, %t.1` takes its type from the text.
  const auto &call = std::get<Instr>(main->Body.Stmts[0]);
  EXPECT_EQ(call.Op, Opcode::AsyncCall);
  EXPECT_EQ(call.Callee, "twice");
  EXPECT_EQ(call.Result.Id, 1u);
  EXPECT_EQ(call.Result.Name, "t");
  EXPECT_EQ(call.Result.Ty, Type::Ptr);
  ASSERT_EQ(call.Args.size(), 1u);
  EXPECT_TRUE(call.Own.empty());
  const auto &wait = std::get<Instr>(main->Body.Stmts[1]);
  EXPECT_EQ(wait.Op, Opcode::Await);
  EXPECT_EQ(wait.Result.Name, "a");
  EXPECT_EQ(wait.Result.Ty, Type::I64);
  ASSERT_EQ(wait.Args.size(), 1u);
  ASSERT_TRUE(wait.Args[0].isValue());
  EXPECT_EQ(wait.Args[0].valueId(), call.Result.Id);
  // The task of an extern async fn is a ptr too.
  EXPECT_EQ(std::get<Instr>(main->Body.Stmts[2]).Result.Ty, Type::Ptr);
  // An async_call's argument is lent as a call's is.
  const auto &lent = std::get<Instr>(start->Body.Stmts[0]);
  EXPECT_EQ(lent.Op, Opcode::AsyncCall);
  ASSERT_EQ(lent.Own.size(), 1u);
  EXPECT_EQ(lent.ownership(0), ArgOwnership::Lent);
}

TEST(PIRVerifier, ChecksAsyncCallAndAwait) {
  std::string errs = verifyText(R"(module "m"
async fn @f(%n.1: i64) -> i64 {
  ret %n.1
}
fn @plain() -> i64 {
  ret 0
}
fn @main() -> i64 {
  %a = async_call @nope(1)
  %b = async_call @plain()
  %c = async_call @f(true)
  %d = async_call @f(1, 2)
  async_call @f(3)
  %e = await i64, 1
  %g = await weak, %a
  await void, %a
  %h = await i64, %a
  %k = add %h, 1
  ret %k
}
)");
  for (const char *e : {"async_call of undeclared function '@nope'",
                        "async_call of '@plain', which is not an async fn",
                        "argument 0 of '@f' true has type bool, expected i64",
                        "call to '@f' passes 2 argument(s), expected 1",
                        "'async_call' produces a value but has no result",
                        "task of await 1 has type i64, expected ptr",
                        "await of weak", "await of void"})
    EXPECT_NE(errs.find(e), std::string::npos) << e << "\n" << errs;
  // The task of an undeclared callee is still a defined ptr, so the awaits
  // of it report nothing more; an await of an i64 is fine.
  EXPECT_EQ(errs.find("before its definition"), std::string::npos) << errs;
  EXPECT_EQ(errs.find("task of await %"), std::string::npos) << errs;
  EXPECT_EQ(errs.find("await of i64"), std::string::npos) << errs;

  // What the text cannot write: a task of another type, an await whose
  // operand has an ownership.
  Program p = parse(kAsyncProgram);
  ASSERT_EQ(p.Modules.size(), 2u);
  Function &main = functionNamed(p.Modules[0], "main");
  std::get<Instr>(main.Body.Stmts[0]).Result.Ty = Type::I64;
  ArgOwnership lent;
  lent.K = ArgOwnership::Lent;
  std::get<Instr>(main.Body.Stmts[1]).Own.push_back(lent);
  errs = formatErrors(verify(p));
  for (const char *e :
       {"result of 'async_call' has type i64, expected ptr",
        "'await' gives its operands an ownership (only a call's arguments "
        "have one)"})
    EXPECT_NE(errs.find(e), std::string::npos) << e << "\n" << errs;
}

// An await always yields a value: the parser wants its result.
TEST(PIR, AnAwaitHasAResult) {
  EXPECT_NE(parseError(R"(module "m"
async fn @f() -> i64 {
  ret 0
}
fn @main() -> i64 {
  %t = async_call @f()
  await i64, %t
  ret 0
}
)")
                .find("no '%result ='"),
            std::string::npos);
}

// The async flag is a bit of the function record's flags (bit 5, after
// extern, module, symbol, shared and uses), on a definition and on an
// extern alike; the decoded function is async exactly when the bit is set.
TEST(PIRBinary, AnAsyncFnKeepsItsFlag) {
  Program p = parse(kAsyncProgram);
  ASSERT_EQ(p.Modules.size(), 2u);
  for (const Module &m : p.Modules)
    expectRoundTrip(m);
  std::vector<uint8_t> good = binary::encode(p.Modules[0]);
  // A record: its length, its name, then its flags (4 bytes, the low
  // byte first).  The extern's other bits: extern, module, symbol.
  const unsigned kAsync = 0x20, kExtern = 0x01, kModule = 0x02, kSymbol = 0x04;
  const size_t twice = recordOf(good, "twice");
  const size_t slow = recordOf(good, "lib::slow");
  const size_t main = recordOf(good, "main");
  EXPECT_EQ(good[twice + 2], kAsync);
  EXPECT_EQ(good[slow + 2], kAsync | kExtern | kModule | kSymbol);
  EXPECT_EQ(good[main + 2], 0u);
  auto d = binary::decode(good);
  ASSERT_TRUE(d.isOk()) << d.status().message();
  const Function *dt = (*d).findFunction("twice");
  const Function *ds = (*d).findFunction("lib::slow");
  const Function *dm = (*d).findFunction("main");
  ASSERT_NE(dt, nullptr);
  ASSERT_NE(ds, nullptr);
  ASSERT_NE(dm, nullptr);
  EXPECT_TRUE(dt->IsAsync);
  EXPECT_TRUE(ds->IsAsync);
  EXPECT_FALSE(dm->IsAsync);
  // Without the bit @twice is an ordinary function, which prints as one
  // and whose async_call the verifier refuses: the bit is what is read,
  // not the name.
  std::vector<uint8_t> plain = good;
  plain[twice + 2] = 0;
  auto e = binary::decode(plain);
  ASSERT_TRUE(e.isOk()) << e.status().message();
  const Function *et = (*e).findFunction("twice");
  ASSERT_NE(et, nullptr);
  EXPECT_FALSE(et->IsAsync);
  const std::string text = toString(*e);
  EXPECT_NE(text.find("\nfn @twice("), std::string::npos) << text;
  EXPECT_EQ(text.find("async fn @twice("), std::string::npos) << text;
  EXPECT_NE(formatErrors(verify(*e))
                .find("async_call of '@twice', which is not an async fn"),
            std::string::npos)
      << formatErrors(verify(*e));
}

// `async_call` is opcode 63 and `await` 64 (docs/design/pkm.md §5.5), and
// 65 stays reserved.  The ownership mark (the code before a call whose
// arguments are not all owned) is taken before an async_call, as @start's
// lent argument round-trips, but not before an await.
TEST(PIRBinary, AsyncCallAndAwaitHaveCodes63And64) {
  EXPECT_EQ(opcodeEntry(Opcode::AsyncCall).Code, 63u);
  EXPECT_STREQ(opcodeEntry(Opcode::AsyncCall).Name, "async_call");
  EXPECT_EQ(opcodeEntry(Opcode::Await).Code, 64u);
  EXPECT_STREQ(opcodeEntry(Opcode::Await).Name, "await");
  Opcode op;
  ASSERT_TRUE(opcodeFromCode(63, op));
  EXPECT_EQ(op, Opcode::AsyncCall);
  ASSERT_TRUE(opcodeFromCode(64, op));
  EXPECT_EQ(op, Opcode::Await);
  EXPECT_FALSE(opcodeFromCode(65, op));

  Program p = parse(kAsyncProgram);
  ASSERT_EQ(p.Modules.size(), 2u);
  std::vector<uint8_t> good = binary::encode(p.Modules[0]);
  // @main's record: length, name, flags (4), sig (count 0, ret),
  // conventions, params (none), locals (none), next id, the body's count,
  // the first statement's tag, then its opcode.
  const size_t main = recordOf(good, "main");
  const size_t first = main + 1 + 1 + 4 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1;
  ASSERT_EQ(good[first], 63u);
  // `%t.1 = async_call @twice(20)`: the opcode, the result's id, type and
  // name, the callee, one argument (an i64 constant: its kind, its
  // value), then the next statement's tag and `%a.2 = await i64, %t.1`'s
  // opcode.
  const size_t second = first + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1;
  ASSERT_EQ(good[second], 64u);
  // The await: the opcode, the result's id, type and name, one argument
  // (a value: its kind, its id), the tag, then the second async_call.
  const size_t third = second + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1;
  EXPECT_EQ(good[third], 63u);

  Function &start = functionNamed(p.Modules[0], "start");
  ArgOwnership lent;
  lent.K = ArgOwnership::Lent;
  std::get<Instr>(start.Body.Stmts[1]).Own.push_back(lent);
  auto bad = binary::decode(binary::encode(p.Modules[0]));
  ASSERT_FALSE(bad.isOk());
  EXPECT_NE(bad.status().message().find(
                "an ownership mark before an instruction that is no call"),
            std::string::npos)
      << bad.status().message();
}

// -- The parallel annotation on a while

// ` parallel %counter, count [privates(...)] [reduce(%partial add|mul,
// ...)]` after a while's body prints as it is written, verifies, and
// parses into pir::Parallel; a while without it has none.
TEST(PIR, ParallelAnnotationRoundTripsAndVerifies) {
  EXPECT_EQ(reprint(kParallelProgram), kParallelProgram);
  EXPECT_EQ(verifyText(kParallelProgram), "");
  Program p = parse(kParallelProgram);
  ASSERT_EQ(p.Modules.size(), 1u);
  const Module &m = p.Modules[0];
  const Function *sum = m.findFunction("sum");
  const Function *sumprod = m.findFunction("sumprod");
  const Function *main = m.findFunction("main");
  ASSERT_NE(sum, nullptr);
  ASSERT_NE(sumprod, nullptr);
  ASSERT_NE(main, nullptr);

  // @sum: the counter %k.2, the count %n.1, one private, one reduction.
  const auto *w = std::get_if<While>(&sum->Body.Stmts[3]);
  ASSERT_NE(w, nullptr);
  ASSERT_TRUE(w->Par.has_value());
  EXPECT_EQ(w->Par->Counter, 2u);
  ASSERT_TRUE(w->Par->Count.isValue());
  EXPECT_EQ(w->Par->Count.valueId(), 1u);
  ASSERT_EQ(w->Par->Privates.size(), 1u);
  EXPECT_EQ(w->Par->Privates[0], 3u);
  ASSERT_EQ(w->Par->Reductions.size(), 1u);
  EXPECT_EQ(w->Par->Reductions[0].Partial, 1u);
  EXPECT_EQ(w->Par->Reductions[0].Op, Opcode::Add);

  // @sumprod: no private, an add and a mul, in order.
  w = std::get_if<While>(&sumprod->Body.Stmts[3]);
  ASSERT_NE(w, nullptr);
  ASSERT_TRUE(w->Par.has_value());
  EXPECT_EQ(w->Par->Counter, 0u);
  EXPECT_TRUE(w->Par->Privates.empty());
  ASSERT_EQ(w->Par->Reductions.size(), 2u);
  EXPECT_EQ(w->Par->Reductions[0].Partial, 1u);
  EXPECT_EQ(w->Par->Reductions[0].Op, Opcode::Add);
  EXPECT_EQ(w->Par->Reductions[1].Partial, 2u);
  EXPECT_EQ(w->Par->Reductions[1].Op, Opcode::Mul);

  // @main: a constant count, two privates, no reduction; then a plain
  // while.
  w = std::get_if<While>(&main->Body.Stmts[1]);
  ASSERT_NE(w, nullptr);
  ASSERT_TRUE(w->Par.has_value());
  EXPECT_EQ(w->Par->Counter, 0u);
  ASSERT_TRUE(std::holds_alternative<int64_t>(w->Par->Count.V));
  EXPECT_EQ(std::get<int64_t>(w->Par->Count.V), 8);
  ASSERT_EQ(w->Par->Privates.size(), 2u);
  EXPECT_EQ(w->Par->Privates[0], 1u);
  EXPECT_EQ(w->Par->Privates[1], 2u);
  EXPECT_TRUE(w->Par->Reductions.empty());
  w = std::get_if<While>(&main->Body.Stmts[3]);
  ASSERT_NE(w, nullptr);
  EXPECT_FALSE(w->Par.has_value());
}

// The annotation names the function's locals, its count is an operand,
// and a reduction's opcode is an opcode name (the verifier narrows it to
// add and mul).
TEST(PIR, ParallelAnnotationParseErrors) {
  EXPECT_NE(parseError(loop(" parallel %nope, 1")).find("undeclared local"),
            std::string::npos);
  EXPECT_NE(parseError(loop(" parallel 1, 1")).find("expected a '%local'"),
            std::string::npos);
  EXPECT_NE(parseError(loop(" parallel %k.0 1")).find("expected ','"),
            std::string::npos);
  EXPECT_NE(
      parseError(loop(" parallel %k.0, 1 privates %i.1")).find("expected '('"),
      std::string::npos);
  EXPECT_NE(parseError(loop(" parallel %k.0, 1 privates(%i.1 %p.2)"))
                .find("expected ',' or ')'"),
            std::string::npos);
  EXPECT_NE(parseError(loop(" parallel %k.0, 1 reduce(%p.2 frob)"))
                .find("expected 'add' or 'mul'"),
            std::string::npos);
  EXPECT_NE(parseError(loop(" parallel %k.0, 1 reduce(%p.2)"))
                .find("expected 'add' or 'mul'"),
            std::string::npos);
  // The annotation follows a while's body only: elsewhere `parallel`
  // begins no statement.
  EXPECT_NE(parseError(R"(module "m"
fn @main() -> i64 {
  local %k.0: i64
  if true {
  } parallel %k.0, 1
  ret 0
}
)")
                .find("expected an instruction"),
            std::string::npos);
}

TEST(PIRVerifier, ChecksTheParallelAnnotation) {
  EXPECT_EQ(verifyText(loop("")), "");
  EXPECT_EQ(verifyText(loop(" parallel %k.0, %n.1 privates(%i.1) "
                            "reduce(%p.2 add)")),
            "");
  EXPECT_EQ(verifyText(loop(" parallel %k.0, 4 reduce(%p.2 mul)")), "");
  std::string errs = verifyText(loop(" parallel %k.0, 1.5 reduce(%p.2 sub)"));
  for (const char *e : {"parallel count 1.5 has type f64, expected i64",
                        "parallel reduction with 'sub' (only add and mul)"})
    EXPECT_NE(errs.find(e), std::string::npos) << e << "\n" << errs;

  // The text names only locals the function has, so a local it has not,
  // or a reduction by an opcode that is no reduction at all, is a module
  // built in memory (or read from a blob).
  Program p =
      parse(loop(" parallel %k.0, %n.1 privates(%i.1) reduce(%p.2 add)"));
  ASSERT_EQ(p.Modules.size(), 1u);
  Function &f = functionNamed(p.Modules[0], "f");
  While &w = std::get<While>(f.Body.Stmts[2]);
  ASSERT_TRUE(w.Par.has_value());
  w.Par->Counter = 9;
  w.Par->Privates[0] = 9;
  w.Par->Reductions[0].Partial = 9;
  w.Par->Reductions[0].Op = Opcode::Cmp;
  errs = formatErrors(verify(p));
  for (const char *e :
       {"parallel counter names local 9, which the function has not",
        "parallel private names local 9, which the function has not",
        "parallel reduction names local 9, which the function has not",
        "parallel reduction with 'cmp' (only add and mul)"})
    EXPECT_NE(errs.find(e), std::string::npos) << e << "\n" << errs;
}

// A while record ends in a flag byte: 0 for a plain while, 1 followed by
// the annotation (the counter, the count operand, the privates with their
// count, the reductions with theirs, each a local and an opcode code).  The
// annotation survives the round trip, a plain while is written as before
// the flag existed except for the flag itself, and a reader refuses a flag
// that is neither and a reduction by a reserved opcode.
TEST(PIRBinary, AParallelWhileKeepsItsAnnotation) {
  Program p = parse(kParallelProgram);
  ASSERT_EQ(p.Modules.size(), 1u);
  expectRoundTrip(p.Modules[0]);

  // `while { cond false } { }` over locals %k.0, %i.1 and %p.2, annotated
  // ` parallel %k.0, 8 privates(%i.1) reduce(%p.2 add)` or not.
  auto encodeLoop = [](bool annotated) {
    Function f;
    f.Name = "f";
    f.Sig.Ret = Type::Void;
    f.Locals.push_back({"k", Type::I64, 0, 0});
    f.Locals.push_back({"i", Type::I64, 0, 0});
    f.Locals.push_back({"p", Type::I64, 0, 0});
    While w;
    w.CondBlock = std::make_unique<Block>();
    w.Cond = Operand::boolean(false);
    w.Body = std::make_unique<Block>();
    if (annotated) {
      Parallel par;
      par.Counter = 0;
      par.Count = Operand::i64(8);
      par.Privates.push_back(1);
      par.Reductions.push_back({2, Opcode::Add});
      w.Par = std::move(par);
    }
    f.Body.Stmts.push_back(std::move(w));
    f.Body.Stmts.push_back(Return{});
    return binary::encode(oneFunction(std::move(f)));
  };
  const std::vector<uint8_t> plain = encodeLoop(false);
  const std::vector<uint8_t> par = encodeLoop(true);
  // The record: length, name, flags (4), sig (count 0, ret), conventions,
  // params (none), locals (3, each a name and a type), next id, the body's
  // count, the while's tag, its condition's count (0), its cond (a bool:
  // kind and value), its body's count (0), then the flag.
  const size_t rec = recordOf(plain, "f");
  ASSERT_EQ(rec, recordOf(par, "f"));
  const size_t flag =
      rec + 1 + 1 + 4 + 1 + 1 + 1 + 1 + 1 + 6 + 1 + 1 + 1 + 1 + 2 + 1;
  ASSERT_EQ(plain[rec], 26u);     // the record's length
  ASSERT_EQ(plain[rec + 18], 2u); // two statements
  ASSERT_EQ(plain[flag - 1], 0u); // the empty body
  ASSERT_EQ(plain[flag], 0u);
  ASSERT_EQ(par[flag], 1u);
  const std::vector<uint8_t> annotation = {
      0,    // the counter: local 0
      1, 8, // the count: an i64 constant (kind 1), 8
      1, 1, // one private: local 1
      1, 2, opcodeEntry(Opcode::Add).Code}; // one reduction: local 2, add
  std::vector<uint8_t> expected = plain;
  expected[flag] = 1;
  expected.insert(expected.begin() + static_cast<std::ptrdiff_t>(flag + 1),
                  annotation.begin(), annotation.end());
  expected[rec] = static_cast<uint8_t>(expected[rec] + annotation.size());
  EXPECT_EQ(par, expected);

  auto d = binary::decode(par);
  ASSERT_TRUE(d.isOk()) << d.status().message();
  ASSERT_EQ((*d).Functions.size(), 1u);
  const auto *w = std::get_if<While>(&(*d).Functions[0].Body.Stmts[0]);
  ASSERT_NE(w, nullptr);
  ASSERT_TRUE(w->Par.has_value());
  EXPECT_EQ(w->Par->Counter, 0u);
  ASSERT_TRUE(std::holds_alternative<int64_t>(w->Par->Count.V));
  EXPECT_EQ(std::get<int64_t>(w->Par->Count.V), 8);
  ASSERT_EQ(w->Par->Privates.size(), 1u);
  EXPECT_EQ(w->Par->Privates[0], 1u);
  ASSERT_EQ(w->Par->Reductions.size(), 1u);
  EXPECT_EQ(w->Par->Reductions[0].Partial, 2u);
  EXPECT_EQ(w->Par->Reductions[0].Op, Opcode::Add);
  EXPECT_NE(toString(*d).find(
                "  } parallel %k.0, 8 privates(%i.1) reduce(%p.2 add)\n"),
            std::string::npos)
      << toString(*d);
  EXPECT_TRUE(verify(*d).empty()) << formatErrors(verify(*d));
  EXPECT_EQ(binary::encode(*d), par);
  auto e = binary::decode(plain);
  ASSERT_TRUE(e.isOk()) << e.status().message();
  w = std::get_if<While>(&(*e).Functions[0].Body.Stmts[0]);
  ASSERT_NE(w, nullptr);
  EXPECT_FALSE(w->Par.has_value());
  EXPECT_NE(toString(*e).find("  }\n  ret\n"), std::string::npos)
      << toString(*e);

  // A flag that is neither 0 nor 1.
  {
    std::vector<uint8_t> b = par;
    b[flag] = 2;
    auto bad = binary::decode(b);
    ASSERT_FALSE(bad.isOk());
    EXPECT_NE(bad.status().message().find("flag byte is not 0 or 1"),
              std::string::npos)
        << bad.status().message();
  }
  // A reduction by a reserved opcode.
  const size_t reduce = flag + annotation.size();
  ASSERT_EQ(par[reduce], opcodeEntry(Opcode::Add).Code);
  {
    std::vector<uint8_t> b = par;
    b[reduce] = 65;
    auto bad = binary::decode(b);
    ASSERT_FALSE(bad.isOk());
    EXPECT_NE(bad.status().message().find("reserved opcode in a reduction"),
              std::string::npos)
        << bad.status().message();
  }
  // A reduction by an opcode that is no reduction decodes (the codec
  // writes what it reads) and the verifier refuses it.
  {
    std::vector<uint8_t> b = par;
    b[reduce] = opcodeEntry(Opcode::Sub).Code;
    auto sub = binary::decode(b);
    ASSERT_TRUE(sub.isOk()) << sub.status().message();
    EXPECT_NE(toString(*sub).find("reduce(%p.2 sub)"), std::string::npos)
        << toString(*sub);
    EXPECT_NE(formatErrors(verify(*sub))
                  .find("parallel reduction with 'sub' (only add and mul)"),
              std::string::npos)
        << formatErrors(verify(*sub));
    EXPECT_EQ(binary::encode(*sub), b);
  }
}

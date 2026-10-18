// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR `mem` slots (docs/pir.md §1, §6): the bytes of an aggregate in a local
// or a field, written and read through `ptr.add` / `ptr.load` / `ptr.store`,
// copied with `mem.copy` and cleared with `mem.zero`.  Hand-written PIR, run
// through the test backend.

#include "CodeGenTestUtils.h"

#include <gtest/gtest.h>

using namespace paykan::test;

// Every scalar type at its offset in a mem local survives a mem.copy to a
// second one, and mem.zero clears only the bytes it names.
TEST(MemSlot, LocalsHoldScalarsAtOffsets) {
  auto r = compileAndRunPIR(R"(module "t"
fn @main() -> i64 {
  local %a: mem(24, 8)
  local %b: mem(24, 8)
  %pa = local.addr %a
  %pb = local.addr %b
  mem.zero %pa, 24
  ptr.store %pa, 7
  %a1 = ptr.add %pa, 8
  ptr.store %a1, 2.5
  %a2 = ptr.add %pa, 16
  ptr.store %a2, 'x'
  %a3 = ptr.add %pa, 17
  ptr.store %a3, true
  mem.copy %pb, %pa, 24
  mem.zero %pa, 16
  %x = ptr.load i64, %pb
  %b1 = ptr.add %pb, 8
  %y = ptr.load f64, %b1
  %b2 = ptr.add %pb, 16
  %c = ptr.load char, %b2
  %b3 = ptr.add %pb, 17
  %t = ptr.load bool, %b3
  %z = ptr.load i64, %pa
  %kept = ptr.load char, %a2
  %y2 = mul %y, 2.0
  %yi = ftoi %y2
  %ci = cast %c to i64
  %ti = cast %t to i64
  %ki = cast %kept to i64
  %s1 = add %x, %yi
  %s2 = add %s1, %ci
  %s3 = add %s2, %ti
  %s4 = add %s3, %z
  %s5 = sub %s4, %ki
  ret %s5
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 7 + 5 + 'x' + 1 + 0 - 'x');
}

// A `cast` between i64 and f64 reinterprets the bits (the C backend's
// pkrt_f64_bits / pkrt_bits_f64): a float's bits stored in an integer slot
// come back as the same float, the sign of zero included.
TEST(MemSlot, FloatBitsRoundTripThroughAnInteger) {
  auto r = compileAndRunPIR(R"(module "t"
fn @main() -> i64 {
  local %a: mem(16, 8)
  %pa = local.addr %a
  %bits = cast 2.5 to i64
  ptr.store %pa, %bits
  %p1 = ptr.add %pa, 8
  %nzbits = cast -0.0 to i64
  ptr.store %p1, %nzbits
  %got = ptr.load i64, %pa
  %f = cast %got to f64
  %nz = ptr.load i64, %p1
  %g = cast %nz to f64
  %neg = cmp lt %nz, 0
  %zero = cmp eq %g, 0.0
  %f4 = mul %f, 4.0
  %n = ftoi %f4
  %one = cast %neg to i64
  %two = cast %zero to i64
  %s1 = add %n, %one
  %s2 = add %s1, %two
  ret %s2
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  // 2.5 * 4, the sign bit of -0.0 set in its bits, and -0.0 == 0.0.
  EXPECT_EQ(r.ExitCode, 10 + 1 + 1);
}

// mem.copy is a memmove: its source and destination may overlap.
TEST(MemSlot, CopyMayOverlap) {
  auto r = compileAndRunPIR(R"(module "t"
fn @main() -> i64 {
  local %m: mem(32, 8)
  %p = local.addr %m
  ptr.store %p, 1
  %p1 = ptr.add %p, 8
  ptr.store %p1, 2
  %p2 = ptr.add %p, 16
  ptr.store %p2, 3
  mem.copy %p1, %p, 24
  %w0 = ptr.load i64, %p
  %w1 = ptr.load i64, %p1
  %w2 = ptr.load i64, %p2
  %p3 = ptr.add %p, 24
  %w3 = ptr.load i64, %p3
  %h = mul %w0, 1000
  %t = mul %w1, 100
  %u = mul %w2, 10
  %s1 = add %h, %t
  %s2 = add %s1, %u
  %s3 = add %s2, %w3
  %r = sub %s3, 1000
  ret %r
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 123); // 1, 1, 2, 3
}

// A new object's mem fields are zero, and a mem field is laid out at its
// alignment between its neighbours, which keep their values.
TEST(MemSlot, FieldsKeepTheirNeighbours) {
  LeakGuard g;
  auto r = compileAndRunPIR(R"(module "t"
class C {
  field a: char
  field buf: mem(3, 1)
  field n: i64
  field w: mem(16, 8)
  field b: bool
  vtable {
    destroy = @C.destroy : (obj) -> void
  }
}
fn @C.destroy(%self: obj) -> void {
  ret
}
fn @main() -> i64 {
  %o = new C
  %pb = field.addr %o, C.buf
  %pw = field.addr %o, C.w
  %pb2 = ptr.add %pb, 2
  %pw1 = ptr.add %pw, 8
  %zb = ptr.load char, %pb2
  %zw = ptr.load i64, %pw1
  field.store %o, C.a, 'a'
  field.store %o, C.n, 1000
  field.store %o, C.b, true
  ptr.store %pb, 'p'
  ptr.store %pb2, 'q'
  ptr.store %pw, 5
  ptr.store %pw1, 6
  %a = field.load %o, C.a
  %n = field.load %o, C.n
  %b = field.load %o, C.b
  %b0 = ptr.load char, %pb
  %b2 = ptr.load char, %pb2
  %w0 = ptr.load i64, %pw
  %w1 = ptr.load i64, %pw1
  free %o
  %e0 = cmp ne %a, 'a'
  %e1 = cmp ne %n, 1000
  %e2 = not %b
  %e3 = cmp ne %b0, 'p'
  %e4 = cmp ne %b2, 'q'
  %e5 = cmp ne %w0, 5
  %e6 = cmp ne %w1, 6
  %e7 = cmp ne %zb, '\0'
  %e8 = cmp ne %zw, 0
  %i0 = cast %e0 to i64
  %i1 = cast %e1 to i64
  %i2 = cast %e2 to i64
  %i3 = cast %e3 to i64
  %i4 = cast %e4 to i64
  %i5 = cast %e5 to i64
  %i6 = cast %e6 to i64
  %i7 = cast %e7 to i64
  %i8 = cast %e8 to i64
  %s1 = add %i0, %i1
  %s2 = add %s1, %i2
  %s3 = add %s2, %i3
  %s4 = add %s3, %i4
  %s5 = add %s4, %i5
  %s6 = add %s5, %i6
  %s7 = add %s6, %i7
  %s8 = add %s7, %i8
  ret %s8
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << "mismatches";
  g.expectNoLeaks("FieldsKeepTheirNeighbours");
}

// A box lives in a mem slot like any other slot: zeroed is null, and the ARC
// around it stays explicit.
TEST(MemSlot, HoldsBoxes) {
  LeakGuard g;
  auto r = compileAndRunPIR(R"(module "t"
cstr @.s = "hi" len 2
extern fn @$rt.PaykanString_new(ptr, i64) -> obj
extern fn @$rt.Paykan_println(obj) -> void
fn @main() -> i64 {
  local %m: mem(16, 8)
  %p = local.addr %m
  mem.zero %p, 16
  %s = call @$rt.PaykanString_new(@.s, 2)
  %b = box %s
  %p1 = ptr.add %p, 8
  ptr.store %p1, %b
  %b2 = ptr.load box, %p1
  %o = unbox %b2
  call @$rt.Paykan_println(%o)
  release %b2
  %n = ptr.load box, %p
  %isnull = cmp eq %n, null box
  %r = select %isnull, 0, 1
  ret %r
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "hi\n");
  g.expectNoLeaks("HoldsBoxes");
}

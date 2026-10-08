// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: `view` and `inout` parameters, and the PIR address ops an
// `inout` parameter lowers to (local.addr, field.addr, ptr.load, ptr.store).

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// -- PIR address ops

// A callee reads and writes its caller's locals and an object's fields in
// place through the addresses it is given: every scalar type, a local and a
// field.
TEST(ParamMode, AddressOpsReadAndWriteTheCallersSlots) {
  LeakGuard guard;
  auto r = compileAndRunPIR(R"(module "t"
class C {
  field n: i64
  field f: f64
  vtable {
    destroy = @C.destroy : (obj) -> void
  }
}

fn @C.destroy(%self: obj) -> void {
  free %self
  ret
}

fn @bump(%n: ptr, %f: ptr, %b: ptr, %c: ptr) -> void {
  %v = ptr.load i64, %n
  %w = add %v, 1
  ptr.store %n, %w
  %x = ptr.load f64, %f
  %y = add %x, 1.5
  ptr.store %f, %y
  %bv = ptr.load bool, %b
  %nb = not %bv
  ptr.store %b, %nb
  %cv = ptr.load char, %c
  %ci = cast %cv to i64
  %cn = add %ci, 1
  %cc = cast %cn to char
  ptr.store %c, %cc
  ret
}

fn @main() -> i64 {
  local %k: i64
  local %g: f64
  local %t: bool
  local %ch: char
  store %k, 1
  store %g, 1.5
  store %t, false
  store %ch, 'a'
  %o = new C
  field.store %o, C.n, 40
  %pk = local.addr %k
  %pg = local.addr %g
  %pt = local.addr %t
  %pc = local.addr %ch
  call @bump(%pk, %pg, %pt, %pc)
  %pn = field.addr %o, C.n
  %pf = field.addr %o, C.f
  call @bump(%pn, %pf, %pt, %pc)
  %k2 = load %k
  %g2 = load %g
  %t2 = load %t
  %ch2 = load %ch
  %n2 = field.load %o, C.n
  %f2 = field.load %o, C.f
  free %o
  %gi = ftoi %g2
  %fi = ftoi %f2
  %ci2 = cast %ch2 to i64
  %c2 = sub %ci2, 97
  %tb = select %t2, 10, 0
  %s1 = add %k2, %n2
  %s2 = add %s1, %gi
  %s3 = add %s2, %fi
  %s4 = add %s3, %c2
  %s5 = add %s4, %tb
  ret %s5
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  // k 2 + n 41 + g 3 + f 1 + 'c' - 'a' 2 + t false 0.  The sum stays below
  // 128: the C backend's runner reads a higher exit status as a signal.
  EXPECT_EQ(r.ExitCode, 49) << r.StdErr;
  guard.expectNoLeaks("address ops");
}

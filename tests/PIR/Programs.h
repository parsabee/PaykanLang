// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR text shared by the PIR test files.

#pragma once

namespace paykan::pir::test {

// A program exercising every item and instruction of docs/pir.md.
inline const char *const kProgram = R"(module "main"
cstr @.str0 = "Hello\n" len 6
data @.d0 = [1, -2, 3]
bytes @.b0 = [0, 4]
extern obj @$rt.PaykanObject_None
extern vtable @$rt.PaykanArray_vtable

class Point {
  field x: i64
  field name: box
  vtable {
    destroy = @Point_destroy : (obj) -> void
    toString = @$rt.PaykanObject_toString : (obj) -> box
    equals = @$rt.PaykanObject_equals : (obj, box) -> i64
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

extern fn @$rt.PaykanString_new(ptr, i64) -> obj
extern fn @$rt.Paykan_println(obj) -> void
extern fn @$rt.PaykanString_destroy(obj) -> void
extern fn @$rt.PaykanObject_toString(obj) -> box
extern fn @$rt.PaykanObject_equals(obj, box) -> i64
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
  %s.1 = call @$rt.PaykanString_new(@.str0, 6)
  call @$rt.Paykan_println(%s.1)
  call @$rt.PaykanString_destroy(%s.1)
  %p.2 = new Point
  field.store %p.2, Point.x, 7
  field.store %p.2, Point.name, null box
  %vt.3 = vtable.addr Point
  %vt2.4 = vtable.addr @$rt.PaykanArray_vtable
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

} // namespace paykan::pir::test

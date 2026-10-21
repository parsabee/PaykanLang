// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The `pir` dialect: PIR (docs/pir.md) as MLIR.  One op per PIR item,
// statement and instruction, with PIR's types, so a PIR module imports one
// to one (PIRImport.cpp) and `--emit-mlir=pir` shows the program as the
// lowering made it.  The shared lowering (docs/design/mlir-backend.md)
// rewrites it into the upstream dialects both targets reach: func, scf and
// arith, plus the few pir memory ops no upstream dialect lowers to both LLVM
// and EmitC.
//
// Every region of a pir op is a structured block, like PIR's: a `pir.func`
// body, the branches of `pir.if`, the condition and body of `pir.while`.
// They have no terminators: `pir.break`, `pir.continue`, `pir.return` and
// `pir.unreachable` end a block as statements do in PIR, and may be nested
// in `pir.if`s.  PIR's verifier has checked the program before it is
// imported.

#ifndef PAYKAN_PIR_TD
#define PAYKAN_PIR_TD

include "mlir/IR/AttrTypeBase.td"
include "mlir/IR/EnumAttr.td"
include "mlir/IR/OpAsmInterface.td"
include "mlir/IR/OpBase.td"
include "mlir/IR/SymbolInterfaces.td"
include "mlir/Interfaces/FunctionInterfaces.td"
include "mlir/Interfaces/InferTypeOpInterface.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

//===----------------------------------------------------------------------===//
// Dialect
//===----------------------------------------------------------------------===//

def PIR_Dialect : Dialect {
  let name = "pir";
  let summary = "The Paykan intermediate representation";
  let cppNamespace = "::paykan::pir_dialect";
  let useDefaultTypePrinterParser = 1;
}

class PIR_Op<string mnemonic, list<Trait> traits = []>
    : Op<PIR_Dialect, mnemonic, traits>;

//===----------------------------------------------------------------------===//
// Types (docs/pir.md §1): i64, f64, bool (i1) and char (i8) are the builtin
// integer and float types; the rest are these.
//===----------------------------------------------------------------------===//

class PIR_Type<string name, string typeMnemonic, list<Trait> traits = []>
    : TypeDef<PIR_Dialect, name, traits> {
  let mnemonic = typeMnemonic;
}

def PIR_BoxType : PIR_Type<"Box", "box"> {
  let summary = "a strong reference to an object (PaykanShared*), may be null";
}
def PIR_ObjType : PIR_Type<"Obj", "obj"> {
  let summary = "an unboxed object (PaykanObject*)";
}
def PIR_PtrType : PIR_Type<"Ptr", "ptr"> {
  let summary = "any other pointer: a vtable, literal data, a slot's address";
}
def PIR_WeakType : PIR_Type<"Weak", "weak"> {
  let summary = "a weak reference (PaykanWeak*), may be null";
}
def PIR_OptType : PIR_Type<"Opt", "opt"> {
  let summary = "an inline optional of i64, f64, bool or char";
  let parameters = (ins "::mlir::Type":$payload);
  let assemblyFormat = "`<` $payload `>`";
}
def PIR_MemType : PIR_Type<"Mem", "mem"> {
  let summary = "N bytes aligned to A: the type of a local or a field only";
  let parameters = (ins "int64_t":$size, "int64_t":$align);
  let assemblyFormat = "`<` $size `,` $align `>`";
}
def PIR_LocalType : PIR_Type<"Local", "local"> {
  let summary = "a function's `local` slot holding a value of the slot type";
  let parameters = (ins "::mlir::Type":$slot);
  let assemblyFormat = "`<` $slot `>`";
}

def PIR_Bool : I<1>;
def PIR_Char : I<8>;
def PIR_Ref : AnyTypeOf<[PIR_BoxType, PIR_ObjType, PIR_PtrType]>;
def PIR_Scalar : AnyTypeOf<[I64, F64, PIR_Bool, PIR_Char]>;
def PIR_Value : AnyTypeOf<[I64, F64, PIR_Bool, PIR_Char, PIR_BoxType,
                           PIR_ObjType, PIR_PtrType, PIR_WeakType,
                           PIR_OptType]>;
def PIR_Number : AnyTypeOf<[I64, F64]>;

//===----------------------------------------------------------------------===//
// Module-level items (docs/pir.md §4, §5)
//===----------------------------------------------------------------------===//

def PIR_CStrOp : PIR_Op<"cstr", [Symbol]> {
  let summary = "NUL-terminated literal data (`cstr`), its address a `ptr`";
  let arguments = (ins SymbolNameAttr:$sym_name, StrAttr:$value);
  let assemblyFormat = "$sym_name `=` $value attr-dict";
}

def PIR_DataOp : PIR_Op<"data", [Symbol]> {
  let summary = "constant i64 words (`data`), their address a `ptr`";
  let arguments = (ins SymbolNameAttr:$sym_name, DenseI64ArrayAttr:$words);
  let assemblyFormat = "$sym_name `=` $words attr-dict";
}

def PIR_BytesOp : PIR_Op<"bytes", [Symbol]> {
  let summary = "constant bytes (`bytes`), their address a `ptr`";
  let arguments = (ins SymbolNameAttr:$sym_name, DenseI8ArrayAttr:$bytes);
  let assemblyFormat = "$sym_name `=` $bytes attr-dict";
}

def PIR_ExternObjectOp : PIR_Op<"extern_object", [Symbol]> {
  let summary = "a runtime object singleton (`extern obj`), its address an `obj`";
  let arguments = (ins SymbolNameAttr:$sym_name);
  let assemblyFormat = "$sym_name attr-dict";
}

def PIR_ExternVTableOp : PIR_Op<"extern_vtable", [Symbol]> {
  let summary = "a runtime vtable (`extern vtable`), its address a `ptr`";
  let arguments = (ins SymbolNameAttr:$sym_name);
  let assemblyFormat = "$sym_name attr-dict";
}

def PIR_ClassOp : PIR_Op<"class", [NoTerminator, SingleBlock,
                                   NoRegionArguments]> {
  let summary = "a class: its complete layout and vtable (docs/pir.md §5)";
  let description = [{
    The body holds the fields (`pir.field`, ancestors first, in layout order)
    and the vtable slots (`pir.slot`, in slot order).  An `extern` class is
    defined by the module `module` names: only its layout is known here.  A
    class is named by a string, not a symbol: a class and its constructor
    function share their name.
  }];
  let arguments = (ins StrAttr:$name, OptionalAttr<StrAttr>:$super,
                       UnitAttr:$is_extern, OptionalAttr<StrAttr>:$module);
  let regions = (region SizedRegion<1>:$body);
  let assemblyFormat = [{
    $name (`:` $super^)? (`extern` $is_extern^)? (`module` $module^)?
    attr-dict-with-keyword $body
  }];
}

def PIR_FieldOp : PIR_Op<"field", [HasParent<"ClassOp">]> {
  let summary = "a field of a class: i64 f64 bool char box weak, an optional or mem";
  let arguments = (ins StrAttr:$name, TypeAttr:$type);
  let assemblyFormat = "$name `:` $type attr-dict";
}

def PIR_SlotOp : PIR_Op<"slot", [HasParent<"ClassOp">]> {
  let summary = "a vtable slot: its function (none for an abstract slot)";
  let arguments = (ins StrAttr:$name, OptionalAttr<FlatSymbolRefAttr>:$target,
                       TypeAttrOf<FunctionType>:$signature);
  let assemblyFormat = "$name (`=` $target^)? `:` $signature attr-dict";
}

def PIR_FuncOp : PIR_Op<"func", [FunctionOpInterface, IsolatedFromAbove,
                                 NoTerminator, OpAsmOpInterface]> {
  let summary = "a function (`fn`), or a declaration (`extern fn`)";
  let description = [{
    A declaration has no body: a runtime function (`@$rt.<symbol>`), a native
    one (`@$c.<symbol>`), or one of another PIR module of the program, which
    `module` names (`link_symbol` is its name there when that differs).
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                       TypeAttrOf<FunctionType>:$function_type,
                       OptionalAttr<StrAttr>:$sym_visibility,
                       OptionalAttr<DictArrayAttr>:$arg_attrs,
                       OptionalAttr<DictArrayAttr>:$res_attrs,
                       OptionalAttr<StrAttr>:$module,
                       OptionalAttr<StrAttr>:$link_symbol);
  let regions = (region AnyRegion:$body);
  let extraClassDeclaration = [{
    ::mlir::Region *getCallableRegion() {
      return isExternal() ? nullptr : &getBody();
    }
    ::llvm::ArrayRef<::mlir::Type> getArgumentTypes() {
      return getFunctionType().getInputs();
    }
    ::llvm::ArrayRef<::mlir::Type> getResultTypes() {
      return getFunctionType().getResults();
    }
    static ::llvm::StringRef getDefaultDialect() { return "pir"; }
    bool isDeclaration() { return isExternal(); }
  }];
  let hasCustomAssemblyFormat = 1;
}

//===----------------------------------------------------------------------===//
// Structured statements (docs/pir.md §6)
//===----------------------------------------------------------------------===//

def PIR_IfOp : PIR_Op<"if", [NoTerminator, NoRegionArguments,
                             RecursiveMemoryEffects, OpAsmOpInterface]> {
  let summary = "`if %c { ... } else { ... }`; the else region may be empty";
  let arguments = (ins PIR_Bool:$cond);
  let regions = (region MaxSizedRegion<1>:$then_region,
                        MaxSizedRegion<1>:$else_region);
  let assemblyFormat = [{
    $cond $then_region (`else` $else_region^)? attr-dict
  }];
  let extraClassDeclaration = [{
    static ::llvm::StringRef getDefaultDialect() { return "pir"; }
  }];
}

def PIR_WhileOp : PIR_Op<"while", [NoTerminator, NoRegionArguments,
                                   RecursiveMemoryEffects, OpAsmOpInterface]> {
  let summary = "`while { ...; cond %c } { ... }`";
  let description = [{
    The condition region runs first and ends with `pir.cond`, whose operand
    decides whether the body runs.  `pir.break` leaves the innermost loop,
    `pir.continue` re-enters its condition region.
  }];
  let regions = (region SizedRegion<1>:$cond_region,
                        MaxSizedRegion<1>:$body_region);
  let assemblyFormat = "$cond_region `do` $body_region attr-dict";
  let extraClassDeclaration = [{
    static ::llvm::StringRef getDefaultDialect() { return "pir"; }
  }];
}

def PIR_CondOp : PIR_Op<"cond", [HasParent<"WhileOp">]> {
  let summary = "the value a `pir.while` condition region ends with";
  let arguments = (ins PIR_Bool:$cond);
  let assemblyFormat = "$cond attr-dict";
}

def PIR_BreakOp : PIR_Op<"break"> {
  let summary = "leave the innermost `pir.while`";
  let assemblyFormat = "attr-dict";
}

def PIR_ContinueOp : PIR_Op<"continue"> {
  let summary = "re-enter the innermost `pir.while`'s condition region";
  let assemblyFormat = "attr-dict";
}

def PIR_ReturnOp : PIR_Op<"return"> {
  let summary = "return from the function, with its value or none";
  let arguments = (ins Optional<PIR_Value>:$value);
  let assemblyFormat = "($value^ `:` type($value))? attr-dict";
}

def PIR_UnreachableOp : PIR_Op<"unreachable"> {
  let summary = "nothing after a noreturn call (a panic) runs";
  let assemblyFormat = "attr-dict";
}

//===----------------------------------------------------------------------===//
// Constants (docs/pir.md §2): integer and float ones are arith.constant
//===----------------------------------------------------------------------===//

def PIR_NullOp : PIR_Op<"null", [Pure]> {
  let summary = "a null box, obj, ptr or weak";
  let results = (outs AnyTypeOf<[PIR_BoxType, PIR_ObjType, PIR_PtrType,
                                 PIR_WeakType]>:$result);
  let assemblyFormat = "attr-dict `:` type($result)";
}

def PIR_AddressOfOp : PIR_Op<"addressof", [Pure]> {
  let summary = "the address of a module-level symbol (`@sym`)";
  let description = [{
    A cstr, data or bytes global or an extern vtable is a `ptr`, an extern
    object an `obj`.  (PR 3's lowering also takes functions' and vtables'
    addresses with it.)
  }];
  let arguments = (ins FlatSymbolRefAttr:$symbol);
  let results = (outs AnyTypeOf<[PIR_PtrType, PIR_ObjType]>:$result);
  let assemblyFormat = "$symbol attr-dict `:` type($result)";
}

//===----------------------------------------------------------------------===//
// Arithmetic and comparison
//===----------------------------------------------------------------------===//

class PIR_BinaryOp<string mnemonic, Type type>
    : PIR_Op<mnemonic, [Pure, SameOperandsAndResultType]> {
  let arguments = (ins type:$lhs, type:$rhs);
  let results = (outs type:$result);
  let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
}

def PIR_AddOp : PIR_BinaryOp<"add", PIR_Number>;
def PIR_SubOp : PIR_BinaryOp<"sub", PIR_Number>;
def PIR_MulOp : PIR_BinaryOp<"mul", PIR_Number>;
def PIR_DivOp : PIR_BinaryOp<"div", PIR_Number>;
def PIR_RemOp : PIR_BinaryOp<"rem", PIR_Number>;
def PIR_UDivOp : PIR_BinaryOp<"udiv", I64>;
def PIR_URemOp : PIR_BinaryOp<"urem", I64>;

def PIR_NegOp : PIR_Op<"neg", [Pure, SameOperandsAndResultType]> {
  let arguments = (ins PIR_Number:$value);
  let results = (outs PIR_Number:$result);
  let assemblyFormat = "$value attr-dict `:` type($result)";
}

def PIR_NotOp : PIR_Op<"not", [Pure]> {
  let arguments = (ins PIR_Bool:$value);
  let results = (outs PIR_Bool:$result);
  let assemblyFormat = "$value attr-dict";
}

def PIR_CmpPredicate : I64EnumAttr<"CmpPredicate", "a comparison", [
  I64EnumAttrCase<"eq", 0>, I64EnumAttrCase<"ne", 1>,
  I64EnumAttrCase<"lt", 2>, I64EnumAttrCase<"le", 3>,
  I64EnumAttrCase<"gt", 4>, I64EnumAttrCase<"ge", 5>]> {
  let cppNamespace = "::paykan::pir_dialect";
}

def PIR_CmpOp : PIR_Op<"cmp", [Pure, AllTypesMatch<["lhs", "rhs"]>]> {
  let summary = "a signed (or float, bool, char, pointer) comparison";
  let arguments = (ins PIR_CmpPredicate:$predicate,
                       AnyTypeOf<[I64, F64, PIR_Bool, PIR_Char, PIR_BoxType,
                                  PIR_ObjType, PIR_PtrType]>:$lhs,
                       AnyTypeOf<[I64, F64, PIR_Bool, PIR_Char, PIR_BoxType,
                                  PIR_ObjType, PIR_PtrType]>:$rhs);
  let results = (outs PIR_Bool:$result);
  let assemblyFormat = "$predicate $lhs `,` $rhs attr-dict `:` type($lhs)";
}

def PIR_UCmpOp : PIR_Op<"ucmp", [Pure]> {
  let summary = "an unsigned comparison of two i64";
  let arguments = (ins PIR_CmpPredicate:$predicate, I64:$lhs, I64:$rhs);
  let results = (outs PIR_Bool:$result);
  let assemblyFormat = "$predicate $lhs `,` $rhs attr-dict";
}

def PIR_SelectOp : PIR_Op<"select", [Pure,
    AllTypesMatch<["true_value", "false_value", "result"]>]> {
  let arguments = (ins PIR_Bool:$cond, PIR_Value:$true_value,
                       PIR_Value:$false_value);
  let results = (outs PIR_Value:$result);
  let assemblyFormat = [{
    $cond `,` $true_value `,` $false_value attr-dict `:` type($result)
  }];
}

class PIR_ConvertOp<string mnemonic, Type from, Type to>
    : PIR_Op<mnemonic, [Pure]> {
  let arguments = (ins from:$value);
  let results = (outs to:$result);
  let assemblyFormat = "$value attr-dict";
}

def PIR_IToFOp : PIR_ConvertOp<"itof", I64, F64>;
def PIR_FToIOp : PIR_ConvertOp<"ftoi", F64, I64>;
def PIR_UToFOp : PIR_ConvertOp<"utof", I64, F64>;
def PIR_FToUOp : PIR_ConvertOp<"ftou", F64, I64>;
def PIR_FRound32Op : PIR_ConvertOp<"fround32", F64, F64>;

def PIR_CastOp : PIR_Op<"cast", [Pure]> {
  let summary = "reinterpret or resize (the `cast` table of docs/pir.md §6)";
  let arguments = (ins PIR_Value:$value);
  let results = (outs PIR_Value:$result);
  let assemblyFormat = "$value attr-dict `:` type($value) `to` type($result)";
}

class PIR_BitsOp<string mnemonic> : PIR_Op<mnemonic, [Pure]> {
  let arguments = (ins I64:$value, I64Attr:$bits);
  let results = (outs I64:$result);
  let assemblyFormat = "$value `,` $bits attr-dict";
}

def PIR_WrapOp : PIR_BitsOp<"wrap"> {
  let summary = "the low `bits` bits, sign-extended";
}
def PIR_MaskOp : PIR_BitsOp<"mask"> {
  let summary = "the low `bits` bits, zero-extended";
}

//===----------------------------------------------------------------------===//
// Calls
//===----------------------------------------------------------------------===//

def PIR_CallOp : PIR_Op<"call"> {
  let arguments = (ins FlatSymbolRefAttr:$callee, Variadic<PIR_Value>:$args);
  let results = (outs Optional<PIR_Value>:$result);
  let assemblyFormat = [{
    $callee `(` $args `)` attr-dict `:` functional-type($args, results)
  }];
}

def PIR_VCallOp : PIR_Op<"vcall"> {
  let summary = "call a vtable slot of the receiver, args[0] (an obj)";
  let arguments = (ins StrAttr:$class_name, I64Attr:$slot,
                       Variadic<PIR_Value>:$args);
  let results = (outs Optional<PIR_Value>:$result);
  let assemblyFormat = [{
    $class_name `[` $slot `]` `(` $args `)` attr-dict `:`
    functional-type($args, results)
  }];
}

def PIR_ICallOp : PIR_Op<"icall"> {
  let summary = "call the function whose address is `callee`";
  let arguments = (ins PIR_PtrType:$callee, Variadic<PIR_Value>:$args);
  let results = (outs Optional<PIR_Value>:$result);
  let assemblyFormat = [{
    $callee `(` $args `)` attr-dict `:` functional-type($args, results)
  }];
}

def PIR_FnAddrOp : PIR_Op<"fn_addr", [Pure]> {
  let summary = "a function's address";
  let arguments = (ins FlatSymbolRefAttr:$callee);
  let results = (outs PIR_PtrType:$result);
  let assemblyFormat = "$callee attr-dict";
}

//===----------------------------------------------------------------------===//
// ARC and objects
//===----------------------------------------------------------------------===//

def PIR_RetainOp : PIR_Op<"retain"> {
  let arguments = (ins PIR_BoxType:$box);
  let assemblyFormat = "$box attr-dict";
}

def PIR_ReleaseOp : PIR_Op<"release"> {
  let arguments = (ins PIR_BoxType:$box);
  let assemblyFormat = "$box attr-dict";
}

def PIR_BoxOp : PIR_Op<"box"> {
  let summary = "the box of an object (PaykanShared_new)";
  let arguments = (ins PIR_ObjType:$obj);
  let results = (outs PIR_BoxType:$result);
  let assemblyFormat = "$obj attr-dict";
}

def PIR_UnboxOp : PIR_Op<"unbox", [Pure]> {
  let summary = "a box's object";
  let arguments = (ins PIR_BoxType:$box);
  let results = (outs PIR_ObjType:$result);
  let assemblyFormat = "$box attr-dict";
}

def PIR_NewOp : PIR_Op<"new"> {
  let summary = "allocate an object of a class, its header and fields zeroed";
  let arguments = (ins StrAttr:$class_name);
  let results = (outs PIR_ObjType:$result);
  let assemblyFormat = "$class_name attr-dict";
}

def PIR_FreeOp : PIR_Op<"free"> {
  let arguments = (ins PIR_ObjType:$obj);
  let assemblyFormat = "$obj attr-dict";
}

def PIR_FieldLoadOp : PIR_Op<"field_load"> {
  let arguments = (ins PIR_ObjType:$obj, StrAttr:$class_name, StrAttr:$field);
  let results = (outs PIR_Value:$result);
  let assemblyFormat = [{
    $obj `,` $class_name `[` $field `]` attr-dict `:` type($result)
  }];
}

def PIR_FieldStoreOp : PIR_Op<"field_store"> {
  let arguments = (ins PIR_ObjType:$obj, StrAttr:$class_name, StrAttr:$field,
                       PIR_Value:$value);
  let assemblyFormat = [{
    $value `,` $obj `,` $class_name `[` $field `]` attr-dict `:` type($value)
  }];
}

def PIR_FieldAddrOp : PIR_Op<"field_addr", [Pure]> {
  let arguments = (ins PIR_ObjType:$obj, StrAttr:$class_name, StrAttr:$field);
  let results = (outs PIR_PtrType:$result);
  let assemblyFormat = "$obj `,` $class_name `[` $field `]` attr-dict";
}

def PIR_VTableLoadOp : PIR_Op<"vtable_load", [Pure]> {
  let summary = "an object's vtable pointer";
  let arguments = (ins PIR_ObjType:$obj);
  let results = (outs PIR_PtrType:$result);
  let assemblyFormat = "$obj attr-dict";
}

def PIR_VTableAddrOp : PIR_Op<"vtable_addr", [Pure]> {
  let summary = "a class's vtable address, its runtime type identity";
  let arguments = (ins StrAttr:$class_name);
  let results = (outs PIR_PtrType:$result);
  let assemblyFormat = "$class_name attr-dict";
}

//===----------------------------------------------------------------------===//
// Locals and addresses
//===----------------------------------------------------------------------===//

def PIR_LocalOp : PIR_Op<"local", [
    DeclareOpInterfaceMethods<OpAsmOpInterface, ["getAsmResultNames"]>]> {
  let summary = "a function's mutable slot (`local`), uninitialised";
  let description = [{
    Printed with the local's name (`%n = pir.local "n"`).
  }];
  let arguments = (ins StrAttr:$name);
  let results = (outs PIR_LocalType:$slot);
  let assemblyFormat = "$name attr-dict `:` qualified(type($slot))";
}

def PIR_LoadOp : PIR_Op<"load", [
    TypesMatchWith<"the local holds the result", "slot", "result",
                   "::llvm::cast<LocalType>($_self).getSlot()">]> {
  let arguments = (ins PIR_LocalType:$slot);
  let results = (outs PIR_Value:$result);
  let assemblyFormat = "$slot attr-dict `:` qualified(type($slot))";
}

def PIR_StoreOp : PIR_Op<"store", [
    TypesMatchWith<"the local holds the value", "slot", "value",
                   "::llvm::cast<LocalType>($_self).getSlot()">]> {
  let arguments = (ins PIR_Value:$value, PIR_LocalType:$slot);
  let assemblyFormat = "$value `,` $slot attr-dict `:` qualified(type($slot))";
}

def PIR_LocalAddrOp : PIR_Op<"local_addr", [Pure]> {
  let arguments = (ins PIR_LocalType:$slot);
  let results = (outs PIR_PtrType:$result);
  let assemblyFormat = "$slot attr-dict `:` qualified(type($slot))";
}

def PIR_PtrLoadOp : PIR_Op<"ptr_load"> {
  let summary = "read a value through an address";
  let arguments = (ins PIR_PtrType:$ptr);
  let results = (outs PIR_Value:$result);
  let assemblyFormat = "$ptr attr-dict `:` type($result)";
}

def PIR_PtrStoreOp : PIR_Op<"ptr_store"> {
  let summary = "write a value through an address";
  let arguments = (ins PIR_PtrType:$ptr, PIR_Value:$value);
  let assemblyFormat = "$value `,` $ptr attr-dict `:` type($value)";
}

def PIR_PtrAddOp : PIR_Op<"ptr_add", [Pure]> {
  let summary = "an address plus a byte offset";
  let arguments = (ins PIR_PtrType:$ptr, I64:$offset);
  let results = (outs PIR_PtrType:$result);
  let assemblyFormat = "$ptr `,` $offset attr-dict";
}

def PIR_MemCopyOp : PIR_Op<"memcopy"> {
  let summary = "copy bytes; the two ranges may overlap";
  let arguments = (ins PIR_PtrType:$dst, PIR_PtrType:$src, I64:$count);
  let assemblyFormat = "$dst `,` $src `,` $count attr-dict";
}

def PIR_MemZeroOp : PIR_Op<"memzero"> {
  let arguments = (ins PIR_PtrType:$dst, I64:$count);
  let assemblyFormat = "$dst `,` $count attr-dict";
}

//===----------------------------------------------------------------------===//
// Inline optionals
//===----------------------------------------------------------------------===//

def PIR_SomeOp : PIR_Op<"some", [Pure,
    TypesMatchWith<"the optional's payload is the value", "result", "value",
                   "::llvm::cast<OptType>($_self).getPayload()">]> {
  let arguments = (ins PIR_Scalar:$value);
  let results = (outs PIR_OptType:$result);
  let assemblyFormat = "$value attr-dict `:` qualified(type($result))";
}

def PIR_NoneOp : PIR_Op<"none", [Pure]> {
  let results = (outs PIR_OptType:$result);
  let assemblyFormat = "attr-dict `:` qualified(type($result))";
}

def PIR_IsSomeOp : PIR_Op<"is_some", [Pure]> {
  let arguments = (ins PIR_OptType:$value);
  let results = (outs PIR_Bool:$result);
  let assemblyFormat = "$value attr-dict `:` qualified(type($value))";
}

def PIR_UnwrapOp : PIR_Op<"unwrap", [Pure,
    TypesMatchWith<"the result is the optional's payload", "value", "result",
                   "::llvm::cast<OptType>($_self).getPayload()">]> {
  let summary = "an optional's payload (zero for an absent one)";
  let arguments = (ins PIR_OptType:$value);
  let results = (outs PIR_Scalar:$result);
  let assemblyFormat = "$value attr-dict `:` qualified(type($value))";
}

//===----------------------------------------------------------------------===//
// Weak references
//===----------------------------------------------------------------------===//

def PIR_WeakNewOp : PIR_Op<"weak_new"> {
  let arguments = (ins PIR_BoxType:$box);
  let results = (outs PIR_WeakType:$result);
  let assemblyFormat = "$box attr-dict";
}

def PIR_WeakLoadOp : PIR_Op<"weak_load"> {
  let summary = "a +1 box of the object, or null once it is destroyed";
  let arguments = (ins PIR_WeakType:$weak);
  let results = (outs PIR_BoxType:$result);
  let assemblyFormat = "$weak attr-dict";
}

def PIR_WeakReleaseOp : PIR_Op<"weak_release"> {
  let arguments = (ins PIR_WeakType:$weak);
  let assemblyFormat = "$weak attr-dict";
}

#endif // PAYKAN_PIR_TD

#ifndef DIGON_LOWER_H
#define DIGON_LOWER_H

#include <cstdint>

#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "intern.h"

// digon's mid-IR: a small, LLVM-free intermediate form between front and back
// ends. A function is a list of basic blocks; each block is an ordered list of
// instruction ids from the function's flat pool. Instruction k's result is
// "value k" (its pool index); operands reference instruction ids. Control flow
// uses Br / CondBr with block-index targets. Mutable state crosses blocks via
// stack slots (Alloca/Load/Store), so no phi nodes are needed.

constexpr uint32_t M_NO_VALUE = 0xffffffffu;

enum class MTypeKind : uint8_t { Void, Bool, Int, Float, Ptr, Struct, Ref, Enum };

struct MType {
    MTypeKind kind;
    uint16_t  bits;         // Int: 8/16/32/64; Float: 32/64; else unused
    bool      is_signed;    // Int only
    uint32_t  struct_index; // Struct / Ref-to-struct: index into MModule.structs.
                            // Enum: index into MModule.enums.
};

inline MType mt_void()          { return MType{MTypeKind::Void, 0, false, 0}; }
inline MType mt_bool()          { return MType{MTypeKind::Bool, 1, false, 0}; }
inline MType mt_ptr()           { return MType{MTypeKind::Ptr, 0, false, 0}; }
inline MType mt_int(uint16_t b, bool s) { return MType{MTypeKind::Int, b, s, 0}; }
inline MType mt_float(uint16_t b)       { return MType{MTypeKind::Float, b, false, 0}; }
inline MType mt_struct(uint32_t idx)    { return MType{MTypeKind::Struct, 0, false, idx}; }
inline MType mt_ref(uint32_t struct_idx){ return MType{MTypeKind::Ref, 0, false, struct_idx}; }
inline MType mt_enum(uint32_t idx)      { return MType{MTypeKind::Enum, 0, false, idx}; }

enum class MOp : uint8_t {
    ConstInt, ConstFloat, ConstBool, ConstCStr,
    Param,
    Alloca, Load, Store,
    // integer arithmetic / bitwise
    Add, Sub, Mul, SDiv, UDiv, SRem, URem,
    And, Or, Xor, Shl, AShr, LShr,
    // float arithmetic
    FAdd, FSub, FMul, FDiv,
    // integer comparisons (result Bool). Eq/Ne are sign-agnostic; ordered
    // comparisons have signed (ICmp*) and unsigned (ICmpU*) forms.
    ICmpEq, ICmpNe, ICmpLt, ICmpGt, ICmpLe, ICmpGe,
    ICmpULt, ICmpUGt, ICmpULe, ICmpUGe,
    // float comparisons (ordered; result Bool)
    FCmpEq, FCmpNe, FCmpLt, FCmpGt, FCmpLe, FCmpGe,
    // unary
    INeg, FNeg, Not,
    // numeric casts (`as`): unary, operand a, result type = destination.
    // Int<->Int: Trunc / SExt / ZExt. Float<->Float: FpTrunc / FpExt.
    // Int->Float: SiToFp / UiToFp. Float->Int: FpToSi / FpToUi.
    // SignCast: same-width int signedness change. A no-op in LLVM (integer types
    // are signless) but must still rebrand the value's MType so later
    // signedness-sensitive ops (SDiv/UDiv, signed/unsigned compares, AShr/LShr)
    // see the destination signedness.
    Trunc, SExt, ZExt, FpTrunc, FpExt, SiToFp, UiToFp, FpToSi, FpToUi, SignCast,
    Call,
    // aggregates
    MakeStruct, GetField,
    // FieldPtr: address of a struct field for assignment. a = struct pointer,
    // imm_int = struct index, field_index = field; result is a pointer (Store
    // into it). Mirrors GetField but produces an lvalue rather than a value.
    FieldPtr,
    // borrows: AddrOf is a no-op rebrand of an alloca's pointer (operand a = the slot)
    AddrOf,
    // enums with mixed-payload variants are kept in a `{ i32 tag, [N x i8] }`
    // repr where N = max variant-payload size (computed at emit time).
    EnumCons,     // construct: imm_int = variant index; a = payload value (M_NO_VALUE if fieldless);
                  //            result type = mt_enum(enum index).
    EnumTag,      // read tag: a = enum value; result type = u32.
    EnumPayload,  // read variant payload: a = enum value; result type = the variant's payload type.
    // control flow (targets are block indices)
    Br, CondBr,
    Ret, RetVoid, Unreachable,
};

struct MInst {
    MOp      op;
    MType    type;        // result type (Void for Store/Ret/RetVoid)
    Span     span;

    uint64_t imm_int;     // ConstInt
    double   imm_float;   // ConstFloat
    bool     imm_bool;    // ConstBool
    uint32_t str_id;      // ConstCStr: interned string content

    uint32_t a;           // first value operand (CondBr: condition) (or M_NO_VALUE)
    uint32_t b;           // second value operand (or M_NO_VALUE)
    uint32_t param_index; // Param

    uint32_t  callee;     // Call: index into MModule.funcs
    uint32_t* args;       // Call: argument value refs (arena array)
    uint32_t  nargs;

    uint32_t target;      // Br: destination block; CondBr: then-block
    uint32_t target2;     // CondBr: else-block
    uint32_t field_index; // GetField: field index within the struct
};

// A basic block: an ordered list of instruction ids from MFunc.insts. The
// last id is the block's terminator (Br / CondBr / Ret / RetVoid).
struct MBlock {
    uint32_t* inst_ids;
    uint32_t  nids;
};

struct MFunc {
    uint32_t name;       // interned
    bool     is_extern;
    bool     is_main;    // emitted with C `int main(void)` semantics
    bool     variadic;
    MType    ret;
    MType*   param_types;
    uint32_t nparams;

    MInst*   insts;      // flat instruction pool (id = index); null for extern
    uint32_t ninsts;
    MBlock*  blocks;     // basic blocks in emission order; blocks[0] is entry
    uint32_t nblocks;
};

// A struct layout: field names (for lowering field access) and field types.
struct MStructDef {
    uint32_t  name;
    uint32_t* field_names;
    MType*    field_types;
    uint32_t  nfields;
};

// An enum layout. Each variant has a payload type (MType, not Void) or none
// (MType::Void). Emit-time LLVM repr is { i32 tag, [N x i8] } where N = max
// payload size across variants, aligned to the max-aligned payload type.
// Construction stores the payload into the byte buffer (typed via opaque-ptr
// cast); extraction reads it back. An all-fieldless enum is plain u32 (see
// LEnum.any_payload in lower.cc) and never reaches this table.
struct MEnumDef {
    uint32_t  name;
    uint32_t  nvariants;
    uint32_t* variant_names;
    MType*    variant_payloads; // mt_void() when the variant is fieldless
};

struct MModule {
    MFunc*      funcs;
    uint32_t    nfuncs;
    MStructDef* structs;
    uint32_t    nstructs;
    MEnumDef*   enums;
    uint32_t    nenums;
};

// Lower a parsed module into mid-IR. Allocates from `a`. Unsupported
// constructs are reported through `diag`. Returns null only on a hard failure.
MModule* lower_module(const Module* ast, arena* a, Interner* in, Diag* diag);

#endif

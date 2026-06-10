#ifndef DIGON_TYPES_H
#define DIGON_TYPES_H

#include <cstdint>

#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "intern.h"

// `Error` is the recovery type: comparisons against it always succeed so a
// single root error does not cascade into a flood of follow-on diagnostics.
enum class TyKind : uint8_t { Error, Void, Bool, Int, Float, Ptr, Struct, Enum, Ref, Newtype };

struct StructInfo;  // defined in types.cc
struct EnumInfo;    // defined in types.cc
struct NewtypeInfo; // defined in types.cc

struct Type {
    TyKind       kind;
    uint16_t     bits;      // Int: 8/16/32/64/128; Float: 32/64
    bool         is_signed; // Int
    bool         is_mut;    // Ref: ref mut T vs ref T
    Type*        inner;     // Ptr / Ref pointee; Newtype: underlying type
    StructInfo*  sinfo;     // Struct: the declaration info
    EnumInfo*    einfo;     // Enum: the declaration info
    NewtypeInfo* ninfo;     // Newtype: nominal identity + underlying
};

// Type-check a module. Mostly pure validation; the one exception is that an
// unsuffixed integer literal whose type is inferred from context has that width
// recorded back onto its AST node (its `suffix`), so lowering agrees with the
// checker. Returns true when there are no type errors, reporting any through
// `diag`. Intended to run after parsing and before lowering.
bool check_module(const Module* m, arena* a, Interner* in, Diag* diag);

#endif

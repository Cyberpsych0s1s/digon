#ifndef DIGON_AST_H
#define DIGON_AST_H

#include <cstdint>

#include "arena.h"
#include "diag.h"   // Span
#include "intern.h"
#include "lex.h"    // Tok, NumSuffix
#include "vec.h"

// Tagged variants are C-style: a `kind` discriminant plus a named union of POD
// payload structs. All nodes are arena-allocated and never individually freed.
// Cross-node references are pointers into the arena.
struct Expr;
struct Stmt;

// Types: a bare type name, a raw pointer `*T` (for extern C signatures), and a
// reference `ref T` / `ref mut T`.
enum class TypeKind : uint8_t { Named, Pointer, Ref };

struct TypeExpr {
    TypeKind  kind;
    Span      span;
    uint32_t  name;   // Named: interned type name; 0 = none / inferred
    TypeExpr* inner;  // Pointer / Ref: pointee type
    bool      is_mut; // Ref only: ref mut T vs ref T
};

// ---------------------------------------------------------------- Expressions
enum class ExprKind : uint8_t {
    IntLit,
    FloatLit,
    BoolLit,
    NullLit,
    StrLit,
    Ident,
    Unary,
    Binary,
    Call,
    Block,
    If,
    While,
    StructLit,
    Field,
    Match,
    Ref,
    Loop,
    For,
    Cast,
};

// One piece of a (possibly interpolated) string literal.
struct StrPart {
    bool     is_expr;
    uint32_t text; // is_expr == false: interned chunk
    Expr*    expr; // is_expr == true:  embedded expression
};

struct IntLitData   { uint64_t  value; NumSuffix suffix; };
struct FloatLitData { double    value; NumSuffix suffix; };
struct BoolLitData  { bool      value; };
struct StrLitData   { StrPart*  parts; uint32_t nparts; };
struct IdentData    { uint32_t  name; };
struct UnaryData    { Tok op; Expr* operand; };
struct BinaryData   { Tok op; Expr* lhs; Expr* rhs; };
struct CallData     { Expr* callee; Expr** args; uint32_t nargs; };
struct BlockData    { Stmt** stmts; uint32_t nstmts; Expr* tail; }; // tail may be null
struct IfData       { Expr* cond; Expr* then_blk; Expr* else_blk; }; // else_blk: Block, If, or null
struct WhileData    { Expr* cond; Expr* body; };
struct CastData     { Expr* operand; TypeExpr* target; };

// One `name: value` initializer in a struct literal.
struct FieldInit { uint32_t name; Expr* value; Span span; };
struct StructLitData { uint32_t type_name; FieldInit* fields; uint32_t nfields; };
struct FieldData     { Expr* obj; uint32_t name; }; // obj.name (field read / enum variant)

// A match pattern, one of:
//   * Wildcard:  `_`
//   * Variant:   `Enum.Variant` (optionally with a single payload binding)
//   * Literal:   an int or bool literal (`0`, `true`)
//   * Binding:   a bare name that catches anything and binds it
//
// Each arm may carry an optional guard (a bool expr after `if`) that runs
// after the pattern matches; if false, control falls through to the next arm.
enum class PatKind : uint8_t { Wildcard, Variant, Literal, Binding };
struct Pattern {
    PatKind   kind;
    Span      span;
    // Variant fields
    uint32_t  enum_name;
    uint32_t  variant_name;
    bool      has_binding;     // payload binding on a Variant
    uint32_t  binding_name;    // also used as the local name for Binding
    // Literal fields
    uint64_t  lit_int;
    NumSuffix lit_suffix;
    bool      lit_is_bool;
    bool      lit_bool_val;
};
struct MatchArm  { Pattern pat; Expr* guard; Expr* body; Span span; }; // guard may be null
struct MatchData { Expr* scrutinee; MatchArm* arms; uint32_t narms; };

// `ref e` / `ref mut e`: take an immutable / mutable borrow of an lvalue.
struct RefData { bool is_mut; Expr* operand; };

// `loop { body }` — infinite loop, exits only via break/return.
struct LoopData { Expr* body; };

// `for name in iter { body }` — iter must be a range expression.
struct ForData { uint32_t name; Expr* iter; Expr* body; };

struct Expr {
    ExprKind kind;
    Span     span;
    union {
        IntLitData   int_lit;
        FloatLitData float_lit;
        BoolLitData  bool_lit;
        StrLitData   str_lit;
        IdentData    ident;
        UnaryData    unary;
        BinaryData   binary;
        CallData     call;
        BlockData     block;
        IfData        if_;
        WhileData     while_;
        StructLitData struct_lit;
        FieldData     field;
        MatchData     match_;
        RefData       ref_;
        LoopData      loop_;
        ForData       for_;
        CastData      cast;
    } as;
};

// ----------------------------------------------------------------- Statements
enum class StmtKind : uint8_t {
    Binding,  // let / var
    Return,
    ExprStmt,
    Defer,
    Break,
    Continue,
};

struct BindingData { bool is_var; uint32_t name; TypeExpr* type; Expr* init; }; // type may be null
struct ReturnData  { Expr* value; };  // value may be null
struct ExprStmtData{ Expr* expr; };
struct DeferData   { Expr* expr; };

struct Stmt {
    StmtKind kind;
    Span     span;
    union {
        BindingData  binding;
        ReturnData   ret;
        ExprStmtData expr_stmt;
        DeferData    defer_;
    } as;
};

// ---------------------------------------------------------------------- Items
struct Param {
    uint32_t  name;
    TypeExpr* type;
    Span      span;
};

enum class ItemKind : uint8_t { Func, Struct, Enum };

struct FieldDecl { uint32_t name; TypeExpr* type; Span span; };
struct StructData { uint32_t name; FieldDecl* fields; uint32_t nfields; bool is_must_defer; };
// A variant is either fieldless OR carries a single typed payload (tuple-style
// with arity 1, e.g. `Some(i32)`). All payload-having variants within one enum
// must share the same payload type.
struct EnumVariant {
    uint32_t  name;
    bool      has_payload;
    TypeExpr* payload_type; // null when has_payload == false
    Span      span;
};
struct EnumData { uint32_t name; EnumVariant* variants; uint32_t nvariants; bool is_must_defer; };

struct FuncData {
    bool      is_pub;
    bool      is_extern; // `extern "abi" func ...`, no body
    uint32_t  abi;       // interned ABI string (e.g. "c"); 0 if none
    uint32_t  name;
    Param*    params;
    uint32_t  nparams;
    TypeExpr* ret;  // null if `-> T` omitted
    Expr*     body; // a Block expression; null for extern
};

struct Item {
    ItemKind kind;
    Span     span;
    union {
        FuncData   func;
        StructData struct_;
        EnumData   enum_;
    } as;
};

struct Module {
    Item**   items;
    uint32_t nitems;
};

// Append a canonical S-expression dump of the module to `out` (for tests/debug).
void ast_dump_module(const Module* m, const Interner* in, Vec<char>* out);

#endif

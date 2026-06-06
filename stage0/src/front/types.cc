#include "types.h"

#include <cstdio>
#include <cstring>

#include "map.h"
#include "vec.h"

// Defined at global scope to match the forward declaration in types.h, which
// Type::sinfo points at.
struct StructInfo {
    uint32_t    name;
    const char* name_str;    // stable, for diagnostics
    uint32_t*   field_names;
    Type**      field_types;
    uint32_t    nfields;
    bool        is_must_defer;
};

struct EnumInfo {
    uint32_t    name;
    const char* name_str;
    uint32_t*   variant_names;
    Type**      variant_payload_types; // null per slot when that variant has no payload
    uint32_t    nvariants;
    Type*       payload_type;          // shared by all payload variants; null if all fieldless
    bool        is_must_defer;
};

namespace {

struct FuncSig {
    Type**   params;
    uint32_t nparams;
    Type*    ret;
    bool     is_extern;
};

struct Binding {
    Type* type;
    bool  is_mut;
    bool  moved;   // set when the binding's value is moved away (non-Copy by-value pass)
};

// Per-binding borrow accounting (aliasing rules + NLL), split into two pools:
//   * anon — borrows created mid-expression (e.g. `f(ref x)`); released at the
//            end of the enclosing statement (anonymous lifetime).
//   * let_borrows — borrows captured by a `let`/`var` binding (e.g.
//            `let r = ref x`); live up to and including `expires_at_stmt` (the
//            stmt index of their last use), released from the next stmt on.
// Aliasing (via effective_borrows at the current stmt): multiple shared OK,
// exclusive excludes all others, assigning while either is live is rejected.
struct LetBorrow {
    uint32_t handle_name;      // the binding name that holds this borrow
    bool     is_mut;           // exclusive?
    uint32_t expires_at_stmt;  // last function-root stmt index that mentions handle_name
};

struct BorrowState {
    uint32_t       anon_shared;
    bool           anon_exclusive;
    Vec<LetBorrow> let_borrows;
    Span           first_borrow;  // span of the earliest live borrow, for diagnostics
};

struct Checker {
    arena*    a;
    Interner* in;
    Diag*     diag;
    Map       funcs;        // interned name -> FuncSig*
    Map       structs;      // interned name -> StructInfo*
    Map       enums;        // interned name -> EnumInfo*
    Map       scope;        // interned name -> Binding* (flat, per function)
    Map       borrow_state; // interned name -> BorrowState* (per function)
    int       loop_depth;   // 0 = not inside any loop body
    // NLL state: index of the function-root stmt currently being checked. Used
    // to decide whether a let-bound borrow has expired (its last-use index
    // has been passed). Tail-expression evaluation runs at index == nstmts.
    uint32_t  cur_stmt_idx;
    const BlockData* cur_func_block; // function-root block; null inside nested blocks
    Type*     cur_ret; // return type of the function being checked
    // Bidirectional inference: the integer type an unsuffixed literal should
    // adopt in the current position (null = none; default i32). Set at typed
    // positions (binding annotation, return, call arg, struct field) and
    // propagated through arithmetic, parens, if/else, and block tails.
    Type*     int_hint;

    Type t_error{TyKind::Error, 0, false, false, nullptr, nullptr, nullptr};
    Type t_void{TyKind::Void, 0, false, false, nullptr, nullptr, nullptr};
    Type t_bool{TyKind::Bool, 0, false, false, nullptr, nullptr, nullptr};
    Type t_ptr{TyKind::Ptr, 0, false, false, nullptr, nullptr, nullptr};
};

Type* mk_int(Checker* c, uint16_t bits, bool sign) {
    Type* t = ARENA_NEW(c->a, Type);
    t->kind = TyKind::Int;
    t->bits = bits;
    t->is_signed = sign;
    t->inner = nullptr;
    t->is_mut = false;
    t->sinfo = nullptr;
    t->einfo = nullptr;
    return t;
}
Type* mk_float(Checker* c, uint16_t bits) {
    Type* t = ARENA_NEW(c->a, Type);
    t->kind = TyKind::Float;
    t->bits = bits;
    t->is_signed = false;
    t->inner = nullptr;
    t->is_mut = false;
    t->sinfo = nullptr;
    t->einfo = nullptr;
    return t;
}
Type* mk_struct(Checker* c, StructInfo* si) {
    Type* t = ARENA_NEW(c->a, Type);
    t->kind = TyKind::Struct;
    t->bits = 0;
    t->is_signed = false;
    t->inner = nullptr;
    t->sinfo = si;
    t->einfo = nullptr;
    return t;
}
Type* mk_ref(Checker* c, Type* inner, bool is_mut) {
    Type* t = ARENA_NEW(c->a, Type);
    t->kind      = TyKind::Ref;
    t->bits      = 0;
    t->is_signed = false;
    t->is_mut    = is_mut;
    t->inner     = inner;
    t->sinfo     = nullptr;
    t->einfo     = nullptr;
    return t;
}
Type* mk_enum(Checker* c, EnumInfo* ei) {
    Type* t = ARENA_NEW(c->a, Type);
    t->kind = TyKind::Enum;
    t->bits = 0;
    t->is_signed = false;
    t->inner = nullptr;
    t->sinfo = nullptr;
    t->einfo = ei;
    return t;
}

bool is_numeric(const Type* t) { return t->kind == TyKind::Int || t->kind == TyKind::Float; }

// Fixed-width integer suffix for (bits, signedness). Used to record an
// inferred literal type back onto the AST so lowering agrees with the checker.
NumSuffix suffix_for_int(uint16_t bits, bool sign) {
    switch (bits) {
        case 8:   return sign ? NumSuffix::I8   : NumSuffix::U8;
        case 16:  return sign ? NumSuffix::I16  : NumSuffix::U16;
        case 32:  return sign ? NumSuffix::I32  : NumSuffix::U32;
        case 64:  return sign ? NumSuffix::I64  : NumSuffix::U64;
        case 128: return sign ? NumSuffix::I128 : NumSuffix::U128;
        default:  return NumSuffix::None;
    }
}

bool type_eq(const Type* a, const Type* b) {
    if (a->kind == TyKind::Error || b->kind == TyKind::Error) return true;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TyKind::Int:    return a->bits == b->bits && a->is_signed == b->is_signed;
        case TyKind::Float:  return a->bits == b->bits;
        case TyKind::Ptr:    return true; // all pointers interchangeable
        case TyKind::Struct: return a->sinfo == b->sinfo;
        case TyKind::Enum:   return a->einfo == b->einfo;
        case TyKind::Ref:    return a->is_mut == b->is_mut && a->inner && b->inner && type_eq(a->inner, b->inner);
        default:             return true; // Void, Bool, Error
    }
}

// Render a type into one of a few rotating static buffers so two calls can be
// passed to a single printf-style diagnostic.
const char* tyname(const Type* t) {
    static char  bufs[4][48];
    static int   slot = 0;
    char*        buf  = bufs[slot];
    slot = (slot + 1) & 3;
    switch (t->kind) {
        case TyKind::Error: std::snprintf(buf, 48, "<error>"); break;
        case TyKind::Void:  std::snprintf(buf, 48, "void"); break;
        case TyKind::Bool:  std::snprintf(buf, 48, "bool"); break;
        case TyKind::Int:   std::snprintf(buf, 48, "%c%u", t->is_signed ? 'i' : 'u', t->bits); break;
        case TyKind::Float: std::snprintf(buf, 48, "f%u", t->bits); break;
        case TyKind::Ptr:   std::snprintf(buf, 48, "*%s", t->inner ? tyname(t->inner) : "_"); break;
        case TyKind::Struct: std::snprintf(buf, 48, "%s", t->sinfo ? t->sinfo->name_str : "?"); break;
        case TyKind::Enum:   std::snprintf(buf, 48, "%s", t->einfo ? t->einfo->name_str : "?"); break;
        case TyKind::Ref:    std::snprintf(buf, 48, "ref%s %s", t->is_mut ? " mut" : "",
                                          t->inner ? tyname(t->inner) : "_"); break;
    }
    return buf;
}

Type* resolve_type(Checker* c, const TypeExpr* te) {
    if (!te) return &c->t_void;
    if (te->kind == TypeKind::Pointer) {
        Type* t  = ARENA_NEW(c->a, Type);
        t->kind  = TyKind::Ptr;
        t->bits  = 0;
        t->is_signed = false;
        t->is_mut    = false;
        t->inner = resolve_type(c, te->inner);
        t->sinfo = nullptr;
        t->einfo = nullptr;
        return t;
    }
    if (te->kind == TypeKind::Ref) {
        return mk_ref(c, resolve_type(c, te->inner), te->is_mut);
    }
    size_t      len = 0;
    const char* s   = intern_str(c->in, te->name, &len);
    if (!s) return &c->t_error;

    struct Named { const char* n; TyKind k; uint16_t bits; bool sign; };
    static const Named NAMED[] = {
        {"i8", TyKind::Int, 8, true},   {"i16", TyKind::Int, 16, true},
        {"i32", TyKind::Int, 32, true}, {"i64", TyKind::Int, 64, true},
        {"i128", TyKind::Int, 128, true}, {"iptr", TyKind::Int, 64, true},
        {"u8", TyKind::Int, 8, false},  {"u16", TyKind::Int, 16, false},
        {"u32", TyKind::Int, 32, false},{"u64", TyKind::Int, 64, false},
        {"u128", TyKind::Int, 128, false}, {"uptr", TyKind::Int, 64, false},
        {"f32", TyKind::Float, 32, false}, {"f64", TyKind::Float, 64, false},
        {"bool", TyKind::Bool, 0, false}, {"void", TyKind::Void, 0, false},
    };
    for (const Named& nm : NAMED) {
        if (std::strlen(nm.n) == len && std::memcmp(nm.n, s, len) == 0) {
            if (nm.k == TyKind::Int)   return mk_int(c, nm.bits, nm.sign);
            if (nm.k == TyKind::Float) return mk_float(c, nm.bits);
            if (nm.k == TyKind::Bool)  return &c->t_bool;
            return &c->t_void;
        }
    }
    if (len == 3 && std::memcmp("str", s, 3) == 0) return &c->t_ptr; // placeholder for the real `str` type

    void* si = nullptr;
    if (map_lookup(&c->structs, te->name, &si)) {
        return mk_struct(c, static_cast<StructInfo*>(si));
    }
    void* ei = nullptr;
    if (map_lookup(&c->enums, te->name, &ei)) {
        return mk_enum(c, static_cast<EnumInfo*>(ei));
    }
    diag_errorf(c->diag, te->span, "unknown type '%.*s'", static_cast<int>(len), s);
    return &c->t_error;
}

const char* name_of(Checker* c, uint32_t id) {
    size_t      len = 0;
    const char* s   = intern_str(c->in, id, &len);
    return s ? s : "?";
}

Type* check_expr(Checker* c, const Expr* e);

// Check `e` with `hint` as the expected integer type for unsuffixed literals,
// restoring the previous hint afterwards.
Type* check_expr_as(Checker* c, const Expr* e, Type* hint) {
    Type* save  = c->int_hint;
    c->int_hint = hint;
    Type* t     = check_expr(c, e);
    c->int_hint = save;
    return t;
}

Type* check_block(Checker* c, const BlockData* b, bool is_func_root, Type* tail_hint = nullptr);

bool is_copy_type(const Type* t);
void maybe_move(Checker* c, const Expr* e);

// NLL helpers (documented at their definitions below).
void effective_borrows(const Checker* c, const BorrowState* bs,
                       uint32_t* out_shared, bool* out_excl);
void release_anon_borrows(Checker* c);
uint32_t scan_last_use(const BlockData* b, uint32_t from, uint32_t name);

Type* check_call(Checker* c, const Expr* e) {
    const CallData& call = e->as.call;

    // UFCS: when the callee is `obj.f` and `obj` is not an enum name, rewrite
    // `recv.f(args...)` into `f(recv, args...)` in place so both the checker and
    // the lowerer see a plain call (lowering always runs after type-checking).
    if (call.callee->kind == ExprKind::Field) {
        const Expr* fld = call.callee;
        const Expr* obj = fld->as.field.obj;
        bool        obj_is_enum_name = false;
        if (obj->kind == ExprKind::Ident) {
            void* tmp = nullptr;
            obj_is_enum_name = map_lookup(&c->enums, obj->as.ident.name, &tmp);
        }
        if (!obj_is_enum_name) {
            uint32_t fname = fld->as.field.name;
            void*    fv    = nullptr;
            FuncSig* sig   = map_lookup(&c->funcs, fname, &fv) ? static_cast<FuncSig*>(fv) : nullptr;
            if (!sig || sig->nparams == 0) {
                // No free function can take this receiver. Report with the
                // receiver type, after checking sub-expressions for their errors.
                Type* rt = check_expr(c, obj);
                for (uint32_t i = 0; i < call.nargs; i++) check_expr(c, call.args[i]);
                diag_errorf(c->diag, fld->span,
                            "no function '%s' accepting type %s is in scope",
                            name_of(c, fname), tyname(rt));
                return &c->t_error;
            }
            // Auto-borrow: if param 0 is a reference and the receiver isn't
            // already one, wrap it in `ref`/`ref mut` (a side-effect-free scope
            // peek decides "already a reference"). The synthesized Ref flows
            // through the normal borrow checker.
            bool recv_is_ref = false;
            if (obj->kind == ExprKind::Ident) {
                void* bv = nullptr;
                if (map_lookup(&c->scope, obj->as.ident.name, &bv))
                    recv_is_ref = static_cast<Binding*>(bv)->type->kind == TyKind::Ref;
            }
            Expr* recv = const_cast<Expr*>(obj);
            if (sig->params[0]->kind == TyKind::Ref && !recv_is_ref) {
                Expr* r = ARENA_NEW(c->a, Expr);
                r->kind            = ExprKind::Ref;
                r->span            = obj->span;
                r->as.ref_.is_mut  = sig->params[0]->is_mut;
                r->as.ref_.operand = recv;
                recv = r;
            }
            // Build [recv, original args...] and an Ident callee for `f`.
            Expr** new_args = ARENA_NEW_N(c->a, Expr*, call.nargs + 1);
            new_args[0] = recv;
            for (uint32_t i = 0; i < call.nargs; i++) new_args[i + 1] = call.args[i];
            Expr* callee_id = ARENA_NEW(c->a, Expr);
            callee_id->kind          = ExprKind::Ident;
            callee_id->span          = fld->span;
            callee_id->as.ident.name = fname;

            Expr* m           = const_cast<Expr*>(e);
            m->as.call.callee = callee_id;
            m->as.call.args   = new_args;
            m->as.call.nargs  = call.nargs + 1;
            // Fall through: `call` now aliases the rewritten plain call.
        }
    }

    // Pre-resolve the callee to learn each argument's expected type, so an
    // unsuffixed integer literal argument can adopt it.
    Vec<Type*> hints{};
    for (uint32_t i = 0; i < call.nargs; i++) hints.push(nullptr);
    if (call.callee->kind == ExprKind::Ident) {
        void* v = nullptr;
        if (map_lookup(&c->funcs, call.callee->as.ident.name, &v)) {
            FuncSig* sig = static_cast<FuncSig*>(v);
            for (uint32_t i = 0; i < call.nargs && i < sig->nparams; i++) hints[i] = sig->params[i];
        }
    } else if (call.callee->kind == ExprKind::Field &&
               call.callee->as.field.obj->kind == ExprKind::Ident) {
        void* eiv = nullptr;
        if (map_lookup(&c->enums, call.callee->as.field.obj->as.ident.name, &eiv)) {
            EnumInfo* ei = static_cast<EnumInfo*>(eiv);
            uint32_t  fn = call.callee->as.field.name;
            for (uint32_t k = 0; k < ei->nvariants; k++)
                if (ei->variant_names[k] == fn && ei->variant_payload_types[k]) {
                    if (call.nargs >= 1) hints[0] = ei->variant_payload_types[k];
                    break;
                }
        }
    }

    // Check all arguments regardless, so errors inside them are reported.
    Vec<Type*> argtys{};
    for (uint32_t i = 0; i < call.nargs; i++) argtys.push(check_expr_as(c, call.args[i], hints[i]));
    hints.free();

    // Variant construction with payload: Enum.Variant(arg) is parsed as a
    // Call whose callee is Field(Ident(Enum), Variant).
    if (call.callee->kind == ExprKind::Field &&
        call.callee->as.field.obj->kind == ExprKind::Ident) {
        void* eiv = nullptr;
        if (map_lookup(&c->enums, call.callee->as.field.obj->as.ident.name, &eiv)) {
            EnumInfo* ei  = static_cast<EnumInfo*>(eiv);
            uint32_t  fn  = call.callee->as.field.name;
            uint32_t  idx = ei->nvariants;
            for (uint32_t k = 0; k < ei->nvariants; k++)
                if (ei->variant_names[k] == fn) { idx = k; break; }
            if (idx == ei->nvariants) {
                diag_errorf(c->diag, call.callee->span, "enum %s has no variant '%s'",
                            ei->name_str, name_of(c, fn));
            } else if (!ei->variant_payload_types[idx]) {
                diag_errorf(c->diag, e->span,
                            "variant '%s' is fieldless; drop the '(...)' to construct it",
                            name_of(c, fn));
            } else if (call.nargs != 1) {
                diag_errorf(c->diag, e->span,
                            "variant '%s' takes one payload argument", name_of(c, fn));
            } else if (!type_eq(argtys[0], ei->variant_payload_types[idx])) {
                diag_errorf(c->diag, call.args[0]->span,
                            "variant '%s' payload: expected %s, got %s", name_of(c, fn),
                            tyname(ei->variant_payload_types[idx]), tyname(argtys[0]));
            }
            argtys.free();
            return mk_enum(c, ei);
        }
    }

    if (call.callee->kind != ExprKind::Ident) {
        diag_errorf(c->diag, e->span, "only direct calls to named functions are supported");
        argtys.free();
        return &c->t_error;
    }
    uint32_t name = call.callee->as.ident.name;
    void*    v    = nullptr;
    if (!map_lookup(&c->funcs, name, &v)) {
        diag_errorf(c->diag, e->span, "unknown function '%s'", name_of(c, name));
        argtys.free();
        return &c->t_error;
    }
    FuncSig* sig = static_cast<FuncSig*>(v);
    if (sig->nparams != call.nargs) {
        diag_errorf(c->diag, e->span, "'%s' expects %u argument%s but %u %s given",
                    name_of(c, name), sig->nparams, sig->nparams == 1 ? "" : "s",
                    call.nargs, call.nargs == 1 ? "was" : "were");
    } else {
        for (uint32_t i = 0; i < call.nargs; i++) {
            if (!type_eq(argtys[i], sig->params[i])) {
                diag_errorf(c->diag, call.args[i]->span,
                            "argument %u of '%s': expected %s, got %s",
                            i + 1, name_of(c, name), tyname(sig->params[i]), tyname(argtys[i]));
            }
        }
    }
    // Function call by value moves non-Copy bindings passed as bare idents.
    for (uint32_t i = 0; i < call.nargs; i++) maybe_move(c, call.args[i]);
    argtys.free();
    return sig->ret;
}

// Whether a binary operator's operands should inherit the surrounding
// integer-literal hint (arithmetic, bitwise, and range; not comparison/logical).
bool binop_propagates_hint(Tok op) {
    switch (op) {
        case Tok::Plus: case Tok::Minus: case Tok::Star: case Tok::Slash: case Tok::Percent:
        case Tok::Amp: case Tok::Pipe: case Tok::Caret: case Tok::Shl: case Tok::Shr:
        case Tok::DotDot: case Tok::DotDotEq:
            return true;
        default:
            return false;
    }
}

// An "untyped" integer expression is one whose type is not yet pinned: an
// unsuffixed literal, or an arithmetic/bitwise combination of such. These flex
// to adopt a hint or the type of a concrete neighbour; a suffixed literal,
// ident, or call does not.
bool is_untyped_int_expr(const Expr* e) {
    if (e->kind == ExprKind::IntLit) return e->as.int_lit.suffix == NumSuffix::None;
    if (e->kind == ExprKind::Unary && (e->as.unary.op == Tok::Minus || e->as.unary.op == Tok::Tilde))
        return is_untyped_int_expr(e->as.unary.operand);
    if (e->kind == ExprKind::Binary && binop_propagates_hint(e->as.binary.op))
        return is_untyped_int_expr(e->as.binary.lhs) && is_untyped_int_expr(e->as.binary.rhs);
    return false;
}

// Type the two operands of a binary op, letting an untyped-int side adopt a
// concrete-int neighbour, or a concrete hint flow into untyped operands.
void check_int_operands(Checker* c, const Expr* lhs, const Expr* rhs, bool propagate,
                        Type* hint, Type** out_lt, Type** out_rt) {
    Type* oph = (propagate && hint && hint->kind == TyKind::Int) ? hint : nullptr;
    bool  lu  = is_untyped_int_expr(lhs);
    bool  ru  = is_untyped_int_expr(rhs);
    if (oph) {
        *out_lt = check_expr_as(c, lhs, oph);
        *out_rt = check_expr_as(c, rhs, oph);
    } else if (lu && !ru) {
        *out_rt = check_expr(c, rhs);
        *out_lt = check_expr_as(c, lhs, (*out_rt)->kind == TyKind::Int ? *out_rt : nullptr);
    } else if (ru && !lu) {
        *out_lt = check_expr(c, lhs);
        *out_rt = check_expr_as(c, rhs, (*out_lt)->kind == TyKind::Int ? *out_lt : nullptr);
    } else {
        *out_lt = check_expr(c, lhs);
        *out_rt = check_expr(c, rhs);
    }
}

Type* check_binary(Checker* c, const Expr* e, Type* hint) {
    Tok op = e->as.binary.op;

    if (op == Tok::Eq) {
        const Expr* lhs = e->as.binary.lhs;
        if (lhs->kind != ExprKind::Ident) {
            check_expr(c, e->as.binary.rhs);
            diag_errorf(c->diag, e->span, "invalid assignment target");
            return &c->t_error;
        }
        void* v = nullptr;
        if (!map_lookup(&c->scope, lhs->as.ident.name, &v)) {
            check_expr(c, e->as.binary.rhs);
            diag_errorf(c->diag, lhs->span, "unknown identifier '%s'", name_of(c, lhs->as.ident.name));
            return &c->t_error;
        }
        Binding* bnd = static_cast<Binding*>(v);
        // Assigned literal adopts the binding's integer type.
        Type* rt = check_expr_as(c, e->as.binary.rhs, bnd->type);
        if (!bnd->is_mut) {
            diag_errorf(c->diag, e->span, "cannot assign to immutable binding '%s' (declare it with 'var')",
                        name_of(c, lhs->as.ident.name));
        } else if (!type_eq(bnd->type, rt)) {
            diag_errorf(c->diag, e->span, "cannot assign %s to '%s' of type %s",
                        tyname(rt), name_of(c, lhs->as.ident.name), tyname(bnd->type));
        }
        // Aliasing: forbid assignment while any live borrow exists. Released
        // (expired) let-borrows are skipped via effective_borrows.
        void* bsv = nullptr;
        if (map_lookup(&c->borrow_state, lhs->as.ident.name, &bsv)) {
            BorrowState* bs = static_cast<BorrowState*>(bsv);
            uint32_t shared = 0;
            bool     excl   = false;
            effective_borrows(c, bs, &shared, &excl);
            if (shared > 0 || excl)
                diag_errorf(c->diag, e->span,
                            "cannot assign to '%s' while it is borrowed",
                            name_of(c, lhs->as.ident.name));
        }
        return bnd->type;
    }

    // Compound assignment isn't implemented yet.
    switch (op) {
        case Tok::PlusEq: case Tok::MinusEq: case Tok::StarEq: case Tok::SlashEq:
        case Tok::PercentEq: case Tok::AmpEq: case Tok::PipeEq: case Tok::CaretEq:
        case Tok::ShlEq: case Tok::ShrEq:
            diag_errorf(c->diag, e->span, "compound assignment is not supported yet");
            check_expr(c, e->as.binary.lhs);
            check_expr(c, e->as.binary.rhs);
            return &c->t_error;
        default:
            break;
    }

    // Operand typing with literal inference (see check_int_operands).
    Type* lt;
    Type* rt;
    check_int_operands(c, e->as.binary.lhs, e->as.binary.rhs,
                       binop_propagates_hint(op), hint, &lt, &rt);

    switch (op) {
        case Tok::AmpAmp: case Tok::PipePipe:
            if (lt->kind != TyKind::Bool || rt->kind != TyKind::Bool) {
                if (lt->kind != TyKind::Error && rt->kind != TyKind::Error)
                    diag_errorf(c->diag, e->span, "'%s' requires bool operands, got %s and %s",
                                tok_name(op), tyname(lt), tyname(rt));
            }
            return &c->t_bool;

        case Tok::EqEq: case Tok::NotEq:
            if (!type_eq(lt, rt))
                diag_errorf(c->diag, e->span, "cannot compare %s with %s", tyname(lt), tyname(rt));
            return &c->t_bool;

        case Tok::Lt: case Tok::Gt: case Tok::Le: case Tok::Ge:
            if (lt->kind != TyKind::Error && (!is_numeric(lt) || !type_eq(lt, rt)))
                diag_errorf(c->diag, e->span, "cannot order %s and %s", tyname(lt), tyname(rt));
            return &c->t_bool;

        case Tok::Amp: case Tok::Pipe: case Tok::Caret: case Tok::Shl: case Tok::Shr:
            if (lt->kind != TyKind::Error && (lt->kind != TyKind::Int || !type_eq(lt, rt)))
                diag_errorf(c->diag, e->span, "bitwise '%s' requires matching integer operands, got %s and %s",
                            tok_name(op), tyname(lt), tyname(rt));
            return lt;

        case Tok::Percent:
            if (lt->kind != TyKind::Error && (lt->kind != TyKind::Int || !type_eq(lt, rt)))
                diag_errorf(c->diag, e->span, "'%%' requires matching integer operands, got %s and %s",
                            tyname(lt), tyname(rt));
            return lt;

        default: // + - * /
            if (lt->kind != TyKind::Error && (!is_numeric(lt) || !type_eq(lt, rt)))
                diag_errorf(c->diag, e->span, "'%s' requires matching numeric operands, got %s and %s",
                            tok_name(op), tyname(lt), tyname(rt));
            return lt;
    }
}

Type* check_expr(Checker* c, const Expr* e) {
    // Consume the pending int-literal hint: children don't inherit it unless a
    // case explicitly re-applies it (arithmetic, parens, if/else, block tail).
    Type* hint  = c->int_hint;
    c->int_hint = nullptr;
    switch (e->kind) {
        case ExprKind::IntLit: {
            if (e->as.int_lit.suffix == NumSuffix::None && hint && hint->kind == TyKind::Int) {
                // Record the inferred width on the node so lowering matches.
                const_cast<Expr*>(e)->as.int_lit.suffix = suffix_for_int(hint->bits, hint->is_signed);
                return mk_int(c, hint->bits, hint->is_signed);
            }
            switch (e->as.int_lit.suffix) {
                case NumSuffix::I8:   return mk_int(c, 8, true);
                case NumSuffix::I16:  return mk_int(c, 16, true);
                case NumSuffix::I32:  return mk_int(c, 32, true);
                case NumSuffix::I64:  return mk_int(c, 64, true);
                case NumSuffix::I128: return mk_int(c, 128, true);
                case NumSuffix::Iptr: return mk_int(c, 64, true);
                case NumSuffix::U8:   return mk_int(c, 8, false);
                case NumSuffix::U16:  return mk_int(c, 16, false);
                case NumSuffix::U32:  return mk_int(c, 32, false);
                case NumSuffix::U64:  return mk_int(c, 64, false);
                case NumSuffix::U128: return mk_int(c, 128, false);
                case NumSuffix::Uptr: return mk_int(c, 64, false);
                default:              return mk_int(c, 32, true); // untyped -> i32
            }
        }
        case ExprKind::FloatLit:
            return e->as.float_lit.suffix == NumSuffix::F32 ? mk_float(c, 32) : mk_float(c, 64);
        case ExprKind::BoolLit: return &c->t_bool;
        case ExprKind::NullLit: return &c->t_ptr;
        case ExprKind::StrLit: {
            // Check interpolated sub-expressions; the literal itself is a pointer
            // (placeholder for the real `str` type).
            for (uint32_t i = 0; i < e->as.str_lit.nparts; i++)
                if (e->as.str_lit.parts[i].is_expr) check_expr(c, e->as.str_lit.parts[i].expr);
            return &c->t_ptr;
        }
        case ExprKind::Ident: {
            void* v = nullptr;
            if (!map_lookup(&c->scope, e->as.ident.name, &v)) {
                diag_errorf(c->diag, e->span, "unknown identifier '%s'", name_of(c, e->as.ident.name));
                return &c->t_error;
            }
            Binding* bnd = static_cast<Binding*>(v);
            if (bnd->moved)
                diag_errorf(c->diag, e->span, "use of moved value '%s'", name_of(c, e->as.ident.name));
            return bnd->type;
        }
        case ExprKind::Unary: {
            Type* t  = check_expr(c, e->as.unary.operand);
            Tok   op = e->as.unary.op;
            if (op == Tok::KwMove) {
                // Explicit move: consume the operand (a non-Copy named binding),
                // participating in use-after-move and move-after-borrow checks.
                maybe_move(c, e->as.unary.operand);
                return t;
            }
            if (op == Tok::Minus) {
                if (t->kind != TyKind::Error && !is_numeric(t))
                    diag_errorf(c->diag, e->span, "unary '-' requires a numeric operand, got %s", tyname(t));
                return t;
            }
            if (op == Tok::Bang) {
                if (t->kind != TyKind::Error && t->kind != TyKind::Bool)
                    diag_errorf(c->diag, e->span, "'!' requires a bool operand, got %s", tyname(t));
                return &c->t_bool;
            }
            if (op == Tok::Star) {
                if (t->kind == TyKind::Error) return t;
                if (t->kind == TyKind::Ref && t->inner) return t->inner;
                if (t->kind == TyKind::Ptr && t->inner) return t->inner;
                diag_errorf(c->diag, e->span, "cannot dereference %s", tyname(t));
                return &c->t_error;
            }
            // '~'
            if (t->kind != TyKind::Error && t->kind != TyKind::Int)
                diag_errorf(c->diag, e->span, "'~' requires an integer operand, got %s", tyname(t));
            return t;
        }
        case ExprKind::Binary: return check_binary(c, e, hint);
        case ExprKind::Call:   return check_call(c, e);
        case ExprKind::Block:  return check_block(c, &e->as.block, /*is_func_root=*/false, hint);
        case ExprKind::If: {
            Type* cond = check_expr(c, e->as.if_.cond);
            if (cond->kind != TyKind::Error && cond->kind != TyKind::Bool)
                diag_errorf(c->diag, e->as.if_.cond->span, "if condition must be bool, got %s", tyname(cond));
            Type* tt = check_expr_as(c, e->as.if_.then_blk, hint);
            if (e->as.if_.else_blk) {
                Type* et = check_expr_as(c, e->as.if_.else_blk, hint);
                if (!type_eq(tt, et)) {
                    diag_errorf(c->diag, e->span,
                                "if and else branches have different types: %s vs %s",
                                tyname(tt), tyname(et));
                    return &c->t_error;
                }
                return tt;
            }
            return &c->t_void; // if without else yields no value
        }
        case ExprKind::While: {
            Type* cond = check_expr(c, e->as.while_.cond);
            if (cond->kind != TyKind::Error && cond->kind != TyKind::Bool)
                diag_errorf(c->diag, e->as.while_.cond->span, "while condition must be bool, got %s",
                            tyname(cond));
            c->loop_depth++;
            check_expr(c, e->as.while_.body);
            c->loop_depth--;
            return &c->t_void;
        }
        case ExprKind::Loop:
            c->loop_depth++;
            check_expr(c, e->as.loop_.body);
            c->loop_depth--;
            return &c->t_void;
        case ExprKind::For: {
            const Expr* it = e->as.for_.iter;
            Type* lo_ty = nullptr;
            // The iterable must be a range expression.
            if (it->kind == ExprKind::Binary &&
                (it->as.binary.op == Tok::DotDot || it->as.binary.op == Tok::DotDotEq)) {
                Type* lt;
                Type* rt;
                check_int_operands(c, it->as.binary.lhs, it->as.binary.rhs,
                                   /*propagate=*/true, /*hint=*/nullptr, &lt, &rt);
                if (lt->kind != TyKind::Int || rt->kind != TyKind::Int || !type_eq(lt, rt))
                    diag_errorf(c->diag, it->span, "for-range must be over matching integer types (got %s and %s)",
                                tyname(lt), tyname(rt));
                lo_ty = lt;
            } else {
                diag_error(c->diag, it->span,
                           "a for-loop iterates a range expression (e.g. 0..n)");
                lo_ty = mk_int(c, 32, true);
                check_expr(c, it); // still check sub-exprs for errors
            }
            // Bind the loop variable for the duration of the body.
            Binding* bnd = ARENA_NEW(c->a, Binding);
            bnd->type   = lo_ty;
            bnd->is_mut = false; bnd->moved = false;
            void* prev = nullptr;
            bool  had  = map_lookup(&c->scope, e->as.for_.name, &prev);
            map_put(&c->scope, e->as.for_.name, bnd);

            c->loop_depth++;
            check_expr(c, e->as.for_.body);
            c->loop_depth--;

            if (had) map_put(&c->scope, e->as.for_.name, prev);
            else     map_remove(&c->scope, e->as.for_.name);
            return &c->t_void;
        }
        case ExprKind::StructLit: {
            const StructLitData& sl = e->as.struct_lit;
            void* siv = nullptr;
            if (!map_lookup(&c->structs, sl.type_name, &siv)) {
                diag_errorf(c->diag, e->span, "unknown type '%s'", name_of(c, sl.type_name));
                for (uint32_t i = 0; i < sl.nfields; i++) check_expr(c, sl.fields[i].value);
                return &c->t_error;
            }
            StructInfo* si = static_cast<StructInfo*>(siv);

            Vec<bool> seen{};
            for (uint32_t i = 0; i < si->nfields; i++) seen.push(false);

            for (uint32_t i = 0; i < sl.nfields; i++) {
                uint32_t fname = sl.fields[i].name;
                uint32_t idx   = si->nfields;
                for (uint32_t k = 0; k < si->nfields; k++)
                    if (si->field_names[k] == fname) { idx = k; break; }
                // Hint an unsuffixed literal field value with the field's type.
                Type* fhint = (idx < si->nfields) ? si->field_types[idx] : nullptr;
                Type* vt    = check_expr_as(c, sl.fields[i].value, fhint);
                if (idx == si->nfields) {
                    diag_errorf(c->diag, sl.fields[i].span, "struct %s has no field '%s'",
                                si->name_str, name_of(c, fname));
                } else if (seen[idx]) {
                    diag_errorf(c->diag, sl.fields[i].span, "field '%s' is set more than once",
                                name_of(c, fname));
                } else {
                    seen[idx] = true;
                    if (!type_eq(vt, si->field_types[idx]))
                        diag_errorf(c->diag, sl.fields[i].value->span,
                                    "field '%s' expects %s, got %s", name_of(c, fname),
                                    tyname(si->field_types[idx]), tyname(vt));
                }
            }
            for (uint32_t k = 0; k < si->nfields; k++)
                if (!seen[k])
                    diag_errorf(c->diag, e->span, "missing field '%s' in %s literal",
                                name_of(c, si->field_names[k]), si->name_str);
            seen.free();
            return mk_struct(c, si);
        }
        case ExprKind::Ref: {
            const Expr* op = e->as.ref_.operand;
            if (op->kind != ExprKind::Ident) {
                diag_error(c->diag, e->span,
                           "'ref' only takes a borrow of a named binding");
                return &c->t_error;
            }
            void* bv = nullptr;
            if (!map_lookup(&c->scope, op->as.ident.name, &bv)) {
                diag_errorf(c->diag, op->span, "unknown identifier '%s'", name_of(c, op->as.ident.name));
                return &c->t_error;
            }
            Binding* bnd = static_cast<Binding*>(bv);
            if (e->as.ref_.is_mut && !bnd->is_mut) {
                diag_errorf(c->diag, e->span,
                            "cannot take 'ref mut' of immutable binding '%s' (declare it with 'var')",
                            name_of(c, op->as.ident.name));
            }

            // Aliasing rules: ref mut excludes everything; ref excludes mut.
            // The new borrow is added to the binding's anon pool; if this
            // expression turns out to be the RHS of `let r = ref x`, the
            // Binding handler promotes it to a let-bound borrow after the
            // check returns.
            uint32_t bname = op->as.ident.name;
            void*    bsv   = nullptr;
            BorrowState* bs;
            if (map_lookup(&c->borrow_state, bname, &bsv)) {
                bs = static_cast<BorrowState*>(bsv);
            } else {
                bs = ARENA_NEW(c->a, BorrowState);
                bs->anon_shared    = 0;
                bs->anon_exclusive = false;
                bs->let_borrows    = Vec<LetBorrow>{};
                bs->first_borrow   = e->span;
                map_put(&c->borrow_state, bname, bs);
            }
            uint32_t eff_shared = 0;
            bool     eff_excl   = false;
            effective_borrows(c, bs, &eff_shared, &eff_excl);
            if (e->as.ref_.is_mut) {
                if (eff_shared > 0)
                    diag_errorf(c->diag, e->span,
                                "cannot take mutable borrow of '%s': it is already shared-borrowed",
                                name_of(c, bname));
                else if (eff_excl)
                    diag_errorf(c->diag, e->span,
                                "cannot take a second mutable borrow of '%s'", name_of(c, bname));
                bs->anon_exclusive = true;
            } else {
                if (eff_excl)
                    diag_errorf(c->diag, e->span,
                                "cannot take shared borrow of '%s': it is already exclusively borrowed",
                                name_of(c, bname));
                bs->anon_shared++;
            }

            return mk_ref(c, bnd->type, e->as.ref_.is_mut);
        }
        case ExprKind::Field: {
            // `Enum.Variant` is fieldless variant construction. (A variant
            // with a payload would appear as Call(Field, [arg]) and is handled
            // in check_call; reaching here means the user forgot the argument.)
            if (e->as.field.obj->kind == ExprKind::Ident) {
                void* eiv = nullptr;
                if (map_lookup(&c->enums, e->as.field.obj->as.ident.name, &eiv)) {
                    EnumInfo* ei  = static_cast<EnumInfo*>(eiv);
                    uint32_t  idx = ei->nvariants;
                    for (uint32_t k = 0; k < ei->nvariants; k++)
                        if (ei->variant_names[k] == e->as.field.name) { idx = k; break; }
                    if (idx == ei->nvariants)
                        diag_errorf(c->diag, e->span, "enum %s has no variant '%s'",
                                    ei->name_str, name_of(c, e->as.field.name));
                    else if (ei->variant_payload_types[idx])
                        diag_errorf(c->diag, e->span,
                                    "variant '%s' requires a payload; use '%s.%s(arg)'",
                                    name_of(c, e->as.field.name), ei->name_str,
                                    name_of(c, e->as.field.name));
                    return mk_enum(c, ei);
                }
            }
            Type* ot = check_expr(c, e->as.field.obj);
            if (ot->kind == TyKind::Error) return &c->t_error;
            // Auto-deref: field access through a borrow of a struct.
            if (ot->kind == TyKind::Ref && ot->inner && ot->inner->kind == TyKind::Struct) {
                ot = ot->inner;
            }
            if (ot->kind != TyKind::Struct) {
                diag_errorf(c->diag, e->span, "cannot access field '%s' of non-struct type %s",
                            name_of(c, e->as.field.name), tyname(ot));
                return &c->t_error;
            }
            StructInfo* si = ot->sinfo;
            for (uint32_t k = 0; k < si->nfields; k++)
                if (si->field_names[k] == e->as.field.name) return si->field_types[k];
            diag_errorf(c->diag, e->span, "struct %s has no field '%s'", si->name_str,
                        name_of(c, e->as.field.name));
            return &c->t_error;
        }
        case ExprKind::Match: {
            const MatchData& mt = e->as.match_;
            Type*            st = check_expr(c, mt.scrutinee);
            EnumInfo*        ei = (st->kind == TyKind::Enum) ? st->einfo : nullptr;
            // Scrutinee may be an enum, an integer, or a bool; anything else is rejected.
            if (!ei && st->kind != TyKind::Error &&
                st->kind != TyKind::Int && st->kind != TyKind::Bool)
                diag_errorf(c->diag, mt.scrutinee->span,
                            "match scrutinee must be an enum, integer, or bool, got %s",
                            tyname(st));

            Vec<bool> covered{};
            if (ei) for (uint32_t k = 0; k < ei->nvariants; k++) covered.push(false);
            bool  saw_catchall  = false;     // wildcard / unguarded bare-name binding
            bool  saw_bool_true = false, saw_bool_false = false;
            Type* result        = nullptr;

            for (uint32_t i = 0; i < mt.narms; i++) {
                const MatchArm& arm = mt.arms[i];
                bool     bound      = false;
                uint32_t bname      = 0;
                void*    bprev      = nullptr;
                bool     bhad_prev  = false;

                // Unreachable-arm detection: a prior unguarded catch-all, or an
                // earlier arm already covering this exact (unguarded) pattern,
                // makes this arm dead. Guarded arms are never flagged.
                bool unreachable = saw_catchall;
                if (!unreachable && !arm.guard) {
                    if (arm.pat.kind == PatKind::Variant && ei) {
                        for (uint32_t k = 0; k < ei->nvariants; k++)
                            if (ei->variant_names[k] == arm.pat.variant_name) {
                                if (covered[k]) unreachable = true;
                                break;
                            }
                    } else if (arm.pat.kind == PatKind::Literal && arm.pat.lit_is_bool) {
                        unreachable = arm.pat.lit_bool_val ? saw_bool_true : saw_bool_false;
                    }
                }
                if (unreachable)
                    diag_warning(c->diag, arm.span,
                                 "unreachable match arm: already covered by an earlier arm");

                switch (arm.pat.kind) {
                    case PatKind::Wildcard:
                        if (!arm.guard) saw_catchall = true;
                        break;
                    case PatKind::Binding: {
                        // Bind the scrutinee value to the pattern's name for
                        // the duration of this arm (incl. guard + body).
                        bname           = arm.pat.binding_name;
                        Binding* bnd    = ARENA_NEW(c->a, Binding);
                        bnd->type       = st;
                        bnd->is_mut     = false;
                        bnd->moved      = false;
                        bhad_prev       = map_lookup(&c->scope, bname, &bprev);
                        map_put(&c->scope, bname, bnd);
                        bound           = true;
                        if (!arm.guard) saw_catchall = true;
                        break;
                    }
                    case PatKind::Literal: {
                        if (arm.pat.lit_is_bool) {
                            if (st->kind != TyKind::Error && st->kind != TyKind::Bool)
                                diag_errorf(c->diag, arm.span,
                                            "bool literal pattern can't match %s scrutinee",
                                            tyname(st));
                            if (!arm.guard) {
                                if (arm.pat.lit_bool_val) saw_bool_true  = true;
                                else                      saw_bool_false = true;
                            }
                        } else {
                            if (st->kind != TyKind::Error && st->kind != TyKind::Int)
                                diag_errorf(c->diag, arm.span,
                                            "integer literal pattern can't match %s scrutinee",
                                            tyname(st));
                            // Integer literals never make the match exhaustive
                            // on their own (an infinite domain remains).
                        }
                        break;
                    }
                    case PatKind::Variant: {
                        if (!ei) {
                            diag_errorf(c->diag, arm.span,
                                        "variant pattern can't match %s scrutinee",
                                        tyname(st));
                            break;
                        }
                        uint32_t idx = ei->nvariants;
                        for (uint32_t k = 0; k < ei->nvariants; k++)
                            if (ei->variant_names[k] == arm.pat.variant_name) { idx = k; break; }
                        if (idx == ei->nvariants) {
                            diag_errorf(c->diag, arm.span, "enum %s has no variant '%s'",
                                        ei->name_str, name_of(c, arm.pat.variant_name));
                        } else {
                            if (!arm.guard) covered[idx] = true;
                            if (arm.pat.has_binding) {
                                Type* pt = ei->variant_payload_types[idx];
                                if (!pt) {
                                    diag_errorf(c->diag, arm.span,
                                                "variant '%s' is fieldless; remove the '(...)' binding",
                                                name_of(c, arm.pat.variant_name));
                                } else {
                                    bname           = arm.pat.binding_name;
                                    Binding* bnd    = ARENA_NEW(c->a, Binding);
                                    bnd->type       = pt;
                                    bnd->is_mut     = false;
                                    bnd->moved      = false;
                                    bhad_prev       = map_lookup(&c->scope, bname, &bprev);
                                    map_put(&c->scope, bname, bnd);
                                    bound           = true;
                                }
                            }
                        }
                        break;
                    }
                }

                if (arm.guard) {
                    Type* gt = check_expr(c, arm.guard);
                    if (gt->kind != TyKind::Error && gt->kind != TyKind::Bool)
                        diag_errorf(c->diag, arm.guard->span,
                                    "match guard must be bool, got %s", tyname(gt));
                }

                Type* bt = check_expr(c, arm.body);
                if (!result) result = bt;
                else if (!type_eq(result, bt))
                    diag_errorf(c->diag, arm.body->span,
                                "match arms have different types: %s vs %s",
                                tyname(result), tyname(bt));

                if (bound) {
                    if (bhad_prev) map_put(&c->scope, bname, bprev);
                    else           map_remove(&c->scope, bname);
                }
            }

            // Exhaustiveness.
            if (!saw_catchall) {
                if (ei) {
                    for (uint32_t k = 0; k < ei->nvariants; k++)
                        if (!covered[k])
                            diag_errorf(c->diag, e->span,
                                        "non-exhaustive match: variant '%s' is not handled",
                                        name_of(c, ei->variant_names[k]));
                } else if (st->kind == TyKind::Bool) {
                    if (!saw_bool_true || !saw_bool_false)
                        diag_errorf(c->diag, e->span,
                                    "non-exhaustive match on bool: %s is not handled",
                                    saw_bool_true ? "false" : "true");
                } else if (st->kind == TyKind::Int) {
                    diag_error(c->diag, e->span,
                               "non-exhaustive match on integer: add '_' or a binding catch-all");
                }
            }
            covered.free();
            return result ? result : &c->t_void;
        }
        case ExprKind::Cast: {
            Type* src = check_expr(c, e->as.cast.operand);
            Type* dst = resolve_type(c, e->as.cast.target);
            // resolve_type already reported any unknown target type.
            if (dst->kind == TyKind::Error) return &c->t_error;
            // A prior error in the operand shouldn't cascade: trust the
            // annotation and continue with the destination type.
            if (src->kind == TyKind::Error) return dst;
            // `as` converts only between numeric scalars (int/float).
            if (is_numeric(src) && is_numeric(dst)) return dst;
            diag_errorf(c->diag, e->span,
                        "cannot cast %s to %s; 'as' only converts between numeric types",
                        tyname(src), tyname(dst));
            return &c->t_error;
        }
    }
    return &c->t_error;
}

void check_stmt(Checker* c, const Stmt* s) {
    switch (s->kind) {
        case StmtKind::Binding: {
            const BindingData& bd = s->as.binding;
            // Resolve the annotation first so an unsuffixed literal initialiser
            // can adopt it (e.g. `let x: i64 = 5`).
            Type* ann_hint = bd.type ? resolve_type(c, bd.type) : nullptr;
            Type* init = check_expr_as(c, bd.init, ann_hint);
            Type* ty   = init;
            if (bd.type) {
                Type* ann = ann_hint;
                if (!type_eq(ann, init) && init->kind != TyKind::Error)
                    diag_errorf(c->diag, bd.init->span, "binding '%s' expects %s, got %s",
                                name_of(c, bd.name), tyname(ann), tyname(init));
                ty = ann;
            } else if (init->kind == TyKind::Void) {
                diag_errorf(c->diag, s->span, "cannot bind a value of type void");
                ty = &c->t_error;
            }
            // `let y = x` moves x if it's a non-Copy binding.
            maybe_move(c, bd.init);

            // NLL: if the init is `ref [mut] target` of a bare ident, promote
            // the anon borrow we just registered into a let-bound borrow on
            // borrow_state[target], with expiry = last stmt that mentions
            // this binding's name in the function-root block.
            if (c->cur_func_block && bd.init && bd.init->kind == ExprKind::Ref &&
                bd.init->as.ref_.operand &&
                bd.init->as.ref_.operand->kind == ExprKind::Ident) {
                uint32_t target = bd.init->as.ref_.operand->as.ident.name;
                bool     is_mut = bd.init->as.ref_.is_mut;
                void*    bsv    = nullptr;
                if (map_lookup(&c->borrow_state, target, &bsv)) {
                    BorrowState* bs = static_cast<BorrowState*>(bsv);
                    // Undo the anon increment from check_expr Ref...
                    if (is_mut) bs->anon_exclusive = false;
                    else if (bs->anon_shared > 0) bs->anon_shared--;
                    // ...and remember it as a let-borrow expiring at last use.
                    LetBorrow lb{};
                    lb.handle_name      = bd.name;
                    lb.is_mut           = is_mut;
                    lb.expires_at_stmt  =
                        scan_last_use(c->cur_func_block, c->cur_stmt_idx + 1, bd.name);
                    if (lb.expires_at_stmt < c->cur_stmt_idx)
                        lb.expires_at_stmt = c->cur_stmt_idx; // never used again
                    bs->let_borrows.push(lb);
                }
            }

            Binding* bnd = ARENA_NEW(c->a, Binding);
            bnd->type   = ty;
            bnd->is_mut = bd.is_var; bnd->moved = false;
            bnd->moved  = false;
            map_put(&c->scope, bd.name, bnd);
            break;
        }
        case StmtKind::Return: {
            if (s->as.ret.value) {
                Type* t = check_expr_as(c, s->as.ret.value, c->cur_ret);
                if (!type_eq(t, c->cur_ret))
                    diag_errorf(c->diag, s->as.ret.value->span, "return expects %s, got %s",
                                tyname(c->cur_ret), tyname(t));
            } else if (c->cur_ret->kind != TyKind::Void) {
                diag_errorf(c->diag, s->span, "return without a value, but the function returns %s",
                            tyname(c->cur_ret));
            }
            break;
        }
        case StmtKind::ExprStmt:
            check_expr(c, s->as.expr_stmt.expr);
            break;
        case StmtKind::Defer:
            check_expr(c, s->as.defer_.expr);
            break;
        case StmtKind::Break:
            if (c->loop_depth == 0)
                diag_error(c->diag, s->span, "'break' used outside of a loop");
            break;
        case StmtKind::Continue:
            if (c->loop_depth == 0)
                diag_error(c->diag, s->span, "'continue' used outside of a loop");
            break;
    }
    // Anonymous borrows do not survive their enclosing statement.
    release_anon_borrows(c);
}

Type* check_block(Checker* c, const BlockData* b, bool is_func_root, Type* tail_hint) {
    // Nested blocks (if/else bodies, while bodies, match arms, ...) borrow
    // the parent's cur_stmt_idx; only the function-root block resets it.
    const BlockData* prev_func_block = c->cur_func_block;
    uint32_t         prev_stmt_idx   = c->cur_stmt_idx;
    if (is_func_root) {
        c->cur_func_block = b;
        c->cur_stmt_idx   = 0;
    }
    for (uint32_t i = 0; i < b->nstmts; i++) {
        if (b->stmts[i]->kind == StmtKind::Defer && !is_func_root) {
            diag_error(c->diag, b->stmts[i]->span,
                       "defer must appear at the function's top level (nested defer is not supported yet)");
        }
        if (is_func_root) c->cur_stmt_idx = i;
        check_stmt(c, b->stmts[i]);
    }
    Type* result = &c->t_void;
    if (b->tail) {
        // Treat the tail expression as occupying index nstmts (so last-use
        // scans that include tail-mentions stay in scope through it).
        if (is_func_root) c->cur_stmt_idx = b->nstmts;
        result = check_expr_as(c, b->tail, tail_hint);
        release_anon_borrows(c);
    }
    if (is_func_root) {
        c->cur_func_block = prev_func_block;
        c->cur_stmt_idx   = prev_stmt_idx;
    }
    return result;
}

// Conservative syntactic check: does `e` somewhere reference an Ident named
// `name`? Used for @must_defer pairing (defer expression / return-of binding).
bool expr_mentions(const Expr* e, uint32_t name) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::Ident: return e->as.ident.name == name;
        case ExprKind::Unary: return expr_mentions(e->as.unary.operand, name);
        case ExprKind::Binary:
            return expr_mentions(e->as.binary.lhs, name) || expr_mentions(e->as.binary.rhs, name);
        case ExprKind::Call:
            if (expr_mentions(e->as.call.callee, name)) return true;
            for (uint32_t i = 0; i < e->as.call.nargs; i++)
                if (expr_mentions(e->as.call.args[i], name)) return true;
            return false;
        case ExprKind::Field: return expr_mentions(e->as.field.obj, name);
        case ExprKind::Ref:   return expr_mentions(e->as.ref_.operand, name);
        case ExprKind::StructLit:
            for (uint32_t i = 0; i < e->as.struct_lit.nfields; i++)
                if (expr_mentions(e->as.struct_lit.fields[i].value, name)) return true;
            return false;
        case ExprKind::Block: {
            const BlockData& b = e->as.block;
            for (uint32_t i = 0; i < b.nstmts; i++) {
                const Stmt* s = b.stmts[i];
                if (s->kind == StmtKind::ExprStmt && expr_mentions(s->as.expr_stmt.expr, name)) return true;
                if (s->kind == StmtKind::Defer    && expr_mentions(s->as.defer_.expr, name))   return true;
                if (s->kind == StmtKind::Return   && expr_mentions(s->as.ret.value, name))     return true;
                if (s->kind == StmtKind::Binding  && expr_mentions(s->as.binding.init, name))  return true;
            }
            return expr_mentions(b.tail, name);
        }
        case ExprKind::If:
            return expr_mentions(e->as.if_.cond, name) ||
                   expr_mentions(e->as.if_.then_blk, name) ||
                   expr_mentions(e->as.if_.else_blk, name);
        case ExprKind::While: return expr_mentions(e->as.while_.cond, name) || expr_mentions(e->as.while_.body, name);
        case ExprKind::Loop:  return expr_mentions(e->as.loop_.body, name);
        case ExprKind::For:   return expr_mentions(e->as.for_.iter, name) || expr_mentions(e->as.for_.body, name);
        case ExprKind::Match: {
            if (expr_mentions(e->as.match_.scrutinee, name)) return true;
            for (uint32_t i = 0; i < e->as.match_.narms; i++)
                if (expr_mentions(e->as.match_.arms[i].body, name)) return true;
            return false;
        }
        default: return false; // literals (Int/Float/Bool/Null/Str)
    }
}

// Copy types pass by value freely; non-Copy types move on by-value transfer.
// Primitives + Ptr + Ref are Copy; Struct/Enum are not. Error/Void treated as
// Copy to avoid cascading errors.
bool is_copy_type(const Type* t) {
    if (!t) return true;
    switch (t->kind) {
        case TyKind::Struct:
        case TyKind::Enum:   return false;
        default:             return true;
    }
}

// If `e` is a bare Ident of non-Copy type, mark that binding as moved.
// Called from consuming sites (let init, call args) AFTER check_expr.
void maybe_move(Checker* c, const Expr* e) {
    if (!e || e->kind != ExprKind::Ident) return;
    void* bv = nullptr;
    if (!map_lookup(&c->scope, e->as.ident.name, &bv)) return;
    Binding* bnd = static_cast<Binding*>(bv);
    if (is_copy_type(bnd->type)) return;
    // Move-after-borrow: a value cannot be moved while a borrow of it is still
    // live (NLL releases expired let-borrows, so this only fires on real overlap).
    void* bsv = nullptr;
    if (map_lookup(&c->borrow_state, e->as.ident.name, &bsv)) {
        BorrowState* bs     = static_cast<BorrowState*>(bsv);
        uint32_t     shared = 0;
        bool         excl   = false;
        effective_borrows(c, bs, &shared, &excl);
        if (shared > 0 || excl)
            diag_errorf(c->diag, e->span, "cannot move '%s' while it is borrowed",
                        name_of(c, e->as.ident.name));
    }
    bnd->moved = true;
}

// NLL: compute the live borrow counts for one binding at the current stmt
// index. Anonymous borrows contribute directly; let-bound borrows count only
// if the current index is still within their last-use window.
void effective_borrows(const Checker* c, const BorrowState* bs,
                       uint32_t* out_shared, bool* out_excl) {
    uint32_t shared = bs->anon_shared;
    bool     excl   = bs->anon_exclusive;
    for (uint32_t i = 0; i < bs->let_borrows.len; i++) {
        const LetBorrow& lb = bs->let_borrows[i];
        if (c->cur_stmt_idx > lb.expires_at_stmt) continue; // expired
        if (lb.is_mut) excl = true;
        else           shared++;
    }
    *out_shared = shared;
    *out_excl   = excl;
}

// Zero every binding's anon borrow counters; called at end of each stmt.
void release_anon_borrows(Checker* c) {
    for (size_t i = 0; i < c->borrow_state.capacity; i++) {
        if (!c->borrow_state.state[i]) continue;
        BorrowState* bs = static_cast<BorrowState*>(c->borrow_state.slots[i].val);
        bs->anon_shared    = 0;
        bs->anon_exclusive = false;
    }
}

// Find the largest stmt index in [from, b->nstmts] at which `name` is
// mentioned. Tail expression counts at index nstmts. If the name is never
// mentioned from `from` onward, returns from-1 (so the caller can detect
// "no later use" by comparing against the binding's own stmt index).
uint32_t scan_last_use(const BlockData* b, uint32_t from, uint32_t name) {
    uint32_t last = (from == 0) ? 0 : from - 1;
    bool     any  = false;
    for (uint32_t i = from; i < b->nstmts; i++) {
        const Stmt* s = b->stmts[i];
        bool hit = false;
        switch (s->kind) {
            case StmtKind::Binding:  hit = expr_mentions(s->as.binding.init, name); break;
            case StmtKind::Return:   hit = expr_mentions(s->as.ret.value, name); break;
            case StmtKind::ExprStmt: hit = expr_mentions(s->as.expr_stmt.expr, name); break;
            case StmtKind::Defer:    hit = expr_mentions(s->as.defer_.expr, name); break;
            default: break;
        }
        if (hit) { last = i; any = true; }
    }
    if (b->tail && expr_mentions(b->tail, name)) {
        last = b->nstmts;
        any  = true;
    }
    return any ? last : ((from == 0) ? 0 : from - 1);
}

bool type_is_must_defer(const Type* t) {
    if (!t) return false;
    if (t->kind == TyKind::Struct) return t->sinfo && t->sinfo->is_must_defer;
    if (t->kind == TyKind::Enum)   return t->einfo && t->einfo->is_must_defer;
    return false;
}

// After body checking, enforce that each top-level binding of @must_defer type
// is paired with a defer that mentions it, a return-of-it, or a tail-of-it.
void check_must_defer(Checker* c, const FuncData* f) {
    if (!f->body || f->body->kind != ExprKind::Block) return;
    const BlockData& b = f->body->as.block;
    for (uint32_t i = 0; i < b.nstmts; i++) {
        const Stmt* s = b.stmts[i];
        if (s->kind != StmtKind::Binding) continue;
        const BindingData& bd = s->as.binding;
        void* bv = nullptr;
        if (!map_lookup(&c->scope, bd.name, &bv)) continue;
        Binding* bnd = static_cast<Binding*>(bv);
        if (!type_is_must_defer(bnd->type)) continue;

        bool paired = false;
        for (uint32_t j = i + 1; j < b.nstmts && !paired; j++) {
            const Stmt* s2 = b.stmts[j];
            if (s2->kind == StmtKind::Defer  && expr_mentions(s2->as.defer_.expr, bd.name)) paired = true;
            if (s2->kind == StmtKind::Return && expr_mentions(s2->as.ret.value, bd.name))   paired = true;
        }
        if (!paired && expr_mentions(b.tail, bd.name)) paired = true;
        if (!paired) {
            diag_errorf(c->diag, s->span,
                        "binding '%s' has @must_defer type %s; pair it with `defer ...%s...` "
                        "(or return it)", name_of(c, bd.name), tyname(bnd->type), name_of(c, bd.name));
        }
    }
}

void check_func(Checker* c, const FuncData* f, FuncSig* sig) {
    c->scope   = Map{};
    // BorrowStates hold an inner Vec<LetBorrow>; free those first.
    for (size_t i = 0; i < c->borrow_state.capacity; i++) {
        if (!c->borrow_state.state[i]) continue;
        BorrowState* bs = static_cast<BorrowState*>(c->borrow_state.slots[i].val);
        bs->let_borrows.free();
    }
    map_free(&c->borrow_state);
    c->borrow_state = Map{};
    c->loop_depth     = 0;
    c->cur_stmt_idx   = 0;
    c->cur_func_block = nullptr;
    c->cur_ret = sig->ret;

    for (uint32_t i = 0; i < f->nparams; i++) {
        Binding* bnd = ARENA_NEW(c->a, Binding);
        bnd->type   = sig->params[i];
        bnd->is_mut = false; bnd->moved = false; // params are immutable
        map_put(&c->scope, f->params[i].name, bnd);
    }

    if (f->body && f->body->kind == ExprKind::Block) {
        Type* body_ty = check_block(c, &f->body->as.block, /*is_func_root=*/true,
                                    /*tail_hint=*/sig->ret);
        // If the body produces a tail value, it must match the return type.
        // A body with no tail is assumed to return via explicit `return`s
        // (full all-paths-return analysis is deferred).
        if (f->body->as.block.tail && sig->ret->kind != TyKind::Void &&
            !type_eq(body_ty, sig->ret)) {
            diag_errorf(c->diag, f->body->as.block.tail->span,
                        "function returns %s, but its final expression has type %s",
                        tyname(sig->ret), tyname(body_ty));
        }
        check_must_defer(c, f);
    }

    map_free(&c->scope);
}

} // namespace

bool check_module(const Module* m, arena* a, Interner* in, Diag* diag) {
    Checker c{};
    c.a       = a;
    c.in      = in;
    c.diag    = diag;
    c.funcs        = Map{};
    c.structs      = Map{};
    c.enums        = Map{};
    c.scope        = Map{};
    c.borrow_state = Map{};

    uint32_t main_id = intern_cstr(in, "main");

    // Enum declarations. Variants may be fieldless or carry a single typed
    // payload; all payload-having variants of one enum must share a type.
    for (uint32_t i = 0; i < m->nitems; i++) {
        const Item* it = m->items[i];
        if (it->kind != ItemKind::Enum) continue;
        const EnumData& ed = it->as.enum_;
        if (map_contains(&c.enums, ed.name) || map_contains(&c.structs, ed.name)) {
            diag_errorf(diag, it->span, "duplicate definition of type '%s'", name_of(&c, ed.name));
            continue;
        }
        EnumInfo* ei              = ARENA_NEW(a, EnumInfo);
        ei->name                  = ed.name;
        ei->name_str              = name_of(&c, ed.name);
        ei->nvariants             = ed.nvariants;
        ei->variant_names         = ed.nvariants ? ARENA_NEW_N(a, uint32_t, ed.nvariants) : nullptr;
        ei->variant_payload_types = ed.nvariants ? ARENA_NEW_N(a, Type*, ed.nvariants) : nullptr;
        ei->payload_type          = nullptr;
        ei->is_must_defer         = ed.is_must_defer;
        for (uint32_t k = 0; k < ed.nvariants; k++) {
            ei->variant_names[k]         = ed.variants[k].name;
            ei->variant_payload_types[k] = nullptr;
            if (ed.variants[k].has_payload) {
                Type* pt = resolve_type(&c, ed.variants[k].payload_type);
                ei->variant_payload_types[k] = pt;
                if (!ei->payload_type) ei->payload_type = pt; // first-seen, just for diags
            }
        }
        map_put(&c.enums, ed.name, ei);
    }

    // Pass 0a: register struct names (so fields can reference any struct).
    for (uint32_t i = 0; i < m->nitems; i++) {
        const Item* it = m->items[i];
        if (it->kind != ItemKind::Struct) continue;
        const StructData& sd = it->as.struct_;
        if (map_contains(&c.structs, sd.name)) {
            diag_errorf(diag, it->span, "duplicate definition of type '%s'", name_of(&c, sd.name));
            continue;
        }
        StructInfo* si  = ARENA_NEW(a, StructInfo);
        si->name        = sd.name;
        si->name_str    = name_of(&c, sd.name);
        si->nfields     = sd.nfields;
        si->field_names = sd.nfields ? ARENA_NEW_N(a, uint32_t, sd.nfields) : nullptr;
        si->field_types = sd.nfields ? ARENA_NEW_N(a, Type*, sd.nfields) : nullptr;
        si->is_must_defer = sd.is_must_defer;
        for (uint32_t k = 0; k < sd.nfields; k++) si->field_names[k] = sd.fields[k].name;
        map_put(&c.structs, sd.name, si);
    }
    // Pass 0b: resolve field types (may reference other structs).
    for (uint32_t i = 0; i < m->nitems; i++) {
        const Item* it = m->items[i];
        if (it->kind != ItemKind::Struct) continue;
        const StructData& sd = it->as.struct_;
        void* siv = nullptr;
        if (!map_lookup(&c.structs, sd.name, &siv)) continue;
        StructInfo* si = static_cast<StructInfo*>(siv);
        for (uint32_t k = 0; k < sd.nfields; k++) si->field_types[k] = resolve_type(&c, sd.fields[k].type);
    }

    // Pass 1: function signatures.
    for (uint32_t i = 0; i < m->nitems; i++) {
        const Item* it = m->items[i];
        if (it->kind != ItemKind::Func) continue;
        const FuncData& f = it->as.func;

        if (map_contains(&c.funcs, f.name)) {
            diag_errorf(diag, it->span, "duplicate definition of '%s'", name_of(&c, f.name));
        }

        FuncSig* sig  = ARENA_NEW(a, FuncSig);
        sig->nparams  = f.nparams;
        sig->params   = f.nparams ? ARENA_NEW_N(a, Type*, f.nparams) : nullptr;
        for (uint32_t k = 0; k < f.nparams; k++) sig->params[k] = resolve_type(&c, f.params[k].type);
        sig->is_extern = f.is_extern;

        // main returns void or i32 (its declared return); resolve normally.
        sig->ret = resolve_type(&c, f.ret);
        if (sig->ret->kind == TyKind::Ref) {
            diag_error(diag, f.ret ? f.ret->span : it->span,
                       "borrows cannot be returned (the Scope Rule)");
        }
        (void)main_id;

        map_put(&c.funcs, f.name, sig);
    }

    // Pass 2: bodies.
    for (uint32_t i = 0; i < m->nitems; i++) {
        const Item* it = m->items[i];
        if (it->kind != ItemKind::Func) continue;
        const FuncData& f = it->as.func;
        if (f.is_extern) continue;
        void* v = nullptr;
        map_lookup(&c.funcs, f.name, &v);
        check_func(&c, &f, static_cast<FuncSig*>(v));
    }

    map_free(&c.funcs);
    map_free(&c.structs);
    map_free(&c.enums);
    for (size_t i = 0; i < c.borrow_state.capacity; i++) {
        if (!c.borrow_state.state[i]) continue;
        BorrowState* bs = static_cast<BorrowState*>(c.borrow_state.slots[i].val);
        bs->let_borrows.free();
    }
    map_free(&c.borrow_state);
    return !diag_has_errors(diag);
}

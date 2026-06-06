#include "lower.h"

#include <cstring>

#include "map.h"
#include "vec.h"

namespace {

struct Lowerer {
    arena*    a;
    Interner* in;
    Diag*     diag;
    Map       func_index;   // interned name -> (index into funcs)
    Map       struct_index; // interned name -> (index into structs)
    Map       enum_byname;  // interned name -> LEnum* (fieldless variant list)
    MModule*  mod;

    // Per-function build state.
    Vec<MInst>         pool;    // flat instruction pool; id = index
    Vec<Vec<uint32_t>> blocks;  // each block is an ordered list of ids
    uint32_t           entry;   // entry block index (allocas + params)
    uint32_t           current; // block new instructions append to
    Map                scope;   // interned name -> slot (Alloca id)
    bool               is_main;
    MType              ret_type;
    Vec<const Expr*>   pending_defers; // function-scope defer stack (LIFO at exit)
    // Active loop frames for break/continue. continue_bb is the cond/step block
    // (where iteration resumes); break_bb is the loop's exit block.
    struct LoopFrame { uint32_t continue_bb; uint32_t break_bb; };
    Vec<LoopFrame>     loop_stack;
};

// An enum's lower-time representation. Two shapes:
//   * all variants fieldless -> plain u32 tag; no MEnumDef created.
//   * any variant has a payload -> { i32 tag, [N x i8] buf } repr registered as
//     an MEnumDef. Fieldless variants still live in the repr (payload undef).
// Per-variant payload types may differ across variants (mixed unions).
struct LEnum {
    uint32_t* variants;          // variant names
    MType*    variant_payloads;  // per variant; mt_void() if fieldless
    uint32_t  n;
    bool      any_payload;
    uint32_t  enum_index;        // index into MModule.enums (only when any_payload)
};

uint32_t enum_variant_index(const LEnum* le, uint32_t variant) {
    for (uint32_t k = 0; k < le->n; k++)
        if (le->variants[k] == variant) return k;
    return le->n;
}

MInst mk(MOp op, MType ty, Span sp) {
    MInst i{};
    i.op          = op;
    i.type        = ty;
    i.span        = sp;
    i.a           = M_NO_VALUE;
    i.b           = M_NO_VALUE;
    i.param_index = 0;
    i.callee      = 0;
    i.args        = nullptr;
    i.nargs       = 0;
    i.target      = 0;
    i.target2     = 0;
    i.field_index = 0;
    return i;
}

uint32_t emit(Lowerer* L, const MInst& i) {
    uint32_t id = static_cast<uint32_t>(L->pool.len);
    L->pool.push(i);
    L->blocks[L->current].push(id);
    return id;
}

// Allocas and params live in the entry block so they execute once.
uint32_t emit_entry(Lowerer* L, const MInst& i) {
    uint32_t id = static_cast<uint32_t>(L->pool.len);
    L->pool.push(i);
    L->blocks[L->entry].push(id);
    return id;
}

uint32_t new_block(Lowerer* L) {
    uint32_t idx = static_cast<uint32_t>(L->blocks.len);
    L->blocks.push(Vec<uint32_t>{});
    return idx;
}

void set_cur(Lowerer* L, uint32_t b) { L->current = b; }

bool is_terminated(Lowerer* L, uint32_t b) {
    const Vec<uint32_t>& ids = L->blocks[b];
    if (ids.len == 0) return false;
    switch (L->pool[ids[ids.len - 1]].op) {
        case MOp::Br: case MOp::CondBr: case MOp::Ret:
        case MOp::RetVoid: case MOp::Unreachable:
            return true;
        default:
            return false;
    }
}

MType type_of(Lowerer* L, uint32_t ref) { return L->pool[ref].type; }

uint32_t const_i32(Lowerer* L, Span sp, uint64_t v) {
    MInst i   = mk(MOp::ConstInt, mt_int(32, true), sp);
    i.imm_int = v;
    return emit(L, i);
}

uint32_t const_bool(Lowerer* L, Span sp, bool v) {
    MInst i    = mk(MOp::ConstBool, mt_bool(), sp);
    i.imm_bool = v;
    return emit(L, i);
}

// Build a default value for `ty`. Used when constructing a fieldless variant
// of a payload-carrying enum (its payload slot needs *some* well-typed value).
uint32_t emit_default(Lowerer* L, MType ty, Span sp) {
    switch (ty.kind) {
        case MTypeKind::Bool: return const_bool(L, sp, false);
        case MTypeKind::Float: {
            MInst i = mk(MOp::ConstFloat, ty, sp);
            i.imm_float = 0.0;
            return emit(L, i);
        }
        case MTypeKind::Ptr: {
            MInst i = mk(MOp::ConstInt, mt_ptr(), sp);
            i.imm_int = 0;
            return emit(L, i);
        }
        case MTypeKind::Struct: {
            const MStructDef& sd = L->mod->structs[ty.struct_index];
            Vec<uint32_t> args{};
            for (uint32_t k = 0; k < sd.nfields; k++)
                args.push(emit_default(L, sd.field_types[k], sp));
            MInst inst = mk(MOp::MakeStruct, ty, sp);
            inst.nargs = sd.nfields;
            if (sd.nfields) {
                inst.args = ARENA_NEW_N(L->a, uint32_t, sd.nfields);
                std::memcpy(inst.args, args.data, sd.nfields * sizeof(uint32_t));
            }
            args.free();
            return emit(L, inst);
        }
        default: {
            MInst i = mk(MOp::ConstInt, ty.kind == MTypeKind::Int ? ty : mt_int(32, true), sp);
            i.imm_int = 0;
            return emit(L, i);
        }
    }
}

// ----------------------------------------------------------------- type mapping
MType suffix_type(NumSuffix s) {
    switch (s) {
        case NumSuffix::I8:   return mt_int(8, true);
        case NumSuffix::I16:  return mt_int(16, true);
        case NumSuffix::I32:  return mt_int(32, true);
        case NumSuffix::I64:  return mt_int(64, true);
        case NumSuffix::I128: return mt_int(128, true);
        case NumSuffix::Iptr: return mt_int(64, true);
        case NumSuffix::U8:   return mt_int(8, false);
        case NumSuffix::U16:  return mt_int(16, false);
        case NumSuffix::U32:  return mt_int(32, false);
        case NumSuffix::U64:  return mt_int(64, false);
        case NumSuffix::U128: return mt_int(128, false);
        case NumSuffix::Uptr: return mt_int(64, false);
        case NumSuffix::F32:  return mt_float(32);
        case NumSuffix::F64:  return mt_float(64);
        case NumSuffix::None: return mt_void();
    }
    return mt_void();
}

MType map_type(Lowerer* L, const TypeExpr* t) {
    if (!t) return mt_void();
    if (t->kind == TypeKind::Pointer) return mt_ptr();
    if (t->kind == TypeKind::Ref) {
        // ref T: if T resolves to a struct, remember the pointee for Load.
        MType inner = map_type(L, t->inner);
        if (inner.kind == MTypeKind::Struct) return mt_ref(inner.struct_index);
        return mt_ptr(); // ref to non-struct: limited support
    }

    size_t      len = 0;
    const char* s   = intern_str(L->in, t->name, &len);
    if (!s) return mt_void();

    struct Named { const char* n; MType ty; };
    static const Named NAMED[] = {
        {"i8", mt_int(8, true)},     {"i16", mt_int(16, true)},
        {"i32", mt_int(32, true)},   {"i64", mt_int(64, true)},
        {"i128", mt_int(128, true)}, {"iptr", mt_int(64, true)},
        {"u8", mt_int(8, false)},    {"u16", mt_int(16, false)},
        {"u32", mt_int(32, false)},  {"u64", mt_int(64, false)},
        {"u128", mt_int(128, false)},{"uptr", mt_int(64, false)},
        {"f32", mt_float(32)},       {"f64", mt_float(64)},
        {"bool", mt_bool()},         {"void", mt_void()},
        {"str", mt_ptr()},
    };
    for (const Named& nm : NAMED) {
        if (std::strlen(nm.n) == len && std::memcmp(nm.n, s, len) == 0) return nm.ty;
    }
    void* iv = nullptr;
    if (map_lookup(&L->struct_index, t->name, &iv)) {
        return mt_struct(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(iv)));
    }
    {
        void* lev = nullptr;
        if (map_lookup(&L->enum_byname, t->name, &lev)) {
            LEnum* le = static_cast<LEnum*>(lev);
            return le->any_payload ? mt_enum(le->enum_index) : mt_int(32, false);
        }
    }
    diag_errorf(L->diag, t->span, "unknown type '%.*s'", static_cast<int>(len), s);
    return mt_int(32, true);
}

bool is_float(MType t) { return t.kind == MTypeKind::Float; }

bool is_compound_assign(Tok op) {
    switch (op) {
        case Tok::PlusEq: case Tok::MinusEq: case Tok::StarEq: case Tok::SlashEq:
        case Tok::PercentEq: case Tok::AmpEq: case Tok::PipeEq: case Tok::CaretEq:
        case Tok::ShlEq: case Tok::ShrEq:
            return true;
        default:
            return false;
    }
}

void map_binop(Tok op, MType ty, MOp* out, bool* is_cmp) {
    bool f = is_float(ty);
    bool s = ty.is_signed;
    *is_cmp = false;
    switch (op) {
        case Tok::Plus:    *out = f ? MOp::FAdd : MOp::Add; return;
        case Tok::Minus:   *out = f ? MOp::FSub : MOp::Sub; return;
        case Tok::Star:    *out = f ? MOp::FMul : MOp::Mul; return;
        case Tok::Slash:   *out = f ? MOp::FDiv : (s ? MOp::SDiv : MOp::UDiv); return;
        case Tok::Percent: *out = s ? MOp::SRem : MOp::URem; return;
        case Tok::Amp:     *out = MOp::And; return;
        case Tok::Pipe:    *out = MOp::Or;  return;
        case Tok::Caret:   *out = MOp::Xor; return;
        case Tok::Shl:     *out = MOp::Shl; return;
        case Tok::Shr:     *out = s ? MOp::AShr : MOp::LShr; return;
        case Tok::EqEq:    *out = f ? MOp::FCmpEq : MOp::ICmpEq; *is_cmp = true; return;
        case Tok::NotEq:   *out = f ? MOp::FCmpNe : MOp::ICmpNe; *is_cmp = true; return;
        case Tok::Lt:      *out = f ? MOp::FCmpLt : (s ? MOp::ICmpLt : MOp::ICmpULt); *is_cmp = true; return;
        case Tok::Gt:      *out = f ? MOp::FCmpGt : (s ? MOp::ICmpGt : MOp::ICmpUGt); *is_cmp = true; return;
        case Tok::Le:      *out = f ? MOp::FCmpLe : (s ? MOp::ICmpLe : MOp::ICmpULe); *is_cmp = true; return;
        case Tok::Ge:      *out = f ? MOp::FCmpGe : (s ? MOp::ICmpGe : MOp::ICmpUGe); *is_cmp = true; return;
        default:           *out = MOp::Add; return;
    }
}

// ----------------------------------------------------------------- expressions
uint32_t lower_expr(Lowerer* L, const Expr* e);
uint32_t lower_block_value(Lowerer* L, const BlockData* b);

uint32_t lower_call(Lowerer* L, const Expr* e) {
    const CallData& c = e->as.call;

    // Payload variant construction: Enum.Variant(arg) is parsed as a Call.
    if (c.callee->kind == ExprKind::Field &&
        c.callee->as.field.obj->kind == ExprKind::Ident) {
        void* lev = nullptr;
        if (map_lookup(&L->enum_byname, c.callee->as.field.obj->as.ident.name, &lev)) {
            LEnum*   le  = static_cast<LEnum*>(lev);
            uint32_t idx = enum_variant_index(le, c.callee->as.field.name);
            uint32_t payload = M_NO_VALUE;
            if (c.nargs > 0) payload = lower_expr(L, c.args[0]);
            MInst inst   = mk(MOp::EnumCons, mt_enum(le->enum_index), e->span);
            inst.imm_int = idx;
            inst.a       = payload;
            return emit(L, inst);
        }
    }

    if (c.callee->kind != ExprKind::Ident) {
        diag_errorf(L->diag, e->span, "only direct calls to named functions are supported");
        return const_i32(L, e->span, 0);
    }
    uint32_t name = c.callee->as.ident.name;
    void*    v    = nullptr;
    if (!map_lookup(&L->func_index, name, &v)) {
        size_t      len = 0;
        const char* s   = intern_str(L->in, name, &len);
        diag_errorf(L->diag, e->span, "unknown function '%.*s'", static_cast<int>(len), s ? s : "");
        return const_i32(L, e->span, 0);
    }
    uint32_t fidx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(v));

    Vec<uint32_t> args{};
    for (uint32_t i = 0; i < c.nargs; i++) args.push(lower_expr(L, c.args[i]));

    MInst inst  = mk(MOp::Call, L->mod->funcs[fidx].ret, e->span);
    inst.callee = fidx;
    inst.nargs  = static_cast<uint32_t>(args.len);
    if (args.len > 0) {
        inst.args = ARENA_NEW_N(L->a, uint32_t, args.len);
        std::memcpy(inst.args, args.data, args.len * sizeof(uint32_t));
    }
    args.free();
    return emit(L, inst);
}

// `a && b` => if a { b } else { false };  `a || b` => if a { true } else { b }.
uint32_t lower_short_circuit(Lowerer* L, const Expr* e) {
    bool is_and = (e->as.binary.op == Tok::AmpAmp);

    uint32_t slot = emit_entry(L, mk(MOp::Alloca, mt_bool(), e->span));
    uint32_t av   = lower_expr(L, e->as.binary.lhs);

    uint32_t then_bb = new_block(L);
    uint32_t else_bb = new_block(L);
    uint32_t merge   = new_block(L);

    MInst cb   = mk(MOp::CondBr, mt_void(), e->span);
    cb.a       = av;
    cb.target  = then_bb;
    cb.target2 = else_bb;
    emit(L, cb);

    set_cur(L, then_bb);
    {
        uint32_t v = is_and ? lower_expr(L, e->as.binary.rhs) : const_bool(L, e->span, true);
        MInst    st = mk(MOp::Store, mt_void(), e->span);
        st.a = slot; st.b = v;
        emit(L, st);
        MInst br = mk(MOp::Br, mt_void(), e->span); br.target = merge; emit(L, br);
    }
    set_cur(L, else_bb);
    {
        uint32_t v = is_and ? const_bool(L, e->span, false) : lower_expr(L, e->as.binary.rhs);
        MInst    st = mk(MOp::Store, mt_void(), e->span);
        st.a = slot; st.b = v;
        emit(L, st);
        MInst br = mk(MOp::Br, mt_void(), e->span); br.target = merge; emit(L, br);
    }
    set_cur(L, merge);
    MInst ld = mk(MOp::Load, mt_bool(), e->span);
    ld.a = slot;
    return emit(L, ld);
}

uint32_t lower_if(Lowerer* L, const Expr* e) {
    uint32_t cond = lower_expr(L, e->as.if_.cond);

    uint32_t then_bb = new_block(L);
    uint32_t else_bb = new_block(L);
    uint32_t merge   = new_block(L);

    MInst cb   = mk(MOp::CondBr, mt_void(), e->span);
    cb.a       = cond;
    cb.target  = then_bb;
    cb.target2 = else_bb;
    emit(L, cb);

    bool     has_else   = (e->as.if_.else_blk != nullptr);
    uint32_t result_slot = M_NO_VALUE;

    // then-branch
    set_cur(L, then_bb);
    uint32_t tv = lower_block_value(L, &e->as.if_.then_blk->as.block);
    if (has_else && tv != M_NO_VALUE && type_of(L, tv).kind != MTypeKind::Void) {
        result_slot = emit_entry(L, mk(MOp::Alloca, type_of(L, tv), e->span));
        MInst st = mk(MOp::Store, mt_void(), e->span);
        st.a = result_slot; st.b = tv;
        emit(L, st);
    }
    if (!is_terminated(L, L->current)) {
        MInst br = mk(MOp::Br, mt_void(), e->span); br.target = merge; emit(L, br);
    }

    // else-branch
    set_cur(L, else_bb);
    if (has_else) {
        uint32_t ev = lower_expr(L, e->as.if_.else_blk);
        if (result_slot != M_NO_VALUE && ev != M_NO_VALUE) {
            MInst st = mk(MOp::Store, mt_void(), e->span);
            st.a = result_slot; st.b = ev;
            emit(L, st);
        }
    }
    if (!is_terminated(L, L->current)) {
        MInst br = mk(MOp::Br, mt_void(), e->span); br.target = merge; emit(L, br);
    }

    set_cur(L, merge);
    if (result_slot != M_NO_VALUE) {
        MInst ld = mk(MOp::Load, L->pool[result_slot].type, e->span);
        ld.a = result_slot;
        return emit(L, ld);
    }
    return M_NO_VALUE;
}

uint32_t lower_while(Lowerer* L, const Expr* e) {
    uint32_t cond_bb = new_block(L);
    uint32_t body_bb = new_block(L);
    uint32_t end_bb  = new_block(L);

    MInst br0 = mk(MOp::Br, mt_void(), e->span); br0.target = cond_bb; emit(L, br0);

    set_cur(L, cond_bb);
    uint32_t c  = lower_expr(L, e->as.while_.cond);
    MInst    cb = mk(MOp::CondBr, mt_void(), e->span);
    cb.a = c; cb.target = body_bb; cb.target2 = end_bb;
    emit(L, cb);

    set_cur(L, body_bb);
    L->loop_stack.push(Lowerer::LoopFrame{cond_bb, end_bb}); // continue -> cond
    lower_block_value(L, &e->as.while_.body->as.block);
    L->loop_stack.len--;
    if (!is_terminated(L, L->current)) {
        MInst br = mk(MOp::Br, mt_void(), e->span); br.target = cond_bb; emit(L, br);
    }

    set_cur(L, end_bb);
    return M_NO_VALUE;
}

uint32_t lower_loop(Lowerer* L, const Expr* e) {
    uint32_t body_bb = new_block(L);
    uint32_t end_bb  = new_block(L);

    MInst br0 = mk(MOp::Br, mt_void(), e->span);
    br0.target = body_bb;
    emit(L, br0);

    set_cur(L, body_bb);
    L->loop_stack.push(Lowerer::LoopFrame{body_bb, end_bb});
    lower_block_value(L, &e->as.loop_.body->as.block);
    L->loop_stack.len--; // pop

    if (!is_terminated(L, L->current)) {
        MInst br = mk(MOp::Br, mt_void(), e->span);
        br.target = body_bb;
        emit(L, br);
    }
    set_cur(L, end_bb);
    return M_NO_VALUE;
}

uint32_t lower_for(Lowerer* L, const Expr* e) {
    const ForData& fr      = e->as.for_;
    const Expr*    it      = fr.iter;
    bool           inclusive = (it->as.binary.op == Tok::DotDotEq);

    // Lower lo once, store into the loop variable's slot.
    uint32_t lo_val   = lower_expr(L, it->as.binary.lhs);
    MType    int_ty   = type_of(L, lo_val);
    uint32_t var_slot = emit_entry(L, mk(MOp::Alloca, int_ty, e->span));
    {
        MInst st = mk(MOp::Store, mt_void(), e->span);
        st.a = var_slot;
        st.b = lo_val;
        emit(L, st);
    }
    // Lower hi once into a hidden slot so it isn't re-evaluated per iteration.
    uint32_t hi_val  = lower_expr(L, it->as.binary.rhs);
    uint32_t hi_slot = emit_entry(L, mk(MOp::Alloca, int_ty, e->span));
    {
        MInst st = mk(MOp::Store, mt_void(), e->span);
        st.a = hi_slot;
        st.b = hi_val;
        emit(L, st);
    }

    uint32_t cond_bb = new_block(L);
    uint32_t body_bb = new_block(L);
    uint32_t step_bb = new_block(L);
    uint32_t end_bb  = new_block(L);

    MInst br_init = mk(MOp::Br, mt_void(), e->span); br_init.target = cond_bb; emit(L, br_init);

    // cond: i (op) hi  --> CondBr to body / end
    set_cur(L, cond_bb);
    MInst li = mk(MOp::Load, int_ty, e->span); li.a = var_slot;
    uint32_t i_val = emit(L, li);
    MInst lh = mk(MOp::Load, int_ty, e->span); lh.a = hi_slot;
    uint32_t h_val = emit(L, lh);
    MInst cmp = mk(inclusive ? MOp::ICmpLe : MOp::ICmpLt, mt_bool(), e->span);
    cmp.a = i_val; cmp.b = h_val;
    uint32_t test = emit(L, cmp);
    MInst cb = mk(MOp::CondBr, mt_void(), e->span);
    cb.a = test; cb.target = body_bb; cb.target2 = end_bb;
    emit(L, cb);

    // body: bind var to var_slot in scope (save/restore), lower body
    set_cur(L, body_bb);
    void* prev = nullptr;
    bool  had  = map_lookup(&L->scope, fr.name, &prev);
    map_put(&L->scope, fr.name, reinterpret_cast<void*>(static_cast<uintptr_t>(var_slot)));
    L->loop_stack.push(Lowerer::LoopFrame{step_bb, end_bb});
    lower_block_value(L, &fr.body->as.block);
    L->loop_stack.len--;
    if (had) map_put(&L->scope, fr.name, prev);
    else     map_remove(&L->scope, fr.name);
    if (!is_terminated(L, L->current)) {
        MInst br = mk(MOp::Br, mt_void(), e->span); br.target = step_bb; emit(L, br);
    }

    // step: i = i + 1; Br cond
    set_cur(L, step_bb);
    MInst li2 = mk(MOp::Load, int_ty, e->span); li2.a = var_slot;
    uint32_t iv2 = emit(L, li2);
    MInst one = mk(MOp::ConstInt, int_ty, e->span); one.imm_int = 1;
    uint32_t one_v = emit(L, one);
    MInst add = mk(MOp::Add, int_ty, e->span); add.a = iv2; add.b = one_v;
    uint32_t next = emit(L, add);
    MInst st_next = mk(MOp::Store, mt_void(), e->span);
    st_next.a = var_slot; st_next.b = next;
    emit(L, st_next);
    MInst br_back = mk(MOp::Br, mt_void(), e->span); br_back.target = cond_bb; emit(L, br_back);

    set_cur(L, end_bb);
    return M_NO_VALUE;
}

uint32_t lower_match(Lowerer* L, const Expr* e) {
    const MatchData& mt    = e->as.match_;
    uint32_t         scrut = lower_expr(L, mt.scrutinee);
    MType            sty   = type_of(L, scrut);
    bool             scrut_is_enum = (sty.kind == MTypeKind::Enum); // payload enum repr
    // For an enum scrutinee, all variant arms compare against the tag; lower
    // it once. Non-enum scrutinees use the raw value directly per arm.
    uint32_t tag = M_NO_VALUE;
    if (scrut_is_enum) {
        MInst gt = mk(MOp::EnumTag, mt_int(32, false), e->span);
        gt.a = scrut;
        tag = emit(L, gt);
    }

    uint32_t merge        = new_block(L);
    uint32_t result_slot  = M_NO_VALUE;
    bool     hit_catchall = false;

    auto store_result = [&](uint32_t v) {
        if (v != M_NO_VALUE && type_of(L, v).kind != MTypeKind::Void) {
            if (result_slot == M_NO_VALUE)
                result_slot = emit_entry(L, mk(MOp::Alloca, type_of(L, v), e->span));
            MInst st = mk(MOp::Store, mt_void(), e->span);
            st.a = result_slot;
            st.b = v;
            emit(L, st);
        }
    };
    auto br_to = [&](uint32_t dst) {
        if (!is_terminated(L, L->current)) {
            MInst br = mk(MOp::Br, mt_void(), e->span);
            br.target = dst;
            emit(L, br);
        }
    };

    for (uint32_t i = 0; i < mt.narms; i++) {
        const MatchArm& arm   = mt.arms[i];
        bool            is_catchall_pat =
            (arm.pat.kind == PatKind::Wildcard || arm.pat.kind == PatKind::Binding);

        // Step 1: emit the pattern's test value (or skip if always-true with
        // no guard). Variant arms compare tags; literal arms compare scrut.
        uint32_t arm_bb  = M_NO_VALUE;
        uint32_t next_bb = M_NO_VALUE;
        bool     needs_branch = !is_catchall_pat || arm.guard;
        if (needs_branch) {
            arm_bb  = new_block(L);
            next_bb = new_block(L);
        }
        if (!is_catchall_pat) {
            uint32_t test = M_NO_VALUE;
            if (arm.pat.kind == PatKind::Variant) {
                void* lev = nullptr;
                map_lookup(&L->enum_byname, arm.pat.enum_name, &lev);
                LEnum*   le  = static_cast<LEnum*>(lev);
                uint32_t vi  = le ? enum_variant_index(le, arm.pat.variant_name) : 0;
                uint32_t cst = const_i32(L, arm.span, vi);
                MInst cmp = mk(MOp::ICmpEq, mt_bool(), arm.span);
                cmp.a = (tag != M_NO_VALUE) ? tag : scrut;
                cmp.b = cst;
                test  = emit(L, cmp);
            } else { // Literal
                MType cst_ty = (arm.pat.lit_is_bool) ? mt_bool() : sty;
                MInst cst    = mk(arm.pat.lit_is_bool ? MOp::ConstBool : MOp::ConstInt,
                                  cst_ty, arm.span);
                if (arm.pat.lit_is_bool) cst.imm_bool = arm.pat.lit_bool_val;
                else                     cst.imm_int  = arm.pat.lit_int;
                uint32_t cv = emit(L, cst);
                MInst cmp = mk(arm.pat.lit_is_bool ? MOp::ICmpEq : MOp::ICmpEq,
                               mt_bool(), arm.span);
                cmp.a = scrut;
                cmp.b = cv;
                test  = emit(L, cmp);
            }
            MInst cb = mk(MOp::CondBr, mt_void(), arm.span);
            cb.a = test; cb.target = arm_bb; cb.target2 = next_bb;
            emit(L, cb);
            set_cur(L, arm_bb);
        }

        // Step 2: bind any pattern names (variant payload, or bare-name).
        bool     bound      = false;
        uint32_t bname      = 0;
        void*    prev_slot  = nullptr;
        bool     had_prev   = false;
        if (arm.pat.kind == PatKind::Binding) {
            // Bind the scrutinee value itself to the pattern's name.
            uint32_t slot = emit_entry(L, mk(MOp::Alloca, sty, arm.span));
            MInst    st   = mk(MOp::Store, mt_void(), arm.span);
            st.a = slot; st.b = scrut;
            emit(L, st);
            bname    = arm.pat.binding_name;
            had_prev = map_lookup(&L->scope, bname, &prev_slot);
            map_put(&L->scope, bname, reinterpret_cast<void*>(static_cast<uintptr_t>(slot)));
            bound = true;
        } else if (arm.pat.kind == PatKind::Variant && arm.pat.has_binding) {
            void* lev = nullptr;
            map_lookup(&L->enum_byname, arm.pat.enum_name, &lev);
            LEnum*   le = static_cast<LEnum*>(lev);
            uint32_t vi = le ? enum_variant_index(le, arm.pat.variant_name) : 0;
            if (le && le->any_payload &&
                le->variant_payloads[vi].kind != MTypeKind::Void) {
                MType pt = le->variant_payloads[vi];
                MInst gp = mk(MOp::EnumPayload, pt, arm.span);
                gp.a = scrut;
                uint32_t payload = emit(L, gp);
                uint32_t slot    = emit_entry(L, mk(MOp::Alloca, pt, arm.span));
                MInst    st      = mk(MOp::Store, mt_void(), arm.span);
                st.a = slot; st.b = payload;
                emit(L, st);
                bname    = arm.pat.binding_name;
                had_prev = map_lookup(&L->scope, bname, &prev_slot);
                map_put(&L->scope, bname, reinterpret_cast<void*>(static_cast<uintptr_t>(slot)));
                bound = true;
            }
        }

        // Step 3: lower the guard (with bindings in scope) and if it fails
        // unbind + jump to the next arm.
        if (arm.guard) {
            uint32_t gv         = lower_expr(L, arm.guard);
            uint32_t body_bb    = new_block(L);
            MInst    gcb        = mk(MOp::CondBr, mt_void(), arm.span);
            gcb.a = gv; gcb.target = body_bb; gcb.target2 = next_bb;
            emit(L, gcb);
            set_cur(L, body_bb);
        }

        store_result(lower_expr(L, arm.body));
        br_to(merge);

        if (bound) {
            if (had_prev) map_put(&L->scope, bname, prev_slot);
            else          map_remove(&L->scope, bname);
        }

        if (is_catchall_pat && !arm.guard) {
            // An always-matching arm: everything after is dead.
            hit_catchall = true;
            break;
        }
        if (needs_branch) set_cur(L, next_bb);
    }

    // No catch-all reached: any remaining tail block is unreachable. (For an
    // exhaustive enum, the checker has already verified this is sound.)
    if (!hit_catchall && !is_terminated(L, L->current))
        emit(L, mk(MOp::Unreachable, mt_void(), e->span));

    set_cur(L, merge);
    if (result_slot != M_NO_VALUE) {
        MInst ld = mk(MOp::Load, L->pool[result_slot].type, e->span);
        ld.a = result_slot;
        return emit(L, ld);
    }
    return M_NO_VALUE;
}

uint32_t lower_expr(Lowerer* L, const Expr* e) {
    switch (e->kind) {
        case ExprKind::IntLit: {
            MType ty = (e->as.int_lit.suffix == NumSuffix::None) ? mt_int(32, true)
                                                                 : suffix_type(e->as.int_lit.suffix);
            MInst i   = mk(MOp::ConstInt, ty, e->span);
            i.imm_int = e->as.int_lit.value;
            return emit(L, i);
        }
        case ExprKind::FloatLit: {
            MType ty = (e->as.float_lit.suffix == NumSuffix::None) ? mt_float(64)
                                                                   : suffix_type(e->as.float_lit.suffix);
            MInst i     = mk(MOp::ConstFloat, ty, e->span);
            i.imm_float = e->as.float_lit.value;
            return emit(L, i);
        }
        case ExprKind::BoolLit:
            return const_bool(L, e->span, e->as.bool_lit.value);
        case ExprKind::NullLit: {
            MInst i   = mk(MOp::ConstInt, mt_ptr(), e->span);
            i.imm_int = 0;
            return emit(L, i);
        }
        case ExprKind::StrLit: {
            const StrLitData& s = e->as.str_lit;
            if (s.nparts != 1 || s.parts[0].is_expr) {
                diag_errorf(L->diag, e->span, "string interpolation is not supported");
                MInst i  = mk(MOp::ConstCStr, mt_ptr(), e->span);
                i.str_id = (s.nparts > 0 && !s.parts[0].is_expr) ? s.parts[0].text : 0;
                return emit(L, i);
            }
            MInst i  = mk(MOp::ConstCStr, mt_ptr(), e->span);
            i.str_id = s.parts[0].text;
            return emit(L, i);
        }
        case ExprKind::Ident: {
            uint32_t name = e->as.ident.name;
            void*    v    = nullptr;
            if (!map_lookup(&L->scope, name, &v)) {
                size_t      len = 0;
                const char* str = intern_str(L->in, name, &len);
                diag_errorf(L->diag, e->span, "unknown identifier '%.*s'",
                            static_cast<int>(len), str ? str : "");
                return const_i32(L, e->span, 0);
            }
            uint32_t slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(v));
            MInst    ld   = mk(MOp::Load, L->pool[slot].type, e->span);
            ld.a          = slot;
            return emit(L, ld);
        }
        case ExprKind::Unary: {
            Tok      op = e->as.unary.op;
            uint32_t v  = lower_expr(L, e->as.unary.operand);
            MType    ty = type_of(L, v);
            if (op == Tok::KwMove) return v; // move is a no-op at the IR level
            if (op == Tok::Star) {
                // Explicit deref only supports ref-to-struct (we know the
                // pointee). The checker has already validated.
                MType pointee = (ty.kind == MTypeKind::Ref) ? mt_struct(ty.struct_index) : mt_int(32, true);
                MInst ld = mk(MOp::Load, pointee, e->span);
                ld.a = v;
                return emit(L, ld);
            }
            MOp mop;
            if (op == Tok::Minus) mop = is_float(ty) ? MOp::FNeg : MOp::INeg;
            else                  mop = MOp::Not; // '!' on bool or '~' on int
            MInst i = mk(mop, ty, e->span);
            i.a     = v;
            return emit(L, i);
        }
        case ExprKind::Binary: {
            Tok op = e->as.binary.op;
            if (op == Tok::AmpAmp || op == Tok::PipePipe) return lower_short_circuit(L, e);
            if (op == Tok::Eq) {
                const Expr* lhs = e->as.binary.lhs;
                uint32_t    rv  = lower_expr(L, e->as.binary.rhs);
                if (lhs->kind != ExprKind::Ident) {
                    diag_errorf(L->diag, e->span, "assignment target must be a name");
                    return rv;
                }
                void* v = nullptr;
                if (!map_lookup(&L->scope, lhs->as.ident.name, &v)) {
                    diag_errorf(L->diag, e->span, "assignment to unknown variable");
                    return rv;
                }
                uint32_t slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(v));
                MInst    st   = mk(MOp::Store, mt_void(), e->span);
                st.a = slot; st.b = rv;
                emit(L, st);
                return rv;
            }
            if (is_compound_assign(op)) {
                diag_errorf(L->diag, e->span, "compound assignment is not supported");
                return lower_expr(L, e->as.binary.lhs);
            }
            uint32_t lhs = lower_expr(L, e->as.binary.lhs);
            uint32_t rhs = lower_expr(L, e->as.binary.rhs);
            MType    lty = type_of(L, lhs);
            MOp      mop;
            bool     is_cmp;
            map_binop(op, lty, &mop, &is_cmp);
            MInst i = mk(mop, is_cmp ? mt_bool() : lty, e->span);
            i.a = lhs; i.b = rhs;
            return emit(L, i);
        }
        case ExprKind::Call:  return lower_call(L, e);
        case ExprKind::Block: return lower_block_value(L, &e->as.block);
        case ExprKind::If:    return lower_if(L, e);
        case ExprKind::While: return lower_while(L, e);
        case ExprKind::StructLit: {
            const StructLitData& sl = e->as.struct_lit;
            void* iv = nullptr;
            map_lookup(&L->struct_index, sl.type_name, &iv);
            uint32_t          sidx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(iv));
            const MStructDef& sd   = L->mod->structs[sidx];

            // Lower each initializer, placing it at its declared field index.
            Vec<uint32_t> ordered{};
            for (uint32_t k = 0; k < sd.nfields; k++) ordered.push(M_NO_VALUE);
            for (uint32_t i = 0; i < sl.nfields; i++) {
                uint32_t v = lower_expr(L, sl.fields[i].value);
                for (uint32_t k = 0; k < sd.nfields; k++)
                    if (sd.field_names[k] == sl.fields[i].name) { ordered[k] = v; break; }
            }
            MInst inst = mk(MOp::MakeStruct, mt_struct(sidx), e->span);
            inst.nargs = sd.nfields;
            if (sd.nfields) {
                inst.args = ARENA_NEW_N(L->a, uint32_t, sd.nfields);
                std::memcpy(inst.args, ordered.data, sd.nfields * sizeof(uint32_t));
            }
            ordered.free();
            return emit(L, inst);
        }
        case ExprKind::Field: {
            // `Enum.Variant` constructs a (fieldless) variant. For all-fieldless
            // enums this is just the u32 tag; for payload-bearing enums it
            // emits an EnumCons with an undef payload slot.
            if (e->as.field.obj->kind == ExprKind::Ident) {
                void* lev = nullptr;
                if (map_lookup(&L->enum_byname, e->as.field.obj->as.ident.name, &lev)) {
                    LEnum*   le = static_cast<LEnum*>(lev);
                    uint32_t vi = enum_variant_index(le, e->as.field.name);
                    if (!le->any_payload) return const_i32(L, e->span, vi);
                    MInst inst   = mk(MOp::EnumCons, mt_enum(le->enum_index), e->span);
                    inst.imm_int = vi;
                    inst.a       = M_NO_VALUE; // no payload for this variant
                    return emit(L, inst);
                }
            }
            uint32_t obj = lower_expr(L, e->as.field.obj);
            MType    ot  = type_of(L, obj);
            // Auto-deref: ref to struct -> load the struct, then extract.
            if (ot.kind == MTypeKind::Ref) {
                MType pointee = mt_struct(ot.struct_index);
                MInst ld      = mk(MOp::Load, pointee, e->span);
                ld.a          = obj;
                obj           = emit(L, ld);
                ot            = pointee;
            }
            const MStructDef& sd  = L->mod->structs[ot.struct_index];
            uint32_t          fidx = 0;
            MType             fty  = mt_void();
            for (uint32_t k = 0; k < sd.nfields; k++)
                if (sd.field_names[k] == e->as.field.name) { fidx = k; fty = sd.field_types[k]; break; }
            MInst inst       = mk(MOp::GetField, fty, e->span);
            inst.a           = obj;
            inst.field_index = fidx;
            return emit(L, inst);
        }
        case ExprKind::Match:
            return lower_match(L, e);
        case ExprKind::Loop: return lower_loop(L, e);
        case ExprKind::For:  return lower_for(L, e);
        case ExprKind::Ref: {
            // Checker enforces: operand is a bare Ident. We rebrand the
            // binding's alloca pointer as a Ref value.
            const Expr* op = e->as.ref_.operand;
            void*       sv = nullptr;
            if (op->kind != ExprKind::Ident || !map_lookup(&L->scope, op->as.ident.name, &sv))
                return const_i32(L, e->span, 0);
            uint32_t slot    = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sv));
            MType    slot_ty = L->pool[slot].type;
            MType    ref_ty  = (slot_ty.kind == MTypeKind::Struct)
                                   ? mt_ref(slot_ty.struct_index)
                                   : mt_ptr();
            MInst    inst    = mk(MOp::AddrOf, ref_ty, e->span);
            inst.a = slot;
            return emit(L, inst);
        }
        case ExprKind::Cast: {
            uint32_t v   = lower_expr(L, e->as.cast.operand);
            MType    src = type_of(L, v);
            MType    dst = map_type(L, e->as.cast.target);

            bool src_int = src.kind == MTypeKind::Int;
            bool dst_int = dst.kind == MTypeKind::Int;
            bool src_flt = src.kind == MTypeKind::Float;
            bool dst_flt = dst.kind == MTypeKind::Float;

            MOp op;
            if (src_int && dst_int) {
                if (dst.bits == src.bits) {
                    // Identical type: a true no-op. Signedness-only change still
                    // needs an MType rebrand so later ops see dst's signedness.
                    if (dst.is_signed == src.is_signed) return v;
                    op = MOp::SignCast;
                } else {
                    op = dst.bits < src.bits ? MOp::Trunc
                                             : (src.is_signed ? MOp::SExt : MOp::ZExt);
                }
            } else if (src_flt && dst_flt) {
                if (dst.bits == src.bits) return v;
                op = dst.bits < src.bits ? MOp::FpTrunc : MOp::FpExt;
            } else if (src_int && dst_flt) {
                op = src.is_signed ? MOp::SiToFp : MOp::UiToFp;
            } else if (src_flt && dst_int) {
                op = dst.is_signed ? MOp::FpToSi : MOp::FpToUi;
            } else {
                // Unreachable: the checker permits numeric scalars only.
                diag_errorf(L->diag, e->span, "unsupported cast");
                return v;
            }
            MInst i = mk(op, dst, e->span);
            i.a     = v;
            return emit(L, i);
        }
    }
    return const_i32(L, e->span, 0);
}

// ----------------------------------------------------------------- statements
void lower_stmt(Lowerer* L, const Stmt* s);

// Lower every pending defer in reverse-registration order (LIFO at scope exit).
// Called immediately before emitting a Ret / RetVoid (explicit returns and the
// implicit one at the end of a function).
void run_defers(Lowerer* L) {
    for (size_t i = L->pending_defers.len; i-- > 0; ) {
        lower_expr(L, L->pending_defers[i]); // value discarded
    }
}

uint32_t lower_block_value(Lowerer* L, const BlockData* b) {
    for (uint32_t i = 0; i < b->nstmts; i++) {
        if (is_terminated(L, L->current)) return M_NO_VALUE; // dead code
        lower_stmt(L, b->stmts[i]);
    }
    if (b->tail && !is_terminated(L, L->current)) return lower_expr(L, b->tail);
    return M_NO_VALUE;
}

void lower_stmt(Lowerer* L, const Stmt* s) {
    switch (s->kind) {
        case StmtKind::Binding: {
            const BindingData& bd = s->as.binding;
            uint32_t init    = lower_expr(L, bd.init);
            MType    slot_ty = bd.type ? map_type(L, bd.type) : type_of(L, init);
            if (slot_ty.kind == MTypeKind::Void) {
                diag_errorf(L->diag, s->span, "cannot bind a value of type void");
                slot_ty = mt_int(32, true);
            }
            uint32_t slot = emit_entry(L, mk(MOp::Alloca, slot_ty, s->span));
            MInst    st   = mk(MOp::Store, mt_void(), s->span);
            st.a = slot; st.b = init;
            emit(L, st);
            map_put(&L->scope, bd.name, reinterpret_cast<void*>(static_cast<uintptr_t>(slot)));
            break;
        }
        case StmtKind::Return: {
            // Evaluate the return value FIRST so it isn't affected by defer
            // side-effects, then run pending defers, then emit the Ret.
            if (s->as.ret.value) {
                uint32_t v = lower_expr(L, s->as.ret.value);
                run_defers(L);
                MInst r = mk(MOp::Ret, type_of(L, v), s->span);
                r.a     = v;
                emit(L, r);
            } else {
                run_defers(L);
                emit(L, mk(MOp::RetVoid, mt_void(), s->span));
            }
            break;
        }
        case StmtKind::ExprStmt:
            lower_expr(L, s->as.expr_stmt.expr);
            break;
        case StmtKind::Defer:
            L->pending_defers.push(s->as.defer_.expr); // register; runs at scope exit
            break;
        case StmtKind::Break: {
            if (L->loop_stack.len == 0) break; // checker already reported
            MInst br = mk(MOp::Br, mt_void(), s->span);
            br.target = L->loop_stack[L->loop_stack.len - 1].break_bb;
            emit(L, br);
            break;
        }
        case StmtKind::Continue: {
            if (L->loop_stack.len == 0) break;
            MInst br = mk(MOp::Br, mt_void(), s->span);
            br.target = L->loop_stack[L->loop_stack.len - 1].continue_bb;
            emit(L, br);
            break;
        }
    }
}

// ----------------------------------------------------------------- functions
void lower_func_body(Lowerer* L, const FuncData* f, MFunc* mf) {
    L->pool    = Vec<MInst>{};
    L->blocks  = Vec<Vec<uint32_t>>{};
    L->scope   = Map{};
    L->is_main = mf->is_main;
    L->ret_type = mf->ret;
    L->pending_defers = Vec<const Expr*>{};
    L->loop_stack     = Vec<Lowerer::LoopFrame>{};

    L->entry   = new_block(L); // block 0: allocas + params
    uint32_t body = new_block(L); // block 1: body start
    L->current = body;

    for (uint32_t i = 0; i < f->nparams; i++) {
        MInst pv = mk(MOp::Param, mf->param_types[i], f->params[i].span);
        pv.param_index = i;
        uint32_t pval  = emit_entry(L, pv);
        uint32_t slot  = emit_entry(L, mk(MOp::Alloca, mf->param_types[i], f->params[i].span));
        MInst    st    = mk(MOp::Store, mt_void(), f->params[i].span);
        st.a = slot; st.b = pval;
        emit_entry(L, st);
        map_put(&L->scope, f->params[i].name, reinterpret_cast<void*>(static_cast<uintptr_t>(slot)));
    }

    uint32_t tail = M_NO_VALUE;
    if (f->body && f->body->kind == ExprKind::Block) tail = lower_block_value(L, &f->body->as.block);

    Span end_span = f->body ? f->body->span : Span{0, 0, 0};
    if (!is_terminated(L, L->current)) {
        if (tail != M_NO_VALUE && type_of(L, tail).kind != MTypeKind::Void) {
            run_defers(L);
            MInst r = mk(MOp::Ret, type_of(L, tail), end_span);
            r.a     = tail;
            emit(L, r);
        } else if (mf->is_main) {
            uint32_t z = const_i32(L, end_span, 0);
            run_defers(L);
            MInst r = mk(MOp::Ret, mt_int(32, true), end_span);
            r.a     = z;
            emit(L, r);
        } else {
            run_defers(L);
            emit(L, mk(MOp::RetVoid, mt_void(), end_span));
        }
    }

    // Terminate the entry block by jumping into the body.
    {
        L->current = L->entry;
        MInst br   = mk(MOp::Br, mt_void(), end_span);
        br.target  = body;
        emit(L, br);
    }

    // Any block left without a terminator is unreachable; cap it.
    for (uint32_t b = 0; b < L->blocks.len; b++) {
        if (!is_terminated(L, b)) {
            uint32_t save = L->current;
            L->current = b;
            emit(L, mk(MOp::Unreachable, mt_void(), end_span));
            L->current = save;
        }
    }

    // Flatten the build state into arena arrays on the MFunc.
    mf->ninsts = static_cast<uint32_t>(L->pool.len);
    mf->insts  = ARENA_NEW_N(L->a, MInst, L->pool.len ? L->pool.len : 1);
    std::memcpy(mf->insts, L->pool.data, L->pool.len * sizeof(MInst));

    mf->nblocks = static_cast<uint32_t>(L->blocks.len);
    mf->blocks  = ARENA_NEW_N(L->a, MBlock, L->blocks.len ? L->blocks.len : 1);
    for (uint32_t b = 0; b < L->blocks.len; b++) {
        Vec<uint32_t>& ids = L->blocks[b];
        mf->blocks[b].nids     = static_cast<uint32_t>(ids.len);
        mf->blocks[b].inst_ids = ids.len ? ARENA_NEW_N(L->a, uint32_t, ids.len) : nullptr;
        if (ids.len) std::memcpy(mf->blocks[b].inst_ids, ids.data, ids.len * sizeof(uint32_t));
        ids.free();
    }

    L->pool.free();
    L->blocks.free();
    map_free(&L->scope);
    L->pending_defers.free();
    L->loop_stack.free();
}

} // namespace

MModule* lower_module(const Module* ast, arena* a, Interner* in, Diag* diag) {
    Lowerer L{};
    L.a            = a;
    L.in           = in;
    L.diag         = diag;
    L.func_index   = Map{};
    L.struct_index = Map{};
    L.enum_byname  = Map{};

    uint32_t main_id = intern_cstr(in, "main");

    // Build the struct table first (register names, then resolve field types,
    // which may reference other structs). The enum registry is built afterward
    // so payload types may reference structs.
    Vec<MStructDef> sdefs{};
    for (uint32_t i = 0; i < ast->nitems; i++) {
        const Item* it = ast->items[i];
        if (it->kind != ItemKind::Struct) continue;
        const StructData& s = it->as.struct_;
        MStructDef def{};
        def.name        = s.name;
        def.nfields     = s.nfields;
        def.field_names = s.nfields ? ARENA_NEW_N(a, uint32_t, s.nfields) : nullptr;
        def.field_types = s.nfields ? ARENA_NEW_N(a, MType, s.nfields) : nullptr;
        for (uint32_t k = 0; k < s.nfields; k++) def.field_names[k] = s.fields[k].name;
        uint32_t idx = static_cast<uint32_t>(sdefs.len);
        map_put(&L.struct_index, s.name, reinterpret_cast<void*>(static_cast<uintptr_t>(idx)));
        sdefs.push(def);
    }
    {
        uint32_t si = 0;
        for (uint32_t i = 0; i < ast->nitems; i++) {
            const Item* it = ast->items[i];
            if (it->kind != ItemKind::Struct) continue;
            const StructData& s = it->as.struct_;
            for (uint32_t k = 0; k < s.nfields; k++)
                sdefs[si].field_types[k] = map_type(&L, s.fields[k].type);
            si++;
        }
    }

    // Enum registry: register variant lists. Any enum with a payload-carrying
    // variant gets an MEnumDef so the emitter can lay out a { i32 tag, [N x i8]
    // buf } repr. Per-variant payload types may differ (mixed-payload unions).
    Vec<MEnumDef> edefs{};
    for (uint32_t i = 0; i < ast->nitems; i++) {
        const Item* it = ast->items[i];
        if (it->kind != ItemKind::Enum) continue;
        const EnumData& ed = it->as.enum_;

        LEnum* le = ARENA_NEW(a, LEnum);
        le->n                = ed.nvariants;
        le->variants         = ed.nvariants ? ARENA_NEW_N(a, uint32_t, ed.nvariants) : nullptr;
        le->variant_payloads = ed.nvariants ? ARENA_NEW_N(a, MType, ed.nvariants) : nullptr;
        le->any_payload      = false;
        le->enum_index       = 0;
        for (uint32_t k = 0; k < ed.nvariants; k++) {
            le->variants[k]         = ed.variants[k].name;
            le->variant_payloads[k] = ed.variants[k].has_payload
                                          ? map_type(&L, ed.variants[k].payload_type)
                                          : mt_void();
            if (ed.variants[k].has_payload) le->any_payload = true;
        }

        if (le->any_payload) {
            MEnumDef ed_def{};
            ed_def.name             = ed.name;
            ed_def.nvariants        = ed.nvariants;
            ed_def.variant_names    = ed.nvariants ? ARENA_NEW_N(a, uint32_t, ed.nvariants) : nullptr;
            ed_def.variant_payloads = ed.nvariants ? ARENA_NEW_N(a, MType, ed.nvariants) : nullptr;
            for (uint32_t k = 0; k < ed.nvariants; k++) {
                ed_def.variant_names[k]    = le->variants[k];
                ed_def.variant_payloads[k] = le->variant_payloads[k];
            }
            le->enum_index = static_cast<uint32_t>(edefs.len);
            edefs.push(ed_def);
        }

        map_put(&L.enum_byname, ed.name, le);
    }

    Vec<MFunc> funcs{};
    for (uint32_t i = 0; i < ast->nitems; i++) {
        const Item* it = ast->items[i];
        if (it->kind != ItemKind::Func) continue;
        const FuncData& f = it->as.func;

        MFunc mf{};
        mf.name        = f.name;
        mf.is_extern   = f.is_extern;
        mf.is_main     = (f.name == main_id) && !f.is_extern;
        mf.variadic    = false;
        mf.ret         = map_type(&L, f.ret);
        mf.nparams     = f.nparams;
        mf.param_types = f.nparams ? ARENA_NEW_N(a, MType, f.nparams) : nullptr;
        for (uint32_t k = 0; k < f.nparams; k++) mf.param_types[k] = map_type(&L, f.params[k].type);
        mf.insts   = nullptr;
        mf.ninsts  = 0;
        mf.blocks  = nullptr;
        mf.nblocks = 0;

        uint32_t idx = static_cast<uint32_t>(funcs.len);
        map_put(&L.func_index, f.name, reinterpret_cast<void*>(static_cast<uintptr_t>(idx)));
        funcs.push(mf);
    }

    MModule* mod = ARENA_NEW(a, MModule);
    mod->nfuncs   = static_cast<uint32_t>(funcs.len);
    mod->funcs    = funcs.len ? ARENA_NEW_N(a, MFunc, funcs.len) : nullptr;
    if (funcs.len) std::memcpy(mod->funcs, funcs.data, funcs.len * sizeof(MFunc));
    mod->nstructs = static_cast<uint32_t>(sdefs.len);
    mod->structs  = sdefs.len ? ARENA_NEW_N(a, MStructDef, sdefs.len) : nullptr;
    if (sdefs.len) std::memcpy(mod->structs, sdefs.data, sdefs.len * sizeof(MStructDef));
    mod->nenums = static_cast<uint32_t>(edefs.len);
    mod->enums  = edefs.len ? ARENA_NEW_N(a, MEnumDef, edefs.len) : nullptr;
    if (edefs.len) std::memcpy(mod->enums, edefs.data, edefs.len * sizeof(MEnumDef));
    funcs.free();
    sdefs.free();
    edefs.free();
    L.mod = mod;

    uint32_t fi = 0;
    for (uint32_t i = 0; i < ast->nitems; i++) {
        const Item* it = ast->items[i];
        if (it->kind != ItemKind::Func) continue;
        if (!it->as.func.is_extern) lower_func_body(&L, &it->as.func, &mod->funcs[fi]);
        fi++;
    }

    map_free(&L.func_index);
    map_free(&L.struct_index);
    map_free(&L.enum_byname);
    return mod;
}

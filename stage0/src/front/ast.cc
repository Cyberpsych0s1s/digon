#include "ast.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

void emit(Vec<char>* out, const char* s) {
    for (const char* p = s; *p; p++) out->push(*p);
}

void emitf(Vec<char>* out, const char* fmt, ...) {
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof(buf) - 1)) n = sizeof(buf) - 1;
    for (int i = 0; i < n; i++) out->push(buf[i]);
}

void emit_name(Vec<char>* out, const Interner* in, uint32_t id) {
    if (id == 0) { emit(out, "_"); return; }
    size_t      len = 0;
    const char* s   = intern_str(in, id, &len);
    if (!s) { emit(out, "_"); return; }
    for (size_t i = 0; i < len; i++) out->push(s[i]);
}

void dump_type(Vec<char>* out, const Interner* in, const TypeExpr* t) {
    if (!t) { emit(out, "_"); return; }
    if (t->kind == TypeKind::Pointer) {
        emit(out, "(* ");
        dump_type(out, in, t->inner);
        emit(out, ")");
        return;
    }
    if (t->kind == TypeKind::Ref) {
        emit(out, t->is_mut ? "(ref-mut " : "(ref ");
        dump_type(out, in, t->inner);
        emit(out, ")");
        return;
    }
    emit_name(out, in, t->name);
}

void dump_expr(Vec<char>* out, const Interner* in, const Expr* e);

void dump_block(Vec<char>* out, const Interner* in, const BlockData* b);

void dump_stmt(Vec<char>* out, const Interner* in, const Stmt* s) {
    switch (s->kind) {
        case StmtKind::Binding:
            emit(out, s->as.binding.is_var ? "(var " : "(let ");
            emit_name(out, in, s->as.binding.name);
            emit(out, " (ty ");
            dump_type(out, in, s->as.binding.type);
            emit(out, ") ");
            dump_expr(out, in, s->as.binding.init);
            emit(out, ")");
            break;
        case StmtKind::Return:
            emit(out, "(return");
            if (s->as.ret.value) { emit(out, " "); dump_expr(out, in, s->as.ret.value); }
            emit(out, ")");
            break;
        case StmtKind::ExprStmt:
            emit(out, "(exprstmt ");
            dump_expr(out, in, s->as.expr_stmt.expr);
            emit(out, ")");
            break;
        case StmtKind::Defer:
            emit(out, "(defer ");
            dump_expr(out, in, s->as.defer_.expr);
            emit(out, ")");
            break;
        case StmtKind::Break:    emit(out, "(break)"); break;
        case StmtKind::Continue: emit(out, "(continue)"); break;
    }
}

void dump_block(Vec<char>* out, const Interner* in, const BlockData* b) {
    emit(out, "(block");
    for (uint32_t i = 0; i < b->nstmts; i++) {
        emit(out, " ");
        dump_stmt(out, in, b->stmts[i]);
    }
    emit(out, " (tail");
    if (b->tail) { emit(out, " "); dump_expr(out, in, b->tail); }
    emit(out, "))");
}

void dump_expr(Vec<char>* out, const Interner* in, const Expr* e) {
    switch (e->kind) {
        case ExprKind::IntLit:
            emitf(out, "#%llu", static_cast<unsigned long long>(e->as.int_lit.value));
            break;
        case ExprKind::FloatLit:
            emitf(out, "#f%g", e->as.float_lit.value);
            break;
        case ExprKind::BoolLit:
            emit(out, e->as.bool_lit.value ? "true" : "false");
            break;
        case ExprKind::NullLit:
            emit(out, "null");
            break;
        case ExprKind::Ident:
            emit_name(out, in, e->as.ident.name);
            break;
        case ExprKind::Unary:
            emitf(out, "(unary %s ", tok_name(e->as.unary.op));
            dump_expr(out, in, e->as.unary.operand);
            emit(out, ")");
            break;
        case ExprKind::Binary:
            emitf(out, "(%s ", tok_name(e->as.binary.op));
            dump_expr(out, in, e->as.binary.lhs);
            emit(out, " ");
            dump_expr(out, in, e->as.binary.rhs);
            emit(out, ")");
            break;
        case ExprKind::Call:
            emit(out, "(call ");
            dump_expr(out, in, e->as.call.callee);
            for (uint32_t i = 0; i < e->as.call.nargs; i++) {
                emit(out, " ");
                dump_expr(out, in, e->as.call.args[i]);
            }
            emit(out, ")");
            break;
        case ExprKind::StrLit:
            emit(out, "(str");
            for (uint32_t i = 0; i < e->as.str_lit.nparts; i++) {
                const StrPart& p = e->as.str_lit.parts[i];
                if (p.is_expr) {
                    emit(out, " (interp ");
                    dump_expr(out, in, p.expr);
                    emit(out, ")");
                } else {
                    emit(out, " \"");
                    emit_name(out, in, p.text);
                    emit(out, "\"");
                }
            }
            emit(out, ")");
            break;
        case ExprKind::Block:
            dump_block(out, in, &e->as.block);
            break;
        case ExprKind::If:
            emit(out, "(if ");
            dump_expr(out, in, e->as.if_.cond);
            emit(out, " ");
            dump_expr(out, in, e->as.if_.then_blk);
            if (e->as.if_.else_blk) {
                emit(out, " ");
                dump_expr(out, in, e->as.if_.else_blk);
            }
            emit(out, ")");
            break;
        case ExprKind::While:
            emit(out, "(while ");
            dump_expr(out, in, e->as.while_.cond);
            emit(out, " ");
            dump_expr(out, in, e->as.while_.body);
            emit(out, ")");
            break;
        case ExprKind::StructLit:
            emit(out, "(new ");
            emit_name(out, in, e->as.struct_lit.type_name);
            for (uint32_t i = 0; i < e->as.struct_lit.nfields; i++) {
                emit(out, " (init ");
                emit_name(out, in, e->as.struct_lit.fields[i].name);
                emit(out, " ");
                dump_expr(out, in, e->as.struct_lit.fields[i].value);
                emit(out, ")");
            }
            emit(out, ")");
            break;
        case ExprKind::Field:
            emit(out, "(get ");
            dump_expr(out, in, e->as.field.obj);
            emit(out, " ");
            emit_name(out, in, e->as.field.name);
            emit(out, ")");
            break;
        case ExprKind::Ref:
            emit(out, e->as.ref_.is_mut ? "(ref-mut " : "(ref ");
            dump_expr(out, in, e->as.ref_.operand);
            emit(out, ")");
            break;
        case ExprKind::Loop:
            emit(out, "(loop ");
            dump_expr(out, in, e->as.loop_.body);
            emit(out, ")");
            break;
        case ExprKind::For:
            emit(out, "(for ");
            emit_name(out, in, e->as.for_.name);
            emit(out, " ");
            dump_expr(out, in, e->as.for_.iter);
            emit(out, " ");
            dump_expr(out, in, e->as.for_.body);
            emit(out, ")");
            break;
        case ExprKind::Match:
            emit(out, "(match ");
            dump_expr(out, in, e->as.match_.scrutinee);
            for (uint32_t i = 0; i < e->as.match_.narms; i++) {
                const MatchArm& arm = e->as.match_.arms[i];
                emit(out, " (arm ");
                if (arm.pat.kind == PatKind::Wildcard) {
                    emit(out, "_");
                } else {
                    emit_name(out, in, arm.pat.enum_name);
                    emit(out, ".");
                    emit_name(out, in, arm.pat.variant_name);
                    if (arm.pat.has_binding) {
                        emit(out, "(");
                        emit_name(out, in, arm.pat.binding_name);
                        emit(out, ")");
                    }
                }
                emit(out, " ");
                dump_expr(out, in, arm.body);
                emit(out, ")");
            }
            emit(out, ")");
            break;
        case ExprKind::Cast:
            emit(out, "(cast ");
            dump_expr(out, in, e->as.cast.operand);
            emit(out, " ");
            dump_type(out, in, e->as.cast.target);
            emit(out, ")");
            break;
    }
}

void dump_item(Vec<char>* out, const Interner* in, const Item* it) {
    switch (it->kind) {
        case ItemKind::Func: {
            const FuncData& f = it->as.func;
            emit(out, "(func");
            if (f.is_pub) emit(out, " pub");
            if (f.is_extern) emit(out, " extern");
            emit(out, " ");
            emit_name(out, in, f.name);
            emit(out, " (params");
            for (uint32_t i = 0; i < f.nparams; i++) {
                emit(out, " (param ");
                emit_name(out, in, f.params[i].name);
                emit(out, " ");
                dump_type(out, in, f.params[i].type);
                emit(out, ")");
            }
            emit(out, ") (ret ");
            dump_type(out, in, f.ret);
            emit(out, ")");
            if (f.body) { emit(out, " "); dump_expr(out, in, f.body); }
            emit(out, ")");
            break;
        }
        case ItemKind::Struct: {
            const StructData& s = it->as.struct_;
            emit(out, "(struct ");
            emit_name(out, in, s.name);
            for (uint32_t i = 0; i < s.nfields; i++) {
                emit(out, " (field ");
                emit_name(out, in, s.fields[i].name);
                emit(out, " ");
                dump_type(out, in, s.fields[i].type);
                emit(out, ")");
            }
            emit(out, ")");
            break;
        }
        case ItemKind::Enum: {
            const EnumData& en = it->as.enum_;
            emit(out, "(enum ");
            emit_name(out, in, en.name);
            for (uint32_t i = 0; i < en.nvariants; i++) {
                emit(out, " ");
                if (en.variants[i].has_payload) {
                    emit(out, "(");
                    emit_name(out, in, en.variants[i].name);
                    emit(out, " ");
                    dump_type(out, in, en.variants[i].payload_type);
                    emit(out, ")");
                } else {
                    emit_name(out, in, en.variants[i].name);
                }
            }
            emit(out, ")");
            break;
        }
        case ItemKind::Alias: {
            const AliasData& a = it->as.alias;
            emit(out, a.is_newtype ? "(newtype " : "(alias ");
            emit_name(out, in, a.name);
            emit(out, " ");
            dump_type(out, in, a.target);
            emit(out, ")");
            break;
        }
    }
}

} // namespace

void ast_dump_module(const Module* m, const Interner* in, Vec<char>* out) {
    for (uint32_t i = 0; i < m->nitems; i++) {
        if (i > 0) emit(out, "\n");
        dump_item(out, in, m->items[i]);
    }
    out->push('\0'); // NUL-terminate for easy C-string comparison
}

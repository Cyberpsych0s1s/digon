#include "fmt.h"

#include <cstdio>

#include "lex.h" // Tok, tok_name, NumSuffix

namespace {

struct Fmt {
    Vec<char>*      out;
    const Interner* in;
    int             indent;
};

void put(Fmt* f, const char* s) {
    for (const char* p = s; *p; p++) f->out->push(*p);
}

void put_name(Fmt* f, uint32_t id) {
    size_t      len = 0;
    const char* s   = intern_str(f->in, id, &len);
    if (!s) { f->out->push('_'); return; }
    for (size_t i = 0; i < len; i++) f->out->push(s[i]);
}

void newline(Fmt* f) {
    f->out->push('\n');
    for (int i = 0; i < f->indent * 4; i++) f->out->push(' ');
}

const char* suffix_str(NumSuffix s) {
    switch (s) {
        case NumSuffix::I8:   return "i8";   case NumSuffix::I16:  return "i16";
        case NumSuffix::I32:  return "i32";  case NumSuffix::I64:  return "i64";
        case NumSuffix::I128: return "i128"; case NumSuffix::Iptr: return "iptr";
        case NumSuffix::U8:   return "u8";   case NumSuffix::U16:  return "u16";
        case NumSuffix::U32:  return "u32";  case NumSuffix::U64:  return "u64";
        case NumSuffix::U128: return "u128"; case NumSuffix::Uptr: return "uptr";
        case NumSuffix::F32:  return "f32";  case NumSuffix::F64:  return "f64";
        default:              return "";
    }
}

void fmt_type(Fmt* f, const TypeExpr* t) {
    if (!t) { put(f, "void"); return; }
    if (t->kind == TypeKind::Pointer) {
        put(f, "*");
        fmt_type(f, t->inner);
        return;
    }
    put_name(f, t->name);
}

// Binding power, matching the parser. 0 = not a binary operator.
int bin_prec(Tok op) {
    switch (op) {
        case Tok::Eq: case Tok::PlusEq: case Tok::MinusEq: case Tok::StarEq:
        case Tok::SlashEq: case Tok::PercentEq: case Tok::AmpEq: case Tok::PipeEq:
        case Tok::CaretEq: case Tok::ShlEq: case Tok::ShrEq:           return 1;
        case Tok::DotDot: case Tok::DotDotEq:                          return 2;
        case Tok::QuestionQuestion:                                    return 3;
        case Tok::PipePipe:                                            return 4;
        case Tok::AmpAmp:                                              return 5;
        case Tok::EqEq: case Tok::NotEq: case Tok::Lt:
        case Tok::Gt: case Tok::Le: case Tok::Ge:                      return 6;
        case Tok::Pipe:                                               return 7;
        case Tok::Caret:                                              return 8;
        case Tok::Amp:                                                return 9;
        case Tok::Shl: case Tok::Shr:                                 return 10;
        case Tok::Plus: case Tok::Minus:                              return 11;
        case Tok::Star: case Tok::Slash: case Tok::Percent:           return 12;
        default:                                                      return 0;
    }
}

constexpr int PREC_ATOM = 100; // literals, calls, fields — never need parens

void fmt_expr(Fmt* f, const Expr* e, int parent_prec);
void fmt_block(Fmt* f, const Expr* e);

void fmt_pattern(Fmt* f, const Pattern* p) {
    switch (p->kind) {
        case PatKind::Wildcard:
            put(f, "_");
            return;
        case PatKind::Binding:
            put_name(f, p->binding_name);
            return;
        case PatKind::Literal: {
            if (p->lit_is_bool) {
                put(f, p->lit_bool_val ? "true" : "false");
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%llu",
                              static_cast<unsigned long long>(p->lit_int));
                put(f, buf);
                put(f, suffix_str(p->lit_suffix));
            }
            return;
        }
        case PatKind::Variant:
            put_name(f, p->enum_name);
            put(f, ".");
            put_name(f, p->variant_name);
            if (p->has_binding) {
                put(f, "(");
                put_name(f, p->binding_name);
                put(f, ")");
            }
            return;
    }
}

void fmt_string(Fmt* f, const StrLitData* s) {
    put(f, "\"");
    for (uint32_t i = 0; i < s->nparts; i++) {
        const StrPart& part = s->parts[i];
        if (part.is_expr) {
            put(f, "${");
            fmt_expr(f, part.expr, 0);
            put(f, "}");
        } else {
            size_t      len = 0;
            const char* str = intern_str(f->in, part.text, &len);
            for (size_t k = 0; k < len; k++) {
                char c = str[k];
                switch (c) {
                    case '\n': put(f, "\\n"); break;
                    case '\t': put(f, "\\t"); break;
                    case '\r': put(f, "\\r"); break;
                    case '"':  put(f, "\\\""); break;
                    case '\\': put(f, "\\\\"); break;
                    case '$':  put(f, "\\$"); break;
                    default:   f->out->push(c); break;
                }
            }
        }
    }
    put(f, "\"");
}

void fmt_expr(Fmt* f, const Expr* e, int parent_prec) {
    switch (e->kind) {
        case ExprKind::IntLit: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%llu",
                          static_cast<unsigned long long>(e->as.int_lit.value));
            put(f, buf);
            put(f, suffix_str(e->as.int_lit.suffix));
            break;
        }
        case ExprKind::FloatLit: {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%g", e->as.float_lit.value);
            put(f, buf);
            put(f, suffix_str(e->as.float_lit.suffix));
            break;
        }
        case ExprKind::BoolLit: put(f, e->as.bool_lit.value ? "true" : "false"); break;
        case ExprKind::NullLit: put(f, "null"); break;
        case ExprKind::StrLit:  fmt_string(f, &e->as.str_lit); break;
        case ExprKind::Ident:   put_name(f, e->as.ident.name); break;

        case ExprKind::Unary:
            put(f, tok_name(e->as.unary.op));
            if (e->as.unary.op == Tok::KwMove) put(f, " "); // `move x`, not `movex`
            fmt_expr(f, e->as.unary.operand, PREC_ATOM);
            break;

        case ExprKind::Binary: {
            int  p     = bin_prec(e->as.binary.op);
            bool paren = p < parent_prec;
            if (paren) put(f, "(");
            bool right_assoc = (p == 1); // assignment group
            fmt_expr(f, e->as.binary.lhs, right_assoc ? p + 1 : p);
            put(f, " ");
            put(f, tok_name(e->as.binary.op));
            put(f, " ");
            fmt_expr(f, e->as.binary.rhs, right_assoc ? p : p + 1);
            if (paren) put(f, ")");
            break;
        }

        case ExprKind::Call:
            fmt_expr(f, e->as.call.callee, PREC_ATOM);
            put(f, "(");
            for (uint32_t i = 0; i < e->as.call.nargs; i++) {
                if (i > 0) put(f, ", ");
                fmt_expr(f, e->as.call.args[i], 0);
            }
            put(f, ")");
            break;

        case ExprKind::Field:
            fmt_expr(f, e->as.field.obj, PREC_ATOM);
            put(f, ".");
            put_name(f, e->as.field.name);
            break;

        case ExprKind::StructLit:
            put_name(f, e->as.struct_lit.type_name);
            put(f, " { ");
            for (uint32_t i = 0; i < e->as.struct_lit.nfields; i++) {
                if (i > 0) put(f, ", ");
                put_name(f, e->as.struct_lit.fields[i].name);
                put(f, ": ");
                fmt_expr(f, e->as.struct_lit.fields[i].value, 0);
            }
            put(f, " }");
            break;

        case ExprKind::Block:
            fmt_block(f, e);
            break;

        case ExprKind::If:
            put(f, "if ");
            fmt_expr(f, e->as.if_.cond, 0);
            put(f, " ");
            fmt_block(f, e->as.if_.then_blk);
            if (e->as.if_.else_blk) {
                put(f, " else ");
                if (e->as.if_.else_blk->kind == ExprKind::If) fmt_expr(f, e->as.if_.else_blk, 0);
                else                                          fmt_block(f, e->as.if_.else_blk);
            }
            break;

        case ExprKind::While:
            put(f, "while ");
            fmt_expr(f, e->as.while_.cond, 0);
            put(f, " ");
            fmt_block(f, e->as.while_.body);
            break;
        case ExprKind::Loop:
            put(f, "loop ");
            fmt_block(f, e->as.loop_.body);
            break;
        case ExprKind::For:
            put(f, "for ");
            put_name(f, e->as.for_.name);
            put(f, " in ");
            fmt_expr(f, e->as.for_.iter, 0);
            put(f, " ");
            fmt_block(f, e->as.for_.body);
            break;

        case ExprKind::Match:
            put(f, "match ");
            fmt_expr(f, e->as.match_.scrutinee, 0);
            put(f, " {");
            f->indent++;
            for (uint32_t i = 0; i < e->as.match_.narms; i++) {
                newline(f);
                fmt_pattern(f, &e->as.match_.arms[i].pat);
                if (e->as.match_.arms[i].guard) {
                    put(f, " if ");
                    fmt_expr(f, e->as.match_.arms[i].guard, 0);
                }
                put(f, " => ");
                fmt_expr(f, e->as.match_.arms[i].body, 0);
                put(f, ",");
            }
            f->indent--;
            newline(f);
            put(f, "}");
            break;

        case ExprKind::Cast: {
            const int p     = 13; // `as`, matching the parser's binding power
            bool      paren = p < parent_prec;
            if (paren) put(f, "(");
            fmt_expr(f, e->as.cast.operand, p);
            put(f, " as ");
            fmt_type(f, e->as.cast.target);
            if (paren) put(f, ")");
            break;
        }
    }
}

void fmt_stmt(Fmt* f, const Stmt* s) {
    switch (s->kind) {
        case StmtKind::Binding:
            put(f, s->as.binding.is_var ? "var " : "let ");
            put_name(f, s->as.binding.name);
            if (s->as.binding.type) { put(f, ": "); fmt_type(f, s->as.binding.type); }
            put(f, " = ");
            fmt_expr(f, s->as.binding.init, 0);
            break;
        case StmtKind::Return:
            put(f, "return");
            if (s->as.ret.value) { put(f, " "); fmt_expr(f, s->as.ret.value, 0); }
            break;
        case StmtKind::ExprStmt:
            fmt_expr(f, s->as.expr_stmt.expr, 0);
            break;
        case StmtKind::Defer:
            put(f, "defer ");
            fmt_expr(f, s->as.defer_.expr, 0);
            break;
        case StmtKind::Break:    put(f, "break"); break;
        case StmtKind::Continue: put(f, "continue"); break;
    }
}

void fmt_block(Fmt* f, const Expr* e) {
    const BlockData& b = e->as.block;
    if (b.nstmts == 0 && !b.tail) { put(f, "{}"); return; }
    put(f, "{");
    f->indent++;
    for (uint32_t i = 0; i < b.nstmts; i++) { newline(f); fmt_stmt(f, b.stmts[i]); }
    if (b.tail) { newline(f); fmt_expr(f, b.tail, 0); }
    f->indent--;
    newline(f);
    put(f, "}");
}

void fmt_item(Fmt* f, const Item* it) {
    switch (it->kind) {
        case ItemKind::Func: {
            const FuncData& fn = it->as.func;
            if (fn.is_pub) put(f, "pub ");
            if (fn.is_extern) {
                put(f, "extern \"c\" ");
            }
            put(f, "func ");
            put_name(f, fn.name);
            put(f, "(");
            for (uint32_t i = 0; i < fn.nparams; i++) {
                if (i > 0) put(f, ", ");
                put_name(f, fn.params[i].name);
                put(f, ": ");
                fmt_type(f, fn.params[i].type);
            }
            put(f, ")");
            if (fn.ret) { put(f, " -> "); fmt_type(f, fn.ret); }
            if (fn.body) { put(f, " "); fmt_block(f, fn.body); }
            break;
        }
        case ItemKind::Struct: {
            const StructData& s = it->as.struct_;
            put(f, "type ");
            put_name(f, s.name);
            put(f, " {");
            f->indent++;
            for (uint32_t i = 0; i < s.nfields; i++) {
                newline(f);
                put_name(f, s.fields[i].name);
                put(f, ": ");
                fmt_type(f, s.fields[i].type);
            }
            f->indent--;
            newline(f);
            put(f, "}");
            break;
        }
        case ItemKind::Enum: {
            const EnumData& en = it->as.enum_;
            put(f, "type ");
            put_name(f, en.name);
            put(f, " = ");
            for (uint32_t i = 0; i < en.nvariants; i++) {
                if (i > 0) put(f, " | ");
                put_name(f, en.variants[i].name);
                if (en.variants[i].has_payload) {
                    put(f, "(");
                    fmt_type(f, en.variants[i].payload_type);
                    put(f, ")");
                }
            }
            break;
        }
    }
}

} // namespace

void format_module(const Module* m, const Interner* in, Vec<char>* out) {
    Fmt f{out, in, 0};
    for (uint32_t i = 0; i < m->nitems; i++) {
        if (i > 0) { out->push('\n'); out->push('\n'); }
        fmt_item(&f, m->items[i]);
    }
    if (m->nitems > 0) out->push('\n');
    out->push('\0');
}

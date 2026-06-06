#include "parse.h"

#include <cstring>

namespace {

struct Parser {
    Token*    toks;
    uint32_t  n;
    uint32_t  pos;
    arena*    a;
    Interner* in;
    Diag*     diag;
    bool      no_struct_lit; // true while parsing an if/while condition
};

// ----------------------------------------------------------------- cursor ops
const Token& cur(Parser* p)  { return p->toks[p->pos]; }
Tok          curk(Parser* p) { return p->toks[p->pos].kind; }

void advance(Parser* p) {
    if (p->toks[p->pos].kind != Tok::Eof) p->pos++;
}
bool at(Parser* p, Tok k)     { return curk(p) == k; }
bool accept(Parser* p, Tok k) { if (at(p, k)) { advance(p); return true; } return false; }

void error_here(Parser* p, const char* msg) {
    diag_errorf(p->diag, cur(p).span, "%s (found '%s')", msg, tok_name(curk(p)));
}

bool expect(Parser* p, Tok k, const char* msg) {
    if (at(p, k)) { advance(p); return true; }
    error_here(p, msg);
    return false;
}

// Skip statement/item separators and doc comments.
void skip_separators(Parser* p) {
    while (at(p, Tok::Newline) || at(p, Tok::DocComment)) advance(p);
}

// ----------------------------------------------------------------- allocation
template <typename T>
T* arena_dup(arena* a, const Vec<T>& v, uint32_t* out_n) {
    *out_n = static_cast<uint32_t>(v.len);
    if (v.len == 0) return nullptr;
    T* p = ARENA_NEW_N(a, T, v.len);
    std::memcpy(p, v.data, v.len * sizeof(T));
    return p;
}

Expr* new_expr(Parser* p, ExprKind kind, Span span) {
    Expr* e = ARENA_NEW(p->a, Expr);
    e->kind = kind;
    e->span = span;
    return e;
}

// ------------------------------------------------------------ expression prec
// Binding power per Appendix C; 0 means "not a binary operator". Higher binds
// tighter. Only assignment is right-associative.
int binary_prec(Tok op) {
    switch (op) {
        case Tok::Eq: case Tok::PlusEq: case Tok::MinusEq: case Tok::StarEq:
        case Tok::SlashEq: case Tok::PercentEq: case Tok::AmpEq: case Tok::PipeEq:
        case Tok::CaretEq: case Tok::ShlEq: case Tok::ShrEq:
            return 1;
        case Tok::DotDot: case Tok::DotDotEq:       return 2;
        case Tok::QuestionQuestion:                 return 3;
        case Tok::PipePipe:                         return 4;
        case Tok::AmpAmp:                           return 5;
        case Tok::EqEq: case Tok::NotEq: case Tok::Lt:
        case Tok::Gt: case Tok::Le: case Tok::Ge:   return 6;
        case Tok::Pipe:                             return 7;
        case Tok::Caret:                            return 8;
        case Tok::Amp:                              return 9;
        case Tok::Shl: case Tok::Shr:               return 10;
        case Tok::Plus: case Tok::Minus:            return 11;
        case Tok::Star: case Tok::Slash: case Tok::Percent: return 12;
        case Tok::KwAs:                             return 13;
        default:                                    return 0;
    }
}
bool right_assoc(Tok op) { return binary_prec(op) == 1; } // assignment group

Expr* parse_expr(Parser* p);
TypeExpr* parse_type(Parser* p);
Expr* parse_block_expr(Parser* p);
Expr* parse_if(Parser* p);
Expr* parse_while(Parser* p);
Expr* parse_loop(Parser* p);
Expr* parse_for(Parser* p);
Expr* parse_match(Parser* p);
Expr* parse_struct_lit(Parser* p, uint32_t type_name, Span start);

Expr* parse_string(Parser* p) {
    Span  start = cur(p).span;
    advance(p); // StrStart
    Vec<StrPart> parts{};
    for (;;) {
        if (at(p, Tok::StrText)) {
            StrPart sp{};
            sp.is_expr = false;
            sp.text    = cur(p).ident_id;
            sp.expr    = nullptr;
            parts.push(sp);
            advance(p);
        } else if (at(p, Tok::InterpStart)) {
            advance(p);
            Expr* e = parse_expr(p);
            expect(p, Tok::InterpEnd, "expected '}' to close interpolation");
            StrPart sp{};
            sp.is_expr = true;
            sp.text    = 0;
            sp.expr    = e;
            parts.push(sp);
        } else if (at(p, Tok::StrEnd)) {
            advance(p);
            break;
        } else {
            error_here(p, "malformed string literal");
            break;
        }
    }
    Expr* e = new_expr(p, ExprKind::StrLit, start);
    e->as.str_lit.parts = arena_dup(p->a, parts, &e->as.str_lit.nparts);
    parts.free();
    return e;
}

Expr* parse_if(Parser* p) {
    Span start = cur(p).span;
    advance(p); // 'if'
    bool save = p->no_struct_lit;
    p->no_struct_lit = true;
    Expr* cond     = parse_expr(p);
    p->no_struct_lit = save;
    Expr* then_blk = at(p, Tok::LBrace) ? parse_block_expr(p) : nullptr;
    if (!then_blk) error_here(p, "expected '{' after if condition");

    Expr* else_blk = nullptr;
    if (accept(p, Tok::KwElse)) {
        if (at(p, Tok::KwIf))         else_blk = parse_if(p);          // else if
        else if (at(p, Tok::LBrace))  else_blk = parse_block_expr(p);
        else                          error_here(p, "expected '{' or 'if' after 'else'");
    }

    Expr* e = new_expr(p, ExprKind::If, start);
    e->as.if_.cond     = cond;
    e->as.if_.then_blk = then_blk;
    e->as.if_.else_blk = else_blk;
    return e;
}

Expr* parse_while(Parser* p) {
    Span start = cur(p).span;
    advance(p); // 'while'
    bool save = p->no_struct_lit;
    p->no_struct_lit = true;
    Expr* cond = parse_expr(p);
    p->no_struct_lit = save;
    Expr* body = at(p, Tok::LBrace) ? parse_block_expr(p) : nullptr;
    if (!body) error_here(p, "expected '{' after while condition");

    Expr* e = new_expr(p, ExprKind::While, start);
    e->as.while_.cond = cond;
    e->as.while_.body = body;
    return e;
}

Expr* parse_loop(Parser* p) {
    Span start = cur(p).span;
    advance(p); // 'loop'
    Expr* body = at(p, Tok::LBrace) ? parse_block_expr(p) : nullptr;
    if (!body) error_here(p, "expected '{' after 'loop'");
    Expr* e = new_expr(p, ExprKind::Loop, start);
    e->as.loop_.body = body;
    return e;
}

Expr* parse_for(Parser* p) {
    Span start = cur(p).span;
    advance(p); // 'for'
    uint32_t name = 0;
    if (at(p, Tok::Ident)) { name = cur(p).ident_id; advance(p); }
    else error_here(p, "expected the loop variable name after 'for'");
    expect(p, Tok::KwIn, "expected 'in' after the loop variable");

    bool save = p->no_struct_lit;
    p->no_struct_lit = true; // the iterable's `{` would belong to a struct lit, not the body
    Expr* iter = parse_expr(p);
    p->no_struct_lit = save;

    Expr* body = at(p, Tok::LBrace) ? parse_block_expr(p) : nullptr;
    if (!body) error_here(p, "expected '{' after 'for' iterator");

    Expr* e = new_expr(p, ExprKind::For, start);
    e->as.for_.name = name;
    e->as.for_.iter = iter;
    e->as.for_.body = body;
    return e;
}

Pattern parse_pattern(Parser* p) {
    Pattern pat{};
    pat.span         = cur(p).span;
    pat.kind         = PatKind::Wildcard;
    pat.enum_name    = 0;
    pat.variant_name = 0;
    pat.has_binding  = false;
    pat.binding_name = 0;
    pat.lit_int      = 0;
    pat.lit_suffix   = NumSuffix::None;
    pat.lit_is_bool  = false;
    pat.lit_bool_val = false;

    // Integer literal pattern: `0 =>`, `42 =>`, ...
    if (at(p, Tok::Int)) {
        pat.kind       = PatKind::Literal;
        pat.lit_int    = cur(p).int_value;
        pat.lit_suffix = cur(p).num_suffix;
        advance(p);
        return pat;
    }
    // Bool literal pattern: `true =>` / `false =>`.
    if (at(p, Tok::KwTrue) || at(p, Tok::KwFalse)) {
        pat.kind         = PatKind::Literal;
        pat.lit_is_bool  = true;
        pat.lit_bool_val = at(p, Tok::KwTrue);
        advance(p);
        return pat;
    }

    if (!at(p, Tok::Ident)) {
        error_here(p, "expected a pattern (literal, name, '_', or Enum.Variant)");
        return pat;
    }
    uint32_t first = cur(p).ident_id;
    advance(p);
    if (at(p, Tok::Dot)) {
        advance(p);
        pat.kind      = PatKind::Variant;
        pat.enum_name = first;
        if (at(p, Tok::Ident)) { pat.variant_name = cur(p).ident_id; advance(p); }
        else error_here(p, "expected a variant name after '.'");
        // Optional payload binding: Enum.Variant(name)
        if (at(p, Tok::LParen)) {
            advance(p);
            if (at(p, Tok::Ident)) {
                pat.has_binding  = true;
                pat.binding_name = cur(p).ident_id;
                advance(p);
            } else error_here(p, "expected a binding name inside '(...)'");
            expect(p, Tok::RParen, "expected ')' after pattern binding");
        }
        return pat;
    }
    // Bare identifier: `_` is a wildcard, anything else binds.
    if (first == intern_cstr(p->in, "_")) {
        pat.kind = PatKind::Wildcard;
    } else {
        pat.kind         = PatKind::Binding;
        pat.binding_name = first;
    }
    return pat;
}

Expr* parse_match(Parser* p) {
    Span start = cur(p).span;
    advance(p); // 'match'
    bool save = p->no_struct_lit;
    p->no_struct_lit = true;
    Expr* scrut = parse_expr(p);
    p->no_struct_lit = save;

    expect(p, Tok::LBrace, "expected '{' after match scrutinee");
    Vec<MatchArm> arms{};
    skip_separators(p);
    while (!at(p, Tok::RBrace) && !at(p, Tok::Eof)) {
        uint32_t before = p->pos;
        MatchArm arm{};
        arm.span  = cur(p).span;
        arm.guard = nullptr;
        arm.pat   = parse_pattern(p);
        if (at(p, Tok::KwIf)) {
            advance(p);
            // The guard expression terminates at `=>`; struct literals here
            // would be confusing, so disable them like in other condition slots.
            bool sv = p->no_struct_lit;
            p->no_struct_lit = true;
            arm.guard = parse_expr(p);
            p->no_struct_lit = sv;
        }
        expect(p, Tok::FatArrow, "expected '=>' after pattern");
        arm.body = parse_expr(p);
        arms.push(arm);
        if (at(p, Tok::Comma)) advance(p);
        skip_separators(p);
        if (p->pos == before) advance(p); // progress guard
    }
    expect(p, Tok::RBrace, "expected '}' to close match");

    Expr* e = new_expr(p, ExprKind::Match, start);
    e->as.match_.scrutinee = scrut;
    e->as.match_.arms       = arena_dup(p->a, arms, &e->as.match_.narms);
    arms.free();
    return e;
}

Expr* parse_struct_lit(Parser* p, uint32_t type_name, Span start) {
    advance(p); // '{'
    Vec<FieldInit> inits{};
    skip_separators(p);
    while (!at(p, Tok::RBrace) && !at(p, Tok::Eof)) {
        uint32_t before = p->pos;
        FieldInit fi{};
        fi.span  = cur(p).span;
        fi.name  = 0;
        fi.value = nullptr;
        if (at(p, Tok::Ident)) { fi.name = cur(p).ident_id; advance(p); }
        else error_here(p, "expected a field name");
        expect(p, Tok::Colon, "expected ':' after field name");
        bool sv = p->no_struct_lit;
        p->no_struct_lit = false;
        fi.value = parse_expr(p);
        p->no_struct_lit = sv;
        inits.push(fi);
        if (at(p, Tok::Comma)) advance(p);
        skip_separators(p);
        if (p->pos == before) advance(p); // progress guard
    }
    expect(p, Tok::RBrace, "expected '}' to close struct literal");

    Expr* e = new_expr(p, ExprKind::StructLit, start);
    e->as.struct_lit.type_name = type_name;
    e->as.struct_lit.fields    = arena_dup(p->a, inits, &e->as.struct_lit.nfields);
    inits.free();
    return e;
}

Expr* parse_primary(Parser* p) {
    Span span = cur(p).span;
    switch (curk(p)) {
        case Tok::Int: {
            Expr* e = new_expr(p, ExprKind::IntLit, span);
            e->as.int_lit.value  = cur(p).int_value;
            e->as.int_lit.suffix = cur(p).num_suffix;
            advance(p);
            return e;
        }
        case Tok::Float: {
            Expr* e = new_expr(p, ExprKind::FloatLit, span);
            e->as.float_lit.value  = cur(p).float_value;
            e->as.float_lit.suffix = cur(p).num_suffix;
            advance(p);
            return e;
        }
        case Tok::KwTrue:
        case Tok::KwFalse: {
            Expr* e = new_expr(p, ExprKind::BoolLit, span);
            e->as.bool_lit.value = (curk(p) == Tok::KwTrue);
            advance(p);
            return e;
        }
        case Tok::KwNull: {
            advance(p);
            return new_expr(p, ExprKind::NullLit, span);
        }
        case Tok::Ident: {
            uint32_t name = cur(p).ident_id;
            advance(p);
            // `Name { ... }` is a struct literal, except in a condition where
            // the `{` opens the control-flow body.
            if (at(p, Tok::LBrace) && !p->no_struct_lit) {
                return parse_struct_lit(p, name, span);
            }
            Expr* e = new_expr(p, ExprKind::Ident, span);
            e->as.ident.name = name;
            return e;
        }
        case Tok::StrStart:
            return parse_string(p);
        case Tok::KwIf:
            return parse_if(p);
        case Tok::KwWhile:
            return parse_while(p);
        case Tok::KwLoop:
            return parse_loop(p);
        case Tok::KwFor:
            return parse_for(p);
        case Tok::KwMatch:
            return parse_match(p);
        case Tok::LParen: {
            advance(p);
            bool save = p->no_struct_lit;
            p->no_struct_lit = false; // parentheses re-enable struct literals
            Expr* inner = parse_expr(p);
            p->no_struct_lit = save;
            expect(p, Tok::RParen, "expected ')'");
            return inner;
        }
        case Tok::LBrace:
            return parse_block_expr(p);
        default: {
            error_here(p, "expected an expression");
            // Recovery: synthesise a placeholder and make progress.
            Expr* e = new_expr(p, ExprKind::IntLit, span);
            e->as.int_lit.value  = 0;
            e->as.int_lit.suffix = NumSuffix::None;
            advance(p);
            return e;
        }
    }
}

Expr* parse_postfix(Parser* p) {
    Expr* e = parse_primary(p);
    for (;;) {
        if (at(p, Tok::LParen)) {
            advance(p);
            bool       save = p->no_struct_lit;
            p->no_struct_lit = false; // struct literals are fine inside call args
            Vec<Expr*> args{};
            if (!at(p, Tok::RParen)) {
                do {
                    args.push(parse_expr(p));
                } while (accept(p, Tok::Comma));
            }
            p->no_struct_lit = save;
            expect(p, Tok::RParen, "expected ')' to close call arguments");
            Expr* call = new_expr(p, ExprKind::Call, e->span);
            call->as.call.callee = e;
            call->as.call.args   = arena_dup(p->a, args, &call->as.call.nargs);
            args.free();
            e = call;
        } else if (at(p, Tok::Dot)) {
            advance(p);
            Expr* fe = new_expr(p, ExprKind::Field, e->span);
            fe->as.field.obj = e;
            if (at(p, Tok::Ident)) {
                fe->as.field.name = cur(p).ident_id;
                advance(p);
            } else {
                error_here(p, "expected a field name after '.'");
                fe->as.field.name = 0;
            }
            e = fe;
        } else {
            break;
        }
    }
    return e;
}

Expr* parse_unary(Parser* p) {
    Tok op = curk(p);
    if (op == Tok::Minus || op == Tok::Bang || op == Tok::Tilde || op == Tok::Star ||
        op == Tok::KwMove) {
        Span span = cur(p).span;
        advance(p);
        Expr* operand = parse_unary(p);
        Expr* e = new_expr(p, ExprKind::Unary, span);
        e->as.unary.op      = op;
        e->as.unary.operand = operand;
        return e;
    }
    if (op == Tok::KwRef) {
        Span span    = cur(p).span;
        advance(p);
        bool is_mut  = accept(p, Tok::KwMut);
        Expr* operand = parse_unary(p);
        Expr* e = new_expr(p, ExprKind::Ref, span);
        e->as.ref_.is_mut  = is_mut;
        e->as.ref_.operand = operand;
        return e;
    }
    return parse_postfix(p);
}

Expr* parse_binary(Parser* p, int min_prec) {
    Expr* lhs = parse_unary(p);
    for (;;) {
        Tok op   = curk(p);
        int prec = binary_prec(op);
        if (prec == 0 || prec < min_prec) break;
        advance(p);
        if (op == Tok::KwAs){
            TypeExpr* ty = parse_type(p);
            Expr* e = new_expr(p, ExprKind::Cast, lhs->span);
            e->as.cast.operand = lhs;
            e->as.cast.target = ty;
            lhs = e;
            continue; // re-enter loop -> 'x as A as B' left associates
        }
        int   next_min = right_assoc(op) ? prec : prec + 1;
        Expr* rhs      = parse_binary(p, next_min);
        Expr* e        = new_expr(p, ExprKind::Binary, lhs->span);
        e->as.binary.op  = op;
        e->as.binary.lhs = lhs;
        e->as.binary.rhs = rhs;
        lhs = e;
    }
    return lhs;
}

Expr* parse_expr(Parser* p) { return parse_binary(p, 1); }

// --------------------------------------------------------------------- types
TypeExpr* parse_type(Parser* p) {
    Span      span = cur(p).span;
    TypeExpr* t    = ARENA_NEW(p->a, TypeExpr);
    t->span  = span;
    t->name  = 0;
    t->inner = nullptr;

    if (at(p, Tok::Star)) {           // raw pointer `*T`
        advance(p);
        t->kind  = TypeKind::Pointer;
        t->inner = parse_type(p);
        return t;
    }
    if (at(p, Tok::KwRef)) {          // borrow type `ref T` / `ref mut T`
        advance(p);
        t->kind   = TypeKind::Ref;
        t->is_mut = accept(p, Tok::KwMut);
        t->inner  = parse_type(p);
        return t;
    }

    t->kind = TypeKind::Named;
    if (at(p, Tok::Ident)) {
        t->name = cur(p).ident_id;
        advance(p);
    } else {
        error_here(p, "expected a type");
    }
    return t;
}

// --------------------------------------------------------------- statements
Stmt* new_stmt(Parser* p, StmtKind kind, Span span) {
    Stmt* s = ARENA_NEW(p->a, Stmt);
    s->kind = kind;
    s->span = span;
    return s;
}

Stmt* parse_binding(Parser* p) {
    Span start  = cur(p).span;
    bool is_var = (curk(p) == Tok::KwVar);
    advance(p); // 'let' or 'var'

    uint32_t name = 0;
    if (at(p, Tok::Ident)) { name = cur(p).ident_id; advance(p); }
    else error_here(p, "expected a binding name");

    TypeExpr* type = nullptr;
    if (accept(p, Tok::Colon)) type = parse_type(p);

    expect(p, Tok::Eq, "expected '=' in binding");
    Expr* init = parse_expr(p);

    Stmt* s = new_stmt(p, StmtKind::Binding, start);
    s->as.binding.is_var = is_var;
    s->as.binding.name   = name;
    s->as.binding.type   = type;
    s->as.binding.init   = init;
    return s;
}

Stmt* parse_return(Parser* p) {
    Span start = cur(p).span;
    advance(p); // 'return'
    Expr* value = nullptr;
    if (!at(p, Tok::Newline) && !at(p, Tok::RBrace) && !at(p, Tok::Eof)) {
        value = parse_expr(p);
    }
    Stmt* s = new_stmt(p, StmtKind::Return, start);
    s->as.ret.value = value;
    return s;
}

Stmt* parse_defer(Parser* p) {
    Span start = cur(p).span;
    advance(p); // 'defer'
    Expr* expr = parse_expr(p);
    Stmt* s    = new_stmt(p, StmtKind::Defer, start);
    s->as.defer_.expr = expr;
    return s;
}

// Consume a statement terminator (newline / block-end / EOF); recover if junk
// follows a statement on the same line.
void finish_stmt(Parser* p) {
    if (at(p, Tok::Newline) || at(p, Tok::DocComment)) { skip_separators(p); return; }
    if (at(p, Tok::RBrace) || at(p, Tok::Eof)) return;
    error_here(p, "expected end of statement");
    while (!at(p, Tok::Newline) && !at(p, Tok::RBrace) && !at(p, Tok::Eof)) advance(p);
    skip_separators(p);
}

Expr* parse_block_expr(Parser* p) {
    Span start = cur(p).span;
    expect(p, Tok::LBrace, "expected '{'");

    Vec<Stmt*> stmts{};
    Expr*      tail = nullptr;
    for (;;) {
        skip_separators(p);
        if (at(p, Tok::RBrace)) { advance(p); break; }
        if (at(p, Tok::Eof))    { error_here(p, "unterminated block"); break; }

        uint32_t before = p->pos;

        if (at(p, Tok::KwLet) || at(p, Tok::KwVar)) {
            stmts.push(parse_binding(p));
        } else if (at(p, Tok::KwReturn)) {
            stmts.push(parse_return(p));
        } else if (at(p, Tok::KwDefer)) {
            stmts.push(parse_defer(p));
        } else if (at(p, Tok::KwBreak)) {
            Stmt* s = new_stmt(p, StmtKind::Break, cur(p).span);
            advance(p);
            stmts.push(s);
        } else if (at(p, Tok::KwContinue)) {
            Stmt* s = new_stmt(p, StmtKind::Continue, cur(p).span);
            advance(p);
            stmts.push(s);
        } else {
            Expr* e = parse_expr(p);
            Stmt* s = new_stmt(p, StmtKind::ExprStmt, e->span);
            s->as.expr_stmt.expr = e;
            stmts.push(s);
        }
        finish_stmt(p);

        if (p->pos == before) advance(p); // guarantee forward progress
    }

    // digon is expression-oriented: a trailing bare expression becomes the
    // block's value.
    if (stmts.len > 0 && stmts.back()->kind == StmtKind::ExprStmt) {
        tail = stmts.back()->as.expr_stmt.expr;
        stmts.len--;
    }

    Expr* blk = new_expr(p, ExprKind::Block, start);
    blk->as.block.stmts = arena_dup(p->a, stmts, &blk->as.block.nstmts);
    blk->as.block.tail  = tail;
    stmts.free();
    return blk;
}

// -------------------------------------------------------------------- items
// Parse a simple string literal (single text chunk, no interpolation) and
// return its interned text id. Used for the extern ABI string.
uint32_t parse_simple_string(Parser* p) {
    if (!expect(p, Tok::StrStart, "expected a string literal")) return 0;
    uint32_t text = 0;
    if (at(p, Tok::StrText)) { text = cur(p).ident_id; advance(p); }
    expect(p, Tok::StrEnd, "expected end of string literal");
    return text;
}

Item* parse_func(Parser* p) {
    Span start     = cur(p).span;
    bool is_pub    = accept(p, Tok::KwPub);
    bool is_extern = false;
    uint32_t abi   = 0;
    if (accept(p, Tok::KwExtern)) {
        is_extern = true;
        if (at(p, Tok::StrStart)) abi = parse_simple_string(p); // optional ABI string
    }
    if (!expect(p, Tok::KwFunc, "expected 'func'")) return nullptr;

    uint32_t name = 0;
    if (at(p, Tok::Ident)) { name = cur(p).ident_id; advance(p); }
    else error_here(p, "expected a function name");

    expect(p, Tok::LParen, "expected '(' after function name");
    Vec<Param> params{};
    if (!at(p, Tok::RParen)) {
        do {
            Param par{};
            par.span = cur(p).span;
            if (at(p, Tok::Ident)) { par.name = cur(p).ident_id; advance(p); }
            else error_here(p, "expected a parameter name");
            expect(p, Tok::Colon, "expected ':' after parameter name");
            par.type = parse_type(p);
            params.push(par);
        } while (accept(p, Tok::Comma));
    }
    expect(p, Tok::RParen, "expected ')'");

    TypeExpr* ret = nullptr;
    if (accept(p, Tok::Arrow)) ret = parse_type(p);

    Expr* body = nullptr;
    if (is_extern) {
        // No body; the declaration ends at the statement separator.
    } else if (at(p, Tok::LBrace)) {
        body = parse_block_expr(p);
    } else {
        error_here(p, "expected '{' for the function body");
    }

    Item* it = ARENA_NEW(p->a, Item);
    it->kind = ItemKind::Func;
    it->span = start;
    it->as.func.is_pub    = is_pub;
    it->as.func.is_extern = is_extern;
    it->as.func.abi       = abi;
    it->as.func.name      = name;
    it->as.func.params    = arena_dup(p->a, params, &it->as.func.nparams);
    it->as.func.ret       = ret;
    it->as.func.body      = body;
    params.free();
    return it;
}

void skip_balanced_braces(Parser* p) {
    if (!at(p, Tok::LBrace)) return;
    int depth = 0;
    do {
        if (at(p, Tok::LBrace)) depth++;
        else if (at(p, Tok::RBrace)) depth--;
        advance(p);
    } while (depth > 0 && !at(p, Tok::Eof));
}

Item* parse_enum_decl(Parser* p, uint32_t name, Span start) {
    advance(p); // '='
    Vec<EnumVariant> variants{};
    skip_separators(p);
    if (at(p, Tok::Pipe)) { advance(p); skip_separators(p); } // optional leading '|'
    for (;;) {
        if (!at(p, Tok::Ident)) { error_here(p, "expected a variant name"); break; }
        EnumVariant v{};
        v.name         = cur(p).ident_id;
        v.span         = cur(p).span;
        v.has_payload  = false;
        v.payload_type = nullptr;
        advance(p);
        if (at(p, Tok::LParen)) {
            advance(p);
            v.has_payload  = true;
            v.payload_type = parse_type(p);
            expect(p, Tok::RParen, "expected ')' after variant payload type");
        } else if (at(p, Tok::LBrace)) {
            error_here(p, "struct-style variant payloads are not supported yet (use Variant(T))");
            skip_balanced_braces(p);
        }
        variants.push(v);
        skip_separators(p);
        if (at(p, Tok::Pipe)) { advance(p); skip_separators(p); continue; }
        break;
    }
    Item* it = ARENA_NEW(p->a, Item);
    it->kind = ItemKind::Enum;
    it->span = start;
    it->as.enum_.name          = name;
    it->as.enum_.variants      = arena_dup(p->a, variants, &it->as.enum_.nvariants);
    it->as.enum_.is_must_defer = false; // set by parse_item when `@must_defer` precedes
    variants.free();
    return it;
}

Item* parse_type_decl(Parser* p) {
    Span start = cur(p).span;
    advance(p); // 'type'
    uint32_t name = 0;
    if (at(p, Tok::Ident)) { name = cur(p).ident_id; advance(p); }
    else error_here(p, "expected a type name");

    if (at(p, Tok::Eq)) return parse_enum_decl(p, name, start);

    if (!at(p, Tok::LBrace)) {
        error_here(p, "expected '{' (struct) or '=' (enum) after type name");
        return nullptr;
    }
    advance(p); // '{'

    Vec<FieldDecl> fields{};
    skip_separators(p);
    while (!at(p, Tok::RBrace) && !at(p, Tok::Eof)) {
        uint32_t before = p->pos;
        FieldDecl fd{};
        fd.span = cur(p).span;
        fd.name = 0;
        fd.type = nullptr;
        if (at(p, Tok::Ident)) { fd.name = cur(p).ident_id; advance(p); }
        else error_here(p, "expected a field name");
        expect(p, Tok::Colon, "expected ':' after field name");
        fd.type = parse_type(p);
        fields.push(fd);
        if (at(p, Tok::Comma)) advance(p);
        skip_separators(p);
        if (p->pos == before) advance(p); // progress guard
    }
    expect(p, Tok::RBrace, "expected '}' to close struct body");

    Item* it = ARENA_NEW(p->a, Item);
    it->kind = ItemKind::Struct;
    it->span = start;
    it->as.struct_.name          = name;
    it->as.struct_.fields        = arena_dup(p->a, fields, &it->as.struct_.nfields);
    it->as.struct_.is_must_defer = false; // set by parse_item when `@must_defer` precedes
    fields.free();
    return it;
}

Item* parse_item(Parser* p) {
    bool is_must_defer = false;
    if (at(p, Tok::At)) {
        Span at_span = cur(p).span;
        advance(p);
        if (at(p, Tok::Ident) && cur(p).ident_id == intern_cstr(p->in, "must_defer")) {
            is_must_defer = true;
            advance(p);
        } else {
            error_here(p, "the only supported attribute is '@must_defer'");
        }
        skip_separators(p);
        if (!at(p, Tok::KwType)) {
            (void)at_span;
            error_here(p, "'@must_defer' must precede a 'type' declaration");
        }
    }
    if (at(p, Tok::KwType)) {
        Item* it = parse_type_decl(p);
        if (it && is_must_defer) {
            if (it->kind == ItemKind::Struct) it->as.struct_.is_must_defer = true;
            else if (it->kind == ItemKind::Enum) it->as.enum_.is_must_defer = true;
        }
        return it;
    }
    if (at(p, Tok::KwPub) || at(p, Tok::KwFunc) || at(p, Tok::KwExtern)) return parse_func(p);
    error_here(p, "expected an item ('func', 'extern func', or 'type')");
    return nullptr;
}

} // namespace

Module* parse_module(Token* toks, uint32_t ntoks, arena* a, Interner* in, Diag* diag) {
    Parser p;
    p.toks = toks;
    p.n    = ntoks;
    p.pos  = 0;
    p.a    = a;
    p.in   = in;
    p.diag = diag;
    p.no_struct_lit = false;

    Vec<Item*> items{};
    skip_separators(&p);
    while (!at(&p, Tok::Eof)) {
        uint32_t before = p.pos;
        Item*    it     = parse_item(&p);
        if (it) items.push(it);
        // Recover to the next item if we stalled or hit junk.
        if (p.pos == before) {
            while (!at(&p, Tok::KwFunc) && !at(&p, Tok::KwPub) &&
                   !at(&p, Tok::KwExtern) && !at(&p, Tok::KwType) && !at(&p, Tok::Eof)) {
                advance(&p);
            }
            if (p.pos == before) advance(&p);
        }
        skip_separators(&p);
    }

    Module* m = ARENA_NEW(a, Module);
    m->items = arena_dup(a, items, &m->nitems);
    items.free();
    return m;
}

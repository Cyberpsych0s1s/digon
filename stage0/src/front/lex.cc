#include "lex.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

struct Keyword {
    const char* name;
    Tok         kind;
};

// Every reserved word: 28 keywords + 5 literal-like names.
const Keyword KEYWORDS[] = {
    {"func", Tok::KwFunc}, {"let", Tok::KwLet}, {"var", Tok::KwVar},
    {"const", Tok::KwConst}, {"if", Tok::KwIf}, {"else", Tok::KwElse},
    {"while", Tok::KwWhile}, {"for", Tok::KwFor}, {"loop", Tok::KwLoop},
    {"in", Tok::KwIn}, {"match", Tok::KwMatch}, {"break", Tok::KwBreak},
    {"continue", Tok::KwContinue}, {"return", Tok::KwReturn}, {"defer", Tok::KwDefer},
    {"trait", Tok::KwTrait}, {"type", Tok::KwType}, {"alias", Tok::KwAlias},
    {"pub", Tok::KwPub}, {"import", Tok::KwImport}, {"extern", Tok::KwExtern},
    {"comptime", Tok::KwComptime}, {"unsafe", Tok::KwUnsafe}, {"move", Tok::KwMove},
    {"as", Tok::KwAs}, {"where", Tok::KwWhere}, {"ref", Tok::KwRef}, {"mut", Tok::KwMut},
    {"true", Tok::KwTrue}, {"false", Tok::KwFalse}, {"null", Tok::KwNull},
    {"self", Tok::KwSelf}, {"Self", Tok::KwSelfType},
};

struct SuffixEntry {
    const char* name;
    NumSuffix   suf;
    bool        is_float;
};

const SuffixEntry SUFFIXES[] = {
    {"i8", NumSuffix::I8, false},   {"i16", NumSuffix::I16, false},
    {"i32", NumSuffix::I32, false}, {"i64", NumSuffix::I64, false},
    {"i128", NumSuffix::I128, false}, {"iptr", NumSuffix::Iptr, false},
    {"u8", NumSuffix::U8, false},   {"u16", NumSuffix::U16, false},
    {"u32", NumSuffix::U32, false}, {"u64", NumSuffix::U64, false},
    {"u128", NumSuffix::U128, false}, {"uptr", NumSuffix::Uptr, false},
    {"f32", NumSuffix::F32, true},  {"f64", NumSuffix::F64, true},
};

bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_ident_continue(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}
bool is_digit(char c) { return c >= '0' && c <= '9'; }

void* pack_kind(Tok k) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint8_t>(k)));
}
Tok unpack_kind(void* p) {
    return static_cast<Tok>(static_cast<uint8_t>(reinterpret_cast<uintptr_t>(p)));
}

// Tokens after which a trailing newline is a line continuation: open
// delimiters and binary operators. NOTE: '{' is excluded — it opens a block
// where newlines are significant statement separators; only the
// grouping/call/index/generic delimiters '(' and '[' continue a line.
bool op_continues_line(Tok k) {
    switch (k) {
        case Tok::LParen: case Tok::LBracket:
        case Tok::Comma: case Tok::Arrow: case Tok::FatArrow:
        case Tok::Plus: case Tok::Minus: case Tok::Star: case Tok::Slash: case Tok::Percent:
        case Tok::Amp: case Tok::Pipe: case Tok::Caret: case Tok::Shl: case Tok::Shr:
        case Tok::AmpAmp: case Tok::PipePipe:
        case Tok::EqEq: case Tok::NotEq: case Tok::Lt: case Tok::Gt: case Tok::Le: case Tok::Ge:
        case Tok::Eq: case Tok::PlusEq: case Tok::MinusEq: case Tok::StarEq:
        case Tok::SlashEq: case Tok::PercentEq: case Tok::AmpEq: case Tok::PipeEq:
        case Tok::CaretEq: case Tok::ShlEq: case Tok::ShrEq:
        case Tok::DotDot: case Tok::DotDotEq: case Tok::QuestionQuestion:
            return true;
        default:
            return false;
    }
}

} // namespace

void lexer_init(Lexer* lx, const char* src, size_t len, uint32_t file_id,
                Interner* in, Diag* diag) {
    lx->src          = src;
    lx->len          = len;
    lx->file_id      = file_id;
    lx->pos          = 0;
    lx->started      = false;
    lx->last_emitted = Tok::Newline;
    lx->in           = in;
    lx->diag         = diag;
    lx->keywords     = Map{};
    lx->modes        = Vec<LexMode>{};
    for (const Keyword& kw : KEYWORDS) {
        uint32_t id = intern_cstr(in, kw.name);
        map_put(&lx->keywords, id, pack_kind(kw.kind));
    }

    // Reject a UTF-8 BOM but skip past it so lexing can continue.
    if (len >= 3 && static_cast<unsigned char>(src[0]) == 0xEF &&
        static_cast<unsigned char>(src[1]) == 0xBB &&
        static_cast<unsigned char>(src[2]) == 0xBF) {
        diag_error(diag, Span{file_id, 0, 3}, "byte order mark (BOM) is not allowed in source");
        lx->pos = 3;
    }
}

void lexer_free(Lexer* lx) {
    map_free(&lx->keywords);
    lx->modes.free();
}

namespace {

char peek(const Lexer* lx, size_t off) {
    size_t i = lx->pos + off;
    return (i < lx->len) ? lx->src[i] : '\0';
}

Token make(const Lexer* lx, Tok kind, size_t start) {
    Token t;
    t.kind        = kind;
    t.span        = Span{lx->file_id, static_cast<uint32_t>(start),
                        static_cast<uint32_t>(lx->pos)};
    t.ident_id    = 0;
    t.int_value   = 0;
    t.float_value = 0.0;
    t.num_suffix  = NumSuffix::None;
    return t;
}

Token lex_ident(Lexer* lx) {
    size_t start = lx->pos;
    while (is_ident_continue(peek(lx, 0))) lx->pos++;
    uint32_t id = intern(lx->in, lx->src + start, lx->pos - start);

    Token t    = make(lx, Tok::Ident, start);
    t.ident_id = id;

    void* packed = nullptr;
    if (map_lookup(&lx->keywords, id, &packed)) {
        t.kind = unpack_kind(packed);
    }
    return t;
}

// Match a trailing ident-run against the suffix table. Returns true if it is
// a known suffix; fills *out and *is_float.
bool match_suffix(const char* s, size_t len, NumSuffix* out, bool* is_float) {
    for (const SuffixEntry& e : SUFFIXES) {
        if (std::strlen(e.name) == len && std::memcmp(e.name, s, len) == 0) {
            *out      = e.suf;
            *is_float = e.is_float;
            return true;
        }
    }
    return false;
}

// Parse a prefixed integer (hex/bin/oct). `base` and a digit predicate are
// supplied by the caller; `*ok` reports whether at least one digit was seen.
uint64_t parse_prefixed_int(Lexer* lx, int base, bool* ok, bool* overflow) {
    uint64_t value = 0;
    bool     any   = false;
    *overflow      = false;
    for (;;) {
        char c = peek(lx, 0);
        if (c == '_') { lx->pos++; continue; }
        unsigned d;
        if (c >= '0' && c <= '9')      d = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
        else break;
        if (d >= static_cast<unsigned>(base)) break;
        any = true;
        if (value > (UINT64_MAX - d) / static_cast<uint64_t>(base)) *overflow = true;
        value = value * static_cast<uint64_t>(base) + d;
        lx->pos++;
    }
    *ok = any;
    return value;
}

void scan_suffix(Lexer* lx, Token* t, bool literal_is_float, bool decimal) {
    if (!is_ident_start(peek(lx, 0))) return;
    size_t s = lx->pos;
    while (is_ident_continue(peek(lx, 0))) lx->pos++;
    size_t slen = lx->pos - s;

    NumSuffix suf;
    bool      suf_is_float;
    if (!match_suffix(lx->src + s, slen, &suf, &suf_is_float)) {
        diag_errorf(lx->diag, Span{lx->file_id, static_cast<uint32_t>(s),
                                   static_cast<uint32_t>(lx->pos)},
                    "invalid numeric suffix '%.*s'", static_cast<int>(slen), lx->src + s);
        return;
    }
    if (suf_is_float && !decimal) {
        diag_errorf(lx->diag, t->span, "floating-point suffix on a non-decimal literal");
        return;
    }
    if (!suf_is_float && literal_is_float) {
        diag_errorf(lx->diag, t->span, "integer suffix on a floating-point literal");
        return;
    }
    t->num_suffix = suf;
    if (suf_is_float) t->kind = Tok::Float;
}

Token lex_number(Lexer* lx) {
    size_t start = lx->pos;

    // Prefixed bases: 0x / 0b / 0o.
    if (peek(lx, 0) == '0') {
        char p = peek(lx, 1);
        int  base = 0;
        if (p == 'x' || p == 'X') base = 16;
        else if (p == 'b' || p == 'B') base = 2;
        else if (p == 'o' || p == 'O') base = 8;
        if (base != 0) {
            lx->pos += 2;
            bool ok, overflow;
            uint64_t value = parse_prefixed_int(lx, base, &ok, &overflow);
            Token t      = make(lx, Tok::Int, start);
            t.int_value  = value;
            if (!ok) {
                diag_errorf(lx->diag, t.span, "expected digits after base prefix");
            } else if (overflow) {
                diag_errorf(lx->diag, t.span, "integer literal overflows u64");
                t.int_value = UINT64_MAX;
            }
            scan_suffix(lx, &t, /*literal_is_float=*/false, /*decimal=*/false);
            t.span.end = static_cast<uint32_t>(lx->pos);
            return t;
        }
    }

    // Decimal: integer part, optional fraction, optional exponent.
    bool is_float = false;
    while (is_digit(peek(lx, 0)) || peek(lx, 0) == '_') lx->pos++;

    if (peek(lx, 0) == '.' && is_digit(peek(lx, 1))) {
        is_float = true;
        lx->pos++; // consume '.'
        while (is_digit(peek(lx, 0)) || peek(lx, 0) == '_') lx->pos++;
    }

    if (peek(lx, 0) == 'e' || peek(lx, 0) == 'E') {
        size_t save = lx->pos;
        lx->pos++;
        if (peek(lx, 0) == '+' || peek(lx, 0) == '-') lx->pos++;
        if (!is_digit(peek(lx, 0))) {
            lx->pos = save; // not an exponent after all
        } else {
            is_float = true;
            while (is_digit(peek(lx, 0)) || peek(lx, 0) == '_') lx->pos++;
        }
    }

    size_t digits_end = lx->pos; // before any suffix

    Token t = make(lx, is_float ? Tok::Float : Tok::Int, start);
    scan_suffix(lx, &t, is_float, /*decimal=*/true);
    is_float = (t.kind == Tok::Float);
    t.span.end = static_cast<uint32_t>(lx->pos);

    // Build a clean copy of the digit region (underscores stripped) and parse.
    char   buf[128];
    size_t n = 0;
    for (size_t i = start; i < digits_end && n + 1 < sizeof(buf); i++) {
        if (lx->src[i] != '_') buf[n++] = lx->src[i];
    }
    buf[n] = '\0';

    if (is_float) {
        t.float_value = std::strtod(buf, nullptr);
    } else {
        uint64_t value = 0;
        bool     overflow = false;
        for (size_t i = 0; i < n; i++) {
            unsigned d = static_cast<unsigned>(buf[i] - '0');
            if (value > (UINT64_MAX - d) / 10) overflow = true;
            value = value * 10 + d;
        }
        if (overflow) {
            diag_errorf(lx->diag, t.span, "integer literal overflows u64");
            value = UINT64_MAX;
        }
        t.int_value = value;
    }
    return t;
}

Token lex_doc_comment(Lexer* lx) {
    size_t start = lx->pos;
    lx->pos += 3; // consume '///'
    while (lx->pos < lx->len && lx->src[lx->pos] != '\n') lx->pos++;
    return make(lx, Tok::DocComment, start); // span covers '///' .. EOL
}

Token one(Lexer* lx, Tok kind, size_t start) {
    lx->pos = start + 1;
    return make(lx, kind, start);
}

Token lex_operator(Lexer* lx) {
    size_t start = lx->pos;
    char   c     = peek(lx, 0);
    char   c1    = peek(lx, 1);
    char   c2    = peek(lx, 2);

    switch (c) {
        case '(': return one(lx, Tok::LParen, start);
        case ')': return one(lx, Tok::RParen, start);
        case '{': return one(lx, Tok::LBrace, start);
        case '}': return one(lx, Tok::RBrace, start);
        case '[': return one(lx, Tok::LBracket, start);
        case ']': return one(lx, Tok::RBracket, start);
        case ',': return one(lx, Tok::Comma, start);
        case ':': return one(lx, Tok::Colon, start);
        case ';': return one(lx, Tok::Semicolon, start);
        case '~': return one(lx, Tok::Tilde, start);
        case '@': return one(lx, Tok::At, start);

        case '.':
            if (c1 == '.' && c2 == '=') { lx->pos += 3; return make(lx, Tok::DotDotEq, start); }
            if (c1 == '.')              { lx->pos += 2; return make(lx, Tok::DotDot, start); }
            return one(lx, Tok::Dot, start);

        case '?':
            if (c1 == '?') { lx->pos += 2; return make(lx, Tok::QuestionQuestion, start); }
            return one(lx, Tok::Question, start);

        case '+': if (c1 == '=') { lx->pos += 2; return make(lx, Tok::PlusEq, start); }
                  return one(lx, Tok::Plus, start);
        case '*': if (c1 == '=') { lx->pos += 2; return make(lx, Tok::StarEq, start); }
                  return one(lx, Tok::Star, start);
        case '/': if (c1 == '=') { lx->pos += 2; return make(lx, Tok::SlashEq, start); }
                  return one(lx, Tok::Slash, start);
        case '%': if (c1 == '=') { lx->pos += 2; return make(lx, Tok::PercentEq, start); }
                  return one(lx, Tok::Percent, start);
        case '^': if (c1 == '=') { lx->pos += 2; return make(lx, Tok::CaretEq, start); }
                  return one(lx, Tok::Caret, start);

        case '-':
            if (c1 == '>') { lx->pos += 2; return make(lx, Tok::Arrow, start); }
            if (c1 == '=') { lx->pos += 2; return make(lx, Tok::MinusEq, start); }
            return one(lx, Tok::Minus, start);

        case '=':
            if (c1 == '=') { lx->pos += 2; return make(lx, Tok::EqEq, start); }
            if (c1 == '>') { lx->pos += 2; return make(lx, Tok::FatArrow, start); }
            return one(lx, Tok::Eq, start);

        case '!':
            if (c1 == '=') { lx->pos += 2; return make(lx, Tok::NotEq, start); }
            return one(lx, Tok::Bang, start);

        case '&':
            if (c1 == '&') { lx->pos += 2; return make(lx, Tok::AmpAmp, start); }
            if (c1 == '=') { lx->pos += 2; return make(lx, Tok::AmpEq, start); }
            return one(lx, Tok::Amp, start);

        case '|':
            if (c1 == '|') { lx->pos += 2; return make(lx, Tok::PipePipe, start); }
            if (c1 == '=') { lx->pos += 2; return make(lx, Tok::PipeEq, start); }
            return one(lx, Tok::Pipe, start);

        case '<':
            if (c1 == '<' && c2 == '=') { lx->pos += 3; return make(lx, Tok::ShlEq, start); }
            if (c1 == '<')              { lx->pos += 2; return make(lx, Tok::Shl, start); }
            if (c1 == '=')              { lx->pos += 2; return make(lx, Tok::Le, start); }
            return one(lx, Tok::Lt, start);

        case '>':
            if (c1 == '>' && c2 == '=') { lx->pos += 3; return make(lx, Tok::ShrEq, start); }
            if (c1 == '>')              { lx->pos += 2; return make(lx, Tok::Shr, start); }
            if (c1 == '=')              { lx->pos += 2; return make(lx, Tok::Ge, start); }
            return one(lx, Tok::Gt, start);

        default: {
            lx->pos = start + 1;
            Token t = make(lx, Tok::Error, start);
            diag_errorf(lx->diag, t.span, "unexpected character '%c' (0x%02x)",
                        (c >= 32 && c < 127) ? c : '?', static_cast<unsigned char>(c));
            return t;
        }
    }
}

// Pre-scan a triple-quoted body starting at body_start (just past the opening
// """). Locates the closing """, drops a leading and trailing line break, and
// computes the common indentation to strip. Fills the string-mode frame.
void setup_triple(Lexer* lx, LexMode* m, size_t body_start) {
    size_t q     = body_start;
    bool   found = false;
    while (q + 3 <= lx->len) {
        if (lx->src[q] == '"' && lx->src[q + 1] == '"' && lx->src[q + 2] == '"') {
            found = true;
            break;
        }
        q++;
    }
    size_t cend;
    if (found) {
        cend           = q;
        m->terminated  = true;
        m->close_pos   = static_cast<uint32_t>(q);
    } else {
        q              = lx->len;
        cend           = lx->len;
        m->terminated  = false;
        m->close_pos   = static_cast<uint32_t>(lx->len);
        diag_error(lx->diag,
                   Span{lx->file_id, static_cast<uint32_t>(body_start - 3),
                        static_cast<uint32_t>(lx->len)},
                   "unterminated triple-quoted string");
    }

    // Drop one leading line break (so the first line isn't forced blank).
    size_t cs = body_start;
    if (cs < cend && lx->src[cs] == '\r') cs++;
    if (cs < cend && lx->src[cs] == '\n') cs++;

    // Drop a trailing blank line: trailing horizontal ws then one newline.
    size_t ce = cend;
    {
        size_t t = ce;
        while (t > cs && (lx->src[t - 1] == ' ' || lx->src[t - 1] == '\t')) t--;
        if (t > cs && lx->src[t - 1] == '\n') {
            t--;
            if (t > cs && lx->src[t - 1] == '\r') t--;
            ce = t;
        }
    }

    // Common indent across non-blank lines.
    uint32_t min_ind = UINT32_MAX;
    size_t   i       = cs;
    while (i < ce) {
        uint32_t ind = 0;
        size_t   j   = i;
        while (j < ce && (lx->src[j] == ' ' || lx->src[j] == '\t')) { j++; ind++; }
        if (j < ce && lx->src[j] != '\n') {
            if (ind < min_ind) min_ind = ind;
        }
        while (i < ce && lx->src[i] != '\n') i++;
        if (i < ce) i++;
    }
    if (min_ind == UINT32_MAX) min_ind = 0;

    m->content_end   = static_cast<uint32_t>(ce);
    m->min_indent    = min_ind;
    m->at_line_start = true;
    lx->pos          = cs;
}

// Open a string literal from normal mode: emit StrStart, set up and push the
// string-body frame. `pos` points at the opening delimiter.
Token begin_string(Lexer* lx, bool raw, bool triple) {
    size_t start = lx->pos;
    lx->pos += raw ? 1 : 0;          // skip 'r'
    lx->pos += triple ? 3 : 1;       // skip the quote(s)

    Token t = make(lx, Tok::StrStart, start);

    LexMode m{};
    m.is_interp     = false;
    m.raw           = raw;
    m.triple        = triple;
    m.at_line_start = false;
    m.terminated    = true;
    m.content_end   = 0;
    m.close_pos     = 0;
    m.min_indent    = 0;
    if (triple) {
        setup_triple(lx, &m, lx->pos); // also moves lx->pos to content start
    }
    lx->modes.push(m);
    return t;
}

// Process one escape sequence (after the backslash) into `buf`.
void lex_escape(Lexer* lx, Vec<char>* buf) {
    size_t bs = lx->pos;
    lx->pos++; // consume '\'
    if (lx->pos >= lx->len) {
        diag_error(lx->diag, Span{lx->file_id, static_cast<uint32_t>(bs),
                                  static_cast<uint32_t>(lx->pos)},
                   "unterminated escape sequence");
        return;
    }
    char e = lx->src[lx->pos++];
    char out;
    switch (e) {
        case 'n':  out = '\n'; break;
        case 't':  out = '\t'; break;
        case 'r':  out = '\r'; break;
        case '0':  out = '\0'; break;
        case '\\': out = '\\'; break;
        case '"':  out = '"';  break;
        case '\'': out = '\''; break;
        case '$':  out = '$';  break; // escape interpolation
        default:
            diag_errorf(lx->diag, Span{lx->file_id, static_cast<uint32_t>(bs),
                                       static_cast<uint32_t>(lx->pos)},
                        "unknown escape sequence '\\%c'", (e >= 32 && e < 127) ? e : '?');
            out = e; // recovery: take the char literally
            break;
    }
    buf->push(out);
}

Token make_str_text(Lexer* lx, Vec<char>* buf, size_t start) {
    Token t    = make(lx, Tok::StrText, start);
    t.span.end = static_cast<uint32_t>(lx->pos);
    t.ident_id = intern(lx->in, buf->data, buf->len);
    return t;
}

// Scan within a string body: a text chunk, an interpolation boundary, or the
// closing delimiter. The top-of-stack frame is the active string.
Token lex_string_body(Lexer* lx) {
    LexMode* m     = &lx->modes.back();
    size_t   start = lx->pos;
    Vec<char> buf{};

    for (;;) {
        if (m->triple && m->at_line_start) {
            uint32_t k = 0;
            while (k < m->min_indent && (peek(lx, 0) == ' ' || peek(lx, 0) == '\t')) {
                lx->pos++;
                k++;
            }
            m->at_line_start = false;
        }

        bool at_close = false, at_interp = false, unterminated = false;
        if (m->triple) {
            if (lx->pos >= m->content_end) at_close = true;
        } else {
            if (lx->pos >= lx->len || peek(lx, 0) == '\n') unterminated = true;
            else if (peek(lx, 0) == '"')                    at_close = true;
        }
        if (!at_close && !unterminated && peek(lx, 0) == '$' && peek(lx, 1) == '{') {
            at_interp = true;
        }

        if (at_close || at_interp || unterminated) {
            if (buf.len > 0) {
                Token t = make_str_text(lx, &buf, start);
                buf.free();
                return t; // leave pos at the boundary for the next call
            }
            buf.free();

            if (at_interp) {
                size_t s = lx->pos;
                lx->pos += 2; // consume "${"
                LexMode fr{};
                fr.is_interp  = true;
                fr.brace_depth = 0;
                lx->modes.push(fr);
                return make(lx, Tok::InterpStart, s);
            }
            // close or unterminated -> emit StrEnd, pop the frame
            size_t s = lx->pos;
            if (unterminated) {
                diag_error(lx->diag, Span{lx->file_id, static_cast<uint32_t>(s),
                                          static_cast<uint32_t>(s)},
                           "unterminated string literal");
                // do not consume the newline/EOF; let normal mode see it
            } else if (m->triple) {
                lx->pos = m->terminated ? (m->close_pos + 3) : lx->len;
            } else {
                lx->pos += 1; // consume closing quote
            }
            Token t = make(lx, Tok::StrEnd, s);
            lx->modes.len--; // pop string frame
            return t;
        }

        char c = peek(lx, 0);
        if (c == '\n') {
            buf.push('\n');
            lx->pos++;
            if (m->triple) m->at_line_start = true;
        } else if (!m->raw && c == '\\') {
            lex_escape(lx, &buf);
        } else {
            buf.push(c);
            lx->pos++;
        }
    }
}

// Advance over spaces/tabs/CR, newlines, and non-doc comments. Returns true
// if a newline was crossed; sets *nl_start to its offset. Stops at EOF, a doc
// comment, or a real token character.
bool skip_trivia(Lexer* lx, size_t* nl_start) {
    bool saw_nl = false;
    for (;;) {
        char c = peek(lx, 0);
        if (c == ' ' || c == '\t' || c == '\r') {
            lx->pos++;
        } else if (c == '\n') {
            if (!saw_nl) *nl_start = lx->pos;
            saw_nl = true;
            lx->pos++;
        } else if (c == '/' && peek(lx, 1) == '/') {
            // Doc comment '///' (but not '////') stops skipping.
            if (peek(lx, 2) == '/' && peek(lx, 3) != '/') break;
            lx->pos += 2;
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n') lx->pos++;
        } else if (c == '/' && peek(lx, 1) == '*') {
            size_t cstart = lx->pos;
            lx->pos += 2;
            int depth = 1;
            while (lx->pos < lx->len && depth > 0) {
                if (peek(lx, 0) == '/' && peek(lx, 1) == '*') { lx->pos += 2; depth++; }
                else if (peek(lx, 0) == '*' && peek(lx, 1) == '/') { lx->pos += 2; depth--; }
                else lx->pos++;
            }
            if (depth != 0) {
                diag_error(lx->diag,
                           Span{lx->file_id, static_cast<uint32_t>(cstart),
                                static_cast<uint32_t>(lx->pos)},
                           "unterminated block comment");
            }
        } else {
            break;
        }
    }
    return saw_nl;
}

} // namespace

// Normal-mode scan: trivia, EOF, newline continuation, and token dispatch.
// Also handles interpolation brace bookkeeping (when the active frame is an
// interp) and the start of a string literal.
static Token lex_normal(Lexer* lx) {
    size_t nl_start = lx->pos;
    bool   saw_nl   = skip_trivia(lx, &nl_start);

    if (lx->pos >= lx->len) {
        return make(lx, Tok::Eof, lx->pos);
    }

    if (saw_nl && lx->started) {
        // Continuation: suppress the separator after a trailing operator/open
        // delimiter, or when the next line begins with a leading dot.
        bool leading_dot = (lx->src[lx->pos] == '.');
        if (op_continues_line(lx->last_emitted) || leading_dot) {
            return lex_normal(lx); // skip the newline; scan the real token
        }
        Token t    = make(lx, Tok::Newline, nl_start);
        t.span.end = static_cast<uint32_t>(lx->pos);
        return t;
    }

    lx->started = true;
    char c = lx->src[lx->pos];

    // Inside an interpolation expression, '{' / '}' are tracked so the matching
    // '}' at depth 0 closes the interpolation.
    if (lx->modes.len > 0 && lx->modes.back().is_interp) {
        LexMode* fr = &lx->modes.back();
        if (c == '}') {
            if (fr->brace_depth == 0) {
                size_t s = lx->pos;
                lx->pos++;
                Token t = make(lx, Tok::InterpEnd, s);
                lx->modes.len--; // pop interp frame; resume the string body
                return t;
            }
            fr->brace_depth--;
            return one(lx, Tok::RBrace, lx->pos);
        }
        if (c == '{') {
            fr->brace_depth++;
            return one(lx, Tok::LBrace, lx->pos);
        }
    }

    // String literals (checked before identifiers so r"..." isn't read as `r`).
    if (c == '"') {
        bool triple = (peek(lx, 1) == '"' && peek(lx, 2) == '"');
        return begin_string(lx, /*raw=*/false, triple);
    }
    if (c == 'r' && peek(lx, 1) == '"') {
        bool triple = (peek(lx, 2) == '"' && peek(lx, 3) == '"');
        return begin_string(lx, /*raw=*/true, triple);
    }

    if (c == '/' && peek(lx, 1) == '/' && peek(lx, 2) == '/' && peek(lx, 3) != '/') {
        return lex_doc_comment(lx);
    }
    if (is_ident_start(c)) return lex_ident(lx);
    if (is_digit(c))       return lex_number(lx);
    return lex_operator(lx);
}

Token lexer_next(Lexer* lx) {
    Token t;
    if (lx->modes.len > 0 && !lx->modes.back().is_interp) {
        t = lex_string_body(lx); // active frame is a string body
    } else {
        t = lex_normal(lx);
    }
    lx->last_emitted = t.kind;
    return t;
}

void lex_collect(Lexer* lx, Vec<Token>* out) {
    for (;;) {
        Token t = lexer_next(lx);
        out->push(t);
        if (t.kind == Tok::Eof) break;
    }
}

const char* tok_name(Tok k) {
    switch (k) {
        case Tok::Eof: return "Eof";
        case Tok::Error: return "Error";
        case Tok::Newline: return "Newline";
        case Tok::DocComment: return "DocComment";
        case Tok::Ident: return "Ident";
        case Tok::Int: return "Int";
        case Tok::Float: return "Float";
        case Tok::StrStart: return "StrStart";
        case Tok::StrText: return "StrText";
        case Tok::StrEnd: return "StrEnd";
        case Tok::InterpStart: return "InterpStart";
        case Tok::InterpEnd: return "InterpEnd";
        case Tok::KwFunc: return "func";
        case Tok::KwLet: return "let";
        case Tok::KwVar: return "var";
        case Tok::KwConst: return "const";
        case Tok::KwIf: return "if";
        case Tok::KwElse: return "else";
        case Tok::KwWhile: return "while";
        case Tok::KwFor: return "for";
        case Tok::KwLoop: return "loop";
        case Tok::KwIn: return "in";
        case Tok::KwMatch: return "match";
        case Tok::KwBreak: return "break";
        case Tok::KwContinue: return "continue";
        case Tok::KwReturn: return "return";
        case Tok::KwDefer: return "defer";
        case Tok::KwTrait: return "trait";
        case Tok::KwType: return "type";
        case Tok::KwAlias: return "alias";
        case Tok::KwPub: return "pub";
        case Tok::KwImport: return "import";
        case Tok::KwExtern: return "extern";
        case Tok::KwComptime: return "comptime";
        case Tok::KwUnsafe: return "unsafe";
        case Tok::KwMove: return "move";
        case Tok::KwAs: return "as";
        case Tok::KwWhere: return "where";
        case Tok::KwRef: return "ref";
        case Tok::KwMut: return "mut";
        case Tok::KwTrue: return "true";
        case Tok::KwFalse: return "false";
        case Tok::KwNull: return "null";
        case Tok::KwSelf: return "self";
        case Tok::KwSelfType: return "Self";
        case Tok::LParen: return "(";
        case Tok::RParen: return ")";
        case Tok::LBrace: return "{";
        case Tok::RBrace: return "}";
        case Tok::LBracket: return "[";
        case Tok::RBracket: return "]";
        case Tok::Comma: return ",";
        case Tok::Dot: return ".";
        case Tok::DotDot: return "..";
        case Tok::DotDotEq: return "..=";
        case Tok::Colon: return ":";
        case Tok::Semicolon: return ";";
        case Tok::Arrow: return "->";
        case Tok::FatArrow: return "=>";
        case Tok::At: return "@";
        case Tok::Question: return "?";
        case Tok::QuestionQuestion: return "??";
        case Tok::Plus: return "+";
        case Tok::Minus: return "-";
        case Tok::Star: return "*";
        case Tok::Slash: return "/";
        case Tok::Percent: return "%";
        case Tok::Amp: return "&";
        case Tok::Pipe: return "|";
        case Tok::Caret: return "^";
        case Tok::Tilde: return "~";
        case Tok::Shl: return "<<";
        case Tok::Shr: return ">>";
        case Tok::AmpAmp: return "&&";
        case Tok::PipePipe: return "||";
        case Tok::Bang: return "!";
        case Tok::EqEq: return "==";
        case Tok::NotEq: return "!=";
        case Tok::Lt: return "<";
        case Tok::Gt: return ">";
        case Tok::Le: return "<=";
        case Tok::Ge: return ">=";
        case Tok::Eq: return "=";
        case Tok::PlusEq: return "+=";
        case Tok::MinusEq: return "-=";
        case Tok::StarEq: return "*=";
        case Tok::SlashEq: return "/=";
        case Tok::PercentEq: return "%=";
        case Tok::AmpEq: return "&=";
        case Tok::PipeEq: return "|=";
        case Tok::CaretEq: return "^=";
        case Tok::ShlEq: return "<<=";
        case Tok::ShrEq: return ">>=";
    }
    return "?";
}

#include "arena.h"
#include "diag.h"
#include "intern.h"
#include "lex.h"
#include "vec.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

Vec<Token> lex_all(const char* src, arena* a, Interner* in, Diag* d) {
    uint32_t fid = diag_add_file(d, "test.dg", src, std::strlen(src));
    Lexer lx;
    lexer_init(&lx, src, std::strlen(src), fid, in, d);
    Vec<Token> toks{};
    lex_collect(&lx, &toks);
    lexer_free(&lx);
    (void)a;
    return toks;
}

// Assert the kind sequence matches, printing a readable diff on failure.
void expect_kinds(const Vec<Token>& toks, const Tok* expected, size_t n) {
    if (toks.len != n) {
        std::fprintf(stderr, "kind count mismatch: got %zu, expected %zu\n", toks.len, n);
        for (size_t i = 0; i < toks.len; i++)
            std::fprintf(stderr, "  [%zu] %s\n", i, tok_name(toks[i].kind));
        assert(false);
    }
    for (size_t i = 0; i < n; i++) {
        if (toks[i].kind != expected[i]) {
            std::fprintf(stderr, "kind[%zu]: got %s, expected %s\n",
                         i, tok_name(toks[i].kind), tok_name(expected[i]));
            assert(false);
        }
    }
}

} // namespace

int main() {
    arena a = arena_create(0);
    Interner in;
    intern_init(&in, &a);

    // 1. A small function: keywords, idents, delimiters, ops, ints, newlines.
    {
        Diag d{};
        const char* src =
            "func main() {\n"
            "    let x = 1 + 2 * 3\n"
            "    return x\n"
            "}\n";
        Vec<Token> t = lex_all(src, &a, &in, &d);
        const Tok want[] = {
            Tok::KwFunc, Tok::Ident, Tok::LParen, Tok::RParen, Tok::LBrace, Tok::Newline,
            Tok::KwLet, Tok::Ident, Tok::Eq, Tok::Int, Tok::Plus, Tok::Int, Tok::Star, Tok::Int, Tok::Newline,
            Tok::KwReturn, Tok::Ident, Tok::Newline,
            Tok::RBrace,
            Tok::Eof,
        };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        assert(!diag_has_errors(&d));

        // `main` and `x` interned distinctly; `x` appears twice with same id.
        assert(t[1].ident_id == intern_cstr(&in, "main"));
        assert(t[7].ident_id == t[16].ident_id);
        // Integer values.
        assert(t[9].int_value == 1 && t[11].int_value == 2 && t[13].int_value == 3);

        t.free();
        diag_free(&d);
    }

    // 2. Maximal munch over multi-char operators.
    {
        Diag d{};
        const char* src = "== = => -> .. ..= << >> <<= >>= && || ?? <= >= != += -= *= /= %= &= |= ^=";
        Vec<Token> t = lex_all(src, &a, &in, &d);
        const Tok want[] = {
            Tok::EqEq, Tok::Eq, Tok::FatArrow, Tok::Arrow, Tok::DotDot, Tok::DotDotEq,
            Tok::Shl, Tok::Shr, Tok::ShlEq, Tok::ShrEq, Tok::AmpAmp, Tok::PipePipe,
            Tok::QuestionQuestion, Tok::Le, Tok::Ge, Tok::NotEq,
            Tok::PlusEq, Tok::MinusEq, Tok::StarEq, Tok::SlashEq, Tok::PercentEq,
            Tok::AmpEq, Tok::PipeEq, Tok::CaretEq,
            Tok::Eof,
        };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        assert(!diag_has_errors(&d));
        t.free();
        diag_free(&d);
    }

    // 3. All reserved words lex as their keyword kind, not Ident.
    {
        Diag d{};
        const char* src = "ref mut move as where comptime unsafe self Self true false null";
        Vec<Token> t = lex_all(src, &a, &in, &d);
        const Tok want[] = {
            Tok::KwRef, Tok::KwMut, Tok::KwMove, Tok::KwAs, Tok::KwWhere,
            Tok::KwComptime, Tok::KwUnsafe, Tok::KwSelf, Tok::KwSelfType,
            Tok::KwTrue, Tok::KwFalse, Tok::KwNull, Tok::Eof,
        };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        t.free();
        diag_free(&d);
    }

    // 4. Identifier rules: underscore start, digits after, near-keywords stay idents.
    {
        Diag d{};
        const char* src = "_x funcs func_ x1 _ R3f";
        Vec<Token> t = lex_all(src, &a, &in, &d);
        const Tok want[] = {
            Tok::Ident, Tok::Ident, Tok::Ident, Tok::Ident, Tok::Ident, Tok::Ident, Tok::Eof,
        };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        assert(!diag_has_errors(&d));
        t.free();
        diag_free(&d);
    }

    // 5. Integer overflow is diagnosed.
    {
        Diag d{};
        const char* src = "99999999999999999999999999";
        Vec<Token> t = lex_all(src, &a, &in, &d);
        assert(t[0].kind == Tok::Int);
        assert(diag_has_errors(&d));
        t.free();
        diag_free(&d);
    }

    // 6. Unknown character produces an Error token + diagnostic.
    {
        Diag d{};
        const char* src = "a $ b";
        Vec<Token> t = lex_all(src, &a, &in, &d);
        assert(t[0].kind == Tok::Ident);
        assert(t[1].kind == Tok::Error);
        assert(t[2].kind == Tok::Ident);
        assert(diag_has_errors(&d));
        t.free();
        diag_free(&d);
    }

    // 7. Number literals: bases, separators, floats, suffixes.
    {
        Diag d{};
        const char* src = "0xff_aa 0b1010_0101 0o755 3.14 1.5f32 1e9 42i64 100";
        Vec<Token> t = lex_all(src, &a, &in, &d);
        assert(!diag_has_errors(&d));

        assert(t[0].kind == Tok::Int   && t[0].int_value == 0xffaa);
        assert(t[1].kind == Tok::Int   && t[1].int_value == 0xA5);
        assert(t[2].kind == Tok::Int   && t[2].int_value == 0755);
        assert(t[3].kind == Tok::Float && t[3].float_value > 3.13 && t[3].float_value < 3.15);
        assert(t[4].kind == Tok::Float && t[4].num_suffix == NumSuffix::F32);
        assert(t[5].kind == Tok::Float && t[5].float_value > 9.9e8);
        assert(t[6].kind == Tok::Int   && t[6].int_value == 42 && t[6].num_suffix == NumSuffix::I64);
        assert(t[7].kind == Tok::Int   && t[7].int_value == 100);
        assert(t[8].kind == Tok::Eof);
        t.free();
        diag_free(&d);
    }

    // 8. Ranges must not be eaten by the float rule (dot needs a trailing digit).
    {
        Diag d{};
        const char* src = "0..10 1.5..2.5 x.field";
        Vec<Token> t = lex_all(src, &a, &in, &d);
        const Tok want[] = {
            Tok::Int, Tok::DotDot, Tok::Int,
            Tok::Float, Tok::DotDot, Tok::Float,
            Tok::Ident, Tok::Dot, Tok::Ident,
            Tok::Eof,
        };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        assert(!diag_has_errors(&d));
        t.free();
        diag_free(&d);
    }

    // 9. Bad numerics are diagnosed.
    {
        Diag d{};
        Vec<Token> t = lex_all("0xZ", &a, &in, &d);     // no hex digits
        assert(diag_has_errors(&d));
        t.free(); diag_free(&d);

        Diag d2{};
        Vec<Token> t2 = lex_all("3.14i32", &a, &in, &d2); // int suffix on float
        assert(diag_has_errors(&d2));
        t2.free(); diag_free(&d2);

        Diag d3{};
        Vec<Token> t3 = lex_all("10abc", &a, &in, &d3);    // invalid suffix
        assert(diag_has_errors(&d3));
        t3.free(); diag_free(&d3);
    }

    // 10. Comments: line, nested block, and a doc comment token.
    {
        Diag d{};
        const char* src =
            "a // line comment\n"
            "/* block /* nested */ still */ b\n"
            "/// doc text\n"
            "c\n";
        Vec<Token> t = lex_all(src, &a, &in, &d);
        const Tok want[] = {
            Tok::Ident,                 // a
            Tok::Newline,
            Tok::Ident,                 // b
            Tok::Newline,
            Tok::DocComment,            // /// doc text
            Tok::Newline,
            Tok::Ident,                 // c
            Tok::Eof,
        };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        assert(!diag_has_errors(&d));
        t.free();
        diag_free(&d);
    }

    // 11. Unterminated block comment is diagnosed.
    {
        Diag d{};
        Vec<Token> t = lex_all("/* never closed", &a, &in, &d);
        assert(diag_has_errors(&d));
        t.free();
        diag_free(&d);
    }

    // 12. Newline continuation: trailing operator, open delimiter, leading dot.
    {
        Diag d{};
        const char* src =
            "let x = 1 +\n"        // trailing '+' continues
            "    2\n"
            "let y = foo(\n"       // open '(' continues
            "    a,\n"             // trailing ',' continues
            "    b)\n"
            "let z = items\n"
            "    .map(f)\n";       // leading '.' continues
        Vec<Token> t = lex_all(src, &a, &in, &d);
        const Tok want[] = {
            Tok::KwLet, Tok::Ident, Tok::Eq, Tok::Int, Tok::Plus, Tok::Int, Tok::Newline,
            Tok::KwLet, Tok::Ident, Tok::Eq, Tok::Ident, Tok::LParen,
                Tok::Ident, Tok::Comma, Tok::Ident, Tok::RParen, Tok::Newline,
            Tok::KwLet, Tok::Ident, Tok::Eq, Tok::Ident,
                Tok::Dot, Tok::Ident, Tok::LParen, Tok::Ident, Tok::RParen,
            Tok::Eof,
        };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        assert(!diag_has_errors(&d));
        t.free();
        diag_free(&d);
    }

    // 13. CRLF line endings behave like LF; a UTF-8 BOM is rejected.
    {
        Diag d{};
        const char* src = "a\r\nb\r\n";
        Vec<Token> t = lex_all(src, &a, &in, &d);
        const Tok want[] = { Tok::Ident, Tok::Newline, Tok::Ident, Tok::Eof };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        assert(!diag_has_errors(&d));
        t.free();
        diag_free(&d);

        Diag d2{};
        const char* bom = "\xEF\xBB\xBF" "x";
        Vec<Token> t2 = lex_all(bom, &a, &in, &d2);
        assert(t2[0].kind == Tok::Ident); // 'x' still lexes after the BOM
        assert(diag_has_errors(&d2));      // but the BOM was reported
        t2.free();
        diag_free(&d2);
    }

    // Helper: does a StrText token's interned content equal `expect`?
    auto text_is = [&](const Token& t, const char* expect) -> bool {
        size_t      len = 0;
        const char* s   = intern_str(&in, t.ident_id, &len);
        return s && len == std::strlen(expect) && std::memcmp(s, expect, len) == 0;
    };

    // 14. Plain string + escape processing.
    {
        Diag d{};
        Vec<Token> t = lex_all("\"a\\nb\\t\\\"c\"", &a, &in, &d); // "a\nb\t\"c"
        const Tok want[] = { Tok::StrStart, Tok::StrText, Tok::StrEnd, Tok::Eof };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        assert(text_is(t[1], "a\nb\t\"c"));
        assert(!diag_has_errors(&d));
        t.free(); diag_free(&d);
    }

    // 15. Interpolation: text / expr / text, expr lexed as normal tokens.
    {
        Diag d{};
        Vec<Token> t = lex_all("\"x=${a + 1}!\"", &a, &in, &d);
        const Tok want[] = {
            Tok::StrStart, Tok::StrText, Tok::InterpStart,
            Tok::Ident, Tok::Plus, Tok::Int, Tok::InterpEnd,
            Tok::StrText, Tok::StrEnd, Tok::Eof,
        };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        assert(text_is(t[1], "x="));
        assert(text_is(t[7], "!"));
        assert(!diag_has_errors(&d));
        t.free(); diag_free(&d);
    }

    // 16. Leading interpolation produces no empty text chunk.
    {
        Diag d{};
        Vec<Token> t = lex_all("\"${name}\"", &a, &in, &d);
        const Tok want[] = {
            Tok::StrStart, Tok::InterpStart, Tok::Ident, Tok::InterpEnd, Tok::StrEnd, Tok::Eof,
        };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        t.free(); diag_free(&d);
    }

    // 17. Braces inside an interpolation (struct literal) are balanced.
    {
        Diag d{};
        Vec<Token> t = lex_all("\"${ f(P{x:1}) }\"", &a, &in, &d);
        const Tok want[] = {
            Tok::StrStart, Tok::InterpStart,
            Tok::Ident, Tok::LParen, Tok::Ident, Tok::LBrace,
            Tok::Ident, Tok::Colon, Tok::Int, Tok::RBrace, Tok::RParen,
            Tok::InterpEnd, Tok::StrEnd, Tok::Eof,
        };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        assert(!diag_has_errors(&d));
        t.free(); diag_free(&d);
    }

    // 18. A string nested inside an interpolation (mode stack recursion).
    {
        Diag d{};
        Vec<Token> t = lex_all("\"${ \"inner\" }\"", &a, &in, &d);
        const Tok want[] = {
            Tok::StrStart, Tok::InterpStart,
            Tok::StrStart, Tok::StrText, Tok::StrEnd,
            Tok::InterpEnd, Tok::StrEnd, Tok::Eof,
        };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        assert(text_is(t[3], "inner"));
        assert(!diag_has_errors(&d));
        t.free(); diag_free(&d);
    }

    // 19. Raw strings: no escape processing, but \$ cannot escape (it's literal).
    {
        Diag d{};
        Vec<Token> t = lex_all("r\"a\\nb\"", &a, &in, &d); // r"a\nb"
        const Tok want[] = { Tok::StrStart, Tok::StrText, Tok::StrEnd, Tok::Eof };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        assert(text_is(t[1], "a\\nb")); // backslash-n kept literally
        assert(!diag_has_errors(&d));
        t.free(); diag_free(&d);
    }

    // 20. \$ escapes interpolation in a normal string.
    {
        Diag d{};
        Vec<Token> t = lex_all("\"\\${x}\"", &a, &in, &d); // "\${x}"
        const Tok want[] = { Tok::StrStart, Tok::StrText, Tok::StrEnd, Tok::Eof };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        assert(text_is(t[1], "${x}"));
        assert(!diag_has_errors(&d));
        t.free(); diag_free(&d);
    }

    // 21. Triple-quoted: leading/trailing line-break drop + least-indent strip.
    {
        Diag d{};
        const char* src = "\"\"\"\n    alpha\n      beta\n    \"\"\"";
        Vec<Token> t = lex_all(src, &a, &in, &d);
        const Tok want[] = { Tok::StrStart, Tok::StrText, Tok::StrEnd, Tok::Eof };
        expect_kinds(t, want, sizeof(want) / sizeof(want[0]));
        assert(text_is(t[1], "alpha\n  beta"));
        assert(!diag_has_errors(&d));
        t.free(); diag_free(&d);
    }

    // 22. Unterminated string is diagnosed (and recovers).
    {
        Diag d{};
        Vec<Token> t = lex_all("\"abc", &a, &in, &d);
        assert(t[0].kind == Tok::StrStart);
        assert(diag_has_errors(&d));
        t.free(); diag_free(&d);
    }

    intern_free(&in);
    arena_destroy(&a);

    std::printf("lex_test: OK\n");
    return 0;
}

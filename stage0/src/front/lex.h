#ifndef DIGON_LEX_H
#define DIGON_LEX_H

#include <cstddef>
#include <cstdint>

#include "diag.h"
#include "intern.h"
#include "map.h"
#include "vec.h"

// Token kinds. Keywords and literal-like names (true/false/null/self/Self)
// are recognised after identifier scanning. digon uses square brackets for
// generics, so `>>` is unambiguously a shift.
enum class Tok : uint8_t {
    Eof,
    Error,
    Newline,    // statement separator
    DocComment, // /// ... ; attached to the next item by the parser

    Ident,
    Int,
    Float,

    // String pieces. A string literal lexes as:
    //   StrStart (StrText | InterpStart <expr tokens> InterpEnd)* StrEnd
    // The parser reassembles these into a concatenation tree. StrText carries
    // its processed bytes in ident_id (interned).
    StrStart,
    StrText,
    StrEnd,
    InterpStart,
    InterpEnd,

    // Keywords
    KwFunc, KwLet, KwVar, KwConst, KwIf, KwElse, KwWhile,
    KwFor, KwLoop, KwIn, KwMatch, KwBreak, KwContinue, KwReturn,
    KwDefer, KwTrait, KwType, KwAlias, KwPub, KwImport, KwExtern,
    KwComptime, KwUnsafe, KwMove, KwAs, KwWhere, KwRef, KwMut,

    // Literal-like reserved names
    KwTrue, KwFalse, KwNull, KwSelf, KwSelfType,

    // Delimiters
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,

    // Punctuation
    Comma, Dot, DotDot, DotDotEq, Colon, Semicolon,
    Arrow, FatArrow, At, Question, QuestionQuestion,

    // Arithmetic
    Plus, Minus, Star, Slash, Percent,

    // Bitwise / shifts
    Amp, Pipe, Caret, Tilde, Shl, Shr,

    // Logical
    AmpAmp, PipePipe, Bang,

    // Comparison
    EqEq, NotEq, Lt, Gt, Le, Ge,

    // Assignment
    Eq, PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    AmpEq, PipeEq, CaretEq, ShlEq, ShrEq,
};

// Width/kind suffix on a numeric literal. None = unsuffixed.
enum class NumSuffix : uint8_t {
    None,
    I8, I16, I32, I64, I128, Iptr,
    U8, U16, U32, U64, U128, Uptr,
    F32, F64,
};

struct Token {
    Tok       kind;
    Span      span;
    uint32_t  ident_id;    // Ident & keywords: interned name; else 0
    uint64_t  int_value;   // Int: parsed value; else 0
    double    float_value; // Float: parsed value; else 0
    NumSuffix num_suffix;  // Int/Float: explicit suffix, or None
};

// One frame of the lexer's mode stack. A frame is either an interpolation
// expression (normal lexing, tracking brace depth to find the closing '}')
// or a string body (literal scanning until the closing delimiter).
struct LexMode {
    bool     is_interp;
    int      brace_depth;   // interp: nesting of '{' '}' inside ${ ... }

    bool     triple;        // string: """ ... """
    bool     raw;           // string: r"..." (no escape processing)
    bool     at_line_start; // string(triple): next chars are line-leading
    bool     terminated;    // string(triple): closing """ was found
    uint32_t content_end;   // string(triple): byte offset where content ends
    uint32_t close_pos;     // string(triple): byte offset of closing """
    uint32_t min_indent;    // string(triple): common indent to strip
};

struct Lexer {
    const char*    src;
    size_t         len;
    uint32_t       file_id;
    size_t         pos;
    bool           started;      // suppresses a leading Newline token
    Tok            last_emitted; // previous token kind, for continuation rules
    Interner*      in;
    Diag*          diag;
    Map            keywords;     // interned-id -> Tok
    Vec<LexMode>   modes;        // string / interpolation mode stack
};

void  lexer_init(Lexer* lx, const char* src, size_t len, uint32_t file_id,
                 Interner* in, Diag* diag);
void  lexer_free(Lexer* lx);
Token lexer_next(Lexer* lx);            // returns Eof repeatedly at end
void  lex_collect(Lexer* lx, Vec<Token>* out); // pushes through and including Eof

const char* tok_name(Tok k);            // stable name for tests / diagnostics

#endif

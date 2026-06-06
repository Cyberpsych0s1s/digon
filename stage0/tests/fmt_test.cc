#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "fmt.h"
#include "intern.h"
#include "lex.h"
#include "parse.h"
#include "vec.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Lex + parse + format `src`, returning a freshly malloc'd C string. Asserts
// the source parses without diagnostics. Caller frees.
char* format_source(const char* src) {
    arena a = arena_create(0);
    Interner in;
    intern_init(&in, &a);
    Diag d{};

    uint32_t fid = diag_add_file(&d, "t.dg", src, std::strlen(src));
    Lexer lx;
    lexer_init(&lx, src, std::strlen(src), fid, &in, &d);
    Vec<Token> toks{};
    lex_collect(&lx, &toks);
    lexer_free(&lx);

    Module* m = parse_module(toks.data, static_cast<uint32_t>(toks.len), &a, &in, &d);
    assert(!diag_has_errors(&d));

    Vec<char> out{};
    format_module(m, &in, &out);
    char* result = static_cast<char*>(std::malloc(out.len));
    std::memcpy(result, out.data, out.len); // includes trailing NUL

    out.free();
    toks.free();
    diag_free(&d);
    intern_free(&in);
    arena_destroy(&a);
    return result;
}

void check_canonical(const char* src, const char* expected) {
    char* got = format_source(src);
    if (std::strcmp(got, expected) != 0) {
        std::fprintf(stderr, "fmt mismatch\n--- src ---\n%s\n--- expected ---\n%s\n--- got ---\n%s\n",
                     src, expected, got);
        assert(false);
    }
    std::free(got);
}

// Formatting must be a fixed point: fmt(fmt(x)) == fmt(x).
void check_idempotent(const char* src) {
    char* once  = format_source(src);
    char* twice = format_source(once);
    if (std::strcmp(once, twice) != 0) {
        std::fprintf(stderr, "fmt not idempotent\n--- once ---\n%s\n--- twice ---\n%s\n", once, twice);
        assert(false);
    }
    std::free(once);
    std::free(twice);
}

} // namespace

int main() {
    // Canonical reformatting of messy input.
    check_canonical("func   add(a:i32,b:i32)->i32{a+b}",
                    "func add(a: i32, b: i32) -> i32 {\n    a + b\n}\n");

    // Minimal parentheses driven by precedence; redundant ones are dropped.
    check_canonical("func f()->i32{(1+2)*3}",
                    "func f() -> i32 {\n    (1 + 2) * 3\n}\n");
    check_canonical("func f()->i32{1+(2*3)}",
                    "func f() -> i32 {\n    1 + 2 * 3\n}\n");

    // let/var, return, blocks.
    check_canonical("func f()->i32{var x=1\nx=x+2\nreturn x}",
                    "func f() -> i32 {\n    var x = 1\n    x = x + 2\n    return x\n}\n");

    // Struct and enum declarations.
    check_canonical("type   Point{x:i32\ny:i32}",
                    "type Point {\n    x: i32\n    y: i32\n}\n");
    check_canonical("type Color=Red|Green|Blue",
                    "type Color = Red | Green | Blue\n");

    // if / else if / else and match.
    check_canonical("func f(c: Color)->i32{match c{Color.Red=>1,_=>0}}",
                    "func f(c: Color) -> i32 {\n"
                    "    match c {\n"
                    "        Color.Red => 1,\n"
                    "        _ => 0,\n"
                    "    }\n"
                    "}\n");

    // Empty body stays compact.
    check_canonical("func main(){}", "func main() {}\n");

    // Idempotency across a representative spread of constructs.
    check_idempotent("func add(a: i32, b: i32) -> i32 { a + b }");
    check_idempotent("type Point { x: f64, y: f64 }");
    check_idempotent("type Color = Red | Green | Blue");
    check_idempotent(
        "func classify(c: Color) -> i32 {\n"
        "    let p = Point { x: 1, y: 2 }\n"
        "    if p.x > 0 && p.y > 0 {\n"
        "        match c {\n"
        "            Color.Red => 1,\n"
        "            _ => p.x + p.y,\n"
        "        }\n"
        "    } else {\n"
        "        0\n"
        "    }\n"
        "}");

    std::printf("fmt_test: OK\n");
    return 0;
}

#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "intern.h"
#include "lex.h"
#include "parse.h"
#include "vec.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

// Lex + parse `src`, dump the AST, and check it against `expected`. Asserts
// there are no diagnostics. Returns on success; aborts with a diff on failure.
void check(const char* src, const char* expected) {
    arena a = arena_create(0);
    Interner in;
    intern_init(&in, &a);
    Diag d{};

    uint32_t fid = diag_add_file(&d, "test.dg", src, std::strlen(src));
    Lexer lx;
    lexer_init(&lx, src, std::strlen(src), fid, &in, &d);
    Vec<Token> toks{};
    lex_collect(&lx, &toks);
    lexer_free(&lx);

    Module* m = parse_module(toks.data, static_cast<uint32_t>(toks.len), &a, &in, &d);

    Vec<char> out{};
    ast_dump_module(m, &in, &out);

    if (diag_has_errors(&d)) {
        std::fprintf(stderr, "unexpected diagnostics for: %s\n", src);
        diag_flush(&d);
        assert(false);
    }
    if (std::strcmp(out.data, expected) != 0) {
        std::fprintf(stderr, "AST mismatch\n  src:      %s\n  expected: %s\n  got:      %s\n",
                     src, expected, out.data);
        assert(false);
    }

    out.free();
    toks.free();
    diag_free(&d);
    intern_free(&in);
    arena_destroy(&a);
}

// Parse `src` and assert that at least one diagnostic was produced.
void check_fails(const char* src) {
    arena a = arena_create(0);
    Interner in;
    intern_init(&in, &a);
    Diag d{};

    uint32_t fid = diag_add_file(&d, "test.dg", src, std::strlen(src));
    Lexer lx;
    lexer_init(&lx, src, std::strlen(src), fid, &in, &d);
    Vec<Token> toks{};
    lex_collect(&lx, &toks);
    lexer_free(&lx);

    Module* m = parse_module(toks.data, static_cast<uint32_t>(toks.len), &a, &in, &d);
    (void)m;
    assert(diag_has_errors(&d));

    toks.free();
    diag_free(&d);
    intern_free(&in);
    arena_destroy(&a);
}

} // namespace

int main() {
    // Empty function.
    check("func main() {}",
          "(func main (params) (ret _) (block (tail)))");

    // Params, return type, a tail expression.
    check("func add(a: i32, b: i32) -> i32 { a + b }",
          "(func add (params (param a i32) (param b i32)) (ret i32) "
          "(block (tail (+ a b))))");

    // Operator precedence: * binds tighter than +, both left-assoc.
    check("func f() { 1 + 2 * 3 - 4 }",
          "(func f (params) (ret _) (block (tail (- (+ #1 (* #2 #3)) #4))))");

    // Comparison vs arithmetic vs logical precedence.
    check("func f() { a + b == c && d }",
          "(func f (params) (ret _) (block (tail (&& (== (+ a b) c) d))))");

    // Assignment is right-associative and lowest.
    check("func f() { a = b = c }",
          "(func f (params) (ret _) (block (tail (= a (= b c)))))");

    // Unary minus and grouping.
    check("func f() { -(a + b) }",
          "(func f (params) (ret _) (block (tail (unary - (+ a b)))))");

    // Calls, including nested args.
    check("func f() { g(1, h(2), 3) }",
          "(func f (params) (ret _) (block (tail (call g #1 (call h #2) #3))))");

    // let / var bindings and an expression statement, then a tail.
    check("func f() {\n"
          "    let x = 1\n"
          "    var y: i32 = x + 2\n"
          "    g(y)\n"
          "    y\n"
          "}",
          "(func f (params) (ret _) (block "
          "(let x (ty _) #1) "
          "(var y (ty i32) (+ x #2)) "
          "(exprstmt (call g y)) "
          "(tail y)))");

    // return statements (with and without a value).
    check("func f() -> i32 {\n"
          "    return 1 + 2\n"
          "}",
          "(func f (params) (ret i32) (block (return (+ #1 #2)) (tail)))");

    // pub + string with interpolation as a concat tree.
    check("pub func greet(name: str) {\n"
          "    print(\"hi ${name}!\")\n"
          "}",
          "(func pub greet (params (param name str)) (ret _) (block "
          "(tail (call print (str \"hi \" (interp name) \"!\")))))");

    // Multiple top-level functions.
    check("func a() {}\nfunc b() {}",
          "(func a (params) (ret _) (block (tail)))\n"
          "(func b (params) (ret _) (block (tail)))");

    // if / else as an expression.
    check("func f() -> i32 { if a { 1 } else { 2 } }",
          "(func f (params) (ret i32) (block (tail "
          "(if a (block (tail #1)) (block (tail #2))))))");

    // else-if chains nest as an else branch.
    check("func f() -> i32 { if a { 1 } else if b { 2 } else { 3 } }",
          "(func f (params) (ret i32) (block (tail "
          "(if a (block (tail #1)) (if b (block (tail #2)) (block (tail #3)))))))");

    // while loop with a call in the body.
    check("func f() {\n"
          "    while a {\n"
          "        g()\n"
          "    }\n"
          "}",
          "(func f (params) (ret _) (block (tail "
          "(while a (block (tail (call g)))))))");

    // Struct type declaration.
    check("type Point { x: f64, y: f64 }",
          "(struct Point (field x f64) (field y f64))");

    // Struct literal and field access.
    check("func f() -> i32 {\n"
          "    let p = Point { x: 1, y: 2 }\n"
          "    p.x\n"
          "}",
          "(func f (params) (ret i32) (block "
          "(let p (ty _) (new Point (init x #1) (init y #2))) "
          "(tail (get p x))))");

    // Chained field access.
    check("func f() { a.b.c }",
          "(func f (params) (ret _) (block (tail (get (get a b) c))))");

    // Enum declaration (fieldless variants).
    check("type Color = Red | Green | Blue",
          "(enum Color Red Green Blue)");

    // Leading-bar, multi-line enum form.
    check("type Shape =\n    | Circle\n    | Square\n",
          "(enum Shape Circle Square)");

    // match expression with a variant arm and a wildcard.
    check("func f(c: Color) -> i32 {\n"
          "    match c {\n"
          "        Color.Red => 1,\n"
          "        _ => 0,\n"
          "    }\n"
          "}",
          "(func f (params (param c Color)) (ret i32) (block (tail "
          "(match c (arm Color.Red #1) (arm _ #0)))))");

    // A bare identifier in an if-condition is NOT a struct literal.
    check("func f() -> i32 { if cond { 1 } else { 2 } }",
          "(func f (params) (ret i32) (block (tail "
          "(if cond (block (tail #1)) (block (tail #2))))))");

    // Enum with payload variants (tuple-style).
    check("type IntOpt = Some(i32) | None",
          "(enum IntOpt (Some i32) None)");

    // Pattern binding the payload.
    check("func f(o: IntOpt) -> i32 { match o { IntOpt.Some(x) => x, IntOpt.None => 0 } }",
          "(func f (params (param o IntOpt)) (ret i32) (block (tail "
          "(match o (arm IntOpt.Some(x) x) (arm IntOpt.None #0)))))");

    // Borrows: ref / ref mut in types and expressions.
    check("func sum(p: ref Point) -> i32 { p.x }",
          "(func sum (params (param p (ref Point))) (ret i32) (block (tail (get p x))))");
    check("func main() -> i32 { let r = ref x\n 0 }",
          "(func main (params) (ret i32) (block "
          "(let r (ty _) (ref x)) (tail #0)))");
    check("func main() { var v = 0\n let r = ref mut v }",
          "(func main (params) (ret _) (block "
          "(var v (ty _) #0) (let r (ty _) (ref-mut v)) (tail)))");

    // Error recovery: a bad item is reported, the next still parses.
    check_fails("func 123() {}");
    check_fails("func f() { let = 5 }");
    check_fails("func f() { 1 + }");
    check_fails("func f( {}");

    std::printf("parse_test: OK\n");
    return 0;
}

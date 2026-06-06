#include "diag.h"

#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    // Lines:        1:"ab"  2:"cde"  3:"" (after trailing newline)
    const char* src = "ab\ncde\nfg";
    Diag d{};
    uint32_t fid = diag_add_file(&d, "test.dg", src, std::strlen(src));
    assert(fid == 0);

    const SourceFile* f = &d.files[0];

    // Offset 0 -> 'a' at line 1 col 1.
    LineCol lc = diag_line_col(f, 0);
    assert(lc.line == 1 && lc.col == 1);

    // Offset 3 -> 'c' at line 2 col 1.
    lc = diag_line_col(f, 3);
    assert(lc.line == 2 && lc.col == 1);

    // Offset 5 -> 'e' at line 2 col 3.
    lc = diag_line_col(f, 5);
    assert(lc.line == 2 && lc.col == 3);

    // Offset 7 -> 'f' at line 3 col 1.
    lc = diag_line_col(f, 7);
    assert(lc.line == 3 && lc.col == 1);

    assert(!diag_has_errors(&d));

    diag_error(&d, Span{fid, 3, 6}, "unexpected token");
    diag_errorf(&d, Span{fid, 0, 2}, "expected %s, got %s", "';'", "identifier");
    diag_warning(&d, Span{fid, 7, 9}, "unused binding");

    assert(diag_has_errors(&d));
    assert(d.error_count == 2);
    assert(d.diags.len == 3);

    // Render to stderr (sorted by location: offset 0, then 3, then 7).
    std::fprintf(stderr, "--- diag_flush sample output ---\n");
    diag_flush(&d);
    std::fprintf(stderr, "--- end sample ---\n");

    diag_free(&d);
    assert(d.diags.data == nullptr && d.files.data == nullptr);

    std::printf("diag_test: OK\n");
    return 0;
}

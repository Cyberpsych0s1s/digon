#include "build.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "arena.h"
#include "diag.h"
#include "fmt.h"
#include "intern.h"
#include "lex.h"
#include "link.h"
#include "llvm_emit.h"
#include "lower.h"
#include "parse.h"
#include "types.h"
#include "vec.h"

namespace {

char* read_file(const char* path, size_t* out_len) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return nullptr;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    if (n < 0) { std::fclose(f); return nullptr; }
    std::fseek(f, 0, SEEK_SET);
    char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(n) + 1));
    if (!buf) { std::fclose(f); return nullptr; }
    size_t got = std::fread(buf, 1, static_cast<size_t>(n), f);
    buf[got] = '\0';
    std::fclose(f);
    *out_len = got;
    return buf;
}

// Replace a trailing ".dg" with ".exe" (or append ".exe").
std::string exe_path_for(const char* src_path) {
    std::string s = src_path;
    size_t      dot = s.find_last_of('.');
    size_t      sep = s.find_last_of("/\\");
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) {
        s.erase(dot);
    }
    s += ".exe";
    return s;
}

} // namespace

int build_file(const char* src_path, const char* out_exe, bool release) {
    size_t len = 0;
    char*  src = read_file(src_path, &len);
    if (!src) {
        std::fprintf(stderr, "digon: cannot read '%s'\n", src_path);
        return 1;
    }

    arena    a = arena_create(0);
    Interner in;
    intern_init(&in, &a);
    Diag d{};

    uint32_t fid = diag_add_file(&d, src_path, src, len);
    Lexer    lx;
    lexer_init(&lx, src, len, fid, &in, &d);
    Vec<Token> toks{};
    lex_collect(&lx, &toks);
    lexer_free(&lx);

    Module* m = parse_module(toks.data, static_cast<uint32_t>(toks.len), &a, &in, &d);

    int rc = 0;
    if (diag_has_errors(&d)) {
        rc = 1; // parse errors
    } else if (!check_module(m, &a, &in, &d)) {
        rc = 1; // type errors
    } else if (MModule* mm = lower_module(m, &a, &in, &d); diag_has_errors(&d)) {
        (void)mm;
        rc = 1; // unsupported-construct errors from lowering
    } else {
        std::string obj = std::string(out_exe) + ".o";
        if (!emit_object(mm, &in, obj.c_str(), release ? OptLevel::O2 : OptLevel::O0, &d)) {
            rc = 2;
        } else if (!link_executable(obj.c_str(), out_exe, &d)) {
            rc = 2;
        }
        std::remove(obj.c_str());
    }
    // Always flush: emits errors AND warnings (e.g. unreachable match arms),
    // which the compiler can produce even on an otherwise successful build.
    diag_flush(&d);

    toks.free();
    diag_free(&d);
    intern_free(&in);
    arena_destroy(&a);
    std::free(src);
    return rc;
}

int run_file(const char* src_path, bool release) {
    std::string exe = exe_path_for(src_path);
    int         rc  = build_file(src_path, exe.c_str(), release);
    if (rc != 0) return rc;

    // cmd.exe treats '/' as an option separator, so run with native separators.
    std::string native = exe;
#ifdef _WIN32
    for (char& c : native) if (c == '/') c = '\\';
#endif
    std::string cmd = "\"";
    cmd += native;
    cmd += "\"";
    int child = std::system(cmd.c_str());

    std::remove(exe.c_str());
    return child;
}

int format_file(const char* src_path) {
    size_t len = 0;
    char*  src = read_file(src_path, &len);
    if (!src) {
        std::fprintf(stderr, "digon: cannot read '%s'\n", src_path);
        return 1;
    }

    arena    a = arena_create(0);
    Interner in;
    intern_init(&in, &a);
    Diag d{};

    uint32_t fid = diag_add_file(&d, src_path, src, len);
    Lexer    lx;
    lexer_init(&lx, src, len, fid, &in, &d);
    Vec<Token> toks{};
    lex_collect(&lx, &toks);
    lexer_free(&lx);

    Module* m = parse_module(toks.data, static_cast<uint32_t>(toks.len), &a, &in, &d);

    int rc = 0;
    if (diag_has_errors(&d)) {
        diag_flush(&d);
        rc = 1;
    } else {
        Vec<char> out{};
        format_module(m, &in, &out);
        if (out.len > 1) std::fwrite(out.data, 1, out.len - 1, stdout); // drop trailing NUL
        out.free();
    }

    toks.free();
    diag_free(&d);
    intern_free(&in);
    arena_destroy(&a);
    std::free(src);
    return rc;
}

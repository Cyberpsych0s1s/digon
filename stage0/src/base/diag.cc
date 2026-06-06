#include "diag.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const char* severity_label(Severity s) {
    switch (s) {
        case Severity::Error:   return "error";
        case Severity::Warning: return "warning";
        case Severity::Note:    return "note";
    }
    return "error";
}

void add(Diag* d, Severity sev, Span span, const char* message) {
    Diagnostic dg;
    dg.severity = sev;
    dg.span     = span;
    dg.message  = message;
    d->diags.push(dg);
    if (sev == Severity::Error) d->error_count++;
}

} // namespace

uint32_t diag_add_file(Diag* d, const char* path, const char* text, size_t len) {
    SourceFile f;
    f.path = path;
    f.text = text;
    f.len  = len;
    f.line_starts = Vec<uint32_t>{};
    f.line_starts.push(0);
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') f.line_starts.push(static_cast<uint32_t>(i + 1));
    }
    d->files.push(f); // bitwise-copies line_starts pointer; ownership moves to the slot
    return static_cast<uint32_t>(d->files.len - 1);
}

void diag_error(Diag* d, Span span, const char* message) {
    add(d, Severity::Error, span, message);
}
void diag_warning(Diag* d, Span span, const char* message) {
    add(d, Severity::Warning, span, message);
}
void diag_note(Diag* d, Span span, const char* message) {
    add(d, Severity::Note, span, message);
}

void diag_errorf(Diag* d, Span span, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(n) + 1));
    if (!buf) {
        std::fprintf(stderr, "Fatal: diag_errorf out of memory.\n");
        std::exit(EXIT_FAILURE);
    }
    std::vsnprintf(buf, static_cast<size_t>(n) + 1, fmt, ap2);
    va_end(ap2);
    d->owned_msgs.push(buf);
    add(d, Severity::Error, span, buf);
}

bool diag_has_errors(const Diag* d) {
    return d->error_count > 0;
}

LineCol diag_line_col(const SourceFile* f, uint32_t offset) {
    // Largest i with line_starts[i] <= offset (binary search).
    const Vec<uint32_t>& ls = f->line_starts;
    size_t lo = 0, hi = ls.len; // hi exclusive
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ls[mid] <= offset) lo = mid;
        else                   hi = mid;
    }
    LineCol lc;
    lc.line = static_cast<uint32_t>(lo + 1);
    lc.col  = offset - ls[lo] + 1;
    return lc;
}

void diag_flush(Diag* d) {
    std::sort(d->diags.begin(), d->diags.end(),
              [](const Diagnostic& a, const Diagnostic& b) {
                  if (a.span.file_id != b.span.file_id) return a.span.file_id < b.span.file_id;
                  return a.span.start < b.span.start;
              });

    for (const Diagnostic& dg : d->diags) {
        const SourceFile* f =
            (dg.span.file_id < d->files.len) ? &d->files[dg.span.file_id] : nullptr;
        if (!f) {
            std::fprintf(stderr, "%s: %s\n", severity_label(dg.severity), dg.message);
            continue;
        }
        LineCol lc = diag_line_col(f, dg.span.start);
        std::fprintf(stderr, "%s:%u:%u: %s: %s\n",
                     f->path, lc.line, lc.col, severity_label(dg.severity), dg.message);

        // Source line + caret underline.
        uint32_t line_start = f->line_starts[lc.line - 1];
        uint32_t line_end   = line_start;
        while (line_end < f->len && f->text[line_end] != '\n') line_end++;
        std::fprintf(stderr, "  %.*s\n",
                     static_cast<int>(line_end - line_start), f->text + line_start);

        std::fprintf(stderr, "  ");
        for (uint32_t i = 1; i < lc.col; i++) std::fputc(' ', stderr);
        uint32_t carets = (dg.span.end > dg.span.start) ? (dg.span.end - dg.span.start) : 1;
        if (dg.span.start + carets > line_end) carets = line_end - dg.span.start;
        if (carets == 0) carets = 1;
        for (uint32_t i = 0; i < carets; i++) std::fputc('^', stderr);
        std::fputc('\n', stderr);
    }
}

void diag_free(Diag* d) {
    for (SourceFile& f : d->files) f.line_starts.free();
    for (char* m : d->owned_msgs) std::free(m);
    d->files.free();
    d->diags.free();
    d->owned_msgs.free();
    d->error_count = 0;
}

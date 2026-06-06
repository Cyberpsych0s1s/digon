#ifndef DIGON_DIAG_H
#define DIGON_DIAG_H

#include <cstddef>
#include <cstdint>

#include "vec.h"

// A half-open byte range [start, end) within a registered source file.
struct Span {
    uint32_t file_id;
    uint32_t start;
    uint32_t end;
};

enum class Severity : uint8_t { Error, Warning, Note };

struct Diagnostic {
    Severity    severity;
    Span        span;
    const char* message; // owned by the Diag (see diag_error / diag_errorf)
};

struct SourceFile {
    const char*   path;
    const char*   text;        // NUL-terminated; CRLF normalised, BOM rejected, on read
    size_t        len;
    Vec<uint32_t> line_starts; // byte offset of the first char of each line
};

struct Diag {
    Vec<SourceFile> files;
    Vec<Diagnostic> diags;
    Vec<char*>      owned_msgs; // heap strings from diag_errorf, freed in diag_free
    size_t          error_count = 0;
};

struct LineCol {
    uint32_t line; // 1-based
    uint32_t col;  // 1-based, in bytes
};

// Register a source file; returns its file_id. `text` must outlive the Diag.
uint32_t diag_add_file(Diag* d, const char* path, const char* text, size_t len);

// Static-message diagnostics (message must outlive the Diag — literal/arena).
void diag_error(Diag* d, Span span, const char* message);
void diag_warning(Diag* d, Span span, const char* message);
void diag_note(Diag* d, Span span, const char* message);

// printf-style; the formatted string is heap-allocated and owned by the Diag.
void diag_errorf(Diag* d, Span span, const char* fmt, ...);

bool    diag_has_errors(const Diag* d);
LineCol diag_line_col(const SourceFile* f, uint32_t offset);
void    diag_flush(Diag* d); // sort by (file, start) and render to stderr
void    diag_free(Diag* d);

#endif

#ifndef DIGON_FMT_H
#define DIGON_FMT_H

#include "ast.h"
#include "intern.h"
#include "vec.h"

// Append the canonical formatting of `m` to `out` (NUL-terminated). One fixed
// style, no options (gofmt-style). fmt(fmt(x)) == fmt(x).
void format_module(const Module* m, const Interner* in, Vec<char>* out);

#endif

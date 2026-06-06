#ifndef DIGON_PARSE_H
#define DIGON_PARSE_H

#include <cstdint>

#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "intern.h"
#include "lex.h"

// Parse a token stream (terminated by an Eof token) into a Module. Nodes are
// allocated from `a`. Syntax errors are reported through `diag`; parsing
// continues with panic-mode recovery, so a Module is always returned.
Module* parse_module(Token* toks, uint32_t ntoks, arena* a, Interner* in, Diag* diag);

#endif

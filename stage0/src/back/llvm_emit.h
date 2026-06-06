#ifndef DIGON_LLVM_EMIT_H
#define DIGON_LLVM_EMIT_H

#include "diag.h"
#include "intern.h"
#include "lower.h"

// Intentionally LLVM-free so the rest of the toolchain can call codegen without
// pulling in LLVM. All LLVM contact is confined to the .cc.
enum class OptLevel { O0, O2 };

// Emit `mod` to a native object file at `obj_path`. Returns true on success;
// reports failures through `diag`. `in` resolves interned string contents.
bool emit_object(const MModule* mod, const Interner* in, const char* obj_path,
                 OptLevel opt, Diag* diag);

#endif

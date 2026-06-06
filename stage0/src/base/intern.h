#ifndef DIGON_INTERN_H
#define DIGON_INTERN_H

#include <cstddef>
#include <cstdint>

#include "arena.h"
#include "vec.h"

// String interner: identical byte sequences map to the same u32 id.
// Id 0 is reserved as "no symbol" / invalid. String bytes are bump-allocated
// from the borrowed compilation arena, so pointers returned by intern_str stay
// valid for the whole compilation. Hash collisions resolve by byte comparison.
struct InternEntry {
    const char* ptr;   // arena-owned, NUL-terminated for convenience
    uint32_t    len;
};

struct Interner {
    arena*           a          = nullptr;  // borrowed, not owned
    Vec<InternEntry> entries;               // indexed by (id - 1)
    uint32_t*        table      = nullptr;   // open addressing: slot -> id (0 empty)
    size_t           table_cap  = 0;         // 0 or a power of two
};

void        intern_init(Interner* in, arena* a);
void        intern_free(Interner* in);                 // frees table + entries, NOT the arena
uint32_t    intern(Interner* in, const char* s, size_t len); // returns id >= 1
uint32_t    intern_cstr(Interner* in, const char* s);        // convenience for NUL-terminated
const char* intern_str(const Interner* in, uint32_t id, size_t* out_len); // nullptr on bad id

#endif

#include "intern.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

uint64_t fnv1a(const char* s, size_t len) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= static_cast<unsigned char>(s[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

bool entry_matches(const InternEntry& e, const char* s, size_t len) {
    return e.len == len && std::memcmp(e.ptr, s, len) == 0;
}

// Rehash all existing ids into a fresh table of the given capacity.
void rehash(Interner* in, size_t newcap) {
    uint32_t* table = static_cast<uint32_t*>(std::calloc(newcap, sizeof(uint32_t)));
    if (!table) {
        std::fprintf(stderr, "Fatal: Interner out of memory (table %zu).\n", newcap);
        std::exit(EXIT_FAILURE);
    }
    size_t mask = newcap - 1;
    for (size_t e = 0; e < in->entries.len; e++) {
        const InternEntry& ent = in->entries[e];
        uint64_t h = fnv1a(ent.ptr, ent.len);
        size_t   i = h & mask;
        while (table[i]) i = (i + 1) & mask;
        table[i] = static_cast<uint32_t>(e + 1); // store id (1-based)
    }
    std::free(in->table);
    in->table     = table;
    in->table_cap = newcap;
}

} // namespace

void intern_init(Interner* in, arena* a) {
    in->a         = a;
    in->entries   = Vec<InternEntry>{};
    in->table     = nullptr;
    in->table_cap = 0;
    rehash(in, 16);
}

void intern_free(Interner* in) {
    std::free(in->table);
    in->entries.free();
    in->table     = nullptr;
    in->table_cap = 0;
    in->a         = nullptr;
}

uint32_t intern(Interner* in, const char* s, size_t len) {
    // Grow the table at 75% load before inserting.
    if ((in->entries.len + 1) * 4 >= in->table_cap * 3) {
        rehash(in, in->table_cap * 2);
    }

    size_t   mask = in->table_cap - 1;
    uint64_t h    = fnv1a(s, len);
    size_t   i    = h & mask;
    while (in->table[i]) {
        uint32_t id = in->table[i];
        if (entry_matches(in->entries[id - 1], s, len)) {
            return id; // already interned
        }
        i = (i + 1) & mask;
    }

    // Not present: copy bytes into the arena (NUL-terminated), append entry.
    char* dst = static_cast<char*>(arena_alloc_aligned(in->a, len + 1, 1));
    std::memcpy(dst, s, len);
    dst[len] = '\0';

    InternEntry ent;
    ent.ptr = dst;
    ent.len = static_cast<uint32_t>(len);
    in->entries.push(ent);

    uint32_t id  = static_cast<uint32_t>(in->entries.len); // 1-based
    in->table[i] = id;
    return id;
}

uint32_t intern_cstr(Interner* in, const char* s) {
    return intern(in, s, std::strlen(s));
}

const char* intern_str(const Interner* in, uint32_t id, size_t* out_len) {
    if (id == 0 || id > in->entries.len) {
        if (out_len) *out_len = 0;
        return nullptr;
    }
    const InternEntry& e = in->entries[id - 1];
    if (out_len) *out_len = e.len;
    return e.ptr;
}

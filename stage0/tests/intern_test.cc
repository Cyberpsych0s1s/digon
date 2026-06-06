#include "arena.h"
#include "intern.h"

#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    arena a = arena_create(0);
    Interner in;
    intern_init(&in, &a);

    uint32_t foo  = intern_cstr(&in, "foo");
    uint32_t bar  = intern_cstr(&in, "bar");
    uint32_t foo2 = intern_cstr(&in, "foo");

    assert(foo >= 1);
    assert(foo == foo2);   // same bytes -> same id
    assert(foo != bar);    // different bytes -> different id

    // Round-trip back to bytes.
    size_t      len = 0;
    const char* s   = intern_str(&in, foo, &len);
    assert(len == 3 && std::memcmp(s, "foo", 3) == 0);
    assert(s[len] == '\0'); // NUL-terminated for convenience

    // Substring keys must not collide (length is part of identity).
    uint32_t ab  = intern(&in, "ab", 2);
    uint32_t abc = intern(&in, "abc", 3);
    assert(ab != abc);

    // Stress: many ids force table growth; pointers stay valid (arena-backed).
    char buf[16];
    uint32_t ids[500];
    for (int i = 0; i < 500; i++) {
        std::snprintf(buf, sizeof(buf), "sym%d", i);
        ids[i] = intern_cstr(&in, buf);
    }
    for (int i = 0; i < 500; i++) {
        std::snprintf(buf, sizeof(buf), "sym%d", i);
        assert(ids[i] == intern_cstr(&in, buf)); // re-intern yields the same id
        size_t      l = 0;
        const char* p = intern_str(&in, ids[i], &l);
        assert(std::strcmp(p, buf) == 0);
    }

    // Bad id handling.
    assert(intern_str(&in, 0, nullptr) == nullptr);
    assert(intern_str(&in, 999999, nullptr) == nullptr);

    intern_free(&in);
    arena_destroy(&a);

    std::printf("intern_test: OK\n");
    return 0;
}

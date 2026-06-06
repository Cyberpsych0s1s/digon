#include "map.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

int main() {
    Map m{};

    // Absent lookups on an empty map.
    assert(!map_contains(&m, 0));
    assert(map_get(&m, 42) == nullptr);

    // Key 0 must be storable (parallel state byte, not a sentinel key).
    int marker = 7;
    map_put(&m, 0, &marker);
    assert(map_contains(&m, 0));
    assert(map_get(&m, 0) == &marker);

    // Many keys force several growths; values are stable pointers into `vals`.
    static int vals[2000];
    for (int i = 0; i < 2000; i++) {
        vals[i] = i;
        map_put(&m, static_cast<uint64_t>(i + 1), &vals[i]);
    }
    assert(m.count == 2001); // 2000 + the key-0 entry

    for (int i = 0; i < 2000; i++) {
        void* got = map_get(&m, static_cast<uint64_t>(i + 1));
        assert(got == &vals[i]);
        assert(*static_cast<int*>(got) == i);
    }

    // Overwrite semantics.
    int other = 99;
    map_put(&m, 1, &other);
    assert(map_get(&m, 1) == &other);
    assert(m.count == 2001); // overwrite, not insert

    // map_lookup distinguishes "present with null value" from "absent".
    map_put(&m, 12345, nullptr);
    void* out = &marker;
    assert(map_lookup(&m, 12345, &out) && out == nullptr);
    assert(!map_lookup(&m, 999999, &out));

    map_free(&m);
    assert(m.slots == nullptr && m.capacity == 0);

    std::printf("map_test: OK\n");
    return 0;
}

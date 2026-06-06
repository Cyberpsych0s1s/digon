#include "arena.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

struct TestStruct {
    int a, b, c, d;
};

int main() {
    // 1. Create arena and confirm default chunk size
    arena a = arena_create(0);
    std::printf("1. Default chunk size: %zu (Expected: %d)\n",
                a.default_chunk_size, ARENA_DEFAULT_CHUNK_SIZE);

    // 2. Allocate int
    int* p_int = ARENA_NEW(&a, int);
    std::printf("2. int addr:      %p (Aligned to %zu: %s)\n",
                static_cast<void*>(p_int), alignof(int),
                (reinterpret_cast<uintptr_t>(p_int) % alignof(int)) == 0 ? "YES" : "NO");

    // 3. Allocate double
    double* p_double = ARENA_NEW(&a, double);
    std::printf("3. double addr:   %p (Aligned to %zu: %s)\n",
                static_cast<void*>(p_double), alignof(double),
                (reinterpret_cast<uintptr_t>(p_double) % alignof(double)) == 0 ? "YES" : "NO");

    // 4. Allocate char immediately after double
    char* p_char = ARENA_NEW(&a, char);
    char* expected_char_addr = reinterpret_cast<char*>(p_double) + sizeof(double);
    std::printf("4. char addr:     %p (Expected: %p, Match: %s)\n",
                static_cast<void*>(p_char), static_cast<void*>(expected_char_addr),
                p_char == expected_char_addr ? "YES" : "NO");

    // 5. Allocate double after char (forces padding)
    double* p_double2 = ARENA_NEW(&a, double);
    std::printf("5. double2 addr:  %p (Aligned to %zu: %s, Padding applied: %s)\n",
                static_cast<void*>(p_double2), alignof(double),
                (reinterpret_cast<uintptr_t>(p_double2) % alignof(double)) == 0 ? "YES" : "NO",
                reinterpret_cast<uintptr_t>(p_double2) > (reinterpret_cast<uintptr_t>(p_char) + sizeof(char))
                    ? "YES" : "NO");

    // 6. Loop ~10,000 small things to force a chunk boundary
    // 10,000 doubles = 80,000 bytes > 65,536 bytes (default chunk)
    size_t  initial_chunks         = a.nchunks;
    double* last_ptr               = nullptr;
    bool    distinct_and_aligned   = true;

    for (int i = 0; i < 10000; i++) {
        double* p = ARENA_NEW(&a, double);
        if (last_ptr && p == last_ptr) {
            distinct_and_aligned = false;
        }
        if ((reinterpret_cast<uintptr_t>(p) % alignof(double)) != 0) {
            distinct_and_aligned = false;
        }
        last_ptr = p;
    }

    std::printf("6. Looped 10k.    Chunks grew: %zu -> %zu (Valid/Distinct pointers: %s)\n",
                initial_chunks, a.nchunks, distinct_and_aligned ? "YES" : "NO");

    // 7. Force an oversize allocation
    size_t large_req = a.default_chunk_size * 2;
    void*  p_large   = arena_alloc_aligned(&a, large_req, 16);
    std::printf("7. Oversize alloc:(%zu bytes) Address: %p\n", large_req, p_large);

    // 8. ARENA_NEW_ZERO test
    // We allocate a buffer, dirty it with 0xFF, then allocate zeroed memory
    // to prove the arena doesn't accidentally hand back dirty bytes.
    char* padding = static_cast<char*>(arena_alloc_aligned(&a, 128, 1));
    std::memset(padding, 0xFF, 128);

    TestStruct*    p_zero   = ARENA_NEW_ZERO(&a, TestStruct);
    bool           all_zero = true;
    unsigned char* zbytes   = reinterpret_cast<unsigned char*>(p_zero);
    for (size_t i = 0; i < sizeof(TestStruct); i++) {
        if (zbytes[i] != 0) all_zero = false;
    }
    std::printf("8. Zero alloc:    All %zu bytes are exactly 0: %s\n",
                sizeof(TestStruct), all_zero ? "YES" : "NO");

    // 9. Final Stats
    std::printf("\n--- Final Stats ---\n");
    std::printf("nchunks:         %zu\n", a.nchunks);
    std::printf("nalloc:          %zu\n", a.nalloc);
    std::printf("bytes_requested: %zu\n", a.bytes_requested);
    std::printf("bytes_wasted:    %zu\n", a.bytes_wasted);

    // 10. Destroy
    arena_destroy(&a);
    std::printf("10. Arena destroyed.\n");

    return 0;
}

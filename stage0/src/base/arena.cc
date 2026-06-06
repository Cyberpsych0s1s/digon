#include "arena.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// head points to the oldest chunk. next walks oldest -> newest.
// Invariant: arena->current always points to the tail of this linked list.
// Flexible array member is a GCC/Clang extension in C++; ISO C99+ but not C++.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
struct chunk {
    chunk*        next;
    size_t        capacity;
    size_t        used;
    unsigned char data[];
};
#pragma GCC diagnostic pop

static inline uintptr_t align_forward(uintptr_t ptr, size_t align) {
    assert(align > 0 && (align & (align - 1)) == 0); // Must be non-zero power of 2
    return (ptr + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
}

arena arena_create(size_t hint) {
    arena a{};
    a.default_chunk_size = (hint > 0) ? hint : ARENA_DEFAULT_CHUNK_SIZE;
    return a;
}

void arena_destroy(arena* a) {
    if (!a) return;

    chunk* curr = a->head;
    while (curr != nullptr) {
        chunk* next = curr->next;
        std::free(curr);
        curr = next;
    }

    std::memset(a, 0, sizeof(*a));
}

void* arena_alloc_aligned(arena* a, size_t size, size_t align) {
    // size == 0 is safe: 'used' advances only by alignment padding, returning
    // a valid aligned pointer (malloc(0) semantics).

    chunk*    curr        = a->current;
    uintptr_t curr_ptr    = 0;
    uintptr_t aligned_ptr = 0;
    size_t    padding     = 0;

    if (curr != nullptr) {
        curr_ptr    = reinterpret_cast<uintptr_t>(curr->data) + curr->used;
        aligned_ptr = align_forward(curr_ptr, align);
        padding     = aligned_ptr - curr_ptr;

        // Overflow guard for chunk space calculation
        if (size > SIZE_MAX - padding) {
            std::fprintf(stderr, "Fatal: Arena integer overflow calculating required size.\n");
            std::exit(EXIT_FAILURE);
        }

        if (curr->used + padding + size <= curr->capacity) {
            curr->used += padding + size;

            a->bytes_requested += size;
            a->bytes_wasted    += padding;
            a->nalloc++;

            return reinterpret_cast<void*>(aligned_ptr);
        }
    }

    // Overflow guard for new capacity
    if (size > SIZE_MAX - align) {
        std::fprintf(stderr, "Fatal: Arena integer overflow calculating minimum capacity.\n");
        std::exit(EXIT_FAILURE);
    }

    size_t min_capacity = size + align;
    size_t capacity     = (min_capacity > a->default_chunk_size) ? min_capacity : a->default_chunk_size;

    // Overflow guard for malloc size
    if (capacity > SIZE_MAX - sizeof(chunk)) {
        std::fprintf(stderr, "Fatal: Arena integer overflow calculating malloc size.\n");
        std::exit(EXIT_FAILURE);
    }

    chunk* new_chunk = static_cast<chunk*>(std::malloc(sizeof(chunk) + capacity));
    if (!new_chunk) {
        std::fprintf(stderr, "Fatal: Arena out of memory. Requested %zu bytes.\n",
                     sizeof(chunk) + capacity);
        std::exit(EXIT_FAILURE);
    }

    new_chunk->next     = nullptr;
    new_chunk->capacity = capacity;
    new_chunk->used     = 0;

    if (a->current != nullptr) {
        a->current->next = new_chunk;
    } else {
        a->head = new_chunk;
    }
    a->current = new_chunk;
    a->nchunks++;

    curr_ptr    = reinterpret_cast<uintptr_t>(new_chunk->data);
    aligned_ptr = align_forward(curr_ptr, align);
    padding     = aligned_ptr - curr_ptr;

    new_chunk->used = padding + size;

    a->bytes_requested += size;
    a->bytes_wasted    += padding;
    a->nalloc++;

    return reinterpret_cast<void*>(aligned_ptr);
}

void* arena_alloc_aligned_zero(arena* a, size_t size, size_t align) {
    void* ptr = arena_alloc_aligned(a, size, align);
    if (ptr && size > 0) {
        std::memset(ptr, 0, size);
    }
    return ptr;
}

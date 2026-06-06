#ifndef DIGON_ARENA_H
#define DIGON_ARENA_H

#include <cstddef>

#define ARENA_DEFAULT_CHUNK_SIZE (64 * 1024)

#define ARENA_NEW(a, T) \
    (static_cast<T*>(arena_alloc_aligned((a), sizeof(T), alignof(T))))

#define ARENA_NEW_N(a, T, n) \
    (static_cast<T*>(arena_alloc_aligned((a), (n) * sizeof(T), alignof(T))))

#define ARENA_NEW_ZERO(a, T) \
    (static_cast<T*>(arena_alloc_aligned_zero((a), sizeof(T), alignof(T))))

#define ARENA_NEW_N_ZERO(a, T, n) \
    (static_cast<T*>(arena_alloc_aligned_zero((a), (n) * sizeof(T), alignof(T))))

struct chunk;

struct arena {
    chunk*  head;
    chunk*  current;
    size_t  default_chunk_size; // default: 64KB
    size_t  nchunks;
    size_t  nalloc;
    size_t  bytes_requested;
    size_t  bytes_wasted;       // due to alignment padding
};

arena arena_create(size_t hint);
void  arena_destroy(arena* a);

void* arena_alloc_aligned(arena* a, size_t size, size_t align);
void* arena_alloc_aligned_zero(arena* a, size_t size, size_t align);

#endif

#include "vec.h"

#include <cstdio>
#include <cstdlib>

// 1.5x geometric growth, with a floor of 4 and a hard jump to `needed`
// when 1.5x would not reach it (large single reservations).
size_t vec_next_capacity(size_t current, size_t needed) {
    size_t cap = current ? current : 4;
    while (cap < needed) {
        size_t grown = cap + cap / 2;
        cap = (grown > cap) ? grown : needed; // overflow / no-progress guard
        if (cap < needed) cap = needed;
    }
    return cap;
}

void vec_oom(size_t bytes) {
    std::fprintf(stderr, "Fatal: Vec out of memory (requested %zu bytes).\n", bytes);
    std::exit(EXIT_FAILURE);
}

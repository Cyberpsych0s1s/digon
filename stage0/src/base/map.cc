#include "map.h"

#include <cstdio>
#include <cstdlib>

namespace {

// splitmix64 finaliser — strong avalanche, cheap, good for sequential keys
// (interned ids, pointers cast to u64).
uint64_t hash_u64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

bool find_slot(const Map* m, uint64_t key, size_t* idx) {
    if (m->capacity == 0) return false;
    size_t mask = m->capacity - 1;
    size_t i    = hash_u64(key) & mask;
    while (m->state[i]) {
        if (m->slots[i].key == key) {
            *idx = i;
            return true;
        }
        i = (i + 1) & mask;
    }
    *idx = i; // first empty slot, for insertion
    return false;
}

void grow(Map* m, size_t newcap) {
    Map nm;
    nm.slots    = static_cast<MapSlot*>(std::calloc(newcap, sizeof(MapSlot)));
    nm.state    = static_cast<uint8_t*>(std::calloc(newcap, sizeof(uint8_t)));
    nm.capacity = newcap;
    nm.count    = 0;
    if (!nm.slots || !nm.state) {
        std::fprintf(stderr, "Fatal: Map out of memory (capacity %zu).\n", newcap);
        std::exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < m->capacity; i++) {
        if (m->state[i]) map_put(&nm, m->slots[i].key, m->slots[i].val);
    }
    map_free(m);
    *m = nm;
}

} // namespace

void map_put(Map* m, uint64_t key, void* val) {
    if (m->capacity == 0) {
        grow(m, 16);
    } else if ((m->count + 1) * 4 >= m->capacity * 3) { // 75% load
        grow(m, m->capacity * 2);
    }

    size_t idx;
    if (find_slot(m, key, &idx)) {
        m->slots[idx].val = val; // overwrite existing
        return;
    }
    m->state[idx]     = 1;
    m->slots[idx].key = key;
    m->slots[idx].val = val;
    m->count++;
}

bool map_lookup(const Map* m, uint64_t key, void** out) {
    size_t idx;
    if (find_slot(m, key, &idx)) {
        if (out) *out = m->slots[idx].val;
        return true;
    }
    return false;
}

void* map_get(const Map* m, uint64_t key) {
    void* v = nullptr;
    map_lookup(m, key, &v);
    return v;
}

bool map_contains(const Map* m, uint64_t key) {
    return map_lookup(m, key, nullptr);
}

// Open-addressing deletion with backward-shift to preserve probe chains.
bool map_remove(Map* m, uint64_t key) {
    size_t idx;
    if (!find_slot(m, key, &idx)) return false;
    size_t mask = m->capacity - 1;
    size_t i    = idx;
    for (;;) {
        size_t j = (i + 1) & mask;
        if (!m->state[j]) { m->state[i] = 0; break; }
        size_t k = hash_u64(m->slots[j].key) & mask;
        // Shift j into i if i is on j's probe chain from its ideal slot k.
        if (((j - k) & mask) > ((i - k) & mask)) {
            m->slots[i] = m->slots[j];
            m->state[i] = 1;
            i           = j;
        } else {
            m->state[i] = 0;
            break;
        }
    }
    m->count--;
    return true;
}

void map_free(Map* m) {
    std::free(m->slots);
    std::free(m->state);
    m->slots    = nullptr;
    m->state    = nullptr;
    m->count    = 0;
    m->capacity = 0;
}

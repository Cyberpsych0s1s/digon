#ifndef DIGON_MAP_H
#define DIGON_MAP_H

#include <cstddef>
#include <cstdint>

// u64 -> void* open-addressing hashmap with linear probing.
// A parallel state byte tracks occupancy so any u64 key (including 0) is
// valid. Grows at 75% load. malloc-backed; call map_free.
struct MapSlot {
    uint64_t key;
    void*    val;
};

struct Map {
    MapSlot* slots    = nullptr;
    uint8_t* state    = nullptr;  // 0 = empty, 1 = occupied
    size_t   count    = 0;
    size_t   capacity = 0;        // 0 or a power of two
};

void  map_free(Map* m);
void  map_put(Map* m, uint64_t key, void* val);          // insert or overwrite
bool  map_lookup(const Map* m, uint64_t key, void** out);// true if present
void* map_get(const Map* m, uint64_t key);               // value, or nullptr if absent
bool  map_contains(const Map* m, uint64_t key);
bool  map_remove(Map* m, uint64_t key);                  // backward-shift delete

#endif

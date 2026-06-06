#ifndef DIGON_VEC_H
#define DIGON_VEC_H

#include <cstddef>
#include <cstdlib>

// Growth policy and OOM handling live in vec.cc so they exist once.
size_t        vec_next_capacity(size_t current, size_t needed);
[[noreturn]] void vec_oom(size_t bytes);

// Small dynamic array for trivially-copyable T. malloc/realloc backed,
// explicit free, no exceptions; OOM aborts. Growth on realloc is a bitwise
// move, so T must be trivially relocatable.
template <typename T>
struct Vec {
    T*     data     = nullptr;
    size_t len      = 0;
    size_t capacity = 0;

    void reserve(size_t want) {
        if (want <= capacity) return;
        size_t newcap = vec_next_capacity(capacity, want);
        T* p = static_cast<T*>(std::realloc(data, newcap * sizeof(T)));
        if (!p) vec_oom(newcap * sizeof(T));
        data     = p;
        capacity = newcap;
    }

    void push(const T& value) {
        if (len == capacity) reserve(len + 1);
        data[len++] = value;
    }

    T&       operator[](size_t i)       { return data[i]; }
    const T& operator[](size_t i) const { return data[i]; }

    T*       begin()       { return data; }
    T*       end()         { return data + len; }
    const T* begin() const { return data; }
    const T* end()   const { return data + len; }

    T&       back()        { return data[len - 1]; }
    bool     empty() const { return len == 0; }
    void     clear()       { len = 0; }

    void free() {
        std::free(data);
        data     = nullptr;
        len      = 0;
        capacity = 0;
    }
};

#endif

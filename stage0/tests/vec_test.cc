#include "vec.h"

#include <cassert>
#include <cstdio>

int main() {
    Vec<int> v{};
    assert(v.empty());
    assert(v.len == 0);

    for (int i = 0; i < 1000; i++) v.push(i * 2);
    assert(v.len == 1000);
    assert(v.capacity >= 1000);

    for (int i = 0; i < 1000; i++) assert(v[static_cast<size_t>(i)] == i * 2);
    assert(v.back() == 1998);

    // Iteration sums correctly.
    long sum = 0;
    for (int x : v) sum += x;
    assert(sum == 999L * 1000L); // 2 * (0+1+...+999)

    v.clear();
    assert(v.empty());
    assert(v.capacity >= 1000); // clear keeps capacity

    // Large single reservation jumps straight to the request.
    v.reserve(50000);
    assert(v.capacity >= 50000);

    v.free();
    assert(v.data == nullptr && v.capacity == 0);

    std::printf("vec_test: OK\n");
    return 0;
}

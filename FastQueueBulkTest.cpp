#include <array>
#include <cassert>
#include <cstdint>
#include <span>

#if __x86_64__ || _M_X64
#include "fast_queue_x86_64.h"
#elif __aarch64__ || _M_ARM64
#include "fast_queue_arm64.h"
#else
#error Architecture not supported
#endif

using Queue = FastQueue<uint64_t*, 0b111, 64>;

int main() {
    Queue queue;
    std::array<uint64_t, 32> values{};
    std::array<uint64_t*, 9> input{};
    std::array<uint64_t*, 9> output{};
    for (uint64_t i = 0; i < values.size(); ++i) input[i % input.size()] = &values[i];

    assert(queue.tryPushBulk({}) == 0);
    assert(queue.tryPopBulk({}) == 0);
    assert(queue.tryPopBulk(std::span{output}) == 0);

    for (uint64_t i = 0; i < 9; ++i) input[i] = &values[i];
    assert(queue.tryPushBulk(std::span{input}) == 8);
    assert(queue.tryPushBulk(std::span{input}) == 0);
    assert(queue.tryPopBulk(std::span{output}.first(3)) == 3);
    for (uint64_t i = 0; i < 3; ++i) assert(output[i] == &values[i]);
    assert(queue.tryPushBulk(std::span{input}.first(5)) == 3);
    assert(queue.tryPopBulk(std::span{output}) == 8);
    for (uint64_t i = 0; i < 5; ++i) assert(output[i] == &values[i + 3]);
    for (uint64_t i = 0; i < 3; ++i) assert(output[i + 5] == &values[i]);

    return 0;
}

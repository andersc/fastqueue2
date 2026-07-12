#include <algorithm>
#include <array>
#include <atomic>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

#if __x86_64__ || _M_X64
#include "fast_queue_x86_64.h"
#elif __aarch64__ || _M_ARM64
#include "fast_queue_arm64.h"
#else
#error Architecture not supported
#endif

using Queue = FastQueue<uint64_t*, 0b111, 64>;

static void testBoundariesAndWrap() {
    Queue queue;
    std::array<uint64_t, 64> values{};
    std::array<uint64_t*, 9> input{};
    std::array<uint64_t*, 9> output{};
    for (uint64_t i = 0; i < input.size(); ++i) input[i] = &values[i];

    assert(queue.tryPushBulk({}) == 0);
    assert(queue.tryPopBulk({}) == 0);
    assert(queue.tryPopBulk(std::span{output}) == 0);

    // Capacity is eight. Input length nine proves bounded transfer cap.
    assert(queue.tryPushBulk(std::span{input}) == 8);
    assert(queue.tryPushBulk(std::span{input}) == 0);
    assert(queue.tryPopBulk(std::span{output}.first(3)) == 3);
    for (uint64_t i = 0; i < 3; ++i) assert(output[i] == &values[i]);

    // Only three slots are free, so bulk push must partially transfer.
    assert(queue.tryPushBulk(std::span{input}.first(5)) == 3);
    assert(queue.tryPopBulk(std::span{output}) == 8);
    for (uint64_t i = 0; i < 5; ++i) assert(output[i] == &values[i + 3]);
    for (uint64_t i = 0; i < 3; ++i) assert(output[i + 5] == &values[i]);

    // Advance write/read offsets to six, then split five entries at ring wrap.
    for (uint64_t i = 0; i < 3; ++i) assert(queue.tryPush(&values[10 + i]));
    for (uint64_t i = 0; i < 3; ++i) {
        uint64_t* item = nullptr;
        assert(queue.tryPop(item));
        assert(item == &values[10 + i]);
    }
    for (uint64_t i = 0; i < 5; ++i) input[i] = &values[20 + i];
    assert(queue.tryPushBulk(std::span{input}.first(5)) == 5);
    assert(queue.tryPopBulk(std::span{output}.first(5)) == 5);
    for (uint64_t i = 0; i < 5; ++i) assert(output[i] == &values[20 + i]);
}

static void testMixedScalarAndBulk() {
    Queue queue;
    std::array<uint64_t, 32> values{};
    std::array<uint64_t*, 8> output{};
    std::array<uint64_t*, 4> first{};
    for (uint64_t i = 0; i < first.size(); ++i) first[i] = &values[i];

    assert(queue.tryPushBulk(std::span{first}) == 4);
    assert(queue.tryPush(&values[4]));
    assert(queue.tryPush(&values[5]));
    assert(queue.tryPopBulk(std::span{output}.first(3)) == 3);
    for (uint64_t i = 0; i < 3; ++i) assert(output[i] == &values[i]);

    assert(queue.tryPush(&values[6]));
    assert(queue.tryPush(&values[7]));
    assert(queue.tryPush(&values[8]));
    assert(queue.tryPopBulk(std::span{output}) == 6);
    for (uint64_t i = 0; i < 6; ++i) assert(output[i] == &values[3 + i]);
}

static void testThreadedPartialFifo() {
    constexpr uint64_t count = 250000;
    Queue queue;
    std::array<uint64_t, count> values{};
    std::atomic<bool> start = false;
    std::atomic<bool> producerDone = false;

    std::thread producer([&] {
        std::array<uint64_t*, 8> batch{};
        uint64_t next = 0;
        while (!start.load(std::memory_order_acquire)) {}
        while (next < count) {
            const uint64_t requested = std::min<uint64_t>(1 + (next % 8), count - next);
            for (uint64_t i = 0; i < requested; ++i) batch[i] = &values[next + i];
            const uint64_t pushed = queue.tryPushBulk(std::span{batch}.first(requested));
            next += pushed;
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        std::array<uint64_t*, 8> batch{};
        uint64_t expected = 0;
        while (!start.load(std::memory_order_acquire)) {}
        while (expected < count) {
            const uint64_t requested = 1 + (expected % 8);
            const uint64_t popped = queue.tryPopBulk(std::span{batch}.first(requested));
            if (popped == 0) {
                if (producerDone.load(std::memory_order_acquire)) {
                    // Producer completion does not imply local cached index is fresh.
                    continue;
                }
                continue;
            }
            for (uint64_t i = 0; i < popped; ++i) assert(batch[i] == &values[expected + i]);
            expected += popped;
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
}

int main() {
    testBoundariesAndWrap();
    testMixedScalarAndBulk();
    testThreadedPartialFifo();
    return 0;
}

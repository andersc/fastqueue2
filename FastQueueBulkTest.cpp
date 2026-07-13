#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>

#if defined(__aarch64__) || defined(__arm64__)
#include "fast_queue_arm64.h"
#else
#include "fast_queue_x86_64.h"
#endif

using Queue = FastQueue<uint64_t*, 7, 64>; // Capacity 8: force full/partial/wrap cases.
using Batch = FastQueueBatch<uint64_t*>;

static std::size_t push(Queue& queue, const Batch& batch, std::size_t count,
                        std::size_t offset = 0) {
    switch (count) {
        case 1: return queue.tryPushBatch<1>(batch, offset);
        case 2: return queue.tryPushBatch<2>(batch, offset);
        case 3: return queue.tryPushBatch<3>(batch, offset);
        case 4: return queue.tryPushBatch<4>(batch, offset);
        case 5: return queue.tryPushBatch<5>(batch, offset);
        case 6: return queue.tryPushBatch<6>(batch, offset);
        case 7: return queue.tryPushBatch<7>(batch, offset);
        case 8: return queue.tryPushBatch<8>(batch, offset);
        default: return 0;
    }
}

static std::size_t pop(Queue& queue, Batch& batch, std::size_t count,
                       std::size_t offset = 0) {
    switch (count) {
        case 1: return queue.tryPopBatch<1>(batch, offset);
        case 2: return queue.tryPopBatch<2>(batch, offset);
        case 3: return queue.tryPopBatch<3>(batch, offset);
        case 4: return queue.tryPopBatch<4>(batch, offset);
        case 5: return queue.tryPopBatch<5>(batch, offset);
        case 6: return queue.tryPopBatch<6>(batch, offset);
        case 7: return queue.tryPopBatch<7>(batch, offset);
        case 8: return queue.tryPopBatch<8>(batch, offset);
        default: return 0;
    }
}

static void deterministicTests() {
    Queue queue;
    Batch input{};
    Batch output{};
    std::array<uint64_t, 32> values{};
    for (uint64_t i = 0; i < values.size(); ++i) input.items[i & 7] = &values[i];

    assert(pop(queue, output, 8) == 0);
    assert(push(queue, input, 8) == 8);
    assert(push(queue, input, 1) == 0);
    assert(pop(queue, output, 3) == 3);
    for (uint64_t i = 0; i < 3; ++i) assert(*output.items[i] == i);

    for (uint64_t i = 0; i < 5; ++i) input.items[i] = &values[i + 8];
    assert(push(queue, input, 5) == 3); // Partial: only three slots free.
    assert(pop(queue, output, 8) == 8);
    for (uint64_t i = 0; i < 5; ++i) assert(*output.items[i] == i + 3);
    for (uint64_t i = 0; i < 3; ++i) assert(*output.items[i + 5] == i + 8);

    for (uint64_t i = 0; i < 6; ++i) input.items[i] = &values[i + 16];
    assert(push(queue, input, 6) == 6); // Split across ring end.
    assert(pop(queue, output, 6) == 6);
    for (uint64_t i = 0; i < 6; ++i) assert(*output.items[i] == i + 16);

    uint64_t scalar = 30;
    assert(queue.tryPush(&scalar));
    input.items[0] = &values[31];
    assert(push(queue, input, 1) == 1);
    assert(queue.tryPop(output.items[0]) && *output.items[0] == 30);
    assert(pop(queue, output, 1) == 1 && *output.items[0] == 31);
}

static void concurrentTest() {
    constexpr uint64_t count = 250000;
    Queue queue;
    std::array<uint64_t, count> values{};
    std::atomic<bool> start{false};

    std::thread producer([&] {
        Batch batch{};
        uint64_t next = 0;
        while (!start.load(std::memory_order_acquire)) {}
        while (next < count) {
            const std::size_t width = static_cast<std::size_t>(std::min<uint64_t>(8, count - next));
            for (std::size_t i = 0; i < width; ++i) batch.items[i] = &values[next + i];
            std::size_t sent = 0;
            while (sent < width) sent += push(queue, batch, width, sent);
            next += width;
        }
    });
    std::thread consumer([&] {
        Batch batch{};
        uint64_t expected = 0;
        while (!start.load(std::memory_order_acquire)) {}
        while (expected < count) {
            const std::size_t width = static_cast<std::size_t>(std::min<uint64_t>(8, count - expected));
            const std::size_t got = pop(queue, batch, width);
            for (std::size_t i = 0; i < got; ++i) {
                assert(batch.items[i] == &values[expected]);
                ++expected;
            }
        }
    });
    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
}

int main() {
    deterministicTests();
    concurrentTest();
}

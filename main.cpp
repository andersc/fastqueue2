#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#if __x86_64__ || _M_X64
#include "fast_queue_x86_64.h"
#define FASTQUEUE_X86 1
#elif __aarch64__ || _M_ARM64
#include "fast_queue_arm64.h"
#define FASTQUEUE_X86 0
#else
#error Architecture not supported
#endif

#include "spsc_queue.hpp"
#include "spsc-queue.hpp"
#include "pin_thread.h"

#define QUEUE_MASK 0b1111111111
#define L1_CACHE_LINE 64
#ifndef TEST_TIME_DURATION_SEC
#define TEST_TIME_DURATION_SEC 5
#endif
#ifndef ROUNDS
#define ROUNDS 12
#endif
#ifndef CONSUMER_CPU
#define CONSUMER_CPU 1
#endif
#ifndef PRODUCER_CPU
#define PRODUCER_CPU 3
#endif
#ifndef TRANSFER_COUNT
#define TRANSFER_COUNT 200000000ULL
#endif

class MyObject { public: uint64_t mIndex; };

#ifndef POOLED_ONLY
#define POOLED_ONLY 0
#endif
#ifndef HEAP_ONLY
#define HEAP_ONLY 0
#endif
static_assert(!(POOLED_ONLY && HEAP_ONLY), "Select at most one payload mode");
#ifndef SOLO_QUEUE
#define SOLO_QUEUE 0
#endif

static constexpr uint32_t POOL_SZ = 1u << 16;
static MyObject gPool[POOL_SZ];

// Faithful David Alvarez V5 cached-index reference. N includes one sentinel
// slot, unlike FastQueue/Deaod whose mask yields the usable capacity.
template<typename T, size_t N>
class RingBufferV5 {
    std::array<T, N> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) size_t headCached_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) size_t tailCached_{0};

public:
    bool push(const T& value) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        size_t next = head + 1;
        if (next == N) next = 0;
        if (next == tailCached_) {
            tailCached_ = tail_.load(std::memory_order_acquire);
            if (next == tailCached_) return false;
        }
        buffer_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& value) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == headCached_) {
            headCached_ = head_.load(std::memory_order_acquire);
            if (tail == headCached_) return false;
        }
        value = buffer_[tail];
        size_t next = tail + 1;
        if (next == N) next = 0;
        tail_.store(next, std::memory_order_release);
        return true;
    }
};

struct RunControl {
    std::atomic<bool> active{true};
    std::atomic<bool> start{false};
    std::atomic<bool> pinFailed{false};
};

static inline MyObject* allocObj(bool pooled, uint64_t counter) {
    return pooled ? &gPool[counter & (POOL_SZ - 1)] : new MyObject();
}
static inline void freeObj(bool pooled, MyObject* object) { if (!pooled) delete object; }

static void waitStart(const RunControl& control) {
    while (!control.start.load(std::memory_order_acquire)) std::this_thread::yield();
}

struct RunResult {
    double operationsPerSecond{};
    uint64_t occupancySamples{};
    uint64_t occupancyTotal{};
    uint64_t nearEmptySamples{};
    uint64_t occupancyMaximum{};
};

static void verify(MyObject* object, uint64_t expected) {
    if (object->mIndex != expected) {
        std::cerr << "Queue item error: got " << object->mIndex << " expected " << expected << '\n';
        std::terminate();
    }
}

template<typename Queue, typename Push, typename Pop, typename Stop>
static RunResult runOne(Queue& queue, Push push, Pop pop, Stop stop, bool pooled) {
    RunControl control;
    std::atomic<uint64_t> consumed{0};

    std::thread consumer([&] {
        if (!pinThread(CONSUMER_CPU)) { control.pinFailed.store(true); return; }
        waitStart(control);
        for (uint64_t expected = 0; expected < TRANSFER_COUNT; ++expected) {
            MyObject* object = nullptr;
            pop(queue, object);
            verify(object, expected);
            freeObj(pooled, object);
        }
        consumed.store(TRANSFER_COUNT, std::memory_order_relaxed);
    });

    std::thread producer([&] {
        if (!pinThread(PRODUCER_CPU)) { control.pinFailed.store(true); return; }
        waitStart(control);
        for (uint64_t produced = 0; produced < TRANSFER_COUNT; ++produced) {
            MyObject* object = allocObj(pooled, produced);
            object->mIndex = produced;
            push(queue, object);
        }
        // Only producer invokes queue producer-side lifecycle operation.
        stop(queue);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto begin = std::chrono::steady_clock::now();
    control.start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    const auto end = std::chrono::steady_clock::now();
    if (control.pinFailed.load() || consumed.load(std::memory_order_relaxed) != TRANSFER_COUNT) {
        std::cerr << "Pin or transfer failed. Pick valid distinct CPU IDs.\n";
        std::terminate();
    }

    RunResult result{.operationsPerSecond = TRANSFER_COUNT /
        std::chrono::duration<double>(end - begin).count()};
#if FASTQUEUE_X86 && FQ_OCCUPANCY_INSTRUMENT
    if constexpr (requires { queue.occupancyStats(); }) {
        const auto stats = queue.occupancyStats();
        result.occupancySamples = stats.samples;
        result.occupancyTotal = stats.total;
        result.nearEmptySamples = stats.nearEmpty;
        result.occupancyMaximum = stats.maximum;
    }
#endif
    return result;
}

static RunResult runDro(bool pooled) {
    dro::SPSCQueue<MyObject*> queue(QUEUE_MASK + 1);
    return runOne(queue,
                  [](auto& q, auto* object) { while (!q.try_emplace(object)) {} },
                  [](auto& q, auto*& object) { while (!q.try_pop(object)) {} },
                  [](auto&) {}, pooled);
}

static RunResult runDeaod(bool pooled) {
    deaod::spsc_queue<MyObject*, QUEUE_MASK, 6> queue;
    return runOne(queue,
                  [](auto& q, auto* object) { while (!q.push(object)) {} },
                  [](auto& q, auto*& object) { while (!q.pop(object)) {} },
                  [](auto&) {}, pooled);
}

static RunResult runFast(bool pooled) {
    FastQueue<MyObject*, QUEUE_MASK, L1_CACHE_LINE> queue;
    return runOne(queue,
                  [](auto& q, auto* object) { while (!q.tryPush(object)) {} },
                  [](auto& q, auto*& object) { while (!q.tryPop(object)) {} },
                  [](auto& q) { q.stopQueue(); }, pooled);
}

static RunResult runDavid(bool pooled) {
    RingBufferV5<MyObject*, QUEUE_MASK + 2> queue;
    return runOne(queue,
                  [](auto& q, auto* object) { while (!q.push(object)) {} },
                  [](auto& q, auto*& object) { while (!q.pop(object)) {} },
                  [](auto&) {}, pooled);
}

static double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}
static void printDistribution(const char* name, const std::vector<double>& values) {
    std::cout << name << " raw M/s:";
    for (double value : values) std::cout << ' ' << value / 1'000'000.0;
    std::cout << " | median " << median(values) / 1'000'000.0 << " M/s\n";
}

static void runPass(const char* title, bool pooled) {
#if FASTQUEUE_X86 && FQ_OCCUPANCY_INSTRUMENT
    std::vector<RunResult> fastDetails;
#endif
#if SOLO_QUEUE == 1
    std::vector<double> values;
    const auto run = [](bool usePool) { return runDro(usePool); };
    const char* name = "DroSPSC";
#elif SOLO_QUEUE == 2
    std::vector<double> values;
    const auto run = [](bool usePool) { return runDeaod(usePool); };
    const char* name = "DeaodSPSC";
#elif SOLO_QUEUE == 3
    std::vector<double> values;
    const auto run = [](bool usePool) { return runDavid(usePool); };
    const char* name = "DavidV5";
#elif SOLO_QUEUE == 4
    std::vector<double> values;
    const auto run = [](bool usePool) { return runFast(usePool); };
    const char* name = "FastQueue";
#endif
#if SOLO_QUEUE
    std::vector<RunResult> fastDetails;
    run(pooled);
    for (int i = 0; i < ROUNDS; ++i) {
        auto result = run(pooled);
        values.push_back(result.operationsPerSecond);
        if constexpr (FASTQUEUE_X86 && SOLO_QUEUE == 4) fastDetails.push_back(result);
    }
    std::cout << "\n===== " << title << " (" << ROUNDS << " joined solo rounds) =====\n";
    printDistribution(name, values);
#else
    std::vector<double> dro, deaod, david, fast;
#if FASTQUEUE_X86 && FQ_OCCUPANCY_INSTRUMENT
    // Collect per-run refresh statistics only in diagnostic builds.
#else
    // No occupancy counters in normal builds.
#endif
    // Warm all implementations. Every timed run joins both threads before next starts.
    runDro(pooled); runDeaod(pooled); runDavid(pooled); runFast(pooled);
    for (int i = 0; i < ROUNDS; ++i) {
        const auto addDro = [&] { dro.push_back(runDro(pooled).operationsPerSecond); };
        const auto addDeaod = [&] { deaod.push_back(runDeaod(pooled).operationsPerSecond); };
        const auto addDavid = [&] { david.push_back(runDavid(pooled).operationsPerSecond); };
        const auto addFast = [&] {
            auto r = runFast(pooled);
            fast.push_back(r.operationsPerSecond);
#if FASTQUEUE_X86 && FQ_OCCUPANCY_INSTRUMENT
            fastDetails.push_back(r);
#endif
        };
        switch (i % 4) {
            case 0: addDro(); addDeaod(); addDavid(); addFast(); break;
            case 1: addDeaod(); addDavid(); addFast(); addDro(); break;
            case 2: addDavid(); addFast(); addDro(); addDeaod(); break;
            default: addFast(); addDro(); addDeaod(); addDavid(); break;
        }
    }
    std::cout << "\n===== " << title << " (" << ROUNDS << " 4-way rotated, joined rounds) =====\n";
    printDistribution("DroSPSC", dro);
    printDistribution("DeaodSPSC", deaod);
    printDistribution("DavidV5", david);
    printDistribution("FastQueue", fast);
#endif
#if FASTQUEUE_X86 && FQ_OCCUPANCY_INSTRUMENT
    uint64_t samples = 0, total = 0, nearEmpty = 0, maximum = 0;
    for (const auto& result : fastDetails) {
        samples += result.occupancySamples;
        total += result.occupancyTotal;
        nearEmpty += result.nearEmptySamples;
        maximum = std::max(maximum, result.occupancyMaximum);
    }
    if (samples) std::cout << "FastQueue refresh occupancy: avg "
                           << static_cast<double>(total) / samples << ", <=1 "
                           << 100.0 * nearEmpty / samples << "%, max " << maximum << '\n';
#endif
}

int main() {
#if !POOLED_ONLY
    runPass("Heap payload (allocator bound)", false);
#endif
#if !HEAP_ONLY
    runPass("Pooled payload (queue throughput)", true);
#endif
}

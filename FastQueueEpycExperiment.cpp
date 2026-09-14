// Focused scalar SPSC comparison for FastQueue2 policy experiments.
// Compares identical fixed-work loops, pinning, payloads, queue capacity, and
// rotated queue order. AtomicQueue is supplied as a pinned external checkout.

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string_view>
#include <thread>
#include <vector>

#include "pin_thread.h"
#include "fast_queue_x86_64.h"
#include "fast_queue_x86_64_epyc.h"
#include <atomic_queue/atomic_queue.h>

#ifndef EXPERIMENT_CAPACITY
#define EXPERIMENT_CAPACITY 1024
#endif
#ifndef EXPERIMENT_TRANSFERS
#define EXPERIMENT_TRANSFERS 50000000ULL
#endif
#ifndef EXPERIMENT_ROUNDS
#define EXPERIMENT_ROUNDS 12
#endif
#ifndef EXPERIMENT_PRODUCER_CPU
#define EXPERIMENT_PRODUCER_CPU 1
#endif
#ifndef EXPERIMENT_CONSUMER_CPU
#define EXPERIMENT_CONSUMER_CPU 3
#endif

static_assert(EXPERIMENT_CAPACITY >= 2);
static_assert((EXPERIMENT_CAPACITY & (EXPERIMENT_CAPACITY - 1)) == 0,
              "EXPERIMENT_CAPACITY must be a power of two");

namespace {
constexpr std::size_t kCapacity = EXPERIMENT_CAPACITY;
constexpr std::size_t kMask = kCapacity - 1;
constexpr std::uint64_t kTransfers = EXPERIMENT_TRANSFERS;
constexpr int kRounds = EXPERIMENT_ROUNDS;
constexpr int kProducerCpu = EXPERIMENT_PRODUCER_CPU;
constexpr int kConsumerCpu = EXPERIMENT_CONSUMER_CPU;

struct Payload {};

using CachedQueue = FastQueue<Payload*, kMask, 64>;
using SlotQueue = FastQueueEpyc<Payload*, kMask, 64>;
using UpstreamQueue = atomic_queue::AtomicQueue<
    Payload*, static_cast<unsigned>(kCapacity), nullptr,
    true, true, false, true>;

struct Result {
    double mitems = 0.0;
    bool valid = false;
    bool pinned = false;
};

void cpuPause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
}

template<class Queue, class Push, class Pop, class Stop>
Result runFixed(Queue& queue, Push push, Pop pop, Stop stop) {
    std::vector<Payload> pool(static_cast<std::size_t>(kTransfers));
    std::chrono::steady_clock::time_point begin;
    std::barrier start{3, [&begin]() noexcept {
        begin = std::chrono::steady_clock::now();
    }};
    std::atomic<bool> producerPinned{false};
    std::atomic<bool> consumerPinned{false};
    std::atomic<bool> valid{true};

    std::thread producer([&] {
        producerPinned.store(pinThread(kProducerCpu), std::memory_order_relaxed);
        start.arrive_and_wait();
        for (std::uint64_t i = 0; i < kTransfers; ++i) {
            push(queue, &pool[static_cast<std::size_t>(i)]);
        }
        stop(queue);
    });

    std::thread consumer([&] {
        consumerPinned.store(pinThread(kConsumerCpu), std::memory_order_relaxed);
        start.arrive_and_wait();
        for (std::uint64_t expected = 0; expected < kTransfers; ++expected) {
            Payload* item = nullptr;
            pop(queue, item);
            if (item == nullptr) {
                if (expected != kTransfers) valid.store(false, std::memory_order_relaxed);
                break;
            }
            if (item != &pool[static_cast<std::size_t>(expected)])
                valid.store(false, std::memory_order_relaxed);
        }
    });

    start.arrive_and_wait();
    producer.join();
    consumer.join();
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();

    return {static_cast<double>(kTransfers) / elapsed / 1e6,
            valid.load(std::memory_order_relaxed),
            producerPinned.load(std::memory_order_relaxed) &&
                consumerPinned.load(std::memory_order_relaxed)};
}

Result runCached() {
    CachedQueue queue;
    return runFixed(
        queue,
        [](auto& q, Payload* value) { q.push(value); },
        [](auto& q, Payload*& value) { q.pop(value); },
        [](auto&) {});
}

Result runSlot() {
    SlotQueue queue;
    return runFixed(
        queue,
        [](auto& q, Payload* value) { q.push(value); },
        [](auto& q, Payload*& value) { q.pop(value); },
        [](auto&) {});
}

Result runAtomic() {
    UpstreamQueue queue;
    return runFixed(
        queue,
        [](auto& q, Payload* value) { q.push(value); },
        [](auto& q, Payload*& value) { value = q.pop(); },
        [](auto&) {});
}

struct Variant {
    std::string_view name;
    Result (*run)();
};

void printDistribution(const Variant& variant, std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    const auto median = values.size() % 2 == 0
        ? (values[middle - 1] + values[middle]) / 2.0
        : values[middle];
    const auto mean = std::accumulate(values.begin(), values.end(), 0.0) /
                      static_cast<double>(values.size());
    double squareSum = 0.0;
    for (double value : values) squareSum += (value - mean) * (value - mean);
    const double cv = std::sqrt(squareSum / values.size()) / mean * 100.0;

    std::cout << variant.name << "," << std::fixed << std::setprecision(3)
              << median << "," << values.front() << "," << values.back()
              << "," << cv << "\n";
}
} // namespace

int main() {
    constexpr std::array variants{
        Variant{"fastqueue2_cached_index", runCached},
        Variant{"fastqueue2_slot_signaling", runSlot},
        Variant{"atomic_queue_spsc", runAtomic},
    };
    std::array<std::vector<double>, variants.size()> samples;

    std::cout << "configuration,capacity=" << kCapacity
              << ",transfers=" << kTransfers
              << ",rounds=" << kRounds
              << ",producer_cpu=" << kProducerCpu
              << ",consumer_cpu=" << kConsumerCpu << "\n";

    for (int round = 0; round < kRounds; ++round) {
        for (std::size_t offset = 0; offset < variants.size(); ++offset) {
            const std::size_t index = (static_cast<std::size_t>(round) + offset) % variants.size();
            const Result result = variants[index].run();
            if (!result.valid || !result.pinned) {
                std::cerr << "validation failed for " << variants[index].name
                          << ": valid=" << result.valid
                          << " pinned=" << result.pinned << "\n";
                return EXIT_FAILURE;
            }
            samples[index].push_back(result.mitems);
            std::cout << "sample," << round << "," << variants[index].name
                      << "," << std::fixed << std::setprecision(3)
                      << result.mitems << "\n";
        }
    }

    std::cout << "summary,queue,median_mitems_s,min_mitems_s,max_mitems_s,cv_pct\n";
    for (std::size_t i = 0; i < variants.size(); ++i)
        printDistribution(variants[i], std::move(samples[i]));
}

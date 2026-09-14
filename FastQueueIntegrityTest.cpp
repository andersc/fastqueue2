// Timed lock-free SPSC integrity test with irregular producer/consumer delays.
// Uses a shallow queue to exercise full and empty transitions frequently.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#include "pin_thread.h"
#if defined(__x86_64__) || defined(_M_X64)
#if defined(FQ_EPYC_SLOT_SIGNAL) && FQ_EPYC_SLOT_SIGNAL
#include "fast_queue_x86_64_epyc.h"
template<typename T, uint64_t Mask, uint64_t Line>
using IntegrityQueue = FastQueueEpyc<T, Mask, Line>;
#else
#include "fast_queue_x86_64.h"
template<typename T, uint64_t Mask, uint64_t Line>
using IntegrityQueue = FastQueue<T, Mask, Line>;
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#include "fast_queue_arm64.h"
template<typename T, uint64_t Mask, uint64_t Line>
using IntegrityQueue = FastQueue<T, Mask, Line>;
#else
#error Architecture not supported
#endif

#ifndef INTEGRITY_QUEUE_MASK
#define INTEGRITY_QUEUE_MASK 0b1
#endif
#ifndef TEST_TIME_DURATION_SEC
#define TEST_TIME_DURATION_SEC 200
#endif
#ifndef INTEGRITY_PRODUCER_CPU
#define INTEGRITY_PRODUCER_CPU 2
#endif
#ifndef INTEGRITY_CONSUMER_CPU
#define INTEGRITY_CONSUMER_CPU 0
#endif

namespace {
constexpr uint64_t kQueueMask = INTEGRITY_QUEUE_MASK;
constexpr uint64_t kCacheLine = 64;
using Payload = std::vector<uint8_t>;
using Queue = IntegrityQueue<Payload*, kQueueMask, kCacheLine>;

std::atomic<bool> active{true};
std::atomic<bool> start{false};
std::atomic<bool> failed{false};
std::atomic<unsigned> ready{0};
std::atomic<uint64_t> transactions{0};
Payload stopToken;

uint64_t loadWord(const uint8_t* bytes) {
    uint64_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

void storeWord(uint8_t* bytes, uint64_t value) {
    std::memcpy(bytes, &value, sizeof(value));
}

void waitForStart() {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire) &&
           !failed.load(std::memory_order_relaxed))
        std::this_thread::yield();
}

bool pushUnlessFailed(Queue& queue, Payload* value) {
    while (!queue.tryPush(value)) {
        if (failed.load(std::memory_order_relaxed)) return false;
        std::this_thread::yield();
    }
    return true;
}

void producer(Queue& queue) {
    if (!pinThread(INTEGRITY_PRODUCER_CPU)) {
        failed.store(true, std::memory_order_release);
        ready.fetch_add(1, std::memory_order_release);
        return;
    }
    waitForStart();
    if (failed.load(std::memory_order_relaxed)) return;

    std::mt19937 engine{std::random_device{}()};
    std::uniform_int_distribution<int> distribution{1, 500};
    uint64_t counter = 0;
    while (active.load(std::memory_order_acquire) &&
           !failed.load(std::memory_order_relaxed)) {
        auto* data = new Payload(1000);
        std::generate(data->begin(), data->end(), [&] { return distribution(engine); });
        storeWord(data->data(), counter++);
        const uint64_t checksum = std::accumulate(data->begin() + 16, data->end(), uint64_t{0});
        storeWord(data->data() + 8, checksum);
        if (!pushUnlessFailed(queue, data)) {
            delete data;
            return;
        }
        std::this_thread::sleep_for(std::chrono::nanoseconds(distribution(engine)));
    }
    pushUnlessFailed(queue, &stopToken);
}

void consumer(Queue& queue) {
    if (!pinThread(INTEGRITY_CONSUMER_CPU)) {
        failed.store(true, std::memory_order_release);
        ready.fetch_add(1, std::memory_order_release);
        return;
    }
    waitForStart();
    if (failed.load(std::memory_order_relaxed)) return;

    std::mt19937 engine{std::random_device{}()};
    std::uniform_int_distribution<int> distribution{1, 500};
    uint64_t expected = 0;
    while (!failed.load(std::memory_order_relaxed)) {
        Payload* data = nullptr;
        while (!queue.tryPop(data)) {
            if (failed.load(std::memory_order_relaxed)) return;
            std::this_thread::yield();
        }
        if (data == &stopToken) break;

        const uint64_t sequence = loadWord(data->data());
        const uint64_t checksum = std::accumulate(data->begin() + 16, data->end(), uint64_t{0});
        const uint64_t expectedChecksum = loadWord(data->data() + 8);
        if (sequence != expected || checksum != expectedChecksum) {
            std::cerr << "Integrity failure: expected sequence " << expected
                      << ", received " << sequence << "\n";
            delete data;
            failed.store(true, std::memory_order_release);
            return;
        }
        delete data;
        ++expected;
        std::this_thread::sleep_for(std::chrono::nanoseconds(distribution(engine)));
    }
    transactions.store(expected, std::memory_order_relaxed);
}
} // namespace

int main() {
    Queue queue;
    std::thread consumerThread(consumer, std::ref(queue));
    std::thread producerThread(producer, std::ref(queue));

    while (ready.load(std::memory_order_acquire) != 2)
        std::this_thread::yield();
    if (!failed.load(std::memory_order_acquire)) {
        std::cout << "Producer -> Consumer (start)\n";
        start.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::seconds(TEST_TIME_DURATION_SEC));
        active.store(false, std::memory_order_release);
    } else {
        start.store(true, std::memory_order_release);
    }

    producerThread.join();
    consumerThread.join();
    if (failed.load(std::memory_order_acquire)) {
        std::cerr << "Integrity test failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Producer -> Consumer (end)\n"
              << "Test ended. Did " << transactions.load() << " transactions.\n";
    return EXIT_SUCCESS;
}

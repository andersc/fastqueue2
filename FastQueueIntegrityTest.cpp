//
// Created by Anders Cedronius
//

// Lock-free producer (one thread) and consumer (another thread) integrity test
// The test is performed by the producer producing data at an irregular rate in time
// containing random data and a simple checksum + counter.
// And a consumer reading the data at an equally (same dynamic range in time) irregular rate
// verifying the checksum and linearity of the counter. The queue is set shallow (2 entries) to
// make the test face queue full/empty situations as often as possible.

#include <random>
#include <algorithm>
#include <iostream>
#include <thread>
#include <numeric>
#include "pin_thread.h"
#if __x86_64__ || _M_X64
#include "fast_queue_x86_64.h"
#elif __aarch64__ || _M_ARM64
#include "fast_queue_arm64.h"
#else
#error Architecture not supported
#endif

#define QUEUE_MASK 0b1
#define L1_CACHE_LINE 64
#define TEST_TIME_DURATION_SEC 200

bool gActiveProducer = true;
std::atomic<uint64_t> gActiveConsumer = 0;
bool gStartBench = false;
std::atomic<uint64_t> gTransactions = 0;
uint64_t gChk = 0;

void producer(FastQueue<std::vector<uint8_t>*, QUEUE_MASK, L1_CACHE_LINE> *rQueue, int32_t aCPU) {
    std::random_device lRndDevice;
    std::mt19937 lMersenneEngine{lRndDevice()};
    std::uniform_int_distribution<int> lDist{1, 500};
    auto lGen = [&lDist, &lMersenneEngine]() {
        return lDist(lMersenneEngine);
    };
    if (!pinThread(aCPU)) {
        std::cout << "Pin CPU fail. " << std::endl;
        rQueue->stopQueue();
        return;
    }
    while (!gStartBench) {
#ifdef _MSC_VER
        __nop();
#else
        asm("NOP");
#endif
    }
    uint64_t lCounter = 0;
    while (gActiveProducer) {
        auto lpData = new std::vector<uint8_t>(1000);
        std::generate(lpData->begin(), lpData->end(), lGen);
        *(uint64_t *) lpData->data() = lCounter++;
        uint64_t lSimpleSum = std::accumulate(lpData->begin() + 16, lpData->end(), 0);
        *(uint64_t *) (lpData->data() + 8) = lSimpleSum;
        rQueue->push(lpData);
        uint64_t lSleep = lDist(lMersenneEngine);
        std::this_thread::sleep_for(std::chrono::nanoseconds(lSleep));
    }
    rQueue->stopQueue();
}

void consumer(FastQueue<std::vector<uint8_t>*, QUEUE_MASK, L1_CACHE_LINE> *rQueue, int32_t aCPU) {
    uint64_t lCounter = 0;
    std::random_device lRndDevice;
    std::mt19937 lMersenneEngine{lRndDevice()};
    std::uniform_int_distribution<int> lDist{1, 500};
    if (!pinThread(aCPU)) {
        std::cout << "Pin CPU fail. " << std::endl;
        gActiveConsumer--;
        return;
    }
    gActiveConsumer++;
    while (true) {
        std::vector<uint8_t>* lResult = nullptr;
        rQueue->pop(lResult);
        if (lResult == nullptr) {
            break;
        }
        if (lCounter != *(uint64_t *) lResult->data()) {
            std::cout << "Test failed.. Not linear data. " << *(uint64_t *) lResult->data() << std::endl;
            gActiveConsumer--;
            return;
        }
        uint64_t lSimpleSum = std::accumulate(lResult->begin() + 16, lResult->end(), 0);
        if (lSimpleSum != *(uint64_t *) (lResult->data() + 8)) {
            std::cout << "Test failed.. Not consistent data. " << lSimpleSum << " " << lCounter << " " << gChk
                      << std::endl;
            gActiveConsumer--;
            return;
        }
        delete lResult;
        lCounter++;
        uint64_t lSleep = lDist(lMersenneEngine);
        std::this_thread::sleep_for(std::chrono::nanoseconds(lSleep));
    }
    gTransactions = lCounter;
    gActiveConsumer--;
}

int main() {
    auto lQueue1 = new FastQueue<std::vector<uint8_t>*, QUEUE_MASK, L1_CACHE_LINE>();
    std::thread([lQueue1] { return consumer(lQueue1, 0); }).detach();
    std::thread([lQueue1] { return producer(lQueue1, 2); }).detach();
    std::cout << "Producer -> Consumer (start)" << std::endl;
    gStartBench = true;
    std::this_thread::sleep_for(std::chrono::seconds(TEST_TIME_DURATION_SEC));
    gActiveProducer = false;
    lQueue1->stopQueue();
    std::cout << "Producer -> Consumer (end)" << std::endl;
    while (gActiveConsumer) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    delete lQueue1;
    std::cout << "Test ended. Did " << gTransactions << " transactions." << std::endl;
    return EXIT_SUCCESS;
}

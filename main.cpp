#include <iostream>
#include <thread>
#include <new>

#if __x86_64__ || _M_X64
#include "fast_queue_x86_64.h"
#elif __aarch64__ || _M_ARM64
#include "fast_queue_arm64.h"
#else
#error Architecture not supported
#endif

#include "deaod_spsc/spsc_queue.hpp"
#include "pin_thread.h"

#define QUEUE_MASK 0b1111111111
#define L1_CACHE_LINE 64
#define TEST_TIME_DURATION_SEC 20
//Run the consumer on CPU
#define CONSUMER_CPU 1
//Run the producer on CPU
#define PRODUCER_CPU 3

std::atomic<uint64_t> gActiveConsumer = 0;
std::atomic<uint64_t> gCounter = 0;
bool gStartBench = false;
bool gActiveProducer = true;

class MyObject {
public:
    uint64_t mIndex;
};

/// -----------------------------------------------------------
///
/// deaodSPSC section Start
///
/// -----------------------------------------------------------


void deaodSPSCProducer(deaod::spsc_queue<MyObject*, QUEUE_MASK, 6> *pQueue, int32_t aCPU) {
    if (!pinThread(aCPU)) {
        std::cout << "Pin CPU fail. " << std::endl;
        return;
    }
    while (!gStartBench) {
#ifdef _MSC_VER
        __nop();
#else
        asm volatile ("NOP");
#endif
    }
    uint64_t lCounter = 0;
    while (gActiveProducer) {
        auto lTheObject = new MyObject();
        lTheObject->mIndex = lCounter++;
        bool lAbleToPush = false;
        while (!lAbleToPush && gActiveProducer) {
            lAbleToPush = pQueue->push(std::move(lTheObject));
        }
    }
}

void deaodSPSCConsumer(deaod::spsc_queue<MyObject*, QUEUE_MASK, 6> *pQueue, int32_t aCPU) {
    if (!pinThread(aCPU)) {
        std::cout << "Pin CPU fail. " << std::endl;
        gActiveConsumer--;
        return;
    }
    uint64_t lCounter = 0;
    while (true) {

        MyObject* lResult = nullptr;
        bool lAbleToPop = false;
        while (!lAbleToPop && gActiveProducer) {
            lAbleToPop = pQueue->pop(lResult);
        }
        if (lResult == nullptr) {
            break;
        }
        if (lResult->mIndex != lCounter) {
            std::cout << "Queue item error" << std::endl;
        }
        lCounter++;
        delete lResult;
    }
    gCounter += lCounter;
    gActiveConsumer--;
}

/// -----------------------------------------------------------
///
/// deaodSPSC section End
///
/// -----------------------------------------------------------


/// -----------------------------------------------------------
///
/// FastQueue section Start
///
/// -----------------------------------------------------------

void fastQueueProducer(FastQueue<MyObject*, QUEUE_MASK, L1_CACHE_LINE> *pQueue, int32_t aCPU) {
    if (!pinThread(aCPU)) {
        std::cout << "Pin CPU fail. " << std::endl;
        return;
    }
    while (!gStartBench) {
#ifdef _MSC_VER
        __nop();
#else
        asm volatile ("NOP");
#endif
    }
    uint64_t lCounter = 0;
    while (gActiveProducer) {
        auto lTheObject = new MyObject();
        lTheObject->mIndex = lCounter++;
        pQueue->push(lTheObject);
    }
    pQueue->stopQueue();
}

void fastQueueConsumer(FastQueue<MyObject*, QUEUE_MASK, L1_CACHE_LINE> *pQueue, int32_t aCPU) {
    if (!pinThread(aCPU)) {
        std::cout << "Pin CPU fail. " << std::endl;
        gActiveConsumer--;
        return;
    }
    uint64_t lCounter = 0;
    while (true) {
        MyObject* pResult = nullptr;
        pQueue->pop(pResult);
        if (pResult == nullptr) {
            break;
        }
        if (pResult->mIndex != lCounter) {
            std::cout << "Queue item error. got: " << pResult->mIndex << " expected: " << lCounter << std::endl;
        }
        lCounter++;
        delete pResult;
    }
    gCounter += lCounter;
    gActiveConsumer--;
}

/// -----------------------------------------------------------
///
/// FastQueue section End
///
/// -----------------------------------------------------------

int main() {

    ///
    /// DeaodSPSC test ->
    ///

    // Create the queue
    auto deaodSPSC = new deaod::spsc_queue<MyObject*, QUEUE_MASK, 6>();

    // Start the consumer(s) / Producer(s)
    gActiveConsumer++;

    std::thread([deaodSPSC] { deaodSPSCConsumer(deaodSPSC, CONSUMER_CPU); }).detach();
    std::thread([deaodSPSC] { deaodSPSCProducer(deaodSPSC, PRODUCER_CPU); }).detach();

    // Wait for the OS to actually get it done.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Start the test
    std::cout << "DeaodSPSC pointer test started." << std::endl;
    gStartBench = true;
    std::this_thread::sleep_for(std::chrono::seconds(TEST_TIME_DURATION_SEC));

    // End the test
    gActiveProducer = false;
    std::cout << "DeaodSPSC pointer test ended." << std::endl;

    // Wait for the consumers to 'join'
    // Why not the classic join? I prepared for a multi thread case I need this function for.
    while (gActiveConsumer) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Garbage collect the queue
    delete deaodSPSC;

    // Print the result.
    std::cout << "DeaodSPSC Transactions -> " << gCounter / TEST_TIME_DURATION_SEC << "/s" << std::endl;

    // Zero the test parameters.
    gStartBench = false;
    gActiveProducer = true;
    gCounter = 0;
    gActiveConsumer = 0;

    ///
    /// FastQueue test ->
    ///

    // Create the queue
    auto lFastQueue = new FastQueue<MyObject*, QUEUE_MASK, L1_CACHE_LINE>();

    // Start the consumer(s) / Producer(s)
    gActiveConsumer++;
    std::thread([lFastQueue] { return fastQueueConsumer(lFastQueue, CONSUMER_CPU); }).detach();
    std::thread([lFastQueue] { return fastQueueProducer(lFastQueue, PRODUCER_CPU); }).detach();

    // Wait for the OS to actually get it done.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Start the test
    std::cout << "FastQueue pointer test started." << std::endl;
    gStartBench = true;
    std::this_thread::sleep_for(std::chrono::seconds(TEST_TIME_DURATION_SEC));

    // End the test
    gActiveProducer = false;
    std::cout << "FastQueue pointer test ended." << std::endl;

    // Wait for the consumers to 'join'
    // Why not the classic join? I prepared for a multi thread case I need this function for.
    while (gActiveConsumer) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Garbage collect the queue
    delete lFastQueue;

    // Print the result.
    std::cout << "FastQueue Transactions -> " << gCounter / TEST_TIME_DURATION_SEC << "/s" << std::endl;

    // Zero the test parameters.
    gStartBench = false;
    gActiveProducer = true;
    gCounter = 0;
    gActiveConsumer = 0;

    // Create the queue

    auto lObject = std::make_unique<int>(8);

    //auto lFastQueueTest = new FastQueue<std::unique_ptr<int>, QUEUE_MASK, L1_CACHE_LINE>();

    //std::cout << std::cref(lFastQueueTest) << std::endl;

    return 0;
}

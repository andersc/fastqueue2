#include <iostream>
#include <thread>
#include <new>
#include <vector>
#include <algorithm>
#include <cstdint>

#if __x86_64__ || _M_X64
#include "fast_queue_x86_64.h"
#elif __aarch64__ || _M_ARM64
#include "fast_queue_arm64.h"
#else
#error Architecture not supported
#endif

#include "spsc_queue.hpp" //Deaod
#include "spsc-queue.hpp"  //Dro
#include "pin_thread.h"

#define QUEUE_MASK 0b1111111111
#define L1_CACHE_LINE 64
// Per-measurement duration and how many rounds to run. Every round runs all
// three queues once, and the order is ROTATED between rounds so no queue keeps
// the thermal/turbo advantage of always running first. We report the median.
#ifndef TEST_TIME_DURATION_SEC
#define TEST_TIME_DURATION_SEC 5
#endif
#ifndef ROUNDS
#define ROUNDS 5
#endif
//Run the consumer / producer on these CPUs (two cores that share a fast cache
//domain give the best numbers). Override with -D on the command line.
#ifndef CONSUMER_CPU
#define CONSUMER_CPU 1
#endif
#ifndef PRODUCER_CPU
#define PRODUCER_CPU 3
#endif

std::atomic<uint64_t> gActiveConsumer = 0;
std::atomic<uint64_t> gCounter = 0;
bool gStartBench = false;
bool gActiveProducer = true;

class MyObject {
public:
    uint64_t mIndex;
};

// Allocation strategy. Heap mode (new/delete per message) is the classic
// FastQueue benchmark; it is dominated by the allocator and cross-thread free,
// which masks the queue itself. Pooled mode reuses pre-allocated objects (pool
// far larger than the queue, so reuse never races the consumer) and therefore
// measures pure queue throughput.
static bool gPooled = false;
static constexpr uint32_t POOL_SZ = 1u << 16;
static MyObject gPool[POOL_SZ];
static inline MyObject* allocObj(uint64_t c) { return gPooled ? &gPool[c & (POOL_SZ - 1)] : new MyObject(); }
static inline void freeObj(MyObject* p) { if (!gPooled) delete p; }

static inline void spinStart() {
    while (!gStartBench) {
#ifdef _MSC_VER
        __nop();
#else
        asm volatile ("NOP");
#endif
    }
}

/// ---------------- Dro ----------------
void droSPSCProducer(dro::SPSCQueue<MyObject*> *pQueue, int32_t aCPU) {
    if (!pinThread(aCPU)) { std::cout << "Pin CPU fail. " << std::endl; return; }
    spinStart();
    uint64_t lCounter = 0;
    while (gActiveProducer) { auto o = allocObj(lCounter); o->mIndex = lCounter++; pQueue->emplace(o); }
    pQueue->emplace(nullptr);
}
void droSPSCConsumer(dro::SPSCQueue<MyObject*> *pQueue, int32_t aCPU) {
    if (!pinThread(aCPU)) { std::cout << "Pin CPU fail. " << std::endl; --gActiveConsumer; return; }
    uint64_t lCounter = 0;
    while (true) {
        MyObject* r = nullptr; pQueue->pop(r);
        if (r == nullptr) break;
        if (r->mIndex != lCounter) std::cout << "Queue item error" << std::endl;
        lCounter++; freeObj(r);
    }
    gCounter += lCounter; --gActiveConsumer;
}

/// ---------------- Deaod ----------------
void deaodSPSCProducer(deaod::spsc_queue<MyObject*, QUEUE_MASK, 6> *pQueue, int32_t aCPU) {
    if (!pinThread(aCPU)) { std::cout << "Pin CPU fail. " << std::endl; return; }
    spinStart();
    uint64_t lCounter = 0;
    while (gActiveProducer) {
        auto o = allocObj(lCounter); o->mIndex = lCounter++;
        bool ok = false;
        while (!ok && gActiveProducer) ok = pQueue->push(o);
    }
}
void deaodSPSCConsumer(deaod::spsc_queue<MyObject*, QUEUE_MASK, 6> *pQueue, int32_t aCPU) {
    if (!pinThread(aCPU)) { std::cout << "Pin CPU fail. " << std::endl; gActiveConsumer--; return; }
    uint64_t lCounter = 0;
    while (true) {
        MyObject* r = nullptr; bool ok = false;
        while (!ok && gActiveProducer) ok = pQueue->pop(r);
        if (r == nullptr) break;
        if (r->mIndex != lCounter) std::cout << "Queue item error" << std::endl;
        lCounter++; freeObj(r);
    }
    gCounter += lCounter; gActiveConsumer--;
}

/// ---------------- FastQueue ----------------
void fastQueueProducer(FastQueue<MyObject*, QUEUE_MASK, L1_CACHE_LINE> *pQueue, int32_t aCPU) {
    if (!pinThread(aCPU)) { std::cout << "Pin CPU fail. " << std::endl; return; }
    spinStart();
    uint64_t lCounter = 0;
    while (gActiveProducer) { auto o = allocObj(lCounter); o->mIndex = lCounter++; pQueue->push(o); }
    pQueue->stopQueue();
}
void fastQueueConsumer(FastQueue<MyObject*, QUEUE_MASK, L1_CACHE_LINE> *pQueue, int32_t aCPU) {
    if (!pinThread(aCPU)) { std::cout << "Pin CPU fail. " << std::endl; --gActiveConsumer; return; }
    uint64_t lCounter = 0;
    while (true) {
        MyObject* r = nullptr; pQueue->pop(r);
        if (r == nullptr) break;
        if (r->mIndex != lCounter) std::cout << "Queue item error. got: " << r->mIndex << " expected: " << lCounter << std::endl;
        lCounter++; freeObj(r);
    }
    gCounter += lCounter; --gActiveConsumer;
}

template<typename Fc, typename Fp>
static uint64_t runOne(Fc consumer, Fp producer) {
    gActiveConsumer++;
    std::thread([&] { consumer(CONSUMER_CPU); }).detach();
    std::thread([&] { producer(PRODUCER_CPU); }).detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gStartBench = true;
    std::this_thread::sleep_for(std::chrono::seconds(TEST_TIME_DURATION_SEC));
    gActiveProducer = false;
    while (gActiveConsumer) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    uint64_t r = gCounter / TEST_TIME_DURATION_SEC;
    gStartBench = false; gActiveProducer = true; gCounter = 0; gActiveConsumer = 0;
    return r;
}

static uint64_t runDro()   { auto q = new dro::SPSCQueue<MyObject*>(QUEUE_MASK);              uint64_t r = runOne([q](int c){droSPSCConsumer(q,c);},   [q](int p){droSPSCProducer(q,p);});   delete q; return r; }
static uint64_t runDeaod() { auto q = new deaod::spsc_queue<MyObject*, QUEUE_MASK, 6>();      uint64_t r = runOne([q](int c){deaodSPSCConsumer(q,c);}, [q](int p){deaodSPSCProducer(q,p);}); delete q; return r; }
static uint64_t runFast()  { auto q = new FastQueue<MyObject*, QUEUE_MASK, L1_CACHE_LINE>();  uint64_t r = runOne([q](int c){fastQueueConsumer(q,c);}, [q](int p){fastQueueProducer(q,p);}); delete q; return r; }

static uint64_t median(std::vector<uint64_t> v) { std::sort(v.begin(), v.end()); return v[v.size()/2]; }

static void runPass(const char* aTitle) {
    std::vector<uint64_t> dro, deaod, fast;
    // One discarded warmup round to reach DVFS/turbo steady state.
    runDro(); runDeaod(); runFast();
    for (int i = 0; i < ROUNDS; i++) {
        switch (i % 3) {   // rotate first-runner so no queue is favoured
            case 0: dro.push_back(runDro());     deaod.push_back(runDeaod()); fast.push_back(runFast());  break;
            case 1: deaod.push_back(runDeaod()); fast.push_back(runFast());   dro.push_back(runDro());    break;
            case 2: fast.push_back(runFast());   dro.push_back(runDro());     deaod.push_back(runDeaod());break;
        }
    }
    std::cout << "\n===== " << aTitle << " (median of " << ROUNDS << " rotated rounds) =====\n";
    std::cout << "DroSPSC   Transactions -> " << median(dro)   << "/s\n";
    std::cout << "DeaodSPSC Transactions -> " << median(deaod) << "/s\n";
    std::cout << "FastQueue Transactions -> " << median(fast)  << "/s\n";
}

int main() {
    gPooled = false; runPass("Heap payload (new/delete per message - allocator bound)");
    gPooled = true;  runPass("Pooled payload (pre-allocated - pure queue throughput)");
    return 0;
}

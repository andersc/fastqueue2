//
// Created by Anders Cedronius on 2023-06-27.
//

#pragma once

#include <iostream>
#include <cstdint>
#include <atomic>
#include <cstdlib>

// FastQueue2 - bounded lock-free SPSC queue for 8-byte objects.
//
// Design: a single-producer/single-consumer ring tuned so the two cores almost
// never trade control cache lines. Each side keeps a private *cached* copy of
// the other side's index and only re-reads the shared index when its cache says
// the queue is full (producer) or empty (consumer). In steady state the producer
// only writes its own write-index line plus the packed data; the consumer only
// writes its own read-index line. Data flows strictly producer -> consumer (no
// slot is ever written back), so one cache line carries many elements one way
// instead of ping-ponging a whole line per element.
//
// Counters are monotonic 64-bit (they never wrap in any realistic runtime), so
// the hot path has no wrap branch - the power-of-two ring is addressed with a
// mask. Publishing uses release/acquire on the shared index; the payload
// store/load are plain, ordered by that release/acquire pair.
//
// The ring is heap-allocated so it lives far from the control block: on Apple
// silicon, embedding the ring next to the indices let the streaming prefetcher
// drag data lines into the index-owning core and cut throughput ~3x. Separating
// them is the single biggest win on M-series.
//
// FQ_CTL_ALIGN separates each independently-written field. x86's L2 spatial
// prefetcher fetches 128-byte (two-line) pairs, so each index must sit in its
// own 128-byte pair - otherwise fetching the write index drags the consumer's
// read-index line into the producer's core (and vice versa). 128B measured ~18%
// faster than 64B on AMD Zen (EPYC 7702/7702P); 256B+ over-spreads and loses.
#ifndef FQ_CTL_ALIGN
#define FQ_CTL_ALIGN (L1_CACHE_LNE * 2)  // x86: 128B pair per field (defeats the 2-line spatial prefetcher)
#endif

template<typename T, uint64_t RING_BUFFER_SIZE, uint64_t L1_CACHE_LNE>
class FastQueue {
    static_assert(sizeof(T) == 8, "Only 64 bit objects are supported");
    static_assert(sizeof(void*) == 8, "The architecture is not 64-bits");
    static_assert((RING_BUFFER_SIZE & (RING_BUFFER_SIZE + 1)) == 0, "RING_BUFFER_SIZE must be a number of contiguous bits set from LSB. Example: 0b00001111 not 0b01001111");
    static constexpr uint64_t CAP = RING_BUFFER_SIZE + 1;   // power of two
    static constexpr uint64_t MASK = RING_BUFFER_SIZE;
    // aligned_alloc requires the size to be a multiple of the alignment.
    static constexpr uint64_t BUF_BYTES = ((CAP * sizeof(T) + FQ_CTL_ALIGN - 1) / FQ_CTL_ALIGN) * FQ_CTL_ALIGN;
public:
    FastQueue() noexcept {
        mRingBuffer = static_cast<T*>(aligned_alloc(FQ_CTL_ALIGN, BUF_BYTES));
        for (uint64_t i = 0; i < CAP; i++) mRingBuffer[i] = nullptr;
    }
    ~FastQueue() { std::free(mRingBuffer); }
    FastQueue(const FastQueue&) = delete;
    FastQueue& operator=(const FastQueue&) = delete;

    template<typename... Args>
    inline void push(Args&&... args) noexcept {
        const uint64_t w = mWriteIndex.load(std::memory_order_relaxed);
        if (w - mReadIndexCache >= CAP) [[unlikely]] {          // cache says full - verify
            do {
                mReadIndexCache = mReadIndex.load(std::memory_order_acquire);
                if (w - mReadIndexCache < CAP) break;
                if (mExitThreadSemaphore.load(std::memory_order_relaxed)) [[unlikely]] return;
            } while (true);
        }
        mRingBuffer[w & MASK] = T{std::forward<Args>(args)...};
        mWriteIndex.store(w + 1, std::memory_order_release);
    }

    inline void pop(T& aOut) noexcept {
        const uint64_t r = mReadIndex.load(std::memory_order_relaxed);
        if (r == mWriteIndexCache) [[unlikely]] {               // cache says empty - verify
            do {
                mWriteIndexCache = mWriteIndex.load(std::memory_order_acquire);
                if (r != mWriteIndexCache) break;
                if (mExitThreadSemaphore.load(std::memory_order_acquire)) [[unlikely]] {
                    // Stopped: re-read the write index (acquire) so any items the
                    // producer published just before stopping are still drained.
                    mWriteIndexCache = mWriteIndex.load(std::memory_order_acquire);
                    if (r == mWriteIndexCache) { aOut = nullptr; return; }
                    break;
                }
            } while (true);
        }
        aOut = mRingBuffer[r & MASK];
        mReadIndex.store(r + 1, std::memory_order_release);
    }

    //Stop queue (Maybe called from any thread)
    void stopQueue() {
        mExitThreadSemaphore.store(true, std::memory_order_release);
    }

private:
    // Producer-owned line: its write index + a cached copy of the read index.
    alignas(FQ_CTL_ALIGN) std::atomic<uint64_t> mWriteIndex = 0;
    uint64_t mReadIndexCache = 0;
    // Consumer-owned line: its read index + a cached copy of the write index.
    alignas(FQ_CTL_ALIGN) std::atomic<uint64_t> mReadIndex = 0;
    uint64_t mWriteIndexCache = 0;
    // Shared, but only touched when a side runs dry / full.
    alignas(FQ_CTL_ALIGN) std::atomic<bool> mExitThreadSemaphore = false;
    // Heap ring, packed: one cache line carries many elements, producer -> consumer.
    alignas(FQ_CTL_ALIGN) T* mRingBuffer = nullptr;
};

//
// Created by Anders Cedronius on 2023-06-27.
//

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <span>
#include <utility>

// FastQueue2 - bounded lock-free SPSC queue for 8-byte objects.
//
// Default design: cached-index SPSC ring. Each side keeps a private cached copy
// of peer index and only reads peer-owned cache line after cached state says
// full/empty. Producer publishes payload with release write index; consumer
// acquires index before reading payload. Data slots move producer -> consumer
// only, so neither side clears a consumed slot.
//
// FQ_WRAPPED_INDICES selects indexes modulo 2 * capacity. It retains all CAP
// usable slots: high phase bit distinguishes full from empty, while low bits
// address ring. Default monotonic counters have no practical wrap. Wrapped
// mode exists for x86 occupancy-regime experiments and can change coherence
// traffic. Define FQ_WRAPPED_INDICES=0 or FQ_CONSUMER_CUSHION=0 before this
// header for baseline experiments.
// x86 default: modulo 2*capacity indexes. Low bits address ring; phase bit
// distinguishes full from empty. This lets consumer build a stable work batch
// on Zen2 instead of repeatedly refreshing a peer-owned index at occupancy 1.
// Wrapped mode and a batch cushion are Zen2 throughput policies. GCC/Clang
// expose tune macros under -march=native, so Haswell gets immediate monotonic
// indexes automatically. Define either macro explicitly to override profile.
#ifndef FQ_WRAPPED_INDICES
#if defined(__znver2__) || defined(__znver2)
#define FQ_WRAPPED_INDICES 1
#else
#define FQ_WRAPPED_INDICES 0
#endif
#endif

// FQ_CONSUMER_CUSHION delays consumer only after it observed an empty queue,
// until producer has published this many entries. It biases an otherwise
// near-empty queue toward cached-index hits. Zero preserves normal immediate
// consumer behavior. Stop always bypasses cushion so producer's final items
// drain without deadlock.
// x86 default: drain in small batches after an empty refresh. This maintains
// producer lead, sharply cuts peer-index HITM reads on Zen2, and stop bypasses
// cushion so final partial batch always drains.
#ifndef FQ_CONSUMER_CUSHION
#if defined(__znver2__) || defined(__znver2)
#define FQ_CONSUMER_CUSHION 6
#else
#define FQ_CONSUMER_CUSHION 0
#endif
#endif

// FQ_OCCUPANCY_INSTRUMENT records occupancy at consumer empty-cache refreshes.
// It is deliberately compile-time opt-in: atomics perturb exact throughput.
#ifndef FQ_OCCUPANCY_INSTRUMENT
#define FQ_OCCUPANCY_INSTRUMENT 0
#endif

// FQ_CTL_ALIGN separates independently-written fields. x86 L2 spatial
// prefetcher fetches 128-byte pairs, so producer and consumer indices need
// separate pairs to avoid pulling peer index into local cache.
#ifndef FQ_CTL_ALIGN
#define FQ_CTL_ALIGN (L1_CACHE_LNE * 2)
#endif

template<typename T, uint64_t RING_BUFFER_SIZE, uint64_t L1_CACHE_LNE>
class FastQueue {
    static_assert(sizeof(T) == 8, "Only 64 bit objects are supported");
    static_assert(sizeof(void*) == 8, "The architecture is not 64-bits");
    static_assert((RING_BUFFER_SIZE & (RING_BUFFER_SIZE + 1)) == 0,
                  "RING_BUFFER_SIZE must be contiguous low bits. Example: 0b1111");

    static constexpr uint64_t CAP = RING_BUFFER_SIZE + 1;
    static constexpr uint64_t MASK = RING_BUFFER_SIZE;
#if FQ_WRAPPED_INDICES
    static_assert(CAP <= (UINT64_MAX >> 1), "Wrapped-index capacity is too large");
    static constexpr uint64_t INDEX_WRAP_MASK = (CAP << 1) - 1;
#endif
    static_assert(CAP >= 2, "Ring capacity must hold at least two entries");

public:
    struct OccupancyStats {
        uint64_t samples;
        uint64_t total;
        uint64_t nearEmpty;
        uint64_t maximum;
    };

    FastQueue() noexcept = default;
    FastQueue(const FastQueue&) = delete;
    FastQueue& operator=(const FastQueue&) = delete;

    template<typename... Args>
    inline void push(Args&&... args) noexcept {
        const uint64_t w = mWriteIndex.load(std::memory_order_relaxed);
        if (distance(w, mReadIndexCache) >= CAP) [[unlikely]] {
            do {
                mReadIndexCache = mReadIndex.load(std::memory_order_acquire);
                if (distance(w, mReadIndexCache) < CAP) break;
                if (mExitThreadSemaphore.load(std::memory_order_relaxed)) [[unlikely]] return;
            } while (true);
        }
        mRingBuffer[w & MASK] = T{std::forward<Args>(args)...};
        mWriteIndex.store(nextIndex(w), std::memory_order_release);
    }

    inline void pop(T& aOut) noexcept {
        const uint64_t r = mReadIndex.load(std::memory_order_relaxed);
        if (r == mWriteIndexCache) [[unlikely]] {
            do {
                mWriteIndexCache = mWriteIndex.load(std::memory_order_acquire);
                const uint64_t occupancy = distance(mWriteIndexCache, r);
                recordOccupancy(occupancy);
                if (occupancy != 0 && cushionSatisfied(occupancy)) break;
                if (mExitThreadSemaphore.load(std::memory_order_acquire)) [[unlikely]] {
                    // Acquire final index before declaring drain complete. Cushion is
                    // bypassed after stop: final partial batch must not strand.
                    mWriteIndexCache = mWriteIndex.load(std::memory_order_acquire);
                    if (r == mWriteIndexCache) { aOut = nullptr; return; }
                    break;
                }
            } while (true);
        }
        aOut = mRingBuffer[r & MASK];
        mReadIndex.store(nextIndex(r), std::memory_order_release);
    }

    inline std::size_t tryPushBulk(std::span<const T> items) noexcept {
        const uint64_t requested = items.size() < 8 ? items.size() : 8;
        if (requested == 0) return 0;

        const uint64_t w = mWriteIndex.load(std::memory_order_relaxed);
        uint64_t free = CAP - distance(w, mReadIndexCache);
        if (free < requested) [[unlikely]] {
            mReadIndexCache = mReadIndex.load(std::memory_order_acquire);
            free = CAP - distance(w, mReadIndexCache);
            if (free == 0) return 0;
        }
        const uint64_t count = requested < free ? requested : free;
        copyIntoRing(w, items.data(), count);
        mWriteIndex.store(advanceIndex(w, count), std::memory_order_release);
        return count;
    }

    inline std::size_t tryPopBulk(std::span<T> output) noexcept {
        const uint64_t requested = output.size() < 8 ? output.size() : 8;
        if (requested == 0) return 0;

        const uint64_t r = mReadIndex.load(std::memory_order_relaxed);
        uint64_t available = distance(mWriteIndexCache, r);
        if (available < requested) [[unlikely]] {
            mWriteIndexCache = mWriteIndex.load(std::memory_order_acquire);
            available = distance(mWriteIndexCache, r);
            if (available == 0) return 0;
        }
        const uint64_t count = requested < available ? requested : available;
        copyFromRing(r, output.data(), count);
        mReadIndex.store(advanceIndex(r, count), std::memory_order_release);
        return count;
    }

    // Stop may be called by any thread. Consumer drains all published entries.
    void stopQueue() noexcept {
        mExitThreadSemaphore.store(true, std::memory_order_release);
    }

    [[nodiscard]] OccupancyStats occupancyStats() const noexcept {
#if FQ_OCCUPANCY_INSTRUMENT
        return {mOccupancySamples.load(std::memory_order_relaxed),
                mOccupancyTotal.load(std::memory_order_relaxed),
                mNearEmptySamples.load(std::memory_order_relaxed),
                mOccupancyMaximum.load(std::memory_order_relaxed)};
#else
        return {0, 0, 0, 0};
#endif
    }

private:
    static constexpr uint64_t nextIndex(uint64_t index) noexcept {
#if FQ_WRAPPED_INDICES
        return (index + 1) & INDEX_WRAP_MASK;
#else
        return index + 1;
#endif
    }

    static constexpr uint64_t advanceIndex(uint64_t index, uint64_t count) noexcept {
#if FQ_WRAPPED_INDICES
        return (index + count) & INDEX_WRAP_MASK;
#else
        return index + count;
#endif
    }

    void copyIntoRing(uint64_t index, const T* input, uint64_t count) noexcept {
        for (uint64_t i = 0; i < count; ++i) mRingBuffer[(index + i) & MASK] = input[i];
    }

    void copyFromRing(uint64_t index, T* output, uint64_t count) noexcept {
        for (uint64_t i = 0; i < count; ++i) output[i] = mRingBuffer[(index + i) & MASK];
    }

    static constexpr uint64_t distance(uint64_t newer, uint64_t older) noexcept {
#if FQ_WRAPPED_INDICES
        return (newer - older) & INDEX_WRAP_MASK;
#else
        return newer - older;
#endif
    }

    static constexpr uint64_t cushionThreshold() noexcept {
#if FQ_CONSUMER_CUSHION == 0
        return 1;
#else
        // Tiny queues cannot accumulate default batch. Waiting for CAP retains
        // forward progress and stop still drains final partial batch.
        return FQ_CONSUMER_CUSHION < CAP ? FQ_CONSUMER_CUSHION : CAP;
#endif
    }

    static constexpr bool cushionSatisfied(uint64_t occupancy) noexcept {
        return occupancy >= cushionThreshold();
    }

    inline void recordOccupancy(uint64_t occupancy) noexcept {
#if FQ_OCCUPANCY_INSTRUMENT
        mOccupancySamples.fetch_add(1, std::memory_order_relaxed);
        mOccupancyTotal.fetch_add(occupancy, std::memory_order_relaxed);
        if (occupancy <= 1) mNearEmptySamples.fetch_add(1, std::memory_order_relaxed);
        uint64_t maximum = mOccupancyMaximum.load(std::memory_order_relaxed);
        while (maximum < occupancy &&
               !mOccupancyMaximum.compare_exchange_weak(maximum, occupancy,
                                                        std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {}
#else
        (void) occupancy;
#endif
    }

    // Producer-owned line: write index + cached consumer index.
    alignas(FQ_CTL_ALIGN) std::atomic<uint64_t> mWriteIndex = 0;
    uint64_t mReadIndexCache = 0;
    // Consumer-owned line: read index + cached producer index.
    alignas(FQ_CTL_ALIGN) std::atomic<uint64_t> mReadIndex = 0;
    uint64_t mWriteIndexCache = 0;
    // Shared only when a side runs dry/full.
    alignas(FQ_CTL_ALIGN) std::atomic<bool> mExitThreadSemaphore = false;
#if FQ_OCCUPANCY_INSTRUMENT
    alignas(FQ_CTL_ALIGN) std::atomic<uint64_t> mOccupancySamples = 0;
    std::atomic<uint64_t> mOccupancyTotal = 0;
    std::atomic<uint64_t> mNearEmptySamples = 0;
    std::atomic<uint64_t> mOccupancyMaximum = 0;
#endif
    // x86 inline ring removes allocation/pointer indirection. Slots are read
    // only after producer published matching index, so no slot initialization.
    alignas(FQ_CTL_ALIGN) std::array<T, CAP> mRingBuffer;
};

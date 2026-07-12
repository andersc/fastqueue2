//
// Created by Anders Cedronius on 2023-06-27.
//

#pragma once

#include <array>
#include <cstdint>
#include <atomic>
#include <cstdlib>
#include <span>
#include <utility>

// FastQueue2 - bounded lock-free SPSC queue for 8-byte objects.
//
// Producer and consumer own separate indexes and cache peer indexes locally.
// A peer index is acquired only when cached state reports full or empty. Payload
// publication uses release/acquire index handoff; payload accesses stay plain.
//
// FQ_CTL_ALIGN separates independently-written control fields. Apple L2/SLC
// coherence uses 128-byte lines, so 128 bytes is minimum safe separation.
#ifndef FQ_CTL_ALIGN
#define FQ_CTL_ALIGN (L1_CACHE_LNE * 4)
#endif

// ARM experiments:
//   FQ_ARM_RING_INLINE  1 = queue-owned contiguous ring, 0 = separately allocated ring.
#ifndef FQ_ARM_RING_INLINE
#define FQ_ARM_RING_INLINE 1
#endif

template<typename T, uint64_t RING_BUFFER_SIZE, uint64_t L1_CACHE_LNE>
class FastQueue {
    static_assert(sizeof(T) == 8, "Only 64 bit objects are supported");
    static_assert(sizeof(void*) == 8, "The architecture is not 64-bits");
    static_assert((RING_BUFFER_SIZE & (RING_BUFFER_SIZE + 1)) == 0,
                  "RING_BUFFER_SIZE must be a number of contiguous bits set from LSB. Example: 0b00001111 not 0b01001111");

    static constexpr uint64_t CAP = RING_BUFFER_SIZE + 1;
    static constexpr uint64_t MASK = RING_BUFFER_SIZE;
#if !FQ_ARM_RING_INLINE
    static constexpr uint64_t BUF_BYTES =
        ((CAP * sizeof(T) + FQ_CTL_ALIGN - 1) / FQ_CTL_ALIGN) * FQ_CTL_ALIGN;
#endif

public:
    FastQueue() noexcept {
#if FQ_ARM_RING_INLINE
        // Slots are written before publication and never read before an index
        // handoff, so value initialization is unnecessary on this hot-path type.
#else
        mRingBuffer = static_cast<T*>(aligned_alloc(FQ_CTL_ALIGN, BUF_BYTES));
        for (uint64_t i = 0; i < CAP; ++i) mRingBuffer[i] = nullptr;
#endif
    }

    ~FastQueue() {
#if !FQ_ARM_RING_INLINE
        std::free(mRingBuffer);
#endif
    }

    FastQueue(const FastQueue&) = delete;
    FastQueue& operator=(const FastQueue&) = delete;

    // Nonblocking SPSC operations. Use these where caller owns retry policy.
    // They match other benchmarked queues: false reports only current full/empty
    // state, with no stop-flag traffic on successful or retry paths.
    template<typename... Args>
    inline bool tryPush(Args&&... args) noexcept {
        const uint64_t w = mWriteIndex.load(std::memory_order_relaxed);
        if (w - mReadIndexCache >= CAP) [[unlikely]] {
            mReadIndexCache = mReadIndex.load(std::memory_order_acquire);
            if (w - mReadIndexCache >= CAP) return false;
        }
        slot(w) = T{std::forward<Args>(args)...};
        mWriteIndex.store(w + 1, std::memory_order_release);
        return true;
    }

    inline bool tryPop(T& aOut) noexcept {
        const uint64_t r = mReadIndex.load(std::memory_order_relaxed);
        if (r == mWriteIndexCache) [[unlikely]] {
            mWriteIndexCache = mWriteIndex.load(std::memory_order_acquire);
            if (r == mWriteIndexCache) return false;
        }
        aOut = slot(r);
        mReadIndex.store(r + 1, std::memory_order_release);
        return true;
    }

    inline std::size_t tryPushBulk(std::span<const T> items) noexcept {
        const uint64_t requested = items.size() < 8 ? items.size() : 8;
        if (requested == 0) return 0;

        const uint64_t w = mWriteIndex.load(std::memory_order_relaxed);
        uint64_t free = CAP - (w - mReadIndexCache);
        if (free < requested) [[unlikely]] {
            mReadIndexCache = mReadIndex.load(std::memory_order_acquire);
            free = CAP - (w - mReadIndexCache);
            if (free == 0) return 0;
        }
        const uint64_t count = requested < free ? requested : free;
        copyIntoRing(w, items.data(), count);
        mWriteIndex.store(w + count, std::memory_order_release);
        return count;
    }

    inline std::size_t tryPopBulk(std::span<T> output) noexcept {
        const uint64_t requested = output.size() < 8 ? output.size() : 8;
        if (requested == 0) return 0;

        const uint64_t r = mReadIndex.load(std::memory_order_relaxed);
        uint64_t available = mWriteIndexCache - r;
        if (available < requested) [[unlikely]] {
            mWriteIndexCache = mWriteIndex.load(std::memory_order_acquire);
            available = mWriteIndexCache - r;
            if (available == 0) return 0;
        }
        const uint64_t count = requested < available ? requested : available;
        copyFromRing(r, output.data(), count);
        mReadIndex.store(r + count, std::memory_order_release);
        return count;
    }

    template<typename... Args>
    inline void push(Args&&... args) noexcept {
        while (!tryPush(std::forward<Args>(args)...)) {
            if (mExitThreadSemaphore.load(std::memory_order_relaxed)) [[unlikely]] return;
        }
    }

    inline void pop(T& aOut) noexcept {
        while (!tryPop(aOut)) {
            if (mExitThreadSemaphore.load(std::memory_order_acquire)) [[unlikely]] {
                // One final acquire checks payload published before producer stop.
                if (!tryPop(aOut)) aOut = nullptr;
                return;
            }
        }
    }

    // May be called from any thread. Existing blocking API uses this only to
    // terminate a blocked producer/consumer; nonblocking operations do not.
    void stopQueue() noexcept {
        mExitThreadSemaphore.store(true, std::memory_order_release);
    }

private:
    inline T& slot(uint64_t index) noexcept {
#if FQ_ARM_RING_INLINE
        return mRingBuffer[index & MASK];
#else
        return mRingBuffer[index & MASK];
#endif
    }

    void copyIntoRing(uint64_t index, const T* input, uint64_t count) noexcept {
        for (uint64_t i = 0; i < count; ++i) slot(index + i) = input[i];
    }

    void copyFromRing(uint64_t index, T* output, uint64_t count) noexcept {
        for (uint64_t i = 0; i < count; ++i) output[i] = slot(index + i);
    }

    // Producer-owned line.
    alignas(FQ_CTL_ALIGN) std::atomic<uint64_t> mWriteIndex = 0;
    uint64_t mReadIndexCache = 0;
    // Consumer-owned line.
    alignas(FQ_CTL_ALIGN) std::atomic<uint64_t> mReadIndex = 0;
    uint64_t mWriteIndexCache = 0;
    // Cold shutdown state.
    alignas(FQ_CTL_ALIGN) std::atomic<bool> mExitThreadSemaphore = false;
#if FQ_ARM_RING_INLINE
    // Inline form gives Apple prefetcher same layout option as Deaod.
    alignas(FQ_CTL_ALIGN) std::array<T, CAP> mRingBuffer;
#else
    // Split form isolates streaming payload from control state.
    alignas(FQ_CTL_ALIGN) T* mRingBuffer = nullptr;
#endif
};

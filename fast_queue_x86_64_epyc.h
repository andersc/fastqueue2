// FastQueue2 opt-in slot-signaling SPSC queue for tested Zen2 EPYC systems.
//
// Each atomic pointer slot is both payload and full/empty state. Consecutive
// logical positions are spread across cache lines when capacity permits. This
// policy is separate from FastQueue2's default cached-index implementation.
//
// Contract:
// - one producer and one consumer;
// - pointer payloads only;
// - nullptr is reserved as empty and cannot be queued;
// - scalar push/pop/tryPush/tryPop only;
// - fixed-count users must consume their known item count; no stopQueue API.

#pragma once

#if !defined(__x86_64__) && !defined(_M_X64)
#error fast_queue_x86_64_epyc.h requires x86_64
#endif

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <immintrin.h>

template<typename T, uint64_t RING_BUFFER_SIZE, uint64_t L1_CACHE_LINE>
class FastQueueEpyc {
    static_assert(std::is_pointer_v<T>,
                  "slot-signaling queue supports pointer payloads only");
    static_assert(sizeof(T) == 8, "Only 64-bit pointers are supported");
    static_assert(sizeof(void*) == 8, "Architecture must use 64-bit pointers");
    static_assert(RING_BUFFER_SIZE != UINT64_MAX,
                  "RING_BUFFER_SIZE + 1 must not overflow");
    static_assert(RING_BUFFER_SIZE <= UINT32_MAX,
                  "slot-signaling queue supports at most 2^32 slots");
    static_assert((RING_BUFFER_SIZE & (RING_BUFFER_SIZE + 1)) == 0,
                  "RING_BUFFER_SIZE must be contiguous low bits");
    static_assert(L1_CACHE_LINE >= sizeof(T) &&
                  (L1_CACHE_LINE & (L1_CACHE_LINE - 1)) == 0,
                  "L1 cache-line size must be a power of two");

    static constexpr uint64_t CAP = RING_BUFFER_SIZE + 1;
    static constexpr uint64_t MASK = RING_BUFFER_SIZE;
    static constexpr uint64_t ELEMENTS_PER_LINE = L1_CACHE_LINE / sizeof(T);
    static_assert((CAP & (CAP - 1)) == 0, "Capacity must be a power of two");
    static_assert((ELEMENTS_PER_LINE & (ELEMENTS_PER_LINE - 1)) == 0,
                  "Pointers per cache line must be a power of two");

    static consteval unsigned shuffleBits() noexcept {
        unsigned bits = 0;
        for (uint64_t n = ELEMENTS_PER_LINE; n > 1; n >>= 1) ++bits;
        return bits;
    }

    static constexpr unsigned SHUFFLE_BITS = shuffleBits();
    static constexpr uint64_t ELEMENT_MASK = ELEMENTS_PER_LINE - 1;
    static constexpr uint64_t LINE_MASK = ELEMENT_MASK << SHUFFLE_BITS;
    static constexpr bool SHUFFLE = CAP >= ELEMENTS_PER_LINE * ELEMENTS_PER_LINE;

    static inline uint64_t physicalIndex(uint32_t logical) noexcept {
        const uint32_t index = logical & static_cast<uint32_t>(MASK);
        if constexpr (!SHUFFLE) return index;
#if defined(__BMI__) && defined(__BMI2__) && (defined(__GNUC__) || defined(__clang__))
        unsigned control = SHUFFLE_BITS | (SHUFFLE_BITS << 8);
        asm("" : "+r"(control));
        unsigned element = __bextr_u32(index, control);
        unsigned line = _bzhi_u32(index, SHUFFLE_BITS) << SHUFFLE_BITS;
        element |= index & static_cast<unsigned>(~(ELEMENT_MASK | LINE_MASK));
        asm("" : : "r"(element), "r"(line));
        return element | line;
#else
        return ((index >> SHUFFLE_BITS) & ELEMENT_MASK) |
               ((index & ELEMENT_MASK) << SHUFFLE_BITS) |
               (index & ~(ELEMENT_MASK | LINE_MASK));
#endif
    }

    static inline void pause() noexcept { _mm_pause(); }

    [[noreturn]] static void rejectNull() noexcept {
        assert(false && "nullptr is reserved as empty sentinel");
        std::abort();
    }

public:
    FastQueueEpyc() noexcept {
        for (auto& slot : mRingBuffer) slot.store(nullptr, std::memory_order_relaxed);
    }

    FastQueueEpyc(const FastQueueEpyc&) = delete;
    FastQueueEpyc& operator=(const FastQueueEpyc&) = delete;

    template<typename... Args>
    inline void push(Args&&... args) noexcept {
        T value{std::forward<Args>(args)...};
        if (value == nullptr) [[unlikely]] rejectNull();

        const uint32_t position = mWritePosition++;
        auto& slot = mRingBuffer[physicalIndex(position)];
        while (slot.load(std::memory_order_acquire) != nullptr) pause();
        slot.store(value, std::memory_order_release);
    }

    inline void pop(T& out) noexcept {
        const uint32_t position = mReadPosition++;
        auto& slot = mRingBuffer[physicalIndex(position)];
        T value;
        while ((value = slot.load(std::memory_order_acquire)) == nullptr) pause();
        out = value;
        slot.store(nullptr, std::memory_order_release);
    }

    [[nodiscard]] inline bool tryPush(T value) noexcept {
        if (value == nullptr) [[unlikely]] rejectNull();

        auto& slot = mRingBuffer[physicalIndex(mWritePosition)];
        if (slot.load(std::memory_order_acquire) != nullptr) return false;
        slot.store(value, std::memory_order_release);
        ++mWritePosition;
        return true;
    }

    [[nodiscard]] inline bool tryPop(T& out) noexcept {
        auto& slot = mRingBuffer[physicalIndex(mReadPosition)];
        const T value = slot.load(std::memory_order_acquire);
        if (value == nullptr) return false;
        out = value;
        slot.store(nullptr, std::memory_order_release);
        ++mReadPosition;
        return true;
    }

    inline void stopQueue() noexcept = delete;

private:
    alignas(L1_CACHE_LINE * 2) uint32_t mWritePosition = 0;
    alignas(L1_CACHE_LINE * 2) uint32_t mReadPosition = 0;
    alignas(L1_CACHE_LINE) std::array<std::atomic<T>, CAP> mRingBuffer;
};

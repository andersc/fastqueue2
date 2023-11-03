//
// Created by Anders Cedronius on 2023-06-27.
//

#pragma once

#include <iostream>
#include <cstdint>
#include <atomic>
#include <bitset>
//#include <arm_acle.h>

template<typename T, uint64_t RING_BUFFER_SIZE, uint64_t L1_CACHE_LNE>
class FastQueue {
    static_assert(sizeof(T) == 8, "Only 64 bit objects are supported");
    static_assert(sizeof(void*) == 8, "The architecture is not 64-bits");
    static_assert((RING_BUFFER_SIZE & (RING_BUFFER_SIZE + 1)) == 0, "RING_BUFFER_SIZE must be a number of contiguous bits set from LSB. Example: 0b00001111 not 0b01001111");
public:
    template<typename... Args>
    inline void push(Args&&... args) noexcept {
        while(mRingBuffer[mWritePosition&RING_BUFFER_SIZE].mObj != nullptr) if (mExitThreadSemaphore) [[unlikely]] return;
        new(&mRingBuffer[mWritePosition++&RING_BUFFER_SIZE].mObj) T{std::forward<Args>(args)...};
    }

    inline void pop(T& aOut) noexcept {
        std::atomic_thread_fence(std::memory_order_consume);
        while (!(aOut = mRingBuffer[mReadPosition & RING_BUFFER_SIZE].mObj)) {
            if (mExitThread == mReadPosition) [[unlikely]] {
                aOut = nullptr;
                return;
            }
        }
        mRingBuffer[mReadPosition++ & RING_BUFFER_SIZE].mObj = nullptr;
        //__builtin_prefetch(aOut, 1);
        //__pldx(0, 0, 1, (const void*)aOut);
    }

    //Stop queue (Maybe called from any thread)
    void stopQueue() {
        mExitThread = mWritePosition;
        mExitThreadSemaphore = true;
    }

private:
    struct AlignedDataObjects {
        alignas(L1_CACHE_LNE) T mObj = nullptr;
    };
    alignas(L1_CACHE_LNE) volatile std::atomic<uint64_t> mReadPosition = 1;
    alignas(L1_CACHE_LNE) volatile std::atomic<uint64_t> mWritePosition = 1;
    alignas(L1_CACHE_LNE) volatile uint64_t mExitThread = 0;
    alignas(L1_CACHE_LNE) volatile bool mExitThreadSemaphore = false;
    alignas(L1_CACHE_LNE) std::array<AlignedDataObjects, RING_BUFFER_SIZE+1> mRingBuffer;
    alignas(L1_CACHE_LNE) volatile uint8_t mBorderDown[L1_CACHE_LNE]{};
};
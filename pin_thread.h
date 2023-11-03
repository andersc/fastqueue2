//
// Created by Anders Cedronius
//

#pragma once

#ifdef __APPLE__
    #include <TargetConditionals.h>
    #ifdef TARGET_OS_MAC

#include <sys/types.h>
#include <sys/sysctl.h>
#import <mach/thread_act.h>

#define SYSCTL_CORE_COUNT   "machdep.cpu.core_count"

typedef struct cpu_set {
    uint32_t    count;
} cpu_set_t;

static inline void
CPU_ZERO(cpu_set_t *cs) { cs->count = 0; }

static inline void
CPU_SET(int num, cpu_set_t *cs) { cs->count |= (1 << num); }

static inline int
CPU_ISSET(int num, cpu_set_t *cs) { return (cs->count & (1 << num)); }

bool raise_thread_priority() {
    /* raise the thread's priority */
    thread_extended_policy_data_t extendedPolicy;
    thread_act_t this_thread = pthread_mach_thread_np(pthread_self());

    extendedPolicy.timeshare = 0;
    kern_return_t error = thread_policy_set(this_thread, THREAD_EXTENDED_POLICY,
                              (thread_policy_t)&extendedPolicy,
                              THREAD_EXTENDED_POLICY_COUNT);
    if (error != KERN_SUCCESS) {
        std::cout << "Couldn't set thread timeshare policy" << std::endl;
        return false;
    }
    return true;
}

int sched_getaffinity(pid_t pid, size_t cpu_size, cpu_set_t *cpu_set)
{
    int32_t core_count = 0;
    size_t  len = sizeof(core_count);
    int ret = sysctlbyname(SYSCTL_CORE_COUNT, &core_count, &len, 0, 0);
    if (ret) {
        return -1;
    }
    cpu_set->count = 0;
    for (int i = 0; i < core_count; i++) {
        cpu_set->count |= (1 << i);
    }
    return 0;
}

int pthread_setaffinity_np(pthread_t thread, size_t cpu_size,
                           cpu_set_t *cpu_set) {
    thread_port_t mach_thread;
    int core = 0;

    for (core = 0; core < 8 * cpu_size; core++) {
        if (CPU_ISSET(core, cpu_set)) break;
    }
    thread_affinity_policy_data_t policy = { core };
    mach_thread = pthread_mach_thread_np(thread);
    thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY,
                      (thread_policy_t)&policy, 1);
    return 0;
}

bool pinThread(int32_t aCpu) {
    if (aCpu < 0) {
        return false;
    }
    cpu_set_t lCpuSet;
    CPU_ZERO(&lCpuSet);
    CPU_SET(aCpu, &lCpuSet);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &lCpuSet)) {
        return false;
    }
    //return raise_thread_priority();
    return true;
}



    #else
    #error Only MacOS supported
    #endif
#elif defined _WIN64
#include <Windows.h>
bool pinThread(int32_t aCpu) {
    if (aCpu > 64) {
        throw std::runtime_error("Support for more than 64 CPU's under Windows is not implemented.");
    }
    HANDLE lThread = GetCurrentThread();
    DWORD_PTR lThreadAffinityMask = 1ULL << aCpu;
    DWORD_PTR lReturn = SetThreadAffinityMask(lThread, lThreadAffinityMask);
    if (lReturn) {
        return true;
    }
    return false;
}
#elif  __linux
bool pinThread(int32_t aCpu) {
    if (aCpu < 0) {
        return false;
    }
    cpu_set_t lCpuSet;
    CPU_ZERO(&lCpuSet);
    CPU_SET(aCpu, &lCpuSet);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &lCpuSet)) {
        return false;
    }
    return true;
}
#else
#error OS not supported
#endif


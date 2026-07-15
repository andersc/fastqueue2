#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if __x86_64__ || _M_X64
#include "fast_queue_x86_64.h"
#elif __aarch64__ || _M_ARM64
#include "fast_queue_arm64.h"
#else
#error FastQueueTopologyMatrix supports existing x86_64 and arm64 queue backends only.
#endif

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

namespace {
constexpr uint64_t kRingMask = 1023;
using Item = void*;
using Queue = FastQueue<Item, kRingMask, 64>;
using Clock = std::chrono::steady_clock;

struct CpuInfo { int id; int socket; int core; int smt; };
struct Options {
    std::string output = "topology-matrix.csv";
    uint64_t transfers = 2162160; // divisible by every width 1..16
    int rounds = 3;
    int warmups = 1;
    int maxCpus = 0; // zero = all CPUs allowed to this process
};
struct Sample { bool pinned; double mps; };

std::string readText(const std::string& path) {
    std::ifstream f(path); std::string s; std::getline(f, s); return s;
}
int readInt(const std::string& path, int fallback = -1) {
    try { return std::stoi(readText(path)); } catch (...) { return fallback; }
}

std::vector<CpuInfo> availableCpus(int maxCpus) {
    std::vector<CpuInfo> result;
#if defined(__linux__)
    long configured = sysconf(_SC_NPROCESSORS_CONF);
    int maxCpu = configured > 0 ? static_cast<int>(configured) : 256;
    cpu_set_t* set = nullptr;
    size_t bytes = 0;
    for (;;) {
        if (set) CPU_FREE(set);
        set = CPU_ALLOC(maxCpu); bytes = CPU_ALLOC_SIZE(maxCpu);
        if (!set) { std::cerr << "Cannot allocate Linux CPU affinity mask\n"; return result; }
        CPU_ZERO_S(bytes, set);
        if (sched_getaffinity(0, bytes, set) == 0) break;
        if (errno != EINVAL || maxCpu > (1 << 20)) { CPU_FREE(set); std::cerr << "Cannot read Linux CPU affinity mask\n"; return result; }
        maxCpu *= 2;
    }
    for (int cpu = 0; cpu < maxCpu; ++cpu) if (CPU_ISSET_S(cpu, bytes, set)) {
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        const int socket = readInt(base + "physical_package_id");
        const int core = readInt(base + "core_id");
        int smt = 0;
        const std::string siblings = readText(base + "thread_siblings_list");
        if (!siblings.empty()) { const auto dash = siblings.find('-'); smt = dash == std::string::npos ? 0 : cpu - std::stoi(siblings.substr(0, dash)); }
        result.push_back({cpu, socket, core, smt});
        if (maxCpus > 0 && static_cast<int>(result.size()) >= maxCpus) break;
    }
    CPU_FREE(set);
#elif defined(__APPLE__)
    const unsigned n = std::thread::hardware_concurrency();
    for (unsigned cpu = 0; cpu < n && (maxCpus == 0 || static_cast<int>(cpu) < maxCpus); ++cpu)
        result.push_back({static_cast<int>(cpu), -1, -1, -1});
#else
    #error Unsupported OS
#endif
    return result;
}

bool pinCurrentThread(int cpu) {
#if defined(__linux__)
    if (cpu < 0 || cpu >= CPU_SETSIZE) return false;
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(__APPLE__)
    // macOS affinity tags bias co-scheduling; they do not guarantee a CPU.
    (void)cpu; return false;
#else
    return false;
#endif
}

inline void spinPause() { std::this_thread::yield(); }

template<size_t N>
Sample runBatch(int producerCpu, int consumerCpu, uint64_t transfers) {
    Queue q;
    std::atomic<bool> go{false}, producerPinned{false}, consumerPinned{false};
    std::atomic<bool> failed{false};
    std::thread consumer([&] {
        consumerPinned.store(pinCurrentThread(consumerCpu), std::memory_order_relaxed);
        while (!go.load(std::memory_order_acquire)) spinPause();
        uint64_t expected = 0;
        FastQueueBatch<Item> batch{};
        while (expected < transfers) {
            const auto got = q.template tryPopBatch<N>(batch);
            if (!got) { spinPause(); continue; }
            for (size_t i = 0; i < got; ++i)
                if (reinterpret_cast<uintptr_t>(batch.items[i]) != expected++ + 1) failed.store(true, std::memory_order_relaxed);
        }
    });
    std::thread producer([&] {
        producerPinned.store(pinCurrentThread(producerCpu), std::memory_order_relaxed);
        while (!go.load(std::memory_order_acquire)) spinPause();
        uint64_t sent = 0;
        FastQueueBatch<Item> batch{};
        while (sent < transfers) {
            for (size_t i = 0; i < N; ++i) batch.items[i] = reinterpret_cast<Item>(static_cast<uintptr_t>(sent + i + 1));
            size_t offset = 0;
            while (offset < N) {
                const size_t moved = q.template tryPushBatch<N>(batch, offset);
                if (!moved) { spinPause(); continue; }
                offset += moved;
            }
            sent += N;
        }
    });
    const auto begin = Clock::now(); go.store(true, std::memory_order_release);
    producer.join(); consumer.join(); const auto end = Clock::now();
    if (failed.load()) return {false, 0};
    return {producerPinned.load() && consumerPinned.load(), transfers / std::chrono::duration<double>(end - begin).count() / 1e6};
}

Sample runScalar(int producerCpu, int consumerCpu, uint64_t transfers) {
    Queue q; std::atomic<bool> go{false}, pp{false}, cp{false}, failed{false};
    std::thread consumer([&] { cp.store(pinCurrentThread(consumerCpu)); while (!go.load(std::memory_order_acquire)) spinPause(); for (uint64_t i=0;i<transfers;) { Item item{}; if (!q.tryPop(item)) { spinPause(); continue; } if (reinterpret_cast<uintptr_t>(item) != ++i) failed.store(true); } });
    std::thread producer([&] { pp.store(pinCurrentThread(producerCpu)); while (!go.load(std::memory_order_acquire)) spinPause(); for (uint64_t i=0;i<transfers;) { if (q.tryPush(reinterpret_cast<Item>(static_cast<uintptr_t>(i+1)))) ++i; else spinPause(); } });
    const auto begin=Clock::now(); go.store(true, std::memory_order_release); producer.join(); consumer.join();
    if (failed.load()) return {false,0}; return {pp.load() && cp.load(), transfers / std::chrono::duration<double>(Clock::now()-begin).count()/1e6};
}

Sample dispatch(int width, int p, int c, uint64_t n) {
    if (width == 0) return runScalar(p,c,n);
#define CASE(W) case W: if constexpr (FastQueueBatch<Item>::max_size >= W) return runBatch<W>(p,c,n); else return {false, 0};
    switch (width) { CASE(1) CASE(2) CASE(3) CASE(4) CASE(5) CASE(6) CASE(7) CASE(8)
                     CASE(9) CASE(10) CASE(11) CASE(12) CASE(13) CASE(14) CASE(15) CASE(16) default: return {false,0}; }
#undef CASE
}
Options parse(int argc, char** argv) {
    Options o; for (int i=1;i<argc;++i) { std::string a=argv[i]; auto value=[&]() -> const char* { if (++i>=argc) { std::cerr << "Missing value for " << a << '\n'; std::exit(2); } return argv[i]; };
        if(a=="--output") o.output=value(); else if(a=="--transfers") o.transfers=std::stoull(value()); else if(a=="--rounds") o.rounds=std::stoi(value()); else if(a=="--warmups") o.warmups=std::stoi(value()); else if(a=="--max-cpus") o.maxCpus=std::stoi(value()); else if(a=="--help") { std::cout << "--output FILE --transfers N --rounds N --warmups N --max-cpus N\n"; std::exit(0); } }
    return o;
}
} // namespace

int main(int argc, char** argv) {
    const Options o=parse(argc,argv); const auto cpus=availableCpus(o.maxCpus);
    constexpr uint64_t kWidthLcm = FastQueueBatch<Item>::max_size == 16 ? 720720 : 840;
    if (o.transfers == 0 || o.transfers % kWidthLcm != 0) {
        std::cerr << "--transfers must be a nonzero multiple of " << kWidthLcm
                  << " so every fixed width moves an exact workload\n";
        return 2;
    }
    if (cpus.size()<2) { std::cerr << "Need two CPUs in process affinity mask\n"; return 2; }
    std::ofstream out(o.output); if (!out) { std::cerr << "Cannot write " << o.output << '\n'; return 2; }
    out << "producer_cpu,consumer_cpu,producer_socket,consumer_socket,producer_core,consumer_core,producer_smt,consumer_smt,width,round,pinned,throughput_mps\n";
    std::cerr << "Matrix: " << cpus.size() << " CPUs, widths 0.." << FastQueueBatch<Item>::max_size << ", " << o.rounds << " rounds\n";
    for (const auto& p:cpus) for (const auto& c:cpus) if (p.id!=c.id) for (size_t w=0;w<=FastQueueBatch<Item>::max_size;++w) {
        for(int warm=0;warm<o.warmups;++warm) (void)dispatch(static_cast<int>(w),p.id,c.id,o.transfers);
        for(int r=0;r<o.rounds;++r) { const auto s=dispatch(static_cast<int>(w),p.id,c.id,o.transfers); out << p.id<<','<<c.id<<','<<p.socket<<','<<c.socket<<','<<p.core<<','<<c.core<<','<<p.smt<<','<<c.smt<<','<<w<<','<<r<<','<<(s.pinned?1:0)<<','<<s.mps<<'\n'; out.flush(); }
    }
}

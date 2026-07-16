#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if __x86_64__ || _M_X64
#include "fast_queue_x86_64.h"
#elif __aarch64__ || _M_ARM64
#include "fast_queue_arm64.h"
#else
#error FastQueueTopologyMatrix supports x86_64 and arm64 only.
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
    uint64_t transfers = 2162160; // calibration work; exact multiple of every width
    int rounds = 3, warmups = 1, maxCpus = 0;
    int minSampleMs = 0; // zero disables calibration
    int producerShard = 0, producerShards = 1;
    std::vector<size_t> widths; // empty means scalar plus every supported fixed width
};
struct Sample { bool pinned; double mps; double millis; };

std::string readText(const std::string& path) { std::ifstream f(path); std::string s; std::getline(f, s); return s; }
int readInt(const std::string& path, int fallback=-1) { try { return std::stoi(readText(path)); } catch (...) { return fallback; } }
std::vector<CpuInfo> availableCpus(int maxCpus) {
    std::vector<CpuInfo> result;
#if defined(__linux__)
    int maxCpu = std::max(256L, sysconf(_SC_NPROCESSORS_CONF)); cpu_set_t* set=nullptr; size_t bytes=0;
    for (;;) {
        if(set) CPU_FREE(set); set=CPU_ALLOC(maxCpu); bytes=CPU_ALLOC_SIZE(maxCpu);
        if(!set) { std::cerr << "Cannot allocate Linux CPU affinity mask\n"; return result; }
        CPU_ZERO_S(bytes,set); if(sched_getaffinity(0,bytes,set)==0) break;
        if(errno!=EINVAL || maxCpu>(1<<20)) { CPU_FREE(set); std::cerr << "Cannot read Linux CPU affinity mask\n"; return result; }
        maxCpu*=2;
    }
    for(int cpu=0;cpu<maxCpu;++cpu) if(CPU_ISSET_S(cpu,bytes,set)) {
        const std::string base="/sys/devices/system/cpu/cpu"+std::to_string(cpu)+"/topology/";
        const auto siblings=readText(base+"thread_siblings_list"); int smt=0;
        if(!siblings.empty()) { const auto dash=siblings.find('-'); if(dash!=std::string::npos) smt=cpu-std::stoi(siblings.substr(0,dash)); }
        result.push_back({cpu,readInt(base+"physical_package_id"),readInt(base+"core_id"),smt});
        if(maxCpus>0 && static_cast<int>(result.size())>=maxCpus) break;
    }
    CPU_FREE(set);
#elif defined(__APPLE__)
    for(unsigned cpu=0,n=std::thread::hardware_concurrency();cpu<n && (!maxCpus || static_cast<int>(cpu)<maxCpus);++cpu) result.push_back({static_cast<int>(cpu),-1,-1,-1});
#endif
    return result;
}
bool pinCurrentThread(int cpu) {
#if defined(__linux__)
    if(cpu<0) return false; const int count=cpu+1; cpu_set_t* set=CPU_ALLOC(count); if(!set) return false;
    const size_t bytes=CPU_ALLOC_SIZE(count); CPU_ZERO_S(bytes,set); CPU_SET_S(cpu,bytes,set);
    const bool ok=pthread_setaffinity_np(pthread_self(),bytes,set)==0; CPU_FREE(set); return ok;
#else
    (void)cpu; return false;
#endif
}
inline void spinPause() { std::this_thread::yield(); }
template<size_t N> Sample runBatch(int p, int c, uint64_t transfers) {
    Queue q; std::atomic<bool> go{false}, failed{false}, pp{false}, cp{false}; std::atomic<int> ready{0};
    std::thread consumer([&] { cp.store(pinCurrentThread(c),std::memory_order_relaxed); ready.fetch_add(1,std::memory_order_release); while(!go.load(std::memory_order_acquire)) spinPause(); uint64_t expected=0; FastQueueBatch<Item> b{}; while(expected<transfers) { auto got=q.template tryPopBatch<N>(b); if(!got) {spinPause();continue;} for(size_t i=0;i<got;++i) if(reinterpret_cast<uintptr_t>(b.items[i])!=++expected) failed.store(true,std::memory_order_relaxed); } });
    std::thread producer([&] { pp.store(pinCurrentThread(p),std::memory_order_relaxed); ready.fetch_add(1,std::memory_order_release); while(!go.load(std::memory_order_acquire)) spinPause(); uint64_t sent=0; FastQueueBatch<Item> b{}; while(sent<transfers) { for(size_t i=0;i<N;++i)b.items[i]=reinterpret_cast<Item>(static_cast<uintptr_t>(sent+i+1)); for(size_t off=0;off<N;) { auto moved=q.template tryPushBatch<N>(b,off); if(!moved){spinPause();continue;} off+=moved; } sent+=N; } });
    while(ready.load(std::memory_order_acquire)!=2) spinPause(); const auto start=Clock::now(); go.store(true,std::memory_order_release); producer.join();consumer.join(); const double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    if(failed.load()) return {false,0,ms}; return {pp.load()&&cp.load(),transfers/(ms*1000.0),ms};
}
Sample runScalar(int p,int c,uint64_t transfers) {
    Queue q; std::atomic<bool> go{false},failed{false},pp{false},cp{false}; std::atomic<int> ready{0};
    std::thread consumer([&]{cp.store(pinCurrentThread(c));ready.fetch_add(1,std::memory_order_release);while(!go.load(std::memory_order_acquire))spinPause();for(uint64_t i=0;i<transfers;){Item item{};if(!q.tryPop(item)){spinPause();continue;}if(reinterpret_cast<uintptr_t>(item)!=++i)failed.store(true);}});
    std::thread producer([&]{pp.store(pinCurrentThread(p));ready.fetch_add(1,std::memory_order_release);while(!go.load(std::memory_order_acquire))spinPause();for(uint64_t i=0;i<transfers;){if(q.tryPush(reinterpret_cast<Item>(static_cast<uintptr_t>(i+1))))++i;else spinPause();}});
    while(ready.load(std::memory_order_acquire)!=2)spinPause();auto start=Clock::now();go.store(true,std::memory_order_release);producer.join();consumer.join();double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();if(failed.load())return{false,0,ms};return{pp.load()&&cp.load(),transfers/(ms*1000.0),ms};
}
Sample dispatch(int w,int p,int c,uint64_t n) { if(w==0)return runScalar(p,c,n);
#define CASE(W) case W: if constexpr(FastQueueBatch<Item>::max_size>=W)return runBatch<W>(p,c,n); else return {false,0,0};
    switch(w){CASE(1)CASE(2)CASE(3)CASE(4)CASE(5)CASE(6)CASE(7)CASE(8)CASE(9)CASE(10)CASE(11)CASE(12)CASE(13)CASE(14)CASE(15)CASE(16)default:return{false,0,0};}
#undef CASE
}
Options parse(int argc,char**argv) { Options o; for(int i=1;i<argc;++i){std::string a=argv[i];auto v=[&]{if(++i>=argc){std::cerr<<"Missing value for "<<a<<'\n';std::exit(2);}return argv[i];}; if(a=="--output")o.output=v();else if(a=="--transfers")o.transfers=std::stoull(v());else if(a=="--rounds")o.rounds=std::stoi(v());else if(a=="--warmups")o.warmups=std::stoi(v());else if(a=="--max-cpus")o.maxCpus=std::stoi(v());else if(a=="--min-sample-ms")o.minSampleMs=std::stoi(v());else if(a=="--producer-shard")o.producerShard=std::stoi(v());else if(a=="--producer-shards")o.producerShards=std::stoi(v());else if(a=="--widths"){std::string list=v();size_t start=0;while(start<=list.size()){size_t end=list.find(',',start);auto token=list.substr(start,end==std::string::npos?std::string::npos:end-start);if(token.empty()){std::cerr<<"Invalid --widths\n";std::exit(2);}o.widths.push_back(static_cast<size_t>(std::stoul(token)));if(end==std::string::npos)break;start=end+1;}}else if(a=="--help"){std::cout<<"--output FILE --transfers N --rounds N --warmups N --max-cpus N --min-sample-ms N --producer-shard I --producer-shards N --widths 0,8\n";std::exit(0);}}return o; }
} // namespace
int main(int argc,char**argv) {
    auto o=parse(argc,argv);const auto cpus=availableCpus(o.maxCpus);constexpr uint64_t lcm=FastQueueBatch<Item>::max_size==16?720720:840;
    if(!o.transfers||o.transfers%lcm||o.rounds<1||o.warmups<0||o.minSampleMs<0||o.producerShards<1||o.producerShard<0||o.producerShard>=o.producerShards){std::cerr<<"Invalid options; --transfers must be nonzero multiple of "<<lcm<<'\n';return 2;}if(cpus.size()<2){std::cerr<<"Need two CPUs\n";return 2;}
    if(o.widths.empty()) for(size_t w=0;w<=FastQueueBatch<Item>::max_size;++w) o.widths.push_back(w);
    std::sort(o.widths.begin(),o.widths.end()); o.widths.erase(std::unique(o.widths.begin(),o.widths.end()),o.widths.end());
    for(const auto w:o.widths) if(w>FastQueueBatch<Item>::max_size){std::cerr<<"Unsupported width "<<w<<"\n";return 2;}
    std::ofstream out(o.output);if(!out){std::cerr<<"Cannot write "<<o.output<<'\n';return 2;}out<<"producer_cpu,consumer_cpu,producer_socket,consumer_socket,producer_core,consumer_core,producer_smt,consumer_smt,width,round,pinned,throughput_mps,effective_transfers,calibration_millis\n";
    const uint64_t pairs=cpus.size()*(cpus.size()-1);const uint64_t total=(pairs*o.widths.size()*o.rounds+o.producerShards-1)/o.producerShards;uint64_t done=0; const auto allStart=Clock::now();
    std::cerr<<"Matrix shard "<<o.producerShard<<'/'<<o.producerShards<<": "<<cpus.size()<<" CPUs, up to "<<total<<" timed samples\n";
    for(size_t pi=0;pi<cpus.size();++pi){if(static_cast<int>(pi%o.producerShards)!=o.producerShard)continue;const auto&p=cpus[pi];for(const auto&c:cpus)if(p.id!=c.id)for(const size_t w:o.widths){uint64_t n=o.transfers;double calMs=0;if(o.minSampleMs){auto cal=dispatch(w,p.id,c.id,n);calMs=cal.millis;if(!cal.pinned&&cal.mps==0){std::cerr<<"Calibration failed producer="<<p.id<<" consumer="<<c.id<<" width="<<w<<" transfers="<<n<<'\n';return 1;}const long double want=static_cast<long double>(n)*o.minSampleMs/std::max(0.001,calMs);n=static_cast<uint64_t>((want+lcm-1)/lcm)*lcm;}for(int i=0;i<o.warmups;++i)(void)dispatch(w,p.id,c.id,n);for(int r=0;r<o.rounds;++r){auto s=dispatch(w,p.id,c.id,n);out<<p.id<<','<<c.id<<','<<p.socket<<','<<c.socket<<','<<p.core<<','<<c.core<<','<<p.smt<<','<<c.smt<<','<<w<<','<<r<<','<<(s.pinned?1:0)<<','<<std::setprecision(12)<<s.mps<<','<<n<<','<<calMs<<'\n';out.flush();++done;auto elapsed=std::chrono::duration<double>(Clock::now()-allStart).count();double eta=done?elapsed*(total-done)/done:0;std::cerr<<"progress "<<done<<'/'<<total<<" ETA "<<static_cast<uint64_t>(eta)<<"s\n";}}}
}

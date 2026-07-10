![Logo](fastqueue2.png)

# FastQueue2

FastQueue2 is a rewrite of [FastQueue](https://github.com/andersc/fastqueue). It supports 8-byte transfers only.

## But first

* Is this queue memory efficient?

	No. This queue aims for speed, not memory efficiency.

* The queue is ‘dramatically under-synchronized’

	Write a test and prove it (you can use FastQueueIntegrityTest.cpp as a boilerplate). Don’t just say stuff out of the blue, prove it!

* Why not use partial specialization for pointers since that's all you support?

	This queue supports the transport of 8 bytes from a producer to a consumer. It might be a pointer and it might not be, so that’s why no specialization is implemented. However, if we gain speed by specializing for pointers, then let’s implement that. I did not see any gain in my tests, and this queue is all about speed.


## Background

When I was playing around with benchmarking various SPSC queues, [deaod’s](https://github.com/Deaod/spsc_queue) and [Dro’s](https://github.com/drogalis/SPSC-Queue/tree/main) queues were unbeatable. The titans — [Rigtorp](https://github.com/rigtorp/SPSCQueue), [Folly](https://github.com/facebook/folly/tree/main), [moodycamel](https://github.com/cameron314/concurrentqueue) and [boost](https://www.boost.org/doc/libs/1_66_0/doc/html/lockfree.html) — were all left in the dust; they were especially fast on Apple silicon. My previous attempt ([FastQueue](https://github.com/andersc/fastqueue)) at beating the titans placed itself in the top tier but not at #1. In my queue I also implemented a stop-queue mechanism that is missing from the other implementations. Anyhow….

So I took a new, egoistic approach: target my own use cases and investigate whether there were any fundamental changes to the system that could be made. I only work with 64-bit CPUs, so let’s only target x86_64 and ARM64. Also, in all my cases I pass pointers around, so limiting the object to an 8-byte object is fine.

In the general SPSC queue implementation there is a circular buffer where push checks whether it’s possible to push an object by looking at the distance between the tail and head pointer/counter. The same goes for popping an object: if there is a distance between tail and head, there is at least one object to pop. That means that if the push runs on one CPU and the pop runs on another CPU, you share the tail/head counters and the object itself between the CPUs.

![Deaod's ring buffer diagram](ring_buffer_concept.png)

*The above picture is taken from Deaod’s repo*

The first version of this queue used the object slot *itself* as the full/empty
flag: pop cleared the slot to `nullptr`, push waited for `nullptr`. Elegant, and
it means the CPUs only ever share the object. The problem only shows up when you
measure the *pure* queue (no per-message malloc masking it): that scheme bounces
a whole cache line **both** directions for every single element (producer
publishes the pointer → consumer reads it → consumer writes the `nullptr` back →
producer reads that back), so it moves an order of magnitude more coherence
traffic than a packed ring. Against a benchmark dominated by `new`/`delete` it
looked great; as a raw queue it was several times slower than the titans.

![My ring buffer diagram](ringbuffer.png)

So this version keeps the FastQueue skin (same `push` / `pop` / `stopQueue`, still
8-byte objects) but changes the engine to the fastest thing that actually wins on
the hardware I measured:

* **Cached indices.** Each side keeps a *private* copy of the other side's index
  and only re-reads the shared atomic when its cache says the queue is full
  (producer) or empty (consumer). In steady state the producer only writes its
  own write-index line plus the data; the consumer only writes its own read-index
  line. The control lines stop ping-ponging.
* **One-directional, packed data.** The slot is never written back, so a single
  cache line carries many elements flowing producer → consumer instead of one
  element bouncing both ways.
* **x86 throughput profiles.** `-march=znver2` selects wrapped phase indexes plus
  six-item cushion: measured best Zen2 throughput. Other x86 targets select
  monotonic indexes plus immediate drain: avoids phase-mask work and batching
  penalty on Haswell. Both remain user-overridable at compile time.
* **Architecture-specific ring placement.** ARM keeps ring storage separate from control state;
  on x86_64, inline ring storage removes allocation and pointer indirection without a measured
  regression. Both layouts preserve control-line isolation.

If the tail catches the head there's nothing to pop; if the head catches the tail
the buffer is full and push waits. Same contract as before, very different cost.

## The need for speed

A word on measuring first, because it bit me hard: the original benchmark ran
each queue back-to-back in a fixed order, and on a laptop that means the queue
that runs **first** gets the cold/turbo advantage and looks fastest. It also does
`new`/`delete` per message, and that allocator cost (cross-thread free is
expensive) dominates the loop and hides the queue entirely. So the numbers below
come from a rewritten benchmark that **rotates the order every round**, reports
the **median**, and runs two passes: a *heap* pass (new/delete per message, the
classic FastQueue benchmark, allocator-bound) and a *pooled* pass (pre-allocated
objects — this is what actually measures the queue).

Pooled pass, fixed transaction count, higher is better, median of 12 rotated rounds.
(Absolute numbers differ per machine mostly because of clock — compare within a
row.) Machines are identified by CPU model and pinned CPU pair; no internal host
names or accounts are published.

| Machine | FastQueue | Deaod | Dro | David V5 |
| --- | ---: | ---: | ---: | ---: |
| AMD EPYC 7702, Zen2 dual socket, CPUs 1/3 | **123.935M** | 90.443M | 107.959M | 102.075M |
| AMD EPYC 7702P, Zen2, CPUs 1/3 | **118.629M** | 75.078M | 90.129M | 79.699M |
| Intel Xeon E5-2630L v3, Haswell, CPUs 1/3 | **117.951M** | 28.725M | 31.869M | 27.067M |

All x86 results use pooled pointers, physical-core pinning, performance governor,
`chrt -f 90`, `g++ -O3 -DNDEBUG -march=native`, exact sequence validation, and
fixed transfer counts. FastQueue leads all three provided hosts: +14.8% versus Dro
on dual-socket Zen2, +31.6% on single-socket Zen2, and +270.1% on this Haswell.

David V5 comes from [David Álvarez Rosa's ring-buffer analysis](https://david.alvarezrosa.com/posts/optimizing-a-lock-free-ring-buffer/)
and is included in benchmark. This demonstrates leadership on tested hosts, not
universal #1 claim across every x86 CPU, compiler, capacity, or latency workload.

Heap pass is allocator-bound. Use pooled objects, rotated order, joined worker
threads, and median distributions for queue comparisons. Local Apple-host smoke is
not x86 certification; reproduce x86 results on pinned physical cores.

### x86 tuning switches

Header selects profile from GCC/Clang `-march` macros:

* `-march=znver2`: `FQ_WRAPPED_INDICES=1`, `FQ_CONSUMER_CUSHION=6`.
* Other x86 targets: `FQ_WRAPPED_INDICES=0`, `FQ_CONSUMER_CUSHION=0`.
* Define either macro before including header to override profile.
* `FQ_OCCUPANCY_INSTRUMENT=1` records empty-refresh occupancy and perturbs
  throughput; diagnosis only.

Fixed-work reproduction:

```bash
g++ -O3 -DNDEBUG -std=c++20 -march=native -DPOOLED_ONLY=1 \
  -DTRANSFER_COUNT=100000000 -DROUNDS=12 -DCONSUMER_CPU=1 -DPRODUCER_CPU=3 \
  -I. -Ideaod_spsc -Idro main.cpp -o bench
sudo cpupower frequency-set -g performance
sudo chrt -f 90 ./bench
```

Push looks like this:

```cpp
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
```
Is there space (per my cached copy of the read index)? If the cache says full,
re-check the real read index; if it is still full, wait (or bail if stopped).
Then write the slot and publish the new write index with a single release store.

Pop looks like this:

```cpp
const uint64_t r = mReadIndex.load(std::memory_order_relaxed);
if (r == mWriteIndexCache) [[unlikely]] {               // cache says empty - verify
    do {
        mWriteIndexCache = mWriteIndex.load(std::memory_order_acquire);
        if (r != mWriteIndexCache) break;
        if (mExitThreadSemaphore.load(std::memory_order_acquire)) [[unlikely]] {
            mWriteIndexCache = mWriteIndex.load(std::memory_order_acquire);
            if (r == mWriteIndexCache) { aOut = nullptr; return; }
            break;
        }
    } while (true);
}
aOut = mRingBuffer[r & MASK];
mReadIndex.store(r + 1, std::memory_order_release);
```
Is there something to read (per my cached copy of the write index)? If the cache
says empty, re-check; if it is still empty and the queue was stopped, drain any
last items and then return nullptr. Otherwise read the slot and publish the new
read index.


Regarding `inline`, `noexcept` and `[[unlikely]]` — they're there. Yes, I know -O3 always inlines, and I have read what people say about `[[unlikely]]`.
If you don't like it, remove it and send a pull request.

## Usage

See the original FastQueue (the link above).

(Just copy the header file for your architecture into your project.)
**fast_queue_arm64.h** / **fast_queue_x86_64.h**

## Build and run the tests

```
git clone https://github.com/andersc/fastqueue2.git
cd fastqueue2
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

(Run the benchmark against Deaod and Dro)

**./fast_queue2**

(Run the integrity test)

**./fast_queue_integrity_test**


## Some thoughts
There are a couple of findings that puzzled me.

1.	Cache-line separation is not one-size-fits-all, but on both families the
	right answer is "keep the two hot index lines out of each other's prefetch
	window". x86's L2 spatial prefetcher pulls **128-byte (two-line) pairs**, so
	the write index and read index must live in different 128-byte pairs — 128-byte
	separation measured ~18% faster than 64 on AMD Zen (with 64, fetching one index
	dragged the other core's index line along). On ARM (Apple M-series *and*
	Cortex-X925) the streaming prefetcher reaches even further and **256-byte**
	separation was best. So the alignment is set per-architecture in the two headers.
2.	Heap-allocating the ring (instead of embedding it in the object) is worth ~3x
	on Apple silicon. Sitting next to the indices, the ring got hoovered up by the
	prefetcher into the wrong core. Moving it out is the single biggest M-series win.
3.	Memory ordering is not free and not uniform: making the slot itself an
	`std::atomic` with acquire/release (LDAR/STLR per access) was ~2.6x *slower* on
	Apple silicon than plain loads/stores ordered by one release/acquire pair on the
	index. Measure, don't assume.
4.	Micro-benchmarks lie. The order you run competitors in, whether you malloc per
	message, and how warm the machine is all swing the numbers more than the code
	does. The benchmark here rotates order and reports medians for that reason —
	the results should still be 'considered with a grain of salt'.
5.	**Thread pinning barely works on macOS — expect noise.** `pin_thread.h` uses
	`thread_policy_set` with `THREAD_AFFINITY_POLICY`, but on Apple silicon that is
	only a *hint* (an affinity *tag* that suggests two threads share an L2); the
	scheduler will still move your producer/consumer between P- and E-cores as it
	pleases. There is no hard "run this thread on this core" on macOS. In practice
	that means M-series numbers swing a lot run-to-run (I saw competitors move 3–4x
	between runs purely from placement). Recommendations on macOS: run several times
	and take the median (the benchmark does), keep the machine otherwise idle and
	cool, and if you need determinism give the hot threads a high QoS
	(`QOS_CLASS_USER_INTERACTIVE`) to bias them onto P-cores — but accept it is a
	bias, not a guarantee. On Linux `pthread_setaffinity_np` is real pinning, so the
	server numbers are far more stable. None of this affects correctness — it is
	purely about reproducible measurement and latency determinism.

Can this be beaten? Yes it can — Deaod and Dro are right there and trade blows on
the servers. On Apple silicon this one runs away. The paid version of me is faster ;-)

Have fun!


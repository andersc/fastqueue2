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

When I was benchmarking SPSC queues, [deaod’s](https://github.com/Deaod/spsc_queue) and [Dro’s](https://github.com/drogalis/SPSC-Queue/tree/main) were strong baselines. [Rigtorp](https://github.com/rigtorp/SPSCQueue), [Folly](https://github.com/facebook/folly/tree/main), [moodycamel](https://github.com/cameron314/concurrentqueue), and [boost](https://www.boost.org/doc/libs/1_66_0/doc/html/lockfree.html) are other established implementations. My previous attempt ([FastQueue](https://github.com/andersc/fastqueue)) reached the top tier but did not lead every measured workload. It also implements `stopQueue`, which the comparison implementations do not all provide.

This project targets measured use cases rather than universal queue rankings: 64-bit x86_64 and arm64 CPUs, one producer, one consumer, and 8-byte transfers. Pointers are common payloads, but any 8-byte value fits.

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

So this version keeps FastQueue API (`push`, `pop`, `stopQueue`, 8-byte objects)
and uses data/control layouts selected per architecture and measured workload:

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
* **Architecture-specific ring placement.** `fast_queue_arm64.h` selects queue-owned inline contiguous storage by default (`FQ_ARM_RING_INLINE=1`); define it as `0` to test separately allocated ARM storage. `fast_queue_x86_64.h` keeps ring storage inline. Both layouts preserve control-line isolation. ARM has no compile-time throughput profile; x86 profile selection is described below.

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
Absolute numbers differ by machine, compiler, clocks, and scheduler; compare queues within
one row.

| Machine | FastQueue | Deaod | Dro | David V5 |
| --- | ---: | ---: | ---: | ---: |
| Apple M5, macOS arm64 | **396.473M** | 165.428M | 77.379M | 154.271M |
| ARM Cortex-X925 + Cortex-A725, Linux arm64, X925 CPUs 5/6 | 83.678M | 86.346M | **87.382M** | 86.551M |
| AMD EPYC 7702, Zen2 dual socket, CPUs 1/3 | **123.935M** | 90.443M | 107.959M | 102.075M |
| AMD EPYC 7702P, Zen2, CPUs 1/3 | **118.629M** | 75.078M | 90.129M | 79.699M |
| Intel Xeon E5-2630L v3, Haswell, CPUs 1/3 | **117.951M** | 28.725M | 31.869M | 27.067M |

### Measurement method

Every table row uses joined workers, atomic start gate, exact transfer count,
sequence validation, four-way order rotation, pooled pointers, and a 12-round
median. Compare queues only within one row.

Apple M5 arm64 row uses 5,000,000 fixed transfers. FastQueue is row winner at
396.473M/s; Deaod is 165.428M/s, David V5 is 154.271M/s, and Dro is 77.379M/s.
`FQ_ARM_RING_INLINE=1` uses queue-owned contiguous storage with peer-index caches
and release/acquire publication. The 100,000,000-transfer confirmation measured
FastQueue 405.951M/s versus Deaod 176.884M/s, David V5 155.166M/s, and Dro
72.701M/s. macOS affinity is a scheduler hint, not hard physical-core pinning;
P-core/E-core placement adds variance.

Cortex-X925 Linux row was re-run with 100,000,000 fixed transfers, physical X925
CPUs 5 and 6, performance governor, `chrt -f 90`, and
`g++ -O3 -DNDEBUG -march=native`. Host has two 5-core Cortex-X925 clusters
(CPUs 5–9 and 15–19) plus Cortex-A725 cores (CPUs 0–4 and 10–14); CPUs 5/6 are
distinct X925 cores in one cluster at 3.9 GHz. Dro wins this workload at
87.382M/s; David V5, Deaod, and FastQueue measure 86.551M/s, 86.346M/s, and
83.678M/s. Results are median of 12 rotated rounds from this current run.

Linux x86 rows use pooled pointers, physical-core pinning, performance governor,
`chrt -f 90`, `g++ -O3 -DNDEBUG -march=native`, exact sequence validation, and
fixed transfer counts. FastQueue is row winner on all three listed x86 hosts:
+14.8% versus Dro on dual-socket Zen2, +31.6% on single-socket Zen2, and +270.1%
on this Haswell. Linux controls improve repeatability; they do not make Linux and
macOS absolute throughput directly comparable.

David V5 comes from [David Álvarez Rosa's ring-buffer analysis](https://david.alvarezrosa.com/posts/optimizing-a-lock-free-ring-buffer/)
Results identify row winners only for stated machine/workload conditions, not
universal #1 across every CPU, compiler, capacity, or latency workload.

Heap pass is allocator-bound. Use pooled objects, rotated order, joined worker
threads, fixed-work sequence validation, and median distributions for queue
comparisons.
### Per-architecture tuning

`fast_queue_arm64.h` selects an inline contiguous ring by default
(`FQ_ARM_RING_INLINE=1`), which is measured fastest for Apple M5 pooled
throughput. Define `FQ_ARM_RING_INLINE=0` to restore separately allocated ARM
storage for experiments. `fast_queue_x86_64.h` selects an x86 profile from
GCC/Clang `-march` macros:

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

## Usage

See the original FastQueue (the link above).

(Just copy the header file for your architecture into your project.)
**fast_queue_arm64.h** / **fast_queue_x86_64.h**

## Bulk API

`FastQueueBatch<T>` is caller-owned cache-line payload staging, not a
pointer-plus-length descriptor. `N` is compile-time fixed; hot batch calls pass
only batch address plus optional retry offset. Keep one batch per
producer/consumer work context.

* x86 batch storage is `alignas(64)`: eight adjacent 8-byte objects. `N=1..8`.
* Apple Silicon batch storage is `alignas(128)`: sixteen adjacent 8-byte
  objects, matching its reported 128-byte data-cache line. `N=1..16`.
* Other AArch64 targets default to 64 bytes / eight objects. Override
  `FQ_ARM_BATCH_BYTES` only after confirming target cache-line size.

`offset` retries only an unsent/undrained suffix.

```cpp
FastQueueBatch<Job*> jobs{};
jobs.items[0] = first;
jobs.items[1] = second;

const auto pushed = queue.tryPushBatch<2>(jobs);
const auto popped = queue.tryPopBatch<2>(jobs);
```

Each returns exact number moved: zero when full or empty, partial count when
fewer than requested fit or are available. FIFO holds across scalar and batch
calls. No blocking batch API exists. Calls may vary width independently: producer
can request `1`, then `6`, `5`, `3`, `8`; consumer can request different widths.
On a partial result, retry same requested width with `offset += moved` until its
suffix is sent or received. Do not reuse or overwrite unsent batch slots before
that retry completes.

### Callback with runtime item count

Callbacks often receive a runtime count: `2`, `56`, `32`, `23`, `11`, `1`, `3`,
`67`, and so on. Split it into chunks no larger than
`FastQueueBatch<T*>::max_size`, stage one chunk, then select its fixed width with
a `switch`. x86/default ARM have max width 8; Apple ARM builds with 128-byte
batches have max width 16.

```cpp
#include <algorithm>
#include <cstddef>
#include <functional>
#include <thread>

// Wait must allow consumer to run: yield, an event-loop wait, or a semaphore.
// This helper is only for one producer context of this SPSC queue.
template <std::size_t N, class Queue, class T, class Wait>
void pushStaged(Queue& queue, const FastQueueBatch<T*>& batch, Wait&& wait) {
    std::size_t offset = 0;
    while (offset != N) {
        const std::size_t moved = queue.template tryPushBatch<N>(batch, offset);
        if (moved == 0) {
            std::invoke(wait);       // Full. batch must stay unchanged.
            continue;
        }
        offset += moved;             // Retry only unsent suffix.
    }
}

template <class Queue, class T, class Wait>
void pushCallbackPointers(Queue& queue, T* const* input,
                          std::size_t count, Wait&& wait) {
    using Batch = FastQueueBatch<T*>;
    static_assert(Batch::max_size == 8 || Batch::max_size == 16);

    while (count != 0) {
        const std::size_t width = std::min(count, Batch::max_size);
        Batch batch{};               // Caller-owned, producer-local staging.
        std::copy_n(input, width, batch.items);

        // Width is runtime data at callback boundary; each case is still a
        // compile-time-specialized queue operation.
        switch (width) {
        case 1: pushStaged<1>(queue, batch, wait); break;
        case 2: pushStaged<2>(queue, batch, wait); break;
        case 3: pushStaged<3>(queue, batch, wait); break;
        case 4: pushStaged<4>(queue, batch, wait); break;
        case 5: pushStaged<5>(queue, batch, wait); break;
        case 6: pushStaged<6>(queue, batch, wait); break;
        case 7: pushStaged<7>(queue, batch, wait); break;
        case 8: pushStaged<8>(queue, batch, wait); break;
        default:
            if constexpr (Batch::max_size == 16) {
                switch (width) {
                case 9:  pushStaged<9>(queue, batch, wait); break;
                case 10: pushStaged<10>(queue, batch, wait); break;
                case 11: pushStaged<11>(queue, batch, wait); break;
                case 12: pushStaged<12>(queue, batch, wait); break;
                case 13: pushStaged<13>(queue, batch, wait); break;
                case 14: pushStaged<14>(queue, batch, wait); break;
                case 15: pushStaged<15>(queue, batch, wait); break;
                case 16: pushStaged<16>(queue, batch, wait); break;
                }
            }
            break; // Unreachable: width <= Batch::max_size.
        }

        input += width;               // Advance only after whole chunk sent.
        count -= width;
    }
}
```

Example callback, with 23 pointers on any target:

```cpp
void onJobs(FastQueue<Job*, 1023, 64>& queue, Job* const* jobs,
            std::size_t jobCount) {
    pushCallbackPointers(queue, jobs, jobCount, [] {
        std::this_thread::yield();    // Replace with app wait/backpressure policy.
    });
}
```

For `23`, x86/default ARM sends `8 + 8 + 7`; Apple ARM can send `16 + 7`.
For callback counts `2, 56, 32, 23, 11, 2, 1, 1, 3, 67`, invoke same helper
each time. `nullptr` remains valid payload; count, never null termination,
defines chunk length. If callback cannot wait, use a one-shot variant that
returns queued count and retain unsent input for a later callback.

Batch payload copy uses only contiguous ring segments. Ring wrap splits into
prefix/suffix copies, so no copy reads or writes beyond either ring or batch.
- x86: scalar fallback; AVX2 copies four pointers/vector (two vectors for 8);
  AVX-512 copies 8 in one vector only when compiled with `__AVX512F__`.
- ARM: NEON copies two pointers/vector; Apple 128-byte batches use up to eight
  vectors for 16 pointers. Other AArch64 defaults remain 8 pointers.

SIMD changes payload copy only. Availability remains one index-distance check;
release/acquire index handoff remains one publication per returned batch.

### Rejected alternatives

Batch width stays a compile-time template selected by caller-side `switch`.
Measured runtime-count loops and function-pointer dispatch did not produce a
repeatable peak-width win; both can prevent full copy unrolling and inlining.
Do not infer count by scanning payload lanes for `nullptr`: FastQueue uses
cached producer/consumer indices for occupancy, permits null 8-byte payloads,
and a lane scan adds payload loads and compares to hot path. Non-temporal stores
also do not fit this hand-off: consumer reads newly published ring slots soon
after producer writes them. Keep fixed-width API unless target benchmark proves
otherwise.


Bulk mode is FastQueue-only. It reports **items/s**, preserves exact FIFO
validation, and uses fixed transfer count, joined producer/consumer workers,
start gate, 12 solo rounds, and median result. It is not a four-queue comparison:
Deaod, Dro, and David V5 have no matching bulk APIs.

### Measured width sweep

| CPU / OS | Build / pinning | Scalar mode | Best fixed width | Best median | Gain vs scalar mode* |
|---|---|---:|---:|---:|---:|
| Apple M5, macOS arm64 | `-O3 -DNDEBUG -march=native`, scheduler affinity | 423.462 M/s | **14 pointers** | **984.153 M/s** | **+132.4%** |
| AMD EPYC 7702P (Zen2), Linux x86_64 | `-march=znver2`, CPUs 5/6 via `taskset` | 87.189 M/s | **2 pointers** | **216.493 M/s** | **+148.3%** |
| ARM Cortex-X925, Linux arm64 | `-march=native`, X925 CPUs 5/6, `taskset` | 92.575 M/s | **8 pointers** | **667.779 M/s** | **+621.3%** |
| AMD EPYC 7702, Zen2 dual socket, Linux x86_64 | `-march=native`, same-socket CPUs 1/3, `taskset` | 91.247 M/s | **2 pointers** | **218.096 M/s** | **+139.0%** |
| Intel Xeon E5-2630L v3, Haswell, Linux x86_64 | `-march=native`, same-socket CPUs 1/3, `taskset` | 26.541 M/s | **1 pointer** | **141.074 M/s** | **+431.5%** |
| AMD EPYC 7702P, Zen2, Linux x86_64 | `-march=native`, CPUs 1/3, `taskset` | 85.442 M/s | **2 pointers** | **211.500 M/s** | **+147.5%** |

\* **Scalar mode and fixed width 1 are not like-for-like API measurements.**
`BULK_BATCH_SIZE=0` calls scalar `tryPush`/`tryPop` through `runOne`; width 1
calls `tryPushBatch<1>`/`tryPopBatch<1>` through bulk producer/consumer loops.
Both move one pointer per successful operation, but their caller loops, call
layout, retry behavior, and generated code differ. Read this table as measured
configuration throughput, not as isolated cost of grouping one pointer.

The Haswell `+431.5%` row is especially unusual. It does **not** mean that
"batching one pointer is generally 431% faster." Its scalar-mode confirmation
(`26.541 M/s`) is far below the separate Haswell sweep's width-0 result
(`121.776 M/s`), showing strong path/profile sensitivity. Keep it as result for
that exact native configuration; rerun scalar and width-1 distributions before
using it for a Haswell tuning decision.

M5 confirmation used 100M transfers/12 rounds. Fixed-14 raw range:
`953.279–995.004 M/s`; fixed-16 was lower at `907.194 M/s` median. Its earlier
30M/9-round width sweep showed width 8 `819.813`, 9 `831.228`, 10 `760.521`,
11 `869.855`, 12 `788.865`, 13 `912.711`, 14 `986.292`, 15 `849.421`, and 16
`920.233 M/s` median. Result is architecture, compiler, CPU placement, and
frequency dependent; do not treat width 14 as universal.

Zen2 100M/12-round sweep medians for widths 1..8 were: `96.676`, `216.493`,
`200.052`, `110.051`, `104.375`, `135.896`, `134.609`, and `183.968 M/s`.
Fixed-2 wins there. Wider batch does **not** guarantee more throughput because
queue occupancy, retry patterns, compiler code shape, and cache/coherence traffic
can dominate payload copy work.

Original Linux benchmark hosts received a native 100M-transfer, 12-round pooled
FastQueue-only sweep using their existing CPU pairs. All selected pairs are
separate physical cores under `performance` governor: Cortex-X925 CPUs 5/6 are
same X925 cluster; f131 Zen2 CPUs 1/3 and f061 Haswell CPUs 1/3 are same socket;
f177 Zen2P CPUs 1/3 are local physical cores. Sweep medians `BULK_BATCH_SIZE=0..8`:

| Host / CPU | Width 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | Winner |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| f181 Cortex-X925 | 92.575 | 83.220 | 135.053 | 230.171 | 500.039 | 359.211 | 413.696 | 453.453 | **670.492** | 8 |
| f131 EPYC 7702 | 97.338 | 100.476 | **220.244** | 67.120 | 122.665 | 107.988 | 136.775 | 132.755 | 176.345 | 2 |
| f061 Haswell | 121.776 | **132.575** | 24.363 | 20.505 | 27.601 | 30.126 | 34.074 | 37.332 | 51.699 | 1 |
| f177 EPYC 7702P | 84.343 | 92.049 | **213.277** | 198.824 | 110.455 | 100.775 | 133.186 | 132.867 | 188.305 | 2 |

Values above are M items/s. Full 100M confirmation selected final winners as
reported table: f181 width 8 `667.779`; f131 width 2 `218.096`; f061 width 1
`141.074`; f177 width 2 `211.500`. These are FastQueue scalar-vs-bulk results
only, not competitor comparisons.

### Reproduce before claims

```sh
# Apple M5: target cache-line capacity supports widths 0..16.
clang++ -std=c++20 -O3 -DNDEBUG -march=native -pthread -I. -Ideaod_spsc -Idro \
  -DSOLO_QUEUE=4 -DPOOLED_ONLY=1 -DTRANSFER_COUNT=100000000ULL -DROUNDS=12 \
  -DBULK_BATCH_SIZE=14 main.cpp -o fastqueue-bulk14

# Zen2: target cache-line capacity supports widths 0..8.
g++ -std=c++20 -O3 -DNDEBUG -march=znver2 -pthread -I. -Ideaod_spsc -Idro \
  -DSOLO_QUEUE=4 -DPOOLED_ONLY=1 -DTRANSFER_COUNT=100000000ULL -DROUNDS=12 \
  -DPRODUCER_CPU=5 -DCONSUMER_CPU=6 -DBULK_BATCH_SIZE=2 main.cpp -o fastqueue-bulk2
taskset -c 5,6 ./fastqueue-bulk2
```

`BULK_BATCH_SIZE=0` selects scalar API. Valid batch widths are target-specific:
`1..8` on x86 and standard 64-byte-line AArch64; `1..16` on Apple Silicon's
128-byte-line default. Build x86 SIMD variants with `-mavx2` or `-mavx512f` only
on supporting CPUs. SIMD modifies payload copy only; it did not make wider Zen2
batches automatically win. Measure each width and ISA on target hardware before
claims.

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

(Run bulk API tests)

**ctest --output-on-failure**

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
2.	On Apple M5, queue-owned inline contiguous ring storage measured fastest in the
	pooled benchmark. `FQ_ARM_RING_INLINE=1` is therefore default; define it as `0`
	to test separately allocated ARM storage.
3.	Memory ordering is not free and not uniform: making the slot itself an
	`std::atomic` with acquire/release (LDAR/STLR per access) was ~2.6x *slower* on
	Apple silicon than plain loads/stores ordered by one release/acquire pair on the
	index.
4.	Micro-benchmarks lie. The order you run competitors in, whether you malloc per
	message, and how warm the machine is all swing the numbers more than the code
	does. The benchmark here rotates order and reports medians for that reason —
	the results should still be 'considered with a grain of salt'.
5.	**macOS and Linux expose different measurement controls.** On macOS,
	`thread_policy_set` with `THREAD_AFFINITY_POLICY` is an affinity tag, not hard
	core pinning; scheduler placement between P- and E-cores can change results.
	Run several rounds and use medians. On Linux, `pthread_setaffinity_np`, a
	performance governor, and `chrt` can constrain placement, frequency policy,
	and scheduling policy; results still depend on CPU model, thermals, and system
	load. These controls affect measurement repeatability, not correctness.

Each benchmark row reports its own setup. Compare queues within a row; do not use
absolute M/s values to compare different machines or operating systems.


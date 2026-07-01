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

When I was playing around with benchmarking various SPSC queues, [deaod’s](https://github.com/Deaod/spsc_queue) queue was unbeatable. The titans — [Rigtorp](https://github.com/rigtorp/SPSCQueue), [Folly](https://github.com/facebook/folly/tree/main), [moodycamel](https://github.com/cameron314/concurrentqueue) and [boost](https://www.boost.org/doc/libs/1_66_0/doc/html/lockfree.html) — were all left in the dust; it was especially fast on Apple silicon. My previous attempt ([FastQueue](https://github.com/andersc/fastqueue)) at beating the titans placed itself in the top tier but not at #1. In my queue I also implemented a stop-queue mechanism that is missing from the other implementations. Anyhow….

(Update: added [Dro](https://github.com/drogalis/SPSC-Queue/tree/main), a promising SPSC kingpin. Run the benchmarks for results on your platform.)

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
* **Monotonic 64-bit counters** addressed with a mask — no wrap branch on the hot
  path. Publishing is a single release store; the matching acquire load carries
  the payload ordering, so the data store/load stay plain.
* **The ring lives on the heap, far from the control block.** This one is the big
  Apple-silicon surprise: embedding the ring next to the indices let the streaming
  prefetcher drag data lines into the index-owning core and cut throughput ~3x.
  Move the ring away and M-series throughput jumps from ~140M to ~490M.

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

Pooled pass, transactions/s, higher is better, median of rotated rounds.
(Absolute numbers differ per machine mostly because of clock — compare within a
row, not across rows; the Xeon is a 1.8 GHz low-power part.)

| Machine | FastQueue | Deaod | Dro |
| --- | --- | --- | --- |
| Apple M5 (ARM) | **~490M** | ~140–290M | ~95–105M |
| ARM Cortex-X925 | ~86M | ~87M | ~93M |
| AMD EPYC 7702 (Zen 2) | **~117M** | ~110M | ~101M |
| AMD EPYC 7702P (Zen 2) | ~102M | ~99M | ~107M |
| Intel Xeon E5-2630L v3 (Haswell) | **~38M** | ~25M | ~36M |

On Apple silicon FastQueue runs away with it (3–5x). On x86 — both AMD EPYC and
Intel Xeon — it leads or ties the pack after aligning the index lines to 128-byte
prefetch pairs (see thought #1). On Cortex-X925 it is a dead heat. In the heap
pass every queue collapses onto the allocator and they tie (~14M on EPYC, ~10M on
Cortex, and FastQueue ~2x ahead at ~50–60M on the M5).

On x86 the three top queues are, in fact, indistinguishable at the hardware level:
`perf stat` shows all of them running at **IPC ≈ 0.3 with ~20% L1-load misses and
near-zero last-level-cache misses** — i.e. purely bound by cross-core cache-line
migration, not compute or bandwidth. There is no clever instruction sequence left
to find; the only remaining lever is batching the index publish, which trades away
per-message latency, so this queue doesn't take it.

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

Can this be beaten? Yes it can — Deaod and Dro are right there and trade blows on
the servers. On Apple silicon this one runs away. The paid version of me is faster ;-)

Have fun!


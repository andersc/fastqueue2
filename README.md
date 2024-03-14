![Logo](fastqueue2.png)

# FastQueue2

FastQueue2 is a rewrite of [FastQueue](https://github.com/andersc/fastqueue). 

## But first

* Is this queue memory efficient?

	No. This queue aims for speed not memory efficiency.

* The queue is ‘dramatically under-synchronized’

	Write a test and prove it (you can use FastQueueIntegrityTest.cpp as a boilerplate). Don’t just say stuff out of the blue, prove it!

* Why not use partial specialization for pointers since that's all you support?

	This queue supports transport of 8 bytes from a producer to a consumer. It might be a pointer and it might not be a pointer so that’s why no specialization is implemented. However, if we gain speed using specialization of pointers then let’s implement that. I did not see any gain in my tests and this queue is all about speed.


## Background

When I was playing around with benchmarking various SPSC queues [deaod’s](https://github.com/Deaod/spsc_queue) queue was unbeatable. The titans: [Rigtorp](https://github.com/rigtorp/SPSCQueue), [Folly](https://github.com/facebook/folly/tree/main), [moodycamel](https://github.com/cameron314/concurrentqueue) and [boost](https://www.boost.org/doc/libs/1_66_0/doc/html/lockfree.html) where all left in the dust, it was especially fast on Apple silicon. My previous attempt ([FastQueue](https://github.com/andersc/fastqueue)) trying to beat the titans is placing itself in the top tier but not #1. In my queue I also implemented a stop-queue mechanism missing from the other implementations. Anyhow….

So I took a new egoistic approach, meaning target my usecases to investigate if there were any fundamental changes to the system that then could be made. I’m only working with 64-bit CPU’s so let’s only target x86_64 and Arm64. Also for all my cases I pass pointers around so limiting the object to a 8 byte object is fine.

In the general SPSC queue implementation there is a circular buffer where push is looking if it’s possible to push an object by looking at the distance between the tail and head pointer/counter. The same goes for popping an object, if there is a distance between tail and head there is at least one object to pop. That means that if the push runs on one CPU and the pop runs on another CPU you share tail/head counters and the object itself between the CPU’s. 

![(deaods ringbuffer picture)](ring_buffer_concept.png)

*The above picture is taken from Deaods repo*

I concluded based on the way I limit the usecase it’s possible to share the queue position by looking at the object itself (I’m aware that this is probably not something revolutionary. Most likely someone at Xerox PARC wrote a paper about this in the 70’s). That means that the CPU’s do not need to share its counters it only need to share the object, and it wants to share that object anyway. So it’s the absolute minimal amount of data. 
However for this to work without sharing pointers/counters the object must be there or not. So when we pop the object in the reader thread then we also need to clear it’s position in the circular buffer by assigning a nullptr. And it also needs to do that without tearing that's why 8 bytes in a 64-bit environment works.

![(my ringbuffer picture)](ringbuffer.png)

So the concept is exactly the same as before it’s just that we now know where we wrote an object last time now we just check if it’s possible to write an object in the next position before actually committing to doing that. If it's nullpt then we can write if not we got a full buffer and need to wait for the consumer.

So if the tail hits the head we will not write any objects, and if the head hit’s the tail there are no objects to pop.

Using that mechanism we only need to share the actual object between the threads/cpus.

## The need for speed

* So what speed do we get on my M1 Pro?

```
DeaodSPSC pointer test started.
DeaodSPSC pointer test ended.
DeaodSPSC Transactions -> 12389023/s
FastQueue pointer test started.
FastQueue pointer test ended.
FastQueue Transactions -> 17516515/s
```

And that’s a significant improvement over my previous attempt that was at around 10+M transactions per second (on my M1 Pro) while Deaod is at 12M.

What sparked me initially was the total dominance by Deaod on Apple Silicone now that's taken care of.. Yes! So in my application, the way the compiler compiles the code I by far beat Deaod. 

* What about x86_64?  

I don’t have access to a lot of x86 systems but I ran the code on a 64 core AMD EPYC 7763. I had to slightly modify the code to beat Deaod.

```
DeaodSPSC pointer test started.
DeaodSPSC pointer test ended.
DeaodSPSC Transactions -> 12397808/s
FastQueue pointer test started.
FastQueue pointer test ended.
FastQueue Transactions -> 13755427/s
```

So great! Still champagne, but did not totally run over the competition. 10% faster so still significant. 

The header file is under 60 lines of code and uses a combination of atomics and memory barriers to what I found the most optimal combination.

Push looks like this:

```cpp
while(mRingBuffer[mWritePosition&RING_BUFFER_SIZE].mObj != nullptr) if (mExitThreadSemaphore) [[unlikely]] return;
new(&mRingBuffer[mWritePosition++&RING_BUFFER_SIZE].mObj) T{std::forward<Args>(args)...};
```
Simple. Is the slot free? if not is the queue still operating?
If it's OK to push the object just put it in the queue.

Pop looks like this:

```cpp
std::atomic_thread_fence(std::memory_order_consume);
while (!(aOut = mRingBuffer[mReadPosition & RING_BUFFER_SIZE].mObj)) {
    if (mExitThread == mReadPosition) [[unlikely]] {
        aOut = nullptr;
        return;
    }
}
mRingBuffer[mReadPosition++ & RING_BUFFER_SIZE].mObj = nullptr;
```
Try popping the object. If sucessfull mark it free.
If not sucessfull popping the object is the queue active?
if it's not then just return nullptr


Regarding inline, noexcept and [[unlikely]].. It's there. Yes I know -O3 always inlines and I have read what people say about [[unlikely]].
If you don't like it. remove and pullrequest.

## Usage

See the orignal fastqueue (the link above)

(just move the header file(s) to your project depending on arhitecture)
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

(Run the benchmark against Deaod)

**./fast_queue2**

(Run the integrity test)

**./fast_queue_integrity_test**


## Some thoughts
There are a couple of findings that puzzled me. 

1.	I had to increase the the spacing between the objects to two times the cache length for x86_64 to gain speed over Deaod. Why? It does not make any sense.
2.	Pre-loading the cache when popping (I did comment out the code but play around yourself in the ARM version) did do nothing. I guess modern CPU’s pre-load the data speculatively anyway.
3.	I got good speed when the ringbuffer size exceeded 1024 entries. Why? My guess is that it irons out the uneven behaviour between the producer consumer. It’s just that my queue there was a significant increase in efficiency while for Deaod I did not see that effect. Well. We’re on the verge on CPU hacks and black magic so well. 

Can this be beaten? Yes it can.. However the free version of me is as fast as this. The paid version of me is faster ;-)

Have fun 


#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// A set of workers that outlives the pass it is running.
//
// It replaces a std::thread per worker per pass. The frame splits itself a dozen
// times — once per cascade to march the light, once per pair to merge it, once
// for the medium, once for the water's read — so at sixteen workers that was
// well over a hundred threads created and joined every frame. Creating one is
// not free anywhere and on Windows it is dear, and the cost scales with the core
// count, so the effect was a machine with more cores spending more time starting
// threads and less time using them: the same work measured slower on a
// sixteen-core desktop than on an eight-core laptop.
//
// Work is taken a block at a time from a shared counter rather than dealt out in
// advance. Passes are not uniform — a light ray that leaves the region dies
// within a few steps, and those rays are gathered along one edge of it — so a
// fixed split hands one worker all the expensive items. Taking blocks on demand
// lets the workers that drew cheap ones come back for more. The block is large
// enough that two workers never write into the same cache line.
namespace pool {

class Workers {
public:
    static Workers &Instance() {
        static Workers workers;
        return workers;
    }

    int Count() const { return workers_; }

    void Run(void (*body)(void *, int), void *context, int count, int block) {
        body_    = body;
        context_ = context;
        count_   = count;
        block_   = std::max(1, block);

        next_.store(0, std::memory_order_relaxed);

        {
            const std::lock_guard<std::mutex> lock(mutex_);

            pending_ = workers_ - 1;
            generation_++;
        }

        wake_.notify_all();

        Drain();

        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this] { return pending_ == 0; });
    }

    Workers(const Workers &)            = delete;
    Workers &operator=(const Workers &) = delete;

private:
    Workers() {
        const unsigned cores = std::thread::hardware_concurrency();

        workers_ = static_cast<int>(std::min(std::max(cores, 1u), 16u));

        threads_.reserve(static_cast<std::size_t>(std::max(0, workers_ - 1)));

        for (int w = 1; w < workers_; w++) threads_.emplace_back([this] { Serve(); });
    }

    ~Workers() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);

            stop_ = true;
            generation_++;
        }

        wake_.notify_all();

        for (std::thread &worker : threads_) worker.join();
    }

    // Takes blocks until there are none left. Run by the workers and by the
    // calling thread alike, so the caller is one of the hands rather than an idle
    // overseer.
    void Drain() {
        for (;;) {
            const int start = next_.fetch_add(block_, std::memory_order_relaxed);
            if (start >= count_) return;

            const int end = std::min(start + block_, count_);

            for (int i = start; i < end; i++) body_(context_, i);
        }
    }

    void Serve() {
        long long seen = 0;

        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);

                wake_.wait(lock, [this, seen] { return stop_ || generation_ != seen; });

                if (stop_) return;

                seen = generation_;
            }

            Drain();

            {
                const std::lock_guard<std::mutex> lock(mutex_);

                pending_--;

                if (pending_ == 0) idle_.notify_one();
            }
        }
    }

    std::vector<std::thread> threads_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;

    long long generation_ = 0;
    int pending_          = 0;
    bool stop_            = false;

    // The pass being run. Written before the generation is bumped and read only
    // after a worker has seen the bump, which the mutex orders.
    void (*body_)(void *, int) = nullptr;
    void *context_             = nullptr;
    int count_                 = 0;
    int block_                 = 1;

    std::atomic<int> next_{0};

    int workers_ = 1;
};

// Runs `body` over [0, count) across the machine's cores.
//
// `body` must write only to what belongs to its own index and must not touch
// anything the caller is also writing. Nothing here is a mutex.
//
// `least` is how many items make it worth waking the workers at all, and `block`
// how many each of them takes at a time. Both matter, and in opposite
// directions: a pass over a hundred thousand cheap samples wants large blocks so
// that two workers never write into one cache line, while a pass over four
// chunks that cost a millisecond each wants one item per block and no floor
// under the count. The defaults suit the first kind, which is most of them.
//
// The block size is also what decides whether an uneven pass balances. Where the
// expensive items are clustered — the columns of the grass band that have just
// scrolled into it are all at one end — a large block hands them to one worker
// and leaves the rest with nothing to do.
template <typename Body>
void For(int count, Body body, int least = 256, int block = 0) {
    Workers &workers = Workers::Instance();

    if (workers.Count() <= 1 || count < std::max(2, least)) {
        for (int i = 0; i < count; i++) body(i);
        return;
    }

    // Enough consecutive items that no two workers write into one cache line, and
    // small enough that there are many blocks per worker to balance with.
    const int taken = (block > 0) ? block : std::max(32, count / (workers.Count() * 8));

    workers.Run([](void *context, int i) { (*static_cast<Body *>(context))(i); }, &body, count, taken);
}

} // namespace pool

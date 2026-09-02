#pragma once

// A fixed set of worker threads that a loop can be handed to.
//
// The shape is not a guess. Measured on this machine with a job shaped like the
// cull - 1591 tasks whose sizes match the real batch distribution, biggest 2367
// and median 6 - handing out an empty job and collecting it again costs:
//
//     threads   sleeping   spinning
//        4        2 us       1 us
//        8       17 us       2 us
//       16       30 us       3 us
//
// and on a job of 0.10 ms the speedups were 3.6x at four threads, 5.2x at
// eight, and 2.1x at sixteen - sixteen being worse than eight, because the
// dispatch costs more than the extra threads return.
//
// So: eight, and sleeping. Spinning wakes an order of magnitude faster and it
// does not matter, because the cull is 0.66 ms and 17 microseconds of it is two
// and a half per cent; what spinning would cost is seven cores burning while
// the loader thread and the driver want them. The number that would change this
// is the job getting much smaller, not the thread count getting larger.
//
// The calling thread works too, so N threads means N-1 of these.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace render
{

class Pool
{
  public:
    // Total threads including the caller. Zero picks eight, or fewer on a
    // smaller machine.
    explicit Pool(unsigned threads = 0);
    ~Pool();

    Pool(const Pool &) = delete;
    Pool &operator=(const Pool &) = delete;

    // Runs body(0..count-1) across the pool and returns when all of it is done.
    // Tasks are taken in runs of `grain`, so a median task of six instances is
    // not one atomic increment each; the run size wants to be large enough that
    // the increment is noise and small enough that the last thread is not left
    // holding a tail.
    void forEach(std::size_t count, std::size_t grain, const std::function<void(std::size_t)> &body);

    unsigned threads() const { return m_workers + 1; }

  private:
    void run();
    void work();

    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::mutex m_finishing;
    std::condition_variable m_finished;

    std::uint64_t m_epoch = 0;
    bool m_stopping = false;
    unsigned m_workers = 0;

    std::atomic<std::size_t> m_next{0};
    std::atomic<unsigned> m_done{0};
    std::size_t m_count = 0;
    std::size_t m_grain = 1;
    const std::function<void(std::size_t)> *m_body = nullptr;
};

} // namespace render

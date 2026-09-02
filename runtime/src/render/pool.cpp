#include "pool.h"

#include <algorithm>

namespace render
{

Pool::Pool(unsigned threads)
{
    // Eight was measured as the best of 2, 4, 8 and 16 on a job the size and
    // shape of the cull. Sixteen was worse than eight.
    const unsigned available = std::max(1u, std::thread::hardware_concurrency());
    const unsigned wanted = threads != 0 ? threads : std::min(8u, available);
    m_workers = wanted > 1 ? wanted - 1 : 0;

    m_threads.reserve(m_workers);
    for (unsigned index = 0; index < m_workers; ++index)
        m_threads.emplace_back([this, index] { run(index); });
}

Pool::~Pool()
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    for (std::thread &thread : m_threads)
        thread.join();
}

void Pool::forEach(std::size_t count, std::size_t grain, const std::function<void(std::size_t, unsigned)> &body)
{
    if (count == 0)
        return;
    if (m_workers == 0)
    {
        for (std::size_t at = 0; at < count; ++at)
            body(at, 0);
        return;
    }

    m_body = &body;
    m_count = count;
    m_grain = std::max<std::size_t>(grain, 1);
    m_next.store(0, std::memory_order_relaxed);
    m_done.store(0, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        ++m_epoch;
    }
    m_wake.notify_all();

    // The caller is a worker too, which is both one more thread and the reason
    // the common case of a tiny job costs almost nothing: it is finished before
    // the others have woken.
    work(m_workers);

    std::unique_lock<std::mutex> guard(m_finishing);
    m_finished.wait(guard, [this] { return m_done.load(std::memory_order_acquire) == m_workers; });
    m_body = nullptr;
}

void Pool::run(unsigned index)
{
    std::uint64_t seen = 0;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> guard(m_mutex);
            m_wake.wait(guard, [this, seen] { return m_stopping || m_epoch != seen; });
            if (m_stopping)
                return;
            seen = m_epoch;
        }

        work(index);

        if (m_done.fetch_add(1, std::memory_order_acq_rel) + 1 == m_workers)
        {
            // Taken so that a caller already inside wait() cannot miss this.
            std::lock_guard<std::mutex> guard(m_finishing);
            m_finished.notify_one();
        }
    }
}

void Pool::work(unsigned index)
{
    for (;;)
    {
        const std::size_t at = m_next.fetch_add(m_grain, std::memory_order_relaxed);
        if (at >= m_count)
            return;
        const std::size_t end = std::min(at + m_grain, m_count);
        for (std::size_t task = at; task < end; ++task)
            (*m_body)(task, index);
    }
}

} // namespace render

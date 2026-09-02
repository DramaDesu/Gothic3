// Measures what the pool costs to dispatch, so the numbers in pool.h stay
// checkable rather than becoming folklore.
//
// The job is shaped like the cull: 1591 tasks whose sizes match the real batch
// distribution - biggest 2367, median 6, the ten biggest a quarter of the whole
// - each doing a fixed amount of arithmetic per instance.

#include "render/pool.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

std::vector<std::size_t> batchesLikeTheWorld()
{
    std::vector<std::size_t> sizes;
    std::uint32_t state = 12345;
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    sizes.push_back(2367);
    for (int big = 0; big < 9; ++big)
        sizes.push_back(900 + next() % 700);
    while (sizes.size() < 1591)
        sizes.push_back(1 + next() % 60);
    return sizes;
}

double medianOf(std::vector<double> times)
{
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

} // namespace

int main(int argc, char **argv)
{
    const int passes = argc > 1 ? std::atoi(argv[1]) : 2000;
    const std::vector<std::size_t> sizes = batchesLikeTheWorld();
    std::size_t instances = 0;
    for (std::size_t size : sizes)
        instances += size;
    std::printf("%zu tasks holding %zu instances\n", sizes.size(), instances);

    std::vector<float> sink(sizes.size(), 0.0f);
    const auto body = [&sizes, &sink](std::size_t task, unsigned) {
        float total = 0.0f;
        for (std::size_t at = 0; at < sizes[task]; ++at)
        {
            float value = float(at) * 0.5f + 1.0f;
            for (int step = 0; step < 6; ++step)
                value = value * 1.000001f + float(step);
            total += value;
        }
        sink[task] = total;
    };

    const auto measure = [passes](const char *what, const std::function<void()> &run) {
        for (int warm = 0; warm < passes / 10 + 1; ++warm)
            run();
        std::vector<double> times;
        times.reserve(std::size_t(passes));
        for (int pass = 0; pass < passes; ++pass)
        {
            const auto start = Clock::now();
            run();
            times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        }
        const double median = medianOf(times);
        std::printf("%-26s median %6.3f ms\n", what, median);
        return median;
    };

    const double serial = measure("serial", [&] {
        for (std::size_t task = 0; task < sizes.size(); ++task)
            body(task, 0);
    });

    for (unsigned threads : {2u, 4u, 8u, 16u})
    {
        render::Pool pool(threads);
        const double got =
            measure((std::to_string(threads) + " threads").c_str(), [&] { pool.forEach(sizes.size(), 8, body); });
        std::printf("    %.2fx of a possible %u\n", serial / got, threads);
    }

    // The dispatch on its own: what a loop pays before any work happens.
    const auto nothing = [](std::size_t, unsigned) {};
    for (unsigned threads : {4u, 8u, 16u})
    {
        render::Pool pool(threads);
        measure(("empty, " + std::to_string(threads) + " threads").c_str(),
                [&] { pool.forEach(threads, 1, nothing); });
    }
    return 0;
}

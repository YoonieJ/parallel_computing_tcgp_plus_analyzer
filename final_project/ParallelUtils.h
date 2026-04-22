#ifndef PARALLELUTILS_H
#define PARALLELUTILS_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#if __has_include(<omp.h>)
#include <omp.h>
#define TCGPPLUS_HAS_OPENMP 1
#else
#define TCGPPLUS_HAS_OPENMP 0
#endif

namespace parallel {

inline int &requestedThreadCount() {
    static int value = 0;
    return value;
}

inline int getMaxThreads() {
#if TCGPPLUS_HAS_OPENMP
    return std::max(1, omp_get_max_threads());
#else
    const unsigned int hardwareThreads = std::thread::hardware_concurrency();
    return std::max(1u, hardwareThreads);
#endif
}

inline void setThreadCount(int threads) {
    requestedThreadCount() = std::max(1, threads);
#if TCGPPLUS_HAS_OPENMP
    omp_set_num_threads(requestedThreadCount());
#endif
}

inline void disableDynamicThreading() {
#if TCGPPLUS_HAS_OPENMP
    omp_set_dynamic(0);
#endif
}

inline int workerCount(std::size_t taskCount) {
    if (taskCount == 0) {
        return 1;
    }

    const int configuredThreads =
        requestedThreadCount() > 0 ? requestedThreadCount() : getMaxThreads();
    return std::max(1, std::min(configuredThreads, static_cast<int>(taskCount)));
}

template <typename T, typename Worker>
std::vector<T> map(std::size_t taskCount, const Worker &worker) {
    std::vector<T> results(taskCount);

#if TCGPPLUS_HAS_OPENMP
    #pragma omp parallel for schedule(dynamic)
    for (long long i = 0; i < static_cast<long long>(taskCount); ++i) {
        results[static_cast<std::size_t>(i)] = worker(static_cast<std::size_t>(i));
    }
#else
    const int threads = workerCount(taskCount);
    if (threads == 1) {
        for (std::size_t i = 0; i < taskCount; ++i) {
            results[i] = worker(i);
        }
        return results;
    }

    std::atomic<std::size_t> nextIndex{0};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));

    for (int thread = 0; thread < threads; ++thread) {
        workers.emplace_back([&]() {
            while (true) {
                const std::size_t index = nextIndex.fetch_add(1);
                if (index >= taskCount) {
                    break;
                }
                results[index] = worker(index);
            }
        });
    }

    for (auto &workerThread : workers) {
        workerThread.join();
    }
#endif

    return results;
}

template <typename Worker>
double sum(int taskCount, const Worker &worker) {
    if (taskCount <= 0) {
        return 0.0;
    }

#if TCGPPLUS_HAS_OPENMP
    double total = 0.0;
    #pragma omp parallel for reduction(+:total) schedule(dynamic)
    for (int i = 0; i < taskCount; ++i) {
        total += worker(i);
    }
    return total;
#else
    const int threads = workerCount(static_cast<std::size_t>(taskCount));
    if (threads == 1) {
        double total = 0.0;
        for (int i = 0; i < taskCount; ++i) {
            total += worker(i);
        }
        return total;
    }

    std::atomic<int> nextIndex{0};
    std::vector<double> partialSums(static_cast<std::size_t>(threads), 0.0);
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));

    for (int thread = 0; thread < threads; ++thread) {
        workers.emplace_back([&, thread]() {
            double localSum = 0.0;
            while (true) {
                const int index = nextIndex.fetch_add(1);
                if (index >= taskCount) {
                    break;
                }
                localSum += worker(index);
            }
            partialSums[static_cast<std::size_t>(thread)] = localSum;
        });
    }

    for (auto &workerThread : workers) {
        workerThread.join();
    }

    double total = 0.0;
    for (double partial : partialSums) {
        total += partial;
    }
    return total;
#endif
}

}  // namespace parallel

#endif

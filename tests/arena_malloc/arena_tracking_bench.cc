/*
 *     Copyright 2019-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

/*
 * Benchmark memory accounting - orginially a kv_engine benchmark which tested
 * EPStats memAllocated/memDeallocated, now ported to execute the JeArenaMalloc
 * tracking code.
 *
 * Two families of benchmark live here:
 *
 * 1) The MemoryAllocationStat benchmarks drive the tracker directly
 *    (memAllocated and the estimate/precise reads) with no jemalloc
 *    allocation, isolating the accounting cost.
 *
 * 2) The ArenaMalloc benchmarks drive the full JEArenaMalloc::malloc/free
 *    path with a registered client, i.e. what every operator new/delete in a
 *    tracked process pays. The NoClient variants run the same allocation
 *    pattern with no client switched in, so the difference between the two is
 *    the cost of tracking. The UnderLoad variants time a single reader thread
 *    whilst background threads allocate and free, which models the stats and
 *    quota-check paths of kv_engine.
 *
 * Every benchmark reports items_per_second where one item is one
 * allocation + free (or one read for the *UnderLoad benchmarks), so results
 * across thread counts and platforms can be compared directly.
 */

#include <benchmark/benchmark.h>
#include <platform/je_arena_malloc.h>
#include <platform/sysinfo.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

template <class trackingImpl>
class TestJEArenaMalloc : public cb::JEArenaMalloc {
public:
    static void clientRegistered(const cb::ArenaMallocClient& client,
                                 bool arenaDebugChecksEnabled) {
        trackingImpl::clientRegistered(client, arenaDebugChecksEnabled);
    }

    static void memAllocated(uint8_t index, size_t size) {
        trackingImpl::memAllocated(index, cb::MemoryDomain::Primary, size);
    }

    static void memDeallocated(uint8_t index, size_t size) {
        trackingImpl::memDeallocated(index, cb::MemoryDomain::Primary, size);
    }
};

using BenchJEArenaMalloc = TestJEArenaMalloc<cb::JEArenaCoreLocalTracker>;

class MemoryAllocationStat : public benchmark::Fixture {
public:
    cb::ArenaMallocClient client{{}, 1, true};
};

BENCHMARK_DEFINE_F(MemoryAllocationStat, AllocNRead1)(benchmark::State& state) {
    if (state.thread_index() == 0) {
        // memUsed merge must be 4 times higher so in theory we merge at the
        // same rate as TLS (because 4 more threads than cores).
        client.estimateUpdateThreshold = 10240 * 4;
        BenchJEArenaMalloc::setAllocatedThreshold(client);
    }

    while (state.KeepRunning()) {
        // range = allocations per read
        for (int i = 0; i < state.range(0); i++) {
            if (i == 0) {
                // calling this will clear the counters
                BenchJEArenaMalloc::clientRegistered(client, false);
            } else {
                BenchJEArenaMalloc::memAllocated(1, 128);
            }
        }
        BenchJEArenaMalloc::getEstimatedAllocated(client);
    }
    state.SetItemsProcessed(state.iterations() *
                            std::max<int64_t>(1, state.range(0)));
}

BENCHMARK_DEFINE_F(MemoryAllocationStat, AllocNReadM)(benchmark::State& state) {
    if (state.thread_index() == 0) {
        // memUsed merge must be 4 times higher so in theory we merge at the
        // same rate as TLS (because 4 more threads than cores).
        client.estimateUpdateThreshold = 10240 * 4;
        BenchJEArenaMalloc::setAllocatedThreshold(client);
    }

    while (state.KeepRunning()) {
        // range = allocations per read
        for (int i = 0; i < state.range(0); i++) {
            if (i == 0) {
                BenchJEArenaMalloc::clientRegistered(client, false);
            } else {
                BenchJEArenaMalloc::memAllocated(1, 128);
            }
        }
        for (int j = 0; j < state.range(1); j++) {
            BenchJEArenaMalloc::getEstimatedAllocated(client);
        }
    }
    state.SetItemsProcessed(state.iterations() *
                            std::max<int64_t>(1, state.range(0)));
}

BENCHMARK_DEFINE_F(MemoryAllocationStat, AllocNReadPreciseM)
(benchmark::State& state) {
    if (state.thread_index() == 0) {
        // memUsed merge must be 4 times higher so in theory we merge at the
        // same rate as TLS (because 4 more threads than cores).
        client.estimateUpdateThreshold = 10240 * 4;
        BenchJEArenaMalloc::setAllocatedThreshold(client);
    }

    while (state.KeepRunning()) {
        // range = allocations per read
        for (int i = 0; i < state.range(0); i++) {
            if (i == 0) {
                BenchJEArenaMalloc::clientRegistered(client, false);
            } else {
                BenchJEArenaMalloc::memAllocated(1, 128);
            }
        }
        for (int j = 0; j < state.range(1); j++) {
            BenchJEArenaMalloc::getPreciseAllocated(client);
        }
    }
    state.SetItemsProcessed(state.iterations() *
                            std::max<int64_t>(1, state.range(0)));
}

// Tests cover a rough, but realistic range seen from a running cluster (with
// pillowfight load). The range was discovered by counting calls to
// memAllocated/deallocated and then logging how many had occurred for each
// getEstimatedTotalMemoryUsed. A previous version of this file used the Range
// API and can be used if this test is being used to perform deeper analysis of
// this code.
BENCHMARK_REGISTER_F(MemoryAllocationStat, AllocNRead1)
        ->Threads(static_cast<int>(cb::get_cpu_count() * 4))
        ->Args({0})
        ->Args({200})
        ->Args({1000});

BENCHMARK_REGISTER_F(MemoryAllocationStat, AllocNReadM)
        ->Threads(static_cast<int>(cb::get_cpu_count() * 4))
        ->Args({0, 10})
        ->Args({200, 10})
        ->Args({1000, 10})
        ->Args({0, 1000})
        ->Args({200, 200})
        ->Args({1000, 10});

BENCHMARK_REGISTER_F(MemoryAllocationStat, AllocNReadPreciseM)
        ->Threads(static_cast<int>(cb::get_cpu_count() * 4))
        // This benchmark is configured to run 'alloc heavy'. The getPrecise
        // function is only used by getStats, which is infrequent relative to
        // memory alloc/dealloc
        ->Args({1000, 10})
        ->Args({100000, 10});

/**
 * Full allocation path benchmarks. These use the real JEArenaMalloc with a
 * registered client (and so a real jemalloc arena and thread caches).
 */
namespace {

/// Number of clients registered by arenaMallocSetup. The multi-client
/// benchmarks spread the threads over these clients to exercise the adjacent
/// per-client tracker state.
constexpr int BenchClients = 4;

/// Clients registered for the duration of a benchmark (see arenaMallocSetup)
std::vector<cb::ArenaMallocClient> benchClients;

/// Background allocation threads (see startBackgroundLoad)
std::vector<std::thread> backgroundThreads;
std::atomic<bool> backgroundStop{false};

/// Allocation size used by the background load threads
constexpr size_t BackgroundAllocSize = 128;

/// How many allocations the background load threads hold before freeing them
constexpr size_t BackgroundBatch = 64;

void arenaMallocSetup(const benchmark::State&) {
    benchClients.clear();
    for (int ii = 0; ii < BenchClients; ii++) {
        benchClients.push_back(cb::JEArenaMalloc::registerClient(true));
    }
}

void arenaMallocTeardown(const benchmark::State&) {
    for (const auto& client : benchClients) {
        cb::JEArenaMalloc::unregisterClient(client);
    }
    benchClients.clear();
}

/**
 * Allocate one block of `size` bytes per element of `pointers` and then free
 * them all. The pointers vector is allocated by the caller so that the
 * benchmark's own bookkeeping is not tracked against the client.
 * @return the number of allocations made.
 */
template <bool SizedFree>
size_t allocateAndFree(std::vector<void*>& pointers, size_t size) {
    for (auto& ptr : pointers) {
        ptr = cb::JEArenaMalloc::malloc(size);
        benchmark::DoNotOptimize(ptr);
    }
    for (auto& ptr : pointers) {
        if constexpr (SizedFree) {
            cb::JEArenaMalloc::sized_free(ptr, size);
        } else {
            cb::JEArenaMalloc::free(ptr);
        }
    }
    return pointers.size();
}

/**
 * Start `count` threads which allocate and free against `client` until
 * stopBackgroundLoad is called.
 */
void startBackgroundLoad(const cb::ArenaMallocClient& client, size_t count) {
    backgroundStop = false;
    for (size_t ii = 0; ii < count; ii++) {
        backgroundThreads.emplace_back([&client]() {
            std::vector<void*> pointers(BackgroundBatch);
            cb::JEArenaMalloc::switchToClient(
                    client, cb::MemoryDomain::Primary, true);
            while (!backgroundStop.load(std::memory_order_relaxed)) {
                allocateAndFree<false>(pointers, BackgroundAllocSize);
            }
            cb::JEArenaMalloc::switchFromClient();
        });
    }
}

void stopBackgroundLoad() {
    backgroundStop = true;
    for (auto& thread : backgroundThreads) {
        thread.join();
    }
    backgroundThreads.clear();
}

void underLoadSetup(const benchmark::State& state) {
    arenaMallocSetup(state);
    // Background threads, leaving one core for the timed reader thread.
    startBackgroundLoad(benchClients.front(),
                        std::max<size_t>(1, cb::get_cpu_count() - 1));
}

void underLoadTeardown(const benchmark::State& state) {
    stopBackgroundLoad();
    arenaMallocTeardown(state);
}

} // namespace

class ArenaMalloc : public benchmark::Fixture {
public:
    /// @return the client this thread should use
    static const cb::ArenaMallocClient& clientFor(const benchmark::State& state,
                                                  int clients) {
        return benchClients.at(state.thread_index() % clients);
    }
};

/**
 * range(0) - allocation size
 * range(1) - allocations held before freeing (batch size)
 */
BENCHMARK_DEFINE_F(ArenaMalloc, MallocFree)(benchmark::State& state) {
    std::vector<void*> pointers(state.range(1));
    cb::JEArenaMalloc::switchToClient(
            clientFor(state, 1), cb::MemoryDomain::Primary, true);
    size_t items = 0;
    for (auto _ : state) {
        items += allocateAndFree<false>(pointers, state.range(0));
    }
    cb::JEArenaMalloc::switchFromClient();
    state.SetItemsProcessed(items);
}

/// As MallocFree but using sized_free (the operator delete(ptr, size) path).
BENCHMARK_DEFINE_F(ArenaMalloc, MallocSizedFree)(benchmark::State& state) {
    std::vector<void*> pointers(state.range(1));
    cb::JEArenaMalloc::switchToClient(
            clientFor(state, 1), cb::MemoryDomain::Primary, true);
    size_t items = 0;
    for (auto _ : state) {
        items += allocateAndFree<true>(pointers, state.range(0));
    }
    cb::JEArenaMalloc::switchFromClient();
    state.SetItemsProcessed(items);
}

/**
 * As MallocFree but with no client switched in; no tracking occurs and the
 * default jemalloc arena is used. The difference between this and MallocFree
 * is the cost of tracking (plus any arena effects).
 */
BENCHMARK_DEFINE_F(ArenaMalloc, MallocFreeNoClient)(benchmark::State& state) {
    cb::JEArenaMalloc::switchFromClient();
    std::vector<void*> pointers(state.range(1));
    size_t items = 0;
    for (auto _ : state) {
        items += allocateAndFree<false>(pointers, state.range(0));
    }
    state.SetItemsProcessed(items);
}

/**
 * As MallocFree but the threads are spread over BenchClients clients, so
 * several clients' tracker state is hot at once.
 */
BENCHMARK_DEFINE_F(ArenaMalloc, MallocFreeMultiClient)
(benchmark::State& state) {
    std::vector<void*> pointers(state.range(1));
    cb::JEArenaMalloc::switchToClient(
            clientFor(state, BenchClients), cb::MemoryDomain::Primary, true);
    size_t items = 0;
    for (auto _ : state) {
        items += allocateAndFree<false>(pointers, state.range(0));
    }
    cb::JEArenaMalloc::switchFromClient();
    state.SetItemsProcessed(items);
}

/**
 * MallocFree where every thread also reads the estimated allocation after
 * each batch, as kv_engine front-end threads do for quota checks.
 * range(0) - allocation size
 * range(1) - allocations held before freeing (batch size), one estimate read
 *            per batch.
 */
BENCHMARK_DEFINE_F(ArenaMalloc, MallocFreeEstimateRead)
(benchmark::State& state) {
    const auto& client = clientFor(state, 1);
    std::vector<void*> pointers(state.range(1));
    cb::JEArenaMalloc::switchToClient(client, cb::MemoryDomain::Primary, true);
    size_t items = 0;
    for (auto _ : state) {
        items += allocateAndFree<false>(pointers, state.range(0));
        benchmark::DoNotOptimize(
                cb::JEArenaMalloc::getEstimatedAllocated(client));
    }
    cb::JEArenaMalloc::switchFromClient();
    state.SetItemsProcessed(items);
}

/**
 * Time getPreciseAllocated on a single thread whilst (cpu_count - 1)
 * background threads allocate and free against the same client.
 */
BENCHMARK_DEFINE_F(ArenaMalloc, PreciseReadUnderLoad)
(benchmark::State& state) {
    const auto& client = benchClients.front();
    for (auto _ : state) {
        benchmark::DoNotOptimize(
                cb::JEArenaMalloc::getPreciseAllocated(client));
    }
    state.SetItemsProcessed(state.iterations());
}

/**
 * Time getEstimatedAllocated on a single thread whilst (cpu_count - 1)
 * background threads allocate and free against the same client.
 */
BENCHMARK_DEFINE_F(ArenaMalloc, EstimateReadUnderLoad)
(benchmark::State& state) {
    const auto& client = benchClients.front();
    for (auto _ : state) {
        benchmark::DoNotOptimize(
                cb::JEArenaMalloc::getEstimatedAllocated(client));
    }
    state.SetItemsProcessed(state.iterations());
}

// Allocation sizes: a small object, a typical document sized allocation and a
// 'large' allocation beyond the thread cache's default limit. Batch of 64 is
// enough to exercise the tcache fill/flush paths.
static void allocArgs(benchmark::internal::Benchmark* bench) {
    bench->Args({64, 64})->Args({512, 64})->Args({32768, 8});
}

static const int cpuCount = static_cast<int>(cb::get_cpu_count());

BENCHMARK_REGISTER_F(ArenaMalloc, MallocFree)
        ->Apply(allocArgs)
        ->Setup(arenaMallocSetup)
        ->Teardown(arenaMallocTeardown)
        ->Threads(1)
        ->Threads(cpuCount)
        ->Threads(cpuCount * 4);

BENCHMARK_REGISTER_F(ArenaMalloc, MallocSizedFree)
        ->Apply(allocArgs)
        ->Setup(arenaMallocSetup)
        ->Teardown(arenaMallocTeardown)
        ->Threads(1)
        ->Threads(cpuCount);

BENCHMARK_REGISTER_F(ArenaMalloc, MallocFreeNoClient)
        ->Apply(allocArgs)
        ->Threads(1)
        ->Threads(cpuCount)
        ->Threads(cpuCount * 4);

BENCHMARK_REGISTER_F(ArenaMalloc, MallocFreeMultiClient)
        ->Args({64, 64})
        ->Setup(arenaMallocSetup)
        ->Teardown(arenaMallocTeardown)
        ->Threads(cpuCount)
        ->Threads(cpuCount * 4);

BENCHMARK_REGISTER_F(ArenaMalloc, MallocFreeEstimateRead)
        ->Args({64, 64})
        ->Args({64, 8})
        ->Setup(arenaMallocSetup)
        ->Teardown(arenaMallocTeardown)
        ->Threads(cpuCount)
        ->Threads(cpuCount * 4);

BENCHMARK_REGISTER_F(ArenaMalloc, PreciseReadUnderLoad)
        ->Setup(underLoadSetup)
        ->Teardown(underLoadTeardown)
        ->Threads(1);

BENCHMARK_REGISTER_F(ArenaMalloc, EstimateReadUnderLoad)
        ->Setup(underLoadSetup)
        ->Teardown(underLoadTeardown)
        ->Threads(1);

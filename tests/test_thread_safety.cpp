#include "arena.h"
#include "pool.h"
#include "slab.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
size_t worker_count()
{
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0)
        return 8;
    return std::min<size_t>(hw, 16);
}

void wait_for_start(const std::atomic<bool>& start)
{
    while (!start.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
}

constexpr std::array<size_t, 10> SLAB_SIZE_CLASSES = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096,
};

size_t slab_class_size(size_t requested)
{
    for (size_t size : SLAB_SIZE_CLASSES)
    {
        if (requested <= size)
            return size;
    }
    return 0;
}
} // namespace

TEST_CASE("Arena thread safety: concurrent fixed-size allocations stay unique", "[arena][thread][.]")
{
    const size_t threads = worker_count();
    const size_t alloc_size = 32;
    const size_t allocs_per_thread = 512;
    AL::arena arena(threads * allocs_per_thread * alloc_size);

    std::atomic<bool> start{false};
    std::atomic<size_t> null_allocations{0};
    std::vector<std::vector<void*>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(allocs_per_thread);
            wait_for_start(start);

            for (size_t i = 0; i < allocs_per_thread; ++i)
            {
                void* ptr = arena.alloc(alloc_size);
                if (ptr == nullptr)
                {
                    null_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                local.push_back(ptr);
                static_cast<std::byte*>(ptr)[0] = static_cast<std::byte>((tid + i) & 0xFF);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(null_allocations.load(std::memory_order_relaxed) == 0);

    std::unordered_set<void*> unique_ptrs;
    unique_ptrs.reserve(threads * allocs_per_thread);
    size_t total_allocs = 0;
    for (const auto& local : allocated)
    {
        for (void* ptr : local)
        {
            REQUIRE(unique_ptrs.insert(ptr).second);
        }
        total_allocs += local.size();
    }

    REQUIRE(total_allocs == threads * allocs_per_thread);
    REQUIRE(arena.get_used() == total_allocs * alloc_size);
}

TEST_CASE("Arena thread safety: concurrent exhaustion is bounded by capacity", "[arena][thread][.]")
{
    const size_t threads = worker_count();
    const size_t alloc_size = 16;
    const size_t capacity_slots = 4096;
    const size_t attempts_per_thread = (capacity_slots / threads) + 256;

    AL::arena arena(capacity_slots * alloc_size);
    std::atomic<bool> start{false};
    std::atomic<size_t> successful_allocs{0};
    std::vector<std::vector<void*>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(attempts_per_thread);
            wait_for_start(start);

            for (size_t i = 0; i < attempts_per_thread; ++i)
            {
                void* ptr = arena.alloc(alloc_size);
                if (ptr == nullptr)
                    continue;

                local.push_back(ptr);
                successful_allocs.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    const size_t success_count = successful_allocs.load(std::memory_order_relaxed);
    REQUIRE(success_count == capacity_slots);
    REQUIRE(arena.get_used() == capacity_slots * alloc_size);

    std::unordered_set<void*> unique_ptrs;
    unique_ptrs.reserve(success_count);
    for (const auto& local : allocated)
    {
        for (void* ptr : local)
            REQUIRE(unique_ptrs.insert(ptr).second);
    }
    REQUIRE(unique_ptrs.size() == capacity_slots);
}

TEST_CASE("Arena thread safety: concurrent calloc returns zeroed blocks", "[arena][thread][.]")
{
    const size_t threads = worker_count();
    const size_t alloc_size = 64;
    const size_t allocs_per_thread = 256;
    AL::arena arena(threads * allocs_per_thread * alloc_size);

    std::atomic<bool> start{false};
    std::atomic<size_t> null_allocations{0};
    std::atomic<size_t> non_zero_blocks{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);

            for (size_t i = 0; i < allocs_per_thread; ++i)
            {
                auto* ptr = static_cast<std::byte*>(arena.calloc(alloc_size));
                if (ptr == nullptr)
                {
                    null_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                bool all_zero = true;
                for (size_t j = 0; j < alloc_size; ++j)
                {
                    if (ptr[j] != std::byte{0})
                    {
                        all_zero = false;
                        break;
                    }
                }
                if (!all_zero)
                    non_zero_blocks.fetch_add(1, std::memory_order_relaxed);

                std::memset(ptr, static_cast<int>((tid + i) & 0xFF), alloc_size);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(null_allocations.load(std::memory_order_relaxed) == 0);
    REQUIRE(non_zero_blocks.load(std::memory_order_relaxed) == 0);
    REQUIRE(arena.get_used() == threads * allocs_per_thread * alloc_size);
}

TEST_CASE("Arena thread safety: zero-length alloc remains stable under contention", "[arena][thread][.]")
{
    const size_t threads = worker_count();
    const size_t iterations = 10000;
    AL::arena arena(4096);

    std::atomic<bool> start{false};
    std::atomic<size_t> non_null_returns{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                if (arena.alloc(0) != nullptr)
                    non_null_returns.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(non_null_returns.load(std::memory_order_relaxed) == 0);
    REQUIRE(arena.get_used() == 0);
}

TEST_CASE("Arena thread safety: reset after synchronized workers restores allocator", "[arena][thread][.]")
{
    const size_t threads = worker_count();
    const size_t alloc_size = 24;
    const size_t allocs_per_thread = 200;
    AL::arena arena(threads * allocs_per_thread * alloc_size);

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&] {
            wait_for_start(start);
            for (size_t i = 0; i < allocs_per_thread; ++i)
                static_cast<void>(arena.alloc(alloc_size));
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(arena.get_used() > 0);
    REQUIRE(arena.reset() == 0);
    REQUIRE(arena.get_used() == 0);
    REQUIRE(arena.alloc(alloc_size) != nullptr);
}

TEST_CASE("Pool thread safety: concurrent full allocation returns unique blocks", "[pool][thread][.]")
{
    const size_t threads = worker_count();
    const size_t block_size = 64;
    const size_t allocs_per_thread = 128;
    const size_t block_count = threads * allocs_per_thread;
    AL::pool pool(block_size, block_count);

    std::atomic<bool> start{false};
    std::atomic<size_t> null_allocations{0};
    std::vector<std::vector<void*>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(allocs_per_thread);
            wait_for_start(start);

            for (size_t i = 0; i < allocs_per_thread; ++i)
            {
                void* ptr = pool.alloc();
                if (ptr == nullptr)
                {
                    null_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                local.push_back(ptr);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(null_allocations.load(std::memory_order_relaxed) == 0);

    std::unordered_set<void*> unique_ptrs;
    unique_ptrs.reserve(block_count);
    size_t total_allocs = 0;
    for (const auto& local : allocated)
    {
        for (void* ptr : local)
            REQUIRE(unique_ptrs.insert(ptr).second);
        total_allocs += local.size();
    }

    REQUIRE(total_allocs == block_count);
    REQUIRE(unique_ptrs.size() == block_count);
    REQUIRE(pool.get_free_space() == 0);
}

TEST_CASE("Pool thread safety: concurrent exhaustion is bounded by block count", "[pool][thread][.]")
{
    const size_t threads = worker_count();
    const size_t block_size = 128;
    const size_t block_count = threads * 64;
    const size_t attempts_per_thread = 128;
    AL::pool pool(block_size, block_count);

    std::atomic<bool> start{false};
    std::atomic<size_t> successful_allocs{0};
    std::vector<std::vector<void*>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(attempts_per_thread);
            wait_for_start(start);

            for (size_t i = 0; i < attempts_per_thread; ++i)
            {
                void* ptr = pool.alloc();
                if (ptr == nullptr)
                    continue;
                local.push_back(ptr);
                successful_allocs.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(successful_allocs.load(std::memory_order_relaxed) == block_count);

    std::unordered_set<void*> unique_ptrs;
    unique_ptrs.reserve(block_count);
    for (const auto& local : allocated)
    {
        for (void* ptr : local)
            REQUIRE(unique_ptrs.insert(ptr).second);
    }
    REQUIRE(unique_ptrs.size() == block_count);

    start.store(false, std::memory_order_release);
    workers.clear();
    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (void* ptr : allocated[tid])
                pool.free(ptr);
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(pool.get_free_space() == pool.get_block_size() * pool.get_block_count());
}

TEST_CASE("Pool thread safety: concurrent alloc/free churn keeps accounting stable", "[pool][thread][.]")
{
    const size_t threads = worker_count();
    const size_t block_size = 64;
    const size_t block_count = threads * 32;
    const size_t iterations = 5000;
    AL::pool pool(block_size, block_count);

    std::atomic<bool> start{false};
    std::atomic<size_t> successful_cycles{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);

            for (size_t i = 0; i < iterations; ++i)
            {
                void* ptr = pool.alloc();
                if (ptr == nullptr)
                    continue;

                std::memset(ptr, static_cast<int>((tid + i) & 0xFF), block_size);
                pool.free(ptr);
                successful_cycles.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(successful_cycles.load(std::memory_order_relaxed) > 0);
    REQUIRE(pool.get_free_space() == pool.get_block_size() * pool.get_block_count());
}

TEST_CASE("Pool thread safety: concurrent calloc returns zeroed blocks", "[pool][thread][.]")
{
    const size_t threads = worker_count();
    const size_t block_size = 128;
    const size_t block_count = threads * 32;
    const size_t iterations = 2000;
    AL::pool pool(block_size, block_count);

    std::atomic<bool> start{false};
    std::atomic<size_t> non_zero_blocks{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);

            for (size_t i = 0; i < iterations; ++i)
            {
                auto* ptr = static_cast<std::byte*>(pool.calloc());
                if (ptr == nullptr)
                    continue;

                bool all_zero = true;
                for (size_t j = 0; j < block_size; ++j)
                {
                    if (ptr[j] != std::byte{0})
                    {
                        all_zero = false;
                        break;
                    }
                }
                if (!all_zero)
                    non_zero_blocks.fetch_add(1, std::memory_order_relaxed);

                std::memset(ptr, static_cast<int>((tid + i) & 0xFF), block_size);
                pool.free(ptr);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(non_zero_blocks.load(std::memory_order_relaxed) == 0);
    REQUIRE(pool.get_free_space() == pool.get_block_size() * pool.get_block_count());
}

TEST_CASE("Pool thread safety: concurrent free(nullptr) is safe", "[pool][thread][.]")
{
    const size_t threads = worker_count();
    const size_t iterations = 20000;
    AL::pool pool(64, 256);
    const size_t initial_free = pool.get_free_space();

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
                pool.free(nullptr);
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(pool.get_free_space() == initial_free);
}

TEST_CASE("Pool thread safety: reset after synchronized workers restores pool", "[pool][thread][.]")
{
    const size_t threads = worker_count();
    const size_t block_size = 64;
    const size_t block_count = threads * 32;
    const size_t iterations = 1500;
    AL::pool pool(block_size, block_count);

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                void* ptr = pool.alloc();
                if (ptr == nullptr)
                    continue;
                std::memset(ptr, static_cast<int>((tid + i) & 0xFF), block_size);
                pool.free(ptr);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    pool.reset();
    REQUIRE(pool.get_free_space() == pool.get_block_size() * pool.get_block_count());
    REQUIRE(pool.alloc() != nullptr);
}

TEST_CASE("Slab thread safety: concurrent mixed-size alloc/free remains stable", "[slab][thread][.]")
{
    const size_t threads = worker_count();
    const size_t iterations = 4000;
    constexpr std::array<size_t, 28> request_sizes = {
        1, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 2047, 2048, 2049, 4096,
    };

    AL::slab slab(4.0);
    const size_t initial_total_free = slab.get_total_free();
    std::atomic<bool> start{false};
    std::atomic<size_t> null_allocations{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);

            for (size_t i = 0; i < iterations; ++i)
            {
                const size_t request_size = request_sizes[(tid + i) % request_sizes.size()];
                void* ptr = slab.alloc(request_size);
                if (ptr == nullptr)
                {
                    null_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                auto* bytes = static_cast<std::byte*>(ptr);
                bytes[0] = static_cast<std::byte>((tid + i) & 0xFF);
                slab.free(ptr, request_size);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(null_allocations.load(std::memory_order_relaxed) == 0);
    REQUIRE(slab.get_total_free() == initial_total_free);
}

TEST_CASE("Slab thread safety: per-class contention restores each pool", "[slab][thread][.]")
{
    const size_t threads = std::max<size_t>(worker_count(), SLAB_SIZE_CLASSES.size());
    const size_t iterations = 5000;
    AL::slab slab(2.0);

    std::array<size_t, SLAB_SIZE_CLASSES.size()> initial_free{};
    for (size_t i = 0; i < SLAB_SIZE_CLASSES.size(); ++i)
        initial_free[i] = slab.get_pool_free_space(i);

    std::atomic<bool> start{false};
    std::atomic<size_t> null_allocations{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            const size_t size = SLAB_SIZE_CLASSES[tid % SLAB_SIZE_CLASSES.size()];
            wait_for_start(start);

            for (size_t i = 0; i < iterations; ++i)
            {
                void* ptr = slab.alloc(size);
                if (ptr == nullptr)
                {
                    null_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                slab.free(ptr, size);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(null_allocations.load(std::memory_order_relaxed) == 0);
    for (size_t i = 0; i < SLAB_SIZE_CLASSES.size(); ++i)
        REQUIRE(slab.get_pool_free_space(i) == initial_free[i]);
}

TEST_CASE("Slab thread safety: concurrent exhaustion is bounded within a size class", "[slab][thread][.]")
{
    const size_t threads = worker_count();
    constexpr size_t class_index = 3; // 64-byte size class
    constexpr size_t request_size = 64;
    AL::slab slab(0.05);

    const size_t block_size = slab.get_pool_block_size(class_index);
    const size_t block_count = slab.get_pool_free_space(class_index) / block_size;
    const size_t attempts_per_thread = (block_count / threads) + 32;

    std::atomic<bool> start{false};
    std::atomic<size_t> successful_allocs{0};
    std::vector<std::vector<void*>> allocated(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            auto& local = allocated[tid];
            local.reserve(attempts_per_thread);
            wait_for_start(start);

            for (size_t i = 0; i < attempts_per_thread; ++i)
            {
                void* ptr = slab.alloc(request_size);
                if (ptr == nullptr)
                    continue;
                local.push_back(ptr);
                successful_allocs.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(successful_allocs.load(std::memory_order_relaxed) == block_count);
    REQUIRE(slab.get_pool_free_space(class_index) == 0);

    std::unordered_set<void*> unique_ptrs;
    unique_ptrs.reserve(block_count);
    for (const auto& local : allocated)
    {
        for (void* ptr : local)
            REQUIRE(unique_ptrs.insert(ptr).second);
    }
    REQUIRE(unique_ptrs.size() == block_count);

    start.store(false, std::memory_order_release);
    workers.clear();
    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (void* ptr : allocated[tid])
                slab.free(ptr, request_size);
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(slab.get_pool_free_space(class_index) == block_count * block_size);
}

TEST_CASE("Slab thread safety: concurrent calloc returns zeroed size-class blocks", "[slab][thread][.]")
{
    const size_t threads = worker_count();
    const size_t iterations = 3000;
    constexpr std::array<size_t, 10> request_sizes = {
        7, 9, 17, 33, 65, 129, 257, 513, 1025, 2049,
    };
    AL::slab slab(3.0);

    std::atomic<bool> start{false};
    std::atomic<size_t> null_allocations{0};
    std::atomic<size_t> non_zero_blocks{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);

            for (size_t i = 0; i < iterations; ++i)
            {
                const size_t request_size = request_sizes[(tid + i) % request_sizes.size()];
                const size_t class_size = slab_class_size(request_size);
                auto* ptr = static_cast<std::byte*>(slab.calloc(request_size));
                if (ptr == nullptr)
                {
                    null_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                if (ptr[0] != std::byte{0} || ptr[class_size / 2] != std::byte{0} || ptr[class_size - 1] != std::byte{0})
                    non_zero_blocks.fetch_add(1, std::memory_order_relaxed);

                std::memset(ptr, static_cast<int>((tid + i) & 0xFF), class_size);
                slab.free(ptr, request_size);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    REQUIRE(null_allocations.load(std::memory_order_relaxed) == 0);
    REQUIRE(non_zero_blocks.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Slab thread safety: reset after synchronized workers restores all pools", "[slab][thread][.]")
{
    const size_t threads = worker_count();
    const size_t iterations = 1500;
    AL::slab slab(1.0);
    const size_t initial_total_free = slab.get_total_free();

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (size_t tid = 0; tid < threads; ++tid)
    {
        workers.emplace_back([&, tid] {
            wait_for_start(start);
            for (size_t i = 0; i < iterations; ++i)
            {
                const size_t size = SLAB_SIZE_CLASSES[(tid + i) % SLAB_SIZE_CLASSES.size()];
                void* ptr = slab.alloc(size);
                if (ptr == nullptr)
                    continue;
                slab.free(ptr, size);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : workers)
        t.join();

    slab.reset();
    REQUIRE(slab.get_total_free() == initial_total_free);

    for (size_t size : SLAB_SIZE_CLASSES)
    {
        void* ptr = slab.alloc(size);
        REQUIRE(ptr != nullptr);
        slab.free(ptr, size);
    }
}

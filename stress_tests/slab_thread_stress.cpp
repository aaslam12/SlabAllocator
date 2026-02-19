#include "slab.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace AL;

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
        std::this_thread::yield();
}

constexpr std::array<size_t, 10> SIZE_CLASSES = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096,
};
} // namespace

int main()
{
    const size_t threads = worker_count();

    std::cout << "\n=== Slab Threaded Stress Test ===" << std::endl;
    std::cout << "Threads: " << threads << '\n' << std::endl;

    // ========================================================================
    // Test 1: Mixed-size contention churn
    // ========================================================================
    {
        const size_t iterations_per_thread = 200000;
        constexpr std::array<size_t, 18> requests = {
            1, 8, 9, 16, 17, 32, 33, 64, 65, 128, 129, 256, 512, 1024, 1025, 2048, 2049, 4096,
        };

        slab s(3.0);
        const size_t initial_total_free = s.get_total_free();
        std::atomic<bool> start{false};
        std::atomic<size_t> null_allocations{0};
        std::vector<std::thread> workers;
        workers.reserve(threads);

        auto begin = std::chrono::high_resolution_clock::now();

        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&, tid] {
                wait_for_start(start);
                for (size_t i = 0; i < iterations_per_thread; ++i)
                {
                    const size_t req = requests[(tid + i) % requests.size()];
                    void* ptr = s.alloc(req);
                    if (ptr == nullptr)
                    {
                        null_allocations.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    std::memset(ptr, static_cast<int>((tid + i) & 0xFF), std::min<size_t>(req, 64));
                    s.free(ptr, req);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers)
            t.join();

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - begin;

        if (null_allocations.load(std::memory_order_relaxed) != 0)
        {
            std::cerr << "ERROR: Unexpected allocation failures during mixed-size churn" << std::endl;
            return 1;
        }
        if (s.get_total_free() != initial_total_free)
        {
            std::cerr << "ERROR: Total free space mismatch after mixed-size churn" << std::endl;
            return 1;
        }

        std::cout << "--- Test 1: Mixed-size contention churn ---\n"
                  << "Total operations: " << (threads * iterations_per_thread * 2) << '\n'
                  << "Elapsed:          " << elapsed.count() << " s\n"
                  << "[PASSED]\n"
                  << std::endl;
    }

    // ========================================================================
    // Test 2: One-size-class-per-thread contention
    // ========================================================================
    {
        const size_t contention_threads = std::max<size_t>(threads, SIZE_CLASSES.size());
        const size_t iterations_per_thread = 100000;
        slab s(2.0);

        std::array<size_t, SIZE_CLASSES.size()> initial_pool_free{};
        for (size_t i = 0; i < SIZE_CLASSES.size(); ++i)
            initial_pool_free[i] = s.get_pool_free_space(i);

        std::atomic<bool> start{false};
        std::atomic<size_t> null_allocations{0};
        std::vector<std::thread> workers;
        workers.reserve(contention_threads);

        auto begin = std::chrono::high_resolution_clock::now();

        for (size_t tid = 0; tid < contention_threads; ++tid)
        {
            workers.emplace_back([&, tid] {
                const size_t req = SIZE_CLASSES[tid % SIZE_CLASSES.size()];
                wait_for_start(start);
                for (size_t i = 0; i < iterations_per_thread; ++i)
                {
                    void* ptr = s.alloc(req);
                    if (ptr == nullptr)
                    {
                        null_allocations.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    s.free(ptr, req);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : workers)
            t.join();

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - begin;

        if (null_allocations.load(std::memory_order_relaxed) != 0)
        {
            std::cerr << "ERROR: Unexpected allocation failures in per-class contention test" << std::endl;
            return 1;
        }

        for (size_t i = 0; i < SIZE_CLASSES.size(); ++i)
        {
            if (s.get_pool_free_space(i) != initial_pool_free[i])
            {
                std::cerr << "ERROR: Pool free-space mismatch for class index " << i << std::endl;
                return 1;
            }
        }

        std::cout << "--- Test 2: Per-class contention ---\n"
                  << "Threads:          " << contention_threads << '\n'
                  << "Elapsed:          " << elapsed.count() << " s\n"
                  << "[PASSED]\n"
                  << std::endl;
    }

    // ========================================================================
    // Test 3: Concurrent exhaustion/recovery for one size class
    // ========================================================================
    {
        constexpr size_t class_index = 0; // 8-byte class
        constexpr size_t request_size = 8;
        slab s(0.1);

        const size_t block_size = s.get_pool_block_size(class_index);
        const size_t block_count = s.get_pool_free_space(class_index) / block_size;
        const size_t attempts_per_thread = (block_count / threads) + 64;

        std::atomic<bool> start{false};
        std::atomic<size_t> successful_allocs{0};
        std::vector<std::vector<void*>> allocated(threads);
        std::vector<std::thread> workers;
        workers.reserve(threads);

        auto begin = std::chrono::high_resolution_clock::now();

        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&, tid] {
                auto& local = allocated[tid];
                local.reserve(attempts_per_thread);
                wait_for_start(start);
                for (size_t i = 0; i < attempts_per_thread; ++i)
                {
                    void* ptr = s.alloc(request_size);
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

        if (successful_allocs.load(std::memory_order_relaxed) != block_count)
        {
            std::cerr << "ERROR: Exhaustion mismatch. Expected " << block_count << ", got " << successful_allocs.load(std::memory_order_relaxed)
                      << std::endl;
            return 1;
        }
        if (s.get_pool_free_space(class_index) != 0)
        {
            std::cerr << "ERROR: Target size class should be fully exhausted" << std::endl;
            return 1;
        }

        std::unordered_set<void*> unique_ptrs;
        unique_ptrs.reserve(block_count);
        for (const auto& local : allocated)
        {
            for (void* ptr : local)
            {
                if (!unique_ptrs.insert(ptr).second)
                {
                    std::cerr << "ERROR: Duplicate pointer detected in exhaustion test" << std::endl;
                    return 1;
                }
            }
        }
        if (unique_ptrs.size() != block_count)
        {
            std::cerr << "ERROR: Unique pointer count mismatch in exhaustion test" << std::endl;
            return 1;
        }

        start.store(false, std::memory_order_release);
        workers.clear();
        for (size_t tid = 0; tid < threads; ++tid)
        {
            workers.emplace_back([&, tid] {
                wait_for_start(start);
                for (void* ptr : allocated[tid])
                    s.free(ptr, request_size);
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& t : workers)
            t.join();

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - begin;

        if (s.get_pool_free_space(class_index) != block_count * block_size)
        {
            std::cerr << "ERROR: Target size class free-space not restored after concurrent free" << std::endl;
            return 1;
        }
        void* neighbor = s.alloc(16);
        if (neighbor == nullptr)
        {
            std::cerr << "ERROR: Neighbor size classes should still be usable" << std::endl;
            return 1;
        }
        s.free(neighbor, 16);

        std::cout << "--- Test 3: Size-class exhaustion/recovery ---\n"
                  << "Class blocks:      " << block_count << '\n'
                  << "Elapsed:           " << elapsed.count() << " s\n"
                  << "[PASSED]\n"
                  << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "[PASSED] All slab threaded stress tests passed!" << std::endl;
    std::cout << "========================================\n" << std::endl;
    return 0;
}

#include "pool.h"
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <set>
#include <unistd.h>
#include <vector>

const size_t PAGE_SIZE = getpagesize();

// ============================================================================
// Basic Functionality Tests
// ============================================================================

TEST_CASE("Pool: Basic construction", "[pool][basic]")
{
    SECTION("Single block pool")
    {
        AL::pool p(64, 1);
        REQUIRE(p.get_capacity() >= 64);
        REQUIRE(p.get_free_space() >= 64);
    }

    SECTION("Multiple blocks pool")
    {
        AL::pool p(64, 100);
        REQUIRE(p.get_capacity() >= 64 * 100);
        REQUIRE(p.get_free_space() == 64 * 100);
    }

    SECTION("Small block size (rounds up to pointer size)")
    {
        AL::pool p(1, 10);
        REQUIRE(p.get_capacity() >= sizeof(void*) * 10);
    }

    SECTION("Large block size (power of 2 rounding)")
    {
        AL::pool p(100, 10);
        // Should round up to next power of 2
        REQUIRE(p.get_capacity() >= 128 * 10);
    }
}

// ============================================================================
// Allocation Tests
// ============================================================================

TEST_CASE("Pool: Basic allocations", "[pool][alloc]")
{
    AL::pool p(64, 10);

    SECTION("Single allocation")
    {
        void* ptr = p.alloc();
        REQUIRE(ptr != nullptr);
        REQUIRE(p.get_free_space() < p.get_capacity());
    }

    SECTION("Multiple allocations return distinct pointers")
    {
        void* p1 = p.alloc();
        void* p2 = p.alloc();
        void* p3 = p.alloc();

        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);
        REQUIRE(p3 != nullptr);

        REQUIRE(p1 != p2);
        REQUIRE(p2 != p3);
        REQUIRE(p1 != p3);
    }

    SECTION("Allocate all blocks")
    {
        std::vector<void*> ptrs;

        for (int i = 0; i < 10; ++i)
        {
            void* ptr = p.alloc();
            REQUIRE(ptr != nullptr);
            ptrs.push_back(ptr);
        }

        // All blocks allocated
        void* ptr = p.alloc();
        REQUIRE(ptr == nullptr);
    }

    SECTION("Allocations are properly aligned")
    {
        void* p1 = p.alloc();
        void* p2 = p.alloc();

        // Check alignment: either power of 2 aligned or pointer-size aligned
        bool p1_aligned = (reinterpret_cast<uintptr_t>(p1) % 64 == 0) || (reinterpret_cast<uintptr_t>(p1) % sizeof(void*) == 0);
        bool p2_aligned = (reinterpret_cast<uintptr_t>(p2) % 64 == 0) || (reinterpret_cast<uintptr_t>(p2) % sizeof(void*) == 0);

        REQUIRE(p1_aligned);
        REQUIRE(p2_aligned);
    }
}

TEST_CASE("Pool: Allocation exhaustion", "[pool][alloc][edge]")
{
    AL::pool p(64, 5);

    SECTION("Exactly fill pool")
    {
        std::vector<void*> ptrs;

        for (int i = 0; i < 5; ++i)
        {
            void* ptr = p.alloc();
            REQUIRE(ptr != nullptr);
            ptrs.push_back(ptr);
        }

        // Pool should be full
        void* ptr = p.alloc();
        REQUIRE(ptr == nullptr);

        // Free space should be 0
        REQUIRE(p.get_free_space() == 0);
    }

    SECTION("Allocation after exhaustion returns nullptr")
    {
        // Fill pool
        for (int i = 0; i < 5; ++i)
        {
            p.alloc();
        }

        // Try multiple times to allocate from full pool
        for (int i = 0; i < 10; ++i)
        {
            void* ptr = p.alloc();
            REQUIRE(ptr == nullptr);
        }
    }
}

// ============================================================================
// Free Tests
// ============================================================================

TEST_CASE("Pool: Basic free", "[pool][free]")
{
    AL::pool p(64, 10);

    SECTION("Free single allocation")
    {
        void* ptr = p.alloc();
        REQUIRE(ptr != nullptr);

        size_t free_before = p.get_free_space();
        p.free(ptr);
        size_t free_after = p.get_free_space();

        REQUIRE(free_after > free_before);
    }

    SECTION("Free nullptr is safe")
    {
        size_t free_before = p.get_free_space();
        p.free(nullptr);
        size_t free_after = p.get_free_space();

        REQUIRE(free_after == free_before);
    }

    SECTION("Freed block can be reallocated")
    {
        void* p1 = p.alloc();
        REQUIRE(p1 != nullptr);

        p.free(p1);

        void* p2 = p.alloc();
        REQUIRE(p2 != nullptr);
        REQUIRE(p2 == p1); // Should get same block back
    }

    SECTION("Free multiple allocations")
    {
        void* p1 = p.alloc();
        void* p2 = p.alloc();
        void* p3 = p.alloc();

        p.free(p1);
        p.free(p2);
        p.free(p3);

        // Should be able to allocate again
        void* p4 = p.alloc();
        void* p5 = p.alloc();
        void* p6 = p.alloc();

        REQUIRE(p4 != nullptr);
        REQUIRE(p5 != nullptr);
        REQUIRE(p6 != nullptr);
    }
}

TEST_CASE("Pool: Free order independence", "[pool][free]")
{
    AL::pool p(64, 10);

    SECTION("Free in LIFO order")
    {
        std::vector<void*> ptrs;
        for (int i = 0; i < 5; ++i)
        {
            ptrs.push_back(p.alloc());
        }

        // Free in reverse order
        for (auto it = ptrs.rbegin(); it != ptrs.rend(); ++it)
        {
            p.free(*it);
        }

        // Should be able to reallocate
        void* ptr = p.alloc();
        REQUIRE(ptr != nullptr);
    }

    SECTION("Free in FIFO order")
    {
        std::vector<void*> ptrs;
        for (int i = 0; i < 5; ++i)
        {
            ptrs.push_back(p.alloc());
        }

        // Free in forward order
        for (void* ptr : ptrs)
        {
            p.free(ptr);
        }

        // Should be able to reallocate
        void* ptr = p.alloc();
        REQUIRE(ptr != nullptr);
    }

    SECTION("Free in random order")
    {
        std::vector<void*> ptrs;
        for (int i = 0; i < 10; ++i)
        {
            ptrs.push_back(p.alloc());
        }

        // Free every other one first
        for (size_t i = 0; i < ptrs.size(); i += 2)
        {
            p.free(ptrs[i]);
        }

        // Then free the rest
        for (size_t i = 1; i < ptrs.size(); i += 2)
        {
            p.free(ptrs[i]);
        }

        // All blocks should be free
        REQUIRE(p.get_free_space() == 64 * 10); // block_size * block_count
    }
}

// ============================================================================
// Calloc Tests
// ============================================================================

TEST_CASE("Pool: Calloc zeros memory", "[pool][calloc]")
{
    AL::pool p(128, 10);

    SECTION("Calloc single block")
    {
        char* ptr = static_cast<char*>(p.calloc());
        REQUIRE(ptr != nullptr);

        for (size_t i = 0; i < 128; ++i)
        {
            REQUIRE(ptr[i] == 0);
        }
    }

    SECTION("Calloc after dirty memory")
    {
        char* ptr1 = static_cast<char*>(p.alloc());
        std::memset(ptr1, 0xFF, 128);
        p.free(ptr1);

        char* ptr2 = static_cast<char*>(p.calloc());
        REQUIRE(ptr2 != nullptr);

        for (size_t i = 0; i < 128; ++i)
        {
            REQUIRE(ptr2[i] == 0);
        }
    }

    SECTION("Multiple calloc allocations")
    {
        char* p1 = static_cast<char*>(p.calloc());
        char* p2 = static_cast<char*>(p.calloc());

        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);

        for (size_t i = 0; i < 128; ++i)
        {
            REQUIRE(p1[i] == 0);
            REQUIRE(p2[i] == 0);
        }
    }
}

TEST_CASE("Pool: Calloc exhaustion", "[pool][calloc][edge]")
{
    AL::pool p(64, 2);

    void* p1 = p.calloc();
    void* p2 = p.calloc();
    void* p3 = p.calloc();

    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    REQUIRE(p3 == nullptr); // Pool exhausted
}

// ============================================================================
// Reset Tests
// ============================================================================

TEST_CASE("Pool: Reset functionality", "[pool][reset]")
{
    AL::pool p(64, 10);

    SECTION("Reset empty pool")
    {
        size_t cap_before = p.get_capacity();
        size_t free_before = p.get_free_space();

        p.reset();

        REQUIRE(p.get_capacity() == cap_before);
        REQUIRE(p.get_free_space() == free_before);
    }

    SECTION("Reset partially used pool")
    {
        void* p1 = p.alloc();
        void* p2 = p.alloc();
        void* p3 = p.alloc();

        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);
        REQUIRE(p3 != nullptr);

        p.reset();

        REQUIRE(p.get_free_space() == 64 * 10); // block_size * block_count

        // Should be able to allocate again
        void* p4 = p.alloc();
        REQUIRE(p4 != nullptr);
    }

    SECTION("Reset full pool")
    {
        // Fill pool
        std::vector<void*> ptrs;
        for (int i = 0; i < 10; ++i)
        {
            ptrs.push_back(p.alloc());
        }

        REQUIRE(p.get_free_space() == 0);

        p.reset();

        REQUIRE(p.get_free_space() == 64 * 10); // block_size * block_count

        // All blocks should be available
        for (int i = 0; i < 10; ++i)
        {
            void* ptr = p.alloc();
            REQUIRE(ptr != nullptr);
        }
    }

    SECTION("Multiple reset cycles")
    {
        for (int cycle = 0; cycle < 100; ++cycle)
        {
            // Allocate some blocks
            for (int i = 0; i < 5; ++i)
            {
                p.alloc();
            }

            p.reset();
            REQUIRE(p.get_free_space() == 64 * 10); // block_size * block_count
        }
    }
}

// ============================================================================
// Memory Integrity Tests
// ============================================================================

TEST_CASE("Pool: Memory integrity", "[pool][integrity]")
{
    AL::pool p(128, 10);

    SECTION("Write and read data")
    {
        struct TestData
        {
            int a;
            double b;
            char c[64];
        };

        TestData* data = static_cast<TestData*>(p.alloc());
        REQUIRE(data != nullptr);

        data->a = 42;
        data->b = 3.14159;
        std::strcpy(data->c, "test string");

        REQUIRE(data->a == 42);
        REQUIRE(data->b == 3.14159);
        REQUIRE(std::strcmp(data->c, "test string") == 0);
    }

    SECTION("Multiple allocations don't interfere")
    {
        int* arr1 = static_cast<int*>(p.alloc());
        int* arr2 = static_cast<int*>(p.alloc());
        int* arr3 = static_cast<int*>(p.alloc());

        // Fill with patterns
        for (size_t i = 0; i < 32; ++i)
        {
            arr1[i] = i;
            arr2[i] = i + 1000;
            arr3[i] = i + 2000;
        }

        // Verify no corruption
        for (size_t i = 0; i < 32; ++i)
        {
            REQUIRE(arr1[i] == static_cast<int>(i));
            REQUIRE(arr2[i] == static_cast<int>(i + 1000));
            REQUIRE(arr3[i] == static_cast<int>(i + 2000));
        }
    }

    SECTION("Data persists until freed")
    {
        int* data = static_cast<int*>(p.alloc());
        *data = 12345;

        // Allocate more blocks
        p.alloc();
        p.alloc();

        // Original data should still be intact
        REQUIRE(*data == 12345);

        p.free(data);
    }
}

// ============================================================================
// Edge Cases and Stress Tests
// ============================================================================

TEST_CASE("Pool: Allocation patterns", "[pool][pattern]")
{
    AL::pool p(64, 100);

    SECTION("Alternating alloc/free")
    {
        for (int i = 0; i < 50; ++i)
        {
            void* ptr = p.alloc();
            REQUIRE(ptr != nullptr);
            p.free(ptr);
        }

        // Pool should be fully available
        REQUIRE(p.get_free_space() == 64 * 100); // block_size * block_count
    }

    SECTION("Batch alloc then batch free")
    {
        std::vector<void*> ptrs;

        // Allocate all
        for (int i = 0; i < 100; ++i)
        {
            void* ptr = p.alloc();
            REQUIRE(ptr != nullptr);
            ptrs.push_back(ptr);
        }

        // Free all
        for (void* ptr : ptrs)
        {
            p.free(ptr);
        }

        REQUIRE(p.get_free_space() == 64 * 100); // block_size * block_count
    }

    SECTION("Interleaved alloc/free pattern")
    {
        std::vector<void*> ptrs;

        for (int round = 0; round < 10; ++round)
        {
            // Allocate 10
            for (int i = 0; i < 10; ++i)
            {
                ptrs.push_back(p.alloc());
            }

            // Free 5
            for (int i = 0; i < 5; ++i)
            {
                p.free(ptrs.back());
                ptrs.pop_back();
            }
        }

        // Cleanup
        for (void* ptr : ptrs)
        {
            p.free(ptr);
        }

        REQUIRE(p.get_free_space() == 64 * 100); // block_size * block_count
    }
}

TEST_CASE("Pool: Block size variations", "[pool][sizes]")
{
    SECTION("Tiny blocks")
    {
        AL::pool p(8, 100);
        void* ptr = p.alloc();
        REQUIRE(ptr != nullptr);
        p.free(ptr);
    }

    SECTION("Large blocks")
    {
        AL::pool p(4096, 10);
        void* ptr = p.alloc();
        REQUIRE(ptr != nullptr);
        p.free(ptr);
    }

    SECTION("Non-power-of-2 blocks")
    {
        AL::pool p(100, 10);
        void* ptr = p.alloc();
        REQUIRE(ptr != nullptr);
        p.free(ptr);
    }
}

TEST_CASE("Pool: Block uniqueness", "[pool][unique]")
{
    AL::pool p(64, 50);

    SECTION("All allocated blocks are unique")
    {
        std::set<void*> unique_ptrs;

        for (int i = 0; i < 50; ++i)
        {
            void* ptr = p.alloc();
            REQUIRE(ptr != nullptr);

            // Insert returns false if element already exists
            auto result = unique_ptrs.insert(ptr);
            REQUIRE(result.second == true);
        }

        REQUIRE(unique_ptrs.size() == 50);
    }
}

TEST_CASE("Pool: Fragmentation handling", "[pool][frag]")
{
    AL::pool p(64, 20);

    SECTION("Free every other block")
    {
        std::vector<void*> ptrs;

        // Allocate all
        for (int i = 0; i < 20; ++i)
        {
            ptrs.push_back(p.alloc());
        }

        // Free every other one
        for (size_t i = 0; i < ptrs.size(); i += 2)
        {
            p.free(ptrs[i]);
        }

        // Should be able to allocate 10 more
        for (int i = 0; i < 10; ++i)
        {
            void* ptr = p.alloc();
            REQUIRE(ptr != nullptr);
        }
    }

    SECTION("Random free pattern")
    {
        std::vector<void*> ptrs;

        // Allocate all
        for (int i = 0; i < 20; ++i)
        {
            ptrs.push_back(p.alloc());
        }

        // Free specific indices: 2, 5, 7, 11, 13, 17, 19
        std::vector<int> to_free = {2, 5, 7, 11, 13, 17, 19};
        for (int idx : to_free)
        {
            p.free(ptrs[idx]);
        }

        // Should be able to allocate 7 more
        for (size_t i = 0; i < to_free.size(); ++i)
        {
            void* ptr = p.alloc();
            REQUIRE(ptr != nullptr);
        }
    }
}

TEST_CASE("Pool: Zero blocks pool", "[pool][edge]")
{
    // Note: This might throw or create minimal pool depending on implementation
    // Test documents the behavior
    SECTION("Zero blocks is handled")
    {
        try
        {
            AL::pool p(64, 0);
            // If it doesn't throw, verify behavior
            void* ptr = p.alloc();
            REQUIRE(ptr == nullptr);
        }
        catch (...)
        {
            // Either behavior is acceptable - document it
        }
        REQUIRE(true);
    }
}

TEST_CASE("Pool: Very small block size", "[pool][edge]")
{
    SECTION("1 byte blocks (rounds up to pointer size)")
    {
        AL::pool p(1, 10);
        void* ptr = p.alloc();
        REQUIRE(ptr != nullptr);
        p.free(ptr);
    }

    SECTION("2 byte blocks (rounds up)")
    {
        AL::pool p(2, 10);
        void* ptr = p.alloc();
        REQUIRE(ptr != nullptr);
        p.free(ptr);
    }
}

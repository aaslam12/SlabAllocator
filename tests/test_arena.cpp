#include "arena.h"
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <unistd.h>
#include <vector>

const size_t PAGE_SIZE = getpagesize();

static void check_arena_valid(const AL::arena& a)
{
    REQUIRE(a.get_used() == 0);
    REQUIRE(a.get_capacity() > 0);
}

TEST_CASE("Arena creation", "[arena]")
{
    AL::arena a(PAGE_SIZE);
    check_arena_valid(a);
}

TEST_CASE("Arena allocation", "[arena]")
{
    AL::arena a(PAGE_SIZE);
    check_arena_valid(a);

    size_t* num = static_cast<size_t*>(a.alloc(sizeof(size_t)));
    REQUIRE(num != nullptr);
}

TEST_CASE("Arena alloc beyond capacity", "[arena]")
{
    AL::arena a(PAGE_SIZE);
    check_arena_valid(a);

    // the 2 is just an arbitrary number.
    // we just need to allocate more than the capacity
    size_t* num = static_cast<size_t*>(a.alloc(a.get_capacity() * 2));
    REQUIRE(num == nullptr);
}

TEST_CASE("Arena reset", "[arena]")
{
    AL::arena a(PAGE_SIZE);
    check_arena_valid(a);

    size_t* num = static_cast<size_t*>(a.alloc(sizeof(size_t)));
    REQUIRE(num != nullptr);

    REQUIRE(a.get_used() >= sizeof(size_t));

    a.reset();

    check_arena_valid(a);
}

TEST_CASE("Arena zero allocation", "[arena]")
{
    AL::arena a(PAGE_SIZE);

    void* ptr = a.alloc(0);
    REQUIRE(ptr == nullptr);    // Should return null for 0 bytes
    REQUIRE(a.get_used() == 0); // Should not change used count
}

TEST_CASE("Arena sequential allocations", "[arena]")
{
    AL::arena a(PAGE_SIZE);

    void* p1 = a.alloc(64);
    void* p2 = a.alloc(64);

    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    REQUIRE(p1 != p2);

    // Check they don't overlap (p2 should be after p1)
    char* c1 = static_cast<char*>(p1);
    char* c2 = static_cast<char*>(p2);
    REQUIRE(c2 >= c1 + 64);
}

TEST_CASE("Arena calloc zeros memory", "[arena]")
{
    AL::arena a(PAGE_SIZE);

    // Allocate and dirty some memory first
    char* dirty = static_cast<char*>(a.alloc(64));
    std::memset(dirty, 0xFF, 64); // Fill with garbage

    a.reset();

    // Now calloc should return zeroed memory
    char* clean = static_cast<char*>(a.calloc(64));
    REQUIRE(clean != nullptr);

    for (size_t i = 0; i < 64; ++i)
    {
        REQUIRE(clean[i] == 0);
    }
}

TEST_CASE("Arena exact capacity allocation", "[arena]")
{
    AL::arena a(PAGE_SIZE);

    // Allocate exactly the full capacity
    void* ptr = a.alloc(a.get_capacity());
    REQUIRE(ptr != nullptr);
    REQUIRE(a.get_used() == a.get_capacity());

    // Next allocation should fail
    void* ptr2 = a.alloc(1);
    REQUIRE(ptr2 == nullptr);
}

// ============================================================================
// Basic Functionality Tests
// ============================================================================

TEST_CASE("Arena: Basic construction and initialization", "[arena][basic]")
{
    SECTION("Single page arena")
    {
        AL::arena a(PAGE_SIZE);
        REQUIRE(a.get_used() == 0);
        REQUIRE(a.get_capacity() >= PAGE_SIZE);
        REQUIRE(a.get_capacity() % PAGE_SIZE == 0);
    }

    SECTION("Multi-page arena")
    {
        AL::arena a(PAGE_SIZE * 3);
        REQUIRE(a.get_used() == 0);
        REQUIRE(a.get_capacity() >= PAGE_SIZE * 3);
        REQUIRE(a.get_capacity() % PAGE_SIZE == 0);
    }

    SECTION("Small arena (less than page size)")
    {
        AL::arena a(100);
        REQUIRE(a.get_used() == 0);
        REQUIRE(a.get_capacity() >= 100);
        REQUIRE(a.get_capacity() == PAGE_SIZE); // Should round up to page size
    }

    SECTION("Large arena")
    {
        AL::arena a(PAGE_SIZE * 100);
        REQUIRE(a.get_used() == 0);
        REQUIRE(a.get_capacity() >= PAGE_SIZE * 100);
    }
}

// ============================================================================
// Allocation Tests
// ============================================================================

TEST_CASE("Arena: Basic allocations", "[arena][alloc]")
{
    AL::arena a(PAGE_SIZE);

    SECTION("Single small allocation")
    {
        void* ptr = a.alloc(64);
        REQUIRE(ptr != nullptr);
        REQUIRE(a.get_used() == 64);
    }

    SECTION("Multiple small allocations")
    {
        void* p1 = a.alloc(16);
        void* p2 = a.alloc(32);
        void* p3 = a.alloc(64);

        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);
        REQUIRE(p3 != nullptr);
        REQUIRE(a.get_used() == 112);

        // Verify pointers are distinct
        REQUIRE(p1 != p2);
        REQUIRE(p2 != p3);
        REQUIRE(p1 != p3);
    }

    SECTION("Sequential allocations are contiguous")
    {
        char* p1 = static_cast<char*>(a.alloc(100));
        char* p2 = static_cast<char*>(a.alloc(100));
        char* p3 = static_cast<char*>(a.alloc(100));

        REQUIRE(p2 == p1 + 100);
        REQUIRE(p3 == p2 + 100);
    }
}

TEST_CASE("Arena: Zero-size allocation", "[arena][alloc][edge]")
{
    AL::arena a(PAGE_SIZE);

    void* ptr = a.alloc(0);
    REQUIRE(ptr == nullptr);
    REQUIRE(a.get_used() == 0);
}

TEST_CASE("Arena: Full capacity allocation", "[arena][alloc][edge]")
{
    AL::arena a(PAGE_SIZE);

    SECTION("Allocate exactly full capacity")
    {
        void* ptr = a.alloc(a.get_capacity());
        REQUIRE(ptr != nullptr);
        REQUIRE(a.get_used() == a.get_capacity());
    }

    SECTION("Cannot allocate after full")
    {
        void* p1 = a.alloc(a.get_capacity());
        REQUIRE(p1 != nullptr);

        void* p2 = a.alloc(1);
        REQUIRE(p2 == nullptr);
        REQUIRE(a.get_used() == a.get_capacity());
    }
}

TEST_CASE("Arena: Over-capacity allocation", "[arena][alloc][edge]")
{
    AL::arena a(PAGE_SIZE);

    SECTION("Allocate more than capacity")
    {
        void* ptr = a.alloc(a.get_capacity() + 1);
        REQUIRE(ptr == nullptr);
        REQUIRE(a.get_used() == 0);
    }

    SECTION("Allocate way more than capacity")
    {
        void* ptr = a.alloc(a.get_capacity() * 10);
        REQUIRE(ptr == nullptr);
        REQUIRE(a.get_used() == 0);
    }
}

TEST_CASE("Arena: Fragmentation behavior", "[arena][alloc]")
{
    AL::arena a(1024);

    SECTION("Fill arena with small allocations")
    {
        std::vector<void*> ptrs;
        size_t count = 0;

        while (true)
        {
            void* ptr = a.alloc(10);
            if (ptr == nullptr)
                break;
            ptrs.push_back(ptr);
            count++;
        }

        REQUIRE(count > 0);
        REQUIRE(a.get_used() == count * 10);
        REQUIRE(a.get_used() <= a.get_capacity());
    }
}

// ============================================================================
// Calloc Tests
// ============================================================================

TEST_CASE("Arena: Calloc zeros memory", "[arena][calloc]")
{
    AL::arena a(PAGE_SIZE);

    SECTION("Calloc single allocation")
    {
        char* ptr = static_cast<char*>(a.calloc(100));
        REQUIRE(ptr != nullptr);

        for (size_t i = 0; i < 100; ++i)
        {
            REQUIRE(ptr[i] == 0);
        }
    }

    SECTION("Calloc after dirty memory")
    {
        // Allocate and dirty memory
        char* dirty = static_cast<char*>(a.alloc(100));
        std::memset(dirty, 0xFF, 100);

        // Reset and calloc
        a.reset();
        char* clean = static_cast<char*>(a.calloc(100));
        REQUIRE(clean != nullptr);

        for (size_t i = 0; i < 100; ++i)
        {
            REQUIRE(clean[i] == 0);
        }
    }

    SECTION("Multiple calloc allocations")
    {
        char* p1 = static_cast<char*>(a.calloc(50));
        char* p2 = static_cast<char*>(a.calloc(50));

        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);

        for (size_t i = 0; i < 50; ++i)
        {
            REQUIRE(p1[i] == 0);
            REQUIRE(p2[i] == 0);
        }
    }
}

TEST_CASE("Arena: Calloc zero-size", "[arena][calloc][edge]")
{
    AL::arena a(PAGE_SIZE);

    void* ptr = a.calloc(0);
    REQUIRE(ptr == nullptr);
    REQUIRE(a.get_used() == 0);
}

TEST_CASE("Arena: Calloc over-capacity", "[arena][calloc][edge]")
{
    AL::arena a(PAGE_SIZE);

    void* ptr = a.calloc(a.get_capacity() + 1);
    REQUIRE(ptr == nullptr);
    REQUIRE(a.get_used() == 0);
}

// ============================================================================
// Reset Tests
// ============================================================================

TEST_CASE("Arena: Reset functionality", "[arena][reset]")
{
    AL::arena a(PAGE_SIZE);

    SECTION("Reset empty arena")
    {
        int result = a.reset();
        REQUIRE(result == 0);
        REQUIRE(a.get_used() == 0);
        REQUIRE(a.get_capacity() > 0);
    }

    SECTION("Reset after allocations")
    {
        void* p1 = a.alloc(100);
        void* p2 = a.alloc(200);
        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);
        REQUIRE(a.get_used() == 300);

        int result = a.reset();
        REQUIRE(result == 0);
        REQUIRE(a.get_used() == 0);
        REQUIRE(a.get_capacity() > 0);
    }

    SECTION("Allocate after reset")
    {
        void* p1 = a.alloc(500);
        REQUIRE(p1 != nullptr);

        a.reset();

        void* p2 = a.alloc(500);
        REQUIRE(p2 != nullptr);
        REQUIRE(p2 == p1); // Should get same memory location
    }

    SECTION("Multiple resets")
    {
        for (int i = 0; i < 10; ++i)
        {
            a.alloc(100);
            int result = a.reset();
            REQUIRE(result == 0);
            REQUIRE(a.get_used() == 0);
        }
    }

    SECTION("Reset full arena")
    {
        void* ptr = a.alloc(a.get_capacity());
        REQUIRE(ptr != nullptr);
        REQUIRE(a.get_used() == a.get_capacity());

        a.reset();
        REQUIRE(a.get_used() == 0);

        // Should be able to allocate again
        ptr = a.alloc(100);
        REQUIRE(ptr != nullptr);
    }
}

// ============================================================================
// Memory Integrity Tests
// ============================================================================

TEST_CASE("Arena: Memory integrity", "[arena][integrity]")
{
    AL::arena a(PAGE_SIZE * 2);

    SECTION("Write and read back data")
    {
        struct TestData
        {
            int x;
            double y;
            char z[16];
        };

        TestData* data = static_cast<TestData*>(a.alloc(sizeof(TestData)));
        REQUIRE(data != nullptr);

        data->x = 42;
        data->y = 3.14159;
        std::strcpy(data->z, "test");

        REQUIRE(data->x == 42);
        REQUIRE(data->y == 3.14159);
        REQUIRE(std::strcmp(data->z, "test") == 0);
    }

    SECTION("Multiple allocations don't overlap")
    {
        int* arr1 = static_cast<int*>(a.alloc(sizeof(int) * 10));
        int* arr2 = static_cast<int*>(a.alloc(sizeof(int) * 10));

        REQUIRE(arr1 != nullptr);
        REQUIRE(arr2 != nullptr);

        // Fill with distinct patterns
        for (int i = 0; i < 10; ++i)
        {
            arr1[i] = i;
            arr2[i] = i + 100;
        }

        // Verify no corruption
        for (int i = 0; i < 10; ++i)
        {
            REQUIRE(arr1[i] == i);
            REQUIRE(arr2[i] == i + 100);
        }
    }

    SECTION("Large allocation integrity")
    {
        size_t size = PAGE_SIZE;
        char* buffer = static_cast<char*>(a.alloc(size));
        REQUIRE(buffer != nullptr);

        // Write pattern
        for (size_t i = 0; i < size; ++i)
        {
            buffer[i] = static_cast<char>(i % 256);
        }

        // Verify pattern
        for (size_t i = 0; i < size; ++i)
        {
            REQUIRE(buffer[i] == static_cast<char>(i % 256));
        }
    }
}

// ============================================================================
// Edge Cases and Capacity Tests
// ============================================================================

TEST_CASE("Arena: Edge cases", "[arena][edge]")
{
    SECTION("Many small allocations")
    {
        AL::arena a(PAGE_SIZE * 10);
        std::vector<void*> ptrs;

        for (int i = 0; i < 1000; ++i)
        {
            void* ptr = a.alloc(8);
            if (ptr != nullptr)
            {
                ptrs.push_back(ptr);
            }
        }

        REQUIRE(ptrs.size() > 0);
        REQUIRE(a.get_used() == ptrs.size() * 8);
    }

    SECTION("Alternating large and small allocations")
    {
        AL::arena a(PAGE_SIZE * 4);

        void* p1 = a.alloc(1024);
        void* p2 = a.alloc(8);
        void* p3 = a.alloc(2048);
        void* p4 = a.alloc(16);

        REQUIRE(p1 != nullptr);
        REQUIRE(p2 != nullptr);
        REQUIRE(p3 != nullptr);
        REQUIRE(p4 != nullptr);
    }

    SECTION("Allocation at exact remaining capacity")
    {
        AL::arena a(1024);

        size_t actual_capacity = a.get_capacity();
        size_t half_capacity = actual_capacity / 2;

        void* p1 = a.alloc(half_capacity);
        REQUIRE(p1 != nullptr);

        void* p2 = a.alloc(half_capacity);
        REQUIRE(p2 != nullptr);

        REQUIRE(a.get_used() == actual_capacity);

        // Next allocation should fail
        void* p3 = a.alloc(1);
        REQUIRE(p3 == nullptr);
    }
}

TEST_CASE("Arena: Capacity rounding", "[arena][capacity]")
{
    SECTION("Non-page-aligned sizes round up")
    {
        AL::arena a1(PAGE_SIZE + 1);
        REQUIRE(a1.get_capacity() == PAGE_SIZE * 2);

        AL::arena a2(PAGE_SIZE * 2 + 100);
        REQUIRE(a2.get_capacity() == PAGE_SIZE * 3);
    }

    SECTION("Page-aligned sizes don't change")
    {
        AL::arena a1(PAGE_SIZE);
        REQUIRE(a1.get_capacity() == PAGE_SIZE);

        AL::arena a2(PAGE_SIZE * 5);
        REQUIRE(a2.get_capacity() == PAGE_SIZE * 5);
    }
}

TEST_CASE("Arena: Different allocation patterns", "[arena][pattern]")
{
    AL::arena a(PAGE_SIZE * 4);

    SECTION("Ascending size allocations")
    {
        for (size_t i = 1; i <= 100; ++i)
        {
            void* ptr = a.alloc(i);
            if (ptr == nullptr)
                break;
        }
        REQUIRE(a.get_used() > 0);
    }

    SECTION("Descending size allocations")
    {
        for (size_t i = 100; i >= 1; --i)
        {
            void* ptr = a.alloc(i);
            if (ptr == nullptr)
                break;
        }
        REQUIRE(a.get_used() > 0);
    }

    SECTION("Random-ish size allocations")
    {
        size_t sizes[] = {64, 128, 32, 256, 16, 512, 8, 1024};
        for (size_t size : sizes)
        {
            void* ptr = a.alloc(size);
            REQUIRE(ptr != nullptr);
        }
    }
}

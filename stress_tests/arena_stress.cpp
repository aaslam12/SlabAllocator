#include "arena.h"
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <unistd.h>

using namespace AL;

static const size_t PAGE_SIZE = getpagesize();

TEST_CASE("Arena: Edge cases - Stress", "[arena][stress]")
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
}

TEST_CASE("Arena: Reuse after reset - Stress", "[arena][stress]")
{
    AL::arena a(PAGE_SIZE);

    SECTION("Multiple cycles of use and reset")
    {
        for (int cycle = 0; cycle < 100; ++cycle)
        {
            std::vector<void*> ptrs;

            // Allocate several blocks
            for (int i = 0; i < 10; ++i)
            {
                void* ptr = a.alloc(100);
                REQUIRE(ptr != nullptr);
                ptrs.push_back(ptr);
            }

            REQUIRE(a.get_used() == 1000);

            // Reset
            a.reset();
            REQUIRE(a.get_used() == 0);
        }
    }
}

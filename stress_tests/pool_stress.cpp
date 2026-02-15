#include "pool.h"
#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace AL;

TEST_CASE("Pool: Stress test", "[pool][stress]")
{
    AL::pool p(128, 1000);

    SECTION("Many alloc/free cycles")
    {
        for (int cycle = 0; cycle < 100; ++cycle)
        {
            std::vector<void*> ptrs;

            // Allocate half
            for (int i = 0; i < 500; ++i)
            {
                ptrs.push_back(p.alloc());
            }

            // Free all
            for (void* ptr : ptrs)
            {
                p.free(ptr);
            }
        }

        REQUIRE(p.get_free_space() == 128 * 1000); // block_size * block_count
    }

    SECTION("Allocate all, free all, repeat")
    {
        for (int cycle = 0; cycle < 10; ++cycle)
        {
            std::vector<void*> ptrs;

            // Fill completely
            for (int i = 0; i < 1000; ++i)
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
        }
    }
}

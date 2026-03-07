#include "arena.h"
#include "platform.h"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <new>

namespace AL
{
arena::arena(size_t bytes) : memory(nullptr), used(0), capacity(0)
{
    size_t page_size = AL::platform_mem::page_size();

    // round up to next page boundary
    capacity = ((bytes + page_size - 1) / page_size) * page_size;

    void* ptr = AL::platform_mem::alloc(capacity);

    if (ptr == nullptr)
    {
        throw std::bad_alloc();
    }

    memory = static_cast<std::byte*>(ptr);
    used = 0;
}

arena::~arena()
{
    if (memory == nullptr)
        return;

    bool freed = AL::platform_mem::free(memory, capacity);

#if PALLOC_DEBUG
    if (!freed)
    {
        std::cerr << "WARNING: munmap failed in arena destructor\n";
    }
#endif // PALLOC_DEBUG
}

arena::arena(arena&& other) noexcept : memory(other.memory), used(other.used.load()), capacity(other.capacity)
{
    other.reset();
    other.capacity = 0;
    other.used = 0;
    other.memory = nullptr;
}

arena& arena::operator=(arena&& other) noexcept
{
    if (this == &other)
        return *this;

    if (memory != nullptr)
    {
        AL::platform_mem::free(memory, capacity);
    }

    memory = other.memory;
    used = other.used.load();
    capacity = other.capacity;

    other.reset();
    other.capacity = 0;
    other.used = 0;
    other.memory = nullptr;
    return *this;
}

void* arena::alloc(size_t length)
{
    if (length == 0 || memory == nullptr)
        return nullptr;

    constexpr size_t alignment = alignof(std::max_align_t);

    size_t current;
    size_t aligned;
    while (true)
    {
        current = used.load(std::memory_order::relaxed);

        // align current offset up to the required alignment boundary
        aligned = (current + alignment - 1) & ~(alignment - 1);

        // if we do not have enough space left in the arena
        if (length > (capacity - aligned))
            return nullptr;

        if (used.compare_exchange_weak(current, aligned + length, std::memory_order_release, std::memory_order_relaxed))
            return memory + aligned;
    }
}

void* arena::calloc(size_t length)
{
    void* ptr = alloc(length);

    if (ptr != nullptr)
    {
        std::memset(ptr, 0, length);
    }

    return ptr;
}

int arena::reset()
{
    used = 0;
    return 0;
}

int arena::clear()
{
    if (memory != nullptr)
    {
        bool ok = AL::platform_mem::free(memory, capacity);
        memory = nullptr;

        if (!ok)
            return -1;
    }

    used.store(0);
    capacity = 0;
    return 0;
}

size_t arena::get_used() const
{
    return used;
}

size_t arena::get_capacity() const
{
    return capacity;
}
} // namespace AL

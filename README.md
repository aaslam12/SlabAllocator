# Palloc

A high-performance, thread-safe memory allocator library written in C++20. Implements three complementary allocator strategies — **Arena**, **Pool**, and **Slab** — each optimized for specific allocation patterns. All allocators use `mmap`/`munmap` directly, bypassing the system heap entirely.

---

## Table of Contents

- [Overview](#overview)
- [Getting Started](#getting-started)
  - [Requirements](#requirements)
  - [Building](#building)
  - [Running Tests](#running-tests)
  - [Sanitizers](#sanitizers)

---

## Overview

| Allocator | Strategy | Thread Safety | Best For |
|-----------|----------|--------------|----------|
| `Arena` | Linear bump allocator | ✅ Lock-free (CAS) | Short-lived, frame-scoped allocations |
| `Pool` | Free-list allocator | ✅ Mutex-protected | Fixed-size objects with frequent alloc/free |
| `Slab` | Multi-pool dispatcher | ✅ Inherited from Pool | Variable-size objects within 8B–4KB |

All allocators:
- Map memory directly with `mmap` — no `malloc` or `new`
- Are validated with **ThreadSanitizer** and **AddressSanitizer**

---

## Getting Started

### Requirements

- C++20 compiler (GCC 10+ or Clang 12+)
- CMake 3.10+
- [Catch2 v3](https:
- Ninja (optional, auto-detected for faster builds)

### Building

```bash
python build.py

python build.py --config Release

python build.py --clean

python build.py --build-only

python build.py --stress-test
```

### Running Tests

Tests run automatically on every debug build.

```bash
# run all
./build/Debug/tests

./build/Debug/tests "[arena]"
./build/Debug/tests "[pool]"
./build/Debug/tests "[slab]"

# thread-safety tests (hidden by default)
./build/Debug/tests "[thread]"
```

### Sanitizers

```bash
# detects use-after-free, buffer overflows, leaks
python build.py --asan

# detects data races and concurrency bugs
python build.py --tsan
```

Sanitizers use separate build directories (`build/Debug-asan`, `build/Debug-tsan`) to avoid conflicts. They cannot be used together.


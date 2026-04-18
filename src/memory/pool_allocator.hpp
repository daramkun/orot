#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace orot { namespace deflate {

/**
 * Fixed-size slab allocator.
 * Manages a pool of N slots, each of ObjectSize bytes.
 * alloc() / free() are O(1) via a free-list.
 * Not thread-safe — protect externally if shared.
 */
template<size_t ObjectSize, size_t PoolCount, size_t Align = alignof(max_align_t)>
class PoolAllocator {
    static_assert(ObjectSize >= sizeof(void*), "object too small for free list");
    static_assert(PoolCount  > 0,             "pool must have at least 1 slot");

    static constexpr size_t SLOT_SIZE = (ObjectSize + Align - 1) & ~(Align - 1);

public:
    PoolAllocator() {
        /* Build the free list through the raw storage */
        for (size_t i = 0; i < PoolCount - 1; ++i) {
            void** node = reinterpret_cast<void**>(storage_ + i * SLOT_SIZE);
            *node = storage_ + (i + 1) * SLOT_SIZE;
        }
        void** last = reinterpret_cast<void**>(
            storage_ + (PoolCount - 1) * SLOT_SIZE);
        *last = nullptr;
        free_head_ = storage_;
    }

    PoolAllocator(const PoolAllocator&)            = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    [[nodiscard]] void* alloc() noexcept {
        if (!free_head_) return nullptr;
        void* ptr  = free_head_;
        free_head_ = *reinterpret_cast<void**>(free_head_);
        ++used_;
        return ptr;
    }

    void free(void* ptr) noexcept {
        assert(ptr && "null free");
        *reinterpret_cast<void**>(ptr) = free_head_;
        free_head_ = ptr;
        --used_;
    }

    size_t used()      const noexcept { return used_;          }
    size_t available() const noexcept { return PoolCount - used_; }
    bool   full()      const noexcept { return used_ == PoolCount; }
    bool   empty()     const noexcept { return used_ == 0;         }

private:
    alignas(Align) uint8_t storage_[SLOT_SIZE * PoolCount]{};
    void*  free_head_ = nullptr;
    size_t used_      = 0;
};

} } /* namespace orot::deflate */

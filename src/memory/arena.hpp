#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "aligned_alloc.hpp"

namespace deflate {

/**
 * Linear bump allocator.
 *
 * reset() rewinds the pointer to zero in O(1).
 * No per-allocation free — entire arena is reclaimed at once.
 */
class Arena {
public:
    /* Stack/member variant: caller provides pre-allocated buffer */
    Arena(uint8_t* buf, size_t capacity) noexcept
        : buf_(buf), capacity_(capacity), offset_(0) {}

    Arena(const Arena&)            = delete;
    Arena& operator=(const Arena&) = delete;

    /** Allocate count items of type T with alignment Align. */
    template<typename T, size_t Align = alignof(T)>
    [[nodiscard]] T* alloc(size_t count = 1) noexcept {
        const size_t bytes   = sizeof(T) * count;
        const size_t aligned = (offset_ + Align - 1) & ~(Align - 1);
        const size_t next    = aligned + bytes;
        assert(next <= capacity_ && "Arena overflow");
        if (next > capacity_) return nullptr;
        offset_ = next;
        return reinterpret_cast<T*>(buf_ + aligned);
    }

    /** Allocate and zero-fill. */
    template<typename T, size_t Align = alignof(T)>
    [[nodiscard]] T* alloc_zeroed(size_t count = 1) noexcept {
        T* p = alloc<T, Align>(count);
        if (p) std::memset(p, 0, sizeof(T) * count);
        return p;
    }

    /** Rewind pointer to zero — O(1), no zero-fill. */
    void reset() noexcept { offset_ = 0; }

    /** Rewind and zero the whole buffer. */
    void reset_zero() noexcept {
        offset_ = 0;
        std::memset(buf_, 0, capacity_);
    }

    size_t used()      const noexcept { return offset_;              }
    size_t capacity()  const noexcept { return capacity_;            }
    size_t remaining() const noexcept { return capacity_ - offset_;  }

private:
    uint8_t* buf_      = nullptr;
    size_t   capacity_ = 0;
    size_t   offset_   = 0;
};

/**
 * Arena backed by a heap-allocated aligned buffer.
 * Owns the buffer; freed in destructor.
 */
class DynArena {
public:
    explicit DynArena(size_t capacity)
        : buf_(capacity)
        , arena_(buf_.data(), capacity)
    {}

    Arena& get() noexcept { return arena_; }

    template<typename T, size_t Align = alignof(T)>
    [[nodiscard]] T* alloc(size_t count = 1) noexcept {
        return arena_.alloc<T, Align>(count);
    }

    template<typename T, size_t Align = alignof(T)>
    [[nodiscard]] T* alloc_zeroed(size_t count = 1) noexcept {
        return arena_.alloc_zeroed<T, Align>(count);
    }

    void reset()      noexcept { arena_.reset();      }
    void reset_zero() noexcept { arena_.reset_zero(); }

    size_t used()      const noexcept { return arena_.used();      }
    size_t capacity()  const noexcept { return arena_.capacity();  }
    size_t remaining() const noexcept { return arena_.remaining(); }

private:
    AlignedBuffer<uint8_t> buf_;
    Arena                  arena_;
};

} /* namespace deflate */

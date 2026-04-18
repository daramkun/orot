#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>

namespace orot { namespace deflate {

/*
 * Lock-free SPSC (single-producer single-consumer) ring queue.
 * Producer calls push(), consumer calls pop().
 * Capacity must be a power of two.
 */
template<typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    bool push(const T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & (Capacity - 1);
        if (next == tail_.load(std::memory_order_acquire)) return false;  /* full */
        buf_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& item) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;  /* empty */
        item = buf_[tail];
        tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire)
            == head_.load(std::memory_order_acquire);
    }

    size_t size() const noexcept {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & (Capacity - 1);
    }

private:
    alignas(64) T   buf_[Capacity]{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} } /* namespace orot::deflate */

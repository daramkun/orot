#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

namespace deflate {

/* Minimum alignment for SIMD: 64 bytes covers AVX-512 / cache line */
inline constexpr size_t SIMD_ALIGN = 64;

inline void* aligned_malloc(size_t size, size_t alignment = SIMD_ALIGN) {
    if (size == 0) return nullptr;
#if defined(_MSC_VER)
    void* ptr = _aligned_malloc(size, alignment);
    if (!ptr) throw std::bad_alloc{};
    return ptr;
#else
    void* ptr = nullptr;
    if (::posix_memalign(&ptr, alignment, size) != 0)
        throw std::bad_alloc{};
    return ptr;
#endif
}

inline void aligned_free(void* ptr) noexcept {
    if (!ptr) return;
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    ::free(ptr);
#endif
}

/* RAII aligned buffer */
template<typename T, size_t Align = SIMD_ALIGN>
class AlignedBuffer {
public:
    AlignedBuffer() = default;

    explicit AlignedBuffer(size_t count)
        : data_(static_cast<T*>(aligned_malloc(count * sizeof(T), Align)))
        , count_(count)
    {
        std::memset(data_, 0, count * sizeof(T));
    }

    ~AlignedBuffer() { aligned_free(data_); }

    AlignedBuffer(AlignedBuffer&& o) noexcept
        : data_(o.data_), count_(o.count_)
    { o.data_ = nullptr; o.count_ = 0; }

    AlignedBuffer& operator=(AlignedBuffer&& o) noexcept {
        if (this != &o) {
            aligned_free(data_);
            data_  = o.data_;  o.data_  = nullptr;
            count_ = o.count_; o.count_ = 0;
        }
        return *this;
    }

    AlignedBuffer(const AlignedBuffer&)            = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    T*     data()  const noexcept { return data_;  }
    size_t size()  const noexcept { return count_; }
    bool   empty() const noexcept { return count_ == 0; }

    T& operator[](size_t i) noexcept { return data_[i]; }
    const T& operator[](size_t i) const noexcept { return data_[i]; }

    void zero() noexcept { std::memset(data_, 0, count_ * sizeof(T)); }

private:
    T*     data_  = nullptr;
    size_t count_ = 0;
};

} /* namespace deflate */

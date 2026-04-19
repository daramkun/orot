#include "parallel_decompress.hpp"
#include "../parallel/thread_pool.hpp"
#include "../formats/gzip_wrapper.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace orot { namespace deflate {

static constexpr uint8_t  GZIP_MAGIC0  = 0x1F;
static constexpr uint8_t  GZIP_MAGIC1  = 0x8B;
static constexpr uint8_t  GZIP_METHOD  = 0x08;
static constexpr size_t   GZIP_MIN_LEN = 18;   /* 10 header + 2 deflate + 8 trailer */

/* Parse the minimum fixed gzip header and return the offset just past it.
 * Returns 0 if the header is invalid.  Only the fixed 10 bytes + optional
 * extension fields are consumed; the compressed data starts at the returned
 * offset. */
static size_t parse_gzip_header(const uint8_t* src, size_t src_len)
{
    if (src_len < GZIP_MIN_LEN) return 0;
    if (src[0] != GZIP_MAGIC0 || src[1] != GZIP_MAGIC1) return 0;
    if (src[2] != GZIP_METHOD) return 0;
    if (src[3] >= 0x20) return 0;          /* reserved flag bits set — reject */

    const uint8_t flags = src[3];
    size_t off = 10;                       /* past the fixed 10-byte header */

    if (flags & 0x04) {                    /* FEXTRA */
        if (off + 2 > src_len) return 0;
        const uint16_t xlen = static_cast<uint16_t>(src[off]) |
                              (static_cast<uint16_t>(src[off + 1]) << 8);
        off += 2 + xlen;
    }
    if (flags & 0x08) {                    /* FNAME */
        while (off < src_len && src[off] != 0) ++off;
        ++off;
    }
    if (flags & 0x10) {                    /* FCOMMENT */
        while (off < src_len && src[off] != 0) ++off;
        ++off;
    }
    if (flags & 0x02) off += 2;            /* FHCRC */

    if (off + 8 > src_len) return 0;
    return off;
}

/* Scan src[start..src_len) for the next gzip magic starting at or after
 * `start`.  Returns the position or src_len if not found. */
static size_t find_next_member(const uint8_t* src, size_t src_len, size_t start)
{
    for (size_t i = start; i + 2 < src_len; ++i) {
        if (src[i] == GZIP_MAGIC0 && src[i + 1] == GZIP_MAGIC1 &&
            i + 3 < src_len && src[i + 2] == GZIP_METHOD) {
            const size_t hlen = parse_gzip_header(src + i, src_len - i);
            if (hlen > 0) return i;
        }
    }
    return src_len;
}

struct Member {
    size_t member_start;  /* offset of \x1f\x8b magic in src */
    size_t member_end;    /* exclusive: points to start of next member (or src_len) */
    size_t out_offset;    /* where this member's output starts in the output buffer */
    size_t out_size;      /* expected output size (ISIZE field of trailer) */
};

deflate_result parallel_gzip_decompress(
    const uint8_t* in,  size_t in_len,
    uint8_t*       out, size_t out_capacity,
    size_t*        actual_out_size,
    int            num_threads)
{
    /* ── Step 1: locate all member boundaries ──────────────────────────── */
    std::vector<Member> members;
    {
        size_t pos = 0;
        while (pos < in_len) {
            const size_t hlen = parse_gzip_header(in + pos, in_len - pos);
            if (hlen == 0) break;

            /* Find the next member start to determine this member's end */
            const size_t next = find_next_member(in, in_len, pos + hlen);

            /* Trailer is the 8 bytes immediately before next member start */
            if (next < 8) break;
            const size_t trailer_off = next - 8;
            if (trailer_off < pos + hlen) break;  /* no room for compressed data */

            const uint32_t isize =
                  static_cast<uint32_t>(in[trailer_off + 4])
                | (static_cast<uint32_t>(in[trailer_off + 5]) <<  8)
                | (static_cast<uint32_t>(in[trailer_off + 6]) << 16)
                | (static_cast<uint32_t>(in[trailer_off + 7]) << 24);

            Member m{};
            m.member_start = pos;
            m.member_end   = next;   /* exclusive end of this member's bytes */
            m.out_size     = static_cast<size_t>(isize);
            members.push_back(m);

            if (next >= in_len) break;
            pos = next;
        }
    }

    /* Need at least 2 members to justify parallel decompress */
    if (members.size() < 2) return DEFLATE_DATA_ERROR;

    /* ── Step 2: compute output offsets ──────────────────────────────────── */
    size_t total_out = 0;
    for (auto& m : members) {
        m.out_offset = total_out;
        total_out   += m.out_size;
    }
    if (total_out > out_capacity) return DEFLATE_MEM_ERROR;

    /* ── Step 3: parallel decompress ─────────────────────────────────────── */
    std::atomic<deflate_result> result{DEFLATE_OK};

    {
        ThreadPool pool(num_threads);

        for (const auto& m : members) {
            pool.submit([&m, in, out, &result]() {
                if (result.load(std::memory_order_relaxed) != DEFLATE_OK) return;

                size_t actual = 0;
                const deflate_result r = gzip_decompress(
                    in  + m.member_start,
                    m.member_end - m.member_start,
                    out + m.out_offset,
                    m.out_size,
                    &actual);

                if (r != DEFLATE_OK || actual != m.out_size) {
                    result.store(DEFLATE_DATA_ERROR, std::memory_order_relaxed);
                }
            });
        }

        pool.wait_all();
    }

    if (result.load() != DEFLATE_OK) return DEFLATE_DATA_ERROR;

    *actual_out_size = total_out;
    return DEFLATE_OK;
}

} } /* namespace orot::deflate */

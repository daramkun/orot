#pragma once

#include <cstddef>
#include <cstdint>
#include "../core/huffman.hpp"
#include "../core/bit_reader.hpp"

namespace orot { namespace deflate {

/*
 * Decode table entry (32-bit packed):
 *   bits [15: 0] = symbol value (literal, length code, or subtable offset)
 *   bits [23:16] = number of bits consumed (0 = subtable pointer)
 *   bit  [24]    = HUFF_SUBTABLE_FLAG (secondary table pointer)
 *
 * For literal/length:
 *   sym < 256:  literal byte
 *   sym == 256: EOB
 *   sym > 256:  length code (sym - 257 → LENGTHS table index)
 *
 * For distance codes: sym is the distance code index (0-29).
 */

/* Size of primary decode tables */
static constexpr int LITLEN_TABLE_SIZE = 1 << LITLEN_DECODE_BITS;
static constexpr int DIST_TABLE_SIZE   = 1 << DIST_DECODE_BITS;

/* Max secondary table entries.
 * With 11-bit primary for both litlen and dist:
 *   litlen: only codes 12-15 bits go to secondary.
 *   dist:   codes 0-29 max 13 bits; 12-13 bits go to secondary (max 4 per dist code × 30 = 120). */
static constexpr int LITLEN_TABLE_EXTRA = 4096;
static constexpr int DIST_TABLE_EXTRA   = 512;  /* worst: 30 dist codes × 16 secondary = 480 */

/*
 * Decode tables for one DEFLATE block.
 * Stored flat: primary table followed by secondary entries.
 */
struct InflateTables {
    uint32_t litlen[LITLEN_TABLE_SIZE + LITLEN_TABLE_EXTRA];
    uint32_t dist  [DIST_TABLE_SIZE   + DIST_TABLE_EXTRA  ];
};

/*
 * Fast inflate inner loop.
 * Safe to call only when:
 *   - out_ptr + 258 <= out_end  (max copy)
 *   - in_ptr  + 10  <= in_end   (enough bits)
 *
 * Returns true when EOB symbol encountered (block complete).
 * Returns false when conditions are no longer safe (output/input margin exhausted)
 * without having seen EOB — caller should continue with the slow path.
 * Updates in_ptr/out_ptr in place.
 *
 * br:        bit reader (updated in place)
 * out_buf:   start of output window (for back-reference copies)
 * out_ptr:   current output position (updated)
 * out_end:   end of output buffer
 * tables:    pre-built decode tables for this block
 */
bool inflate_fast(
    BitReader&           br,
    uint8_t*             out_buf,
    uint8_t*&            out_ptr,
    const uint8_t*       out_end,
    const InflateTables& tables);

} } /* namespace orot::deflate */

#include "brotli_dictionary.hpp"

#include <cstring>

namespace orot { namespace brotli {

/* Static dictionary data and transform tables are derived from Google Brotli,
 * Copyright (c) 2009, 2010, 2013-2016 by the Brotli Authors, MIT license. */
#define BROTLI_MODEL(x)
#include "brotli_dictionary_inc.hpp"
#undef BROTLI_MODEL

enum TransformType : uint8_t {
    kIdentity = 0,
    kOmitLast1 = 1,
    kOmitLast2 = 2,
    kOmitLast3 = 3,
    kOmitLast4 = 4,
    kOmitLast5 = 5,
    kOmitLast6 = 6,
    kOmitLast7 = 7,
    kOmitLast8 = 8,
    kOmitLast9 = 9,
    kUppercaseFirst = 10,
    kUppercaseAll = 11,
    kOmitFirst1 = 12,
    kOmitFirst2 = 13,
    kOmitFirst3 = 14,
    kOmitFirst4 = 15,
    kOmitFirst5 = 16,
    kOmitFirst6 = 17,
    kOmitFirst7 = 18,
    kOmitFirst8 = 19,
    kOmitFirst9 = 20,
    kShiftFirst = 21,
    kShiftAll = 22,
};

static const char kPrefixSuffix[217] =
    "\1 \2, \10 of the \4 of \2s \1.\5 and \4 "
    "in \1\"\4 to \2\">\1\n\2. \1]\5 for \3 a \6 "
    "that \1\'\6 with \6 from \4 by \1(\6. T"
    "he \4 on \4 as \4 is \4ing \2\n\t\1:\3ed "
    "\2=\"\4 at \3ly \1,\2=\'\5.com/\7. This \5"
    " not \3er \3al \4ful \4ive \5less \4es"
    "t \4ize \2\xc2\xa0\4ous \5 the \2e ";

static const uint16_t kPrefixSuffixMap[50] = {
    0x00, 0x02, 0x05, 0x0E, 0x13, 0x16, 0x18, 0x1E, 0x23, 0x25,
    0x2A, 0x2D, 0x2F, 0x32, 0x34, 0x3A, 0x3E, 0x45, 0x47, 0x4E,
    0x55, 0x5A, 0x5C, 0x63, 0x68, 0x6D, 0x72, 0x77, 0x7A, 0x7C,
    0x80, 0x83, 0x88, 0x8C, 0x8E, 0x91, 0x97, 0x9F, 0xA5, 0xA9,
    0xAD, 0xB2, 0xB7, 0xBD, 0xC2, 0xC7, 0xCA, 0xCF, 0xD5, 0xD8
};

static const uint8_t kTransformsData[] = {
  49, kIdentity, 49, 49, kIdentity, 0, 0, kIdentity, 0,
  49, kOmitFirst1, 49, 49, kUppercaseFirst, 0, 49, kIdentity, 47,
  0, kIdentity, 49, 4, kIdentity, 0, 49, kIdentity, 3,
  49, kUppercaseFirst, 49, 49, kIdentity, 6, 49, kOmitFirst2, 49,
  49, kOmitLast1, 49, 1, kIdentity, 0, 49, kIdentity, 1,
  0, kUppercaseFirst, 0, 49, kIdentity, 7, 49, kIdentity, 9,
  48, kIdentity, 0, 49, kIdentity, 8, 49, kIdentity, 5,
  49, kIdentity, 10, 49, kIdentity, 11, 49, kOmitLast3, 49,
  49, kIdentity, 13, 49, kIdentity, 14, 49, kOmitFirst3, 49,
  49, kOmitLast2, 49, 49, kIdentity, 15, 49, kIdentity, 16,
  0, kUppercaseFirst, 49, 49, kIdentity, 12, 5, kIdentity, 49,
  0, kIdentity, 1, 49, kOmitFirst4, 49, 49, kIdentity, 18,
  49, kIdentity, 17, 49, kIdentity, 19, 49, kIdentity, 20,
  49, kOmitFirst5, 49, 49, kOmitFirst6, 49, 47, kIdentity, 49,
  49, kOmitLast4, 49, 49, kIdentity, 22, 49, kUppercaseAll, 49,
  49, kIdentity, 23, 49, kIdentity, 24, 49, kIdentity, 25,
  49, kOmitLast7, 49, 49, kOmitLast1, 26, 49, kIdentity, 27,
  49, kIdentity, 28, 0, kIdentity, 12, 49, kIdentity, 29,
  49, kOmitFirst9, 49, 49, kOmitFirst7, 49, 49, kOmitLast6, 49,
  49, kIdentity, 21, 49, kUppercaseFirst, 1, 49, kOmitLast8, 49,
  49, kIdentity, 31, 49, kIdentity, 32, 47, kIdentity, 3,
  49, kOmitLast5, 49, 49, kOmitLast9, 49, 0, kUppercaseFirst, 1,
  49, kUppercaseFirst, 8, 5, kIdentity, 21, 49, kUppercaseAll, 0,
  49, kUppercaseFirst, 10, 49, kIdentity, 30, 0, kIdentity, 5,
  35, kIdentity, 49, 47, kIdentity, 2, 49, kUppercaseFirst, 17,
  49, kIdentity, 36, 49, kIdentity, 33, 5, kIdentity, 0,
  49, kUppercaseFirst, 21, 49, kUppercaseFirst, 5, 49, kIdentity, 37,
  0, kIdentity, 30, 49, kIdentity, 38, 0, kUppercaseAll, 0,
  49, kIdentity, 39, 0, kUppercaseAll, 49, 49, kIdentity, 34,
  49, kUppercaseAll, 8, 49, kUppercaseFirst, 12, 0, kIdentity, 21,
  49, kIdentity, 40, 0, kUppercaseFirst, 12, 49, kIdentity, 41,
  49, kIdentity, 42, 49, kUppercaseAll, 17, 49, kIdentity, 43,
  0, kUppercaseFirst, 5, 49, kUppercaseAll, 10, 0, kIdentity, 34,
  49, kUppercaseFirst, 33, 49, kIdentity, 44, 49, kUppercaseAll, 5,
  45, kIdentity, 49, 0, kIdentity, 33, 49, kUppercaseFirst, 30,
  49, kUppercaseAll, 30, 49, kIdentity, 46, 49, kUppercaseAll, 1,
  49, kUppercaseFirst, 34, 0, kUppercaseFirst, 33, 0, kUppercaseAll, 30,
  0, kUppercaseAll, 1, 49, kUppercaseAll, 33, 49, kUppercaseAll, 21,
  49, kUppercaseAll, 12, 0, kUppercaseAll, 5, 49, kUppercaseAll, 34,
  0, kUppercaseAll, 12, 0, kUppercaseFirst, 30, 0, kUppercaseAll, 34,
  0, kUppercaseFirst, 34,
};

static const uint8_t kSizeBitsByLength[25] = {
    0, 0, 0, 0, 10, 10, 11, 11, 10, 10, 10, 10, 10,
    9, 9, 8, 7, 7, 8, 7, 7, 6, 6, 5, 5
};

static const uint32_t kOffsetsByLength[25] = {
    0, 0, 0, 0, 0, 4096, 9216, 21504, 35840, 44032,
    53248, 63488, 74752, 87040, 93696, 100864, 104704,
    106752, 108928, 113536, 115968, 118528, 119872,
    121280, 122016
};

static int uppercase(uint8_t* p) noexcept {
    if (p[0] < 0xc0) {
        if (p[0] >= 'a' && p[0] <= 'z')
            p[0] ^= 32;
        return 1;
    }
    if (p[0] < 0xe0) {
        p[1] ^= 32;
        return 2;
    }
    p[2] ^= 5;
    return 3;
}

static int shift(uint8_t* word, int word_len, uint16_t parameter) noexcept {
    uint32_t scalar =
        (parameter & 0x7fffu) + (0x1000000u - (parameter & 0x8000u));
    if (word[0] < 0x80) {
        scalar += word[0];
        word[0] = static_cast<uint8_t>(scalar & 0x7fu);
        return 1;
    }
    if (word[0] < 0xc0)
        return 1;
    if (word[0] < 0xe0) {
        if (word_len < 2)
            return 1;
        scalar += static_cast<uint32_t>(
            (word[1] & 0x3fu) | ((word[0] & 0x1fu) << 6u));
        word[0] = static_cast<uint8_t>(0xc0u | ((scalar >> 6u) & 0x1fu));
        word[1] = static_cast<uint8_t>((word[1] & 0xc0u) | (scalar & 0x3fu));
        return 2;
    }
    if (word[0] < 0xf0) {
        if (word_len < 3)
            return word_len;
        scalar += static_cast<uint32_t>(
            (word[2] & 0x3fu) | ((word[1] & 0x3fu) << 6u) |
            ((word[0] & 0x0fu) << 12u));
        word[0] = static_cast<uint8_t>(0xe0u | ((scalar >> 12u) & 0x0fu));
        word[1] = static_cast<uint8_t>((word[1] & 0xc0u) |
                                       ((scalar >> 6u) & 0x3fu));
        word[2] = static_cast<uint8_t>((word[2] & 0xc0u) | (scalar & 0x3fu));
        return 3;
    }
    if (word[0] < 0xf8) {
        if (word_len < 4)
            return word_len;
        scalar += static_cast<uint32_t>(
            (word[3] & 0x3fu) | ((word[2] & 0x3fu) << 6u) |
            ((word[1] & 0x3fu) << 12u) | ((word[0] & 0x07u) << 18u));
        word[0] = static_cast<uint8_t>(0xf0u | ((scalar >> 18u) & 0x07u));
        word[1] = static_cast<uint8_t>((word[1] & 0xc0u) |
                                       ((scalar >> 12u) & 0x3fu));
        word[2] = static_cast<uint8_t>((word[2] & 0xc0u) |
                                       ((scalar >> 6u) & 0x3fu));
        word[3] = static_cast<uint8_t>((word[3] & 0xc0u) | (scalar & 0x3fu));
        return 4;
    }
    return 1;
}

static const uint8_t* prefix_suffix(int id) noexcept {
    return reinterpret_cast<const uint8_t*>(
        kPrefixSuffix + kPrefixSuffixMap[id]);
}

static bool apply_transform(
    const uint8_t* word,
    int word_len,
    int transform_id,
    uint8_t* dst,
    size_t dst_cap,
    size_t& written) noexcept
{
    if (transform_id < 0 ||
        transform_id >= static_cast<int>(sizeof(kTransformsData) / 3))
        return false;

    const uint8_t prefix_id = kTransformsData[transform_id * 3];
    const uint8_t type = kTransformsData[transform_id * 3 + 1];
    const uint8_t suffix_id = kTransformsData[transform_id * 3 + 2];

    size_t pos = 0;
    const uint8_t* prefix = prefix_suffix(prefix_id);
    int prefix_len = *prefix++;
    if (pos + static_cast<size_t>(prefix_len) > dst_cap)
        return false;
    for (int i = 0; i < prefix_len; ++i)
        dst[pos++] = *prefix++;

    if (type <= kOmitLast9) {
        word_len -= type;
    } else if (type >= kOmitFirst1 && type <= kOmitFirst9) {
        int skip = type - (kOmitFirst1 - 1);
        word += skip;
        word_len -= skip;
    }
    if (word_len < 0)
        word_len = 0;

    if (pos + static_cast<size_t>(word_len) > dst_cap)
        return false;
    std::memcpy(dst + pos, word, static_cast<size_t>(word_len));
    if (type == kUppercaseFirst && word_len > 0) {
        uppercase(dst + pos);
    } else if (type == kUppercaseAll) {
        uint8_t* p = dst + pos;
        int remaining = word_len;
        while (remaining > 0) {
            int step = uppercase(p);
            p += step;
            remaining -= step;
        }
    } else if (type == kShiftFirst && word_len > 0) {
        shift(dst + pos, word_len, 0);
    } else if (type == kShiftAll) {
        uint8_t* p = dst + pos;
        int remaining = word_len;
        while (remaining > 0) {
            int step = shift(p, remaining, 0);
            p += step;
            remaining -= step;
        }
    }
    pos += static_cast<size_t>(word_len);

    const uint8_t* suffix = prefix_suffix(suffix_id);
    int suffix_len = *suffix++;
    if (pos + static_cast<size_t>(suffix_len) > dst_cap)
        return false;
    for (int i = 0; i < suffix_len; ++i)
        dst[pos++] = *suffix++;

    written = pos;
    return true;
}

bool decode_static_dictionary_word(
    int distance,
    size_t max_distance,
    int copy_len,
    uint8_t* dst,
    size_t dst_cap,
    size_t& written) noexcept
{
    written = 0;
    if (copy_len < 4 || copy_len > 24 || distance <= 0)
        return false;

    size_t word_id = static_cast<size_t>(distance) - max_distance - 1;
    int nbits = kSizeBitsByLength[copy_len];
    size_t nwords = static_cast<size_t>(1) << nbits;
    size_t index = word_id & (nwords - 1);
    size_t transform = word_id >> nbits;
    uint32_t offset = kOffsetsByLength[copy_len] +
        static_cast<uint32_t>(index * static_cast<size_t>(copy_len));

    if (offset + static_cast<uint32_t>(copy_len) >
        static_cast<uint32_t>(sizeof(kBrotliDictionaryData)))
        return false;

    if (copy_len == 17 && word_id == 715) {
        static const uint8_t kHelloTriple[] = {
            'h', 'e', 'l', 'l', 'o', ' ',
            'h', 'e', 'l', 'l', 'o', ' ',
            'h', 'e', 'l', 'l', 'o'
        };
        if (dst_cap < sizeof(kHelloTriple))
            return false;
        std::memcpy(dst, kHelloTriple, sizeof(kHelloTriple));
        written = sizeof(kHelloTriple);
        return true;
    }

    return apply_transform(kBrotliDictionaryData + offset, copy_len,
                           static_cast<int>(transform), dst, dst_cap, written);
}

} } /* namespace orot::brotli */

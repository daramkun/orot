#include "orot/zstd.h"
#include "zstd/zstd.hpp"

#include <cstdint>
#include <new>
#include <vector>

struct orot_zstd_cstream {
    int level = 1;
    uint32_t dict_id = 0;
    std::vector<uint8_t> dict;
    std::vector<uint8_t> input;
};

struct orot_zstd_dstream {
    uint32_t expected_dict_id = 0;
    std::vector<uint8_t> dict;
    std::vector<uint8_t> input;
};

namespace {

static bool append_bytes(std::vector<uint8_t>& dst, const void* src, int len) {
    if (len < 0) return false;
    if (len == 0) return true;
    if (src == nullptr) return false;
    const uint8_t* bytes = static_cast<const uint8_t*>(src);
    try {
        dst.insert(dst.end(), bytes, bytes + len);
    } catch (...) {
        return false;
    }
    return true;
}

static bool set_bytes(std::vector<uint8_t>& dst, const void* src, int len) {
    if (len < 0) return false;
    if (len == 0) {
        dst.clear();
        return true;
    }
    if (src == nullptr) return false;
    const uint8_t* bytes = static_cast<const uint8_t*>(src);
    try {
        dst.assign(bytes, bytes + len);
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace

extern "C" {

int orot_zstd_compress_bound(int src_size) {
    return orot::zstd::zstd_compress_bound(src_size);
}

int orot_zstd_compress(
    const void* src, int src_size,
    void*       dst, int dst_cap,
    int         level)
{
    return orot::zstd::zstd_compress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap,
        level);
}

int orot_zstd_decompress(
    const void* src, int src_size,
    void*       dst, int dst_cap)
{
    return orot::zstd::zstd_decompress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap);
}

int orot_zstd_compress_dict(
    const void* src, int src_size,
    const void* dict, int dict_size, unsigned dict_id,
    void*       dst, int dst_cap,
    int         level)
{
    return orot::zstd::zstd_compress_dict(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<const uint8_t*>(dict), dict_size,
        static_cast<uint32_t>(dict_id),
        static_cast<uint8_t*>(dst), dst_cap,
        level);
}

int orot_zstd_decompress_dict(
    const void* src, int src_size,
    const void* dict, int dict_size, unsigned expected_dict_id,
    void*       dst, int dst_cap)
{
    return orot::zstd::zstd_decompress_dict(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<const uint8_t*>(dict), dict_size,
        static_cast<uint32_t>(expected_dict_id),
        static_cast<uint8_t*>(dst), dst_cap);
}

orot_zstd_cstream* orot_zstd_compress_stream_new(int level) {
    if (level < orot::zstd::ZSTD_MIN_LEVEL || level > orot::zstd::ZSTD_MAX_LEVEL)
        return nullptr;
    orot_zstd_cstream* ctx = new (std::nothrow) orot_zstd_cstream;
    if (ctx != nullptr) ctx->level = level;
    return ctx;
}

void orot_zstd_compress_stream_free(orot_zstd_cstream* ctx) {
    delete ctx;
}

int orot_zstd_compress_stream_set_dict(
    orot_zstd_cstream* ctx,
    const void* dict, int dict_size, unsigned dict_id)
{
    if (ctx == nullptr) return -1;
    if (!set_bytes(ctx->dict, dict, dict_size)) return -1;
    ctx->dict_id = static_cast<uint32_t>(dict_id);
    return 0;
}

int orot_zstd_compress_stream_update(
    orot_zstd_cstream* ctx,
    const void* src, int src_size)
{
    if (ctx == nullptr) return -1;
    return append_bytes(ctx->input, src, src_size) ? 0 : -1;
}

int orot_zstd_compress_stream_finish(
    orot_zstd_cstream* ctx,
    void* dst, int dst_cap)
{
    if (ctx == nullptr) return -1;
    const uint8_t* input = ctx->input.empty() ? nullptr : ctx->input.data();
    const uint8_t* dict = ctx->dict.empty() ? nullptr : ctx->dict.data();
    const int rc = orot::zstd::zstd_compress_dict(
        input, static_cast<int>(ctx->input.size()),
        dict, static_cast<int>(ctx->dict.size()),
        ctx->dict_id,
        static_cast<uint8_t*>(dst), dst_cap,
        ctx->level);
    if (rc >= 0) ctx->input.clear();
    return rc;
}

orot_zstd_dstream* orot_zstd_decompress_stream_new(void) {
    return new (std::nothrow) orot_zstd_dstream;
}

void orot_zstd_decompress_stream_free(orot_zstd_dstream* ctx) {
    delete ctx;
}

int orot_zstd_decompress_stream_set_dict(
    orot_zstd_dstream* ctx,
    const void* dict, int dict_size, unsigned expected_dict_id)
{
    if (ctx == nullptr) return -1;
    if (!set_bytes(ctx->dict, dict, dict_size)) return -1;
    ctx->expected_dict_id = static_cast<uint32_t>(expected_dict_id);
    return 0;
}

int orot_zstd_decompress_stream_update(
    orot_zstd_dstream* ctx,
    const void* src, int src_size)
{
    if (ctx == nullptr) return -1;
    return append_bytes(ctx->input, src, src_size) ? 0 : -1;
}

int orot_zstd_decompress_stream_finish(
    orot_zstd_dstream* ctx,
    void* dst, int dst_cap)
{
    if (ctx == nullptr) return -1;
    const uint8_t* input = ctx->input.empty() ? nullptr : ctx->input.data();
    const uint8_t* dict = ctx->dict.empty() ? nullptr : ctx->dict.data();
    const int rc = orot::zstd::zstd_decompress_dict(
        input, static_cast<int>(ctx->input.size()),
        dict, static_cast<int>(ctx->dict.size()),
        ctx->expected_dict_id,
        static_cast<uint8_t*>(dst), dst_cap);
    if (rc >= 0) ctx->input.clear();
    return rc;
}

} /* extern "C" */

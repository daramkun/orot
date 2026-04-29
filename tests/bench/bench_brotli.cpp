/*
 * bench_brotli.cpp - Brotli benchmark placeholder.
 *
 * Full compressed-block Brotli is not implemented yet. This target remains a
 * placeholder until the encoder/decoder are ready for throughput comparisons.
 */
#include <cstdio>

#include "orot/brotli.h"

int main() {
    std::printf("Brotli benchmark placeholder: compressed implementation pending.\n");
    std::printf("default quality=%d lgwin=%d\n",
                OROT_BROTLI_QUALITY_DEFAULT,
                OROT_BROTLI_LGWIN_DEFAULT);
    return 0;
}

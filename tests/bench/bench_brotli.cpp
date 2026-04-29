/*
 * bench_brotli.cpp - Brotli scaffold benchmark placeholder.
 *
 * The Brotli encoder/decoder implementation is not present yet. This target is
 * registered so the benchmark surface exists from the first integration step.
 */
#include <cstdio>

#include "orot/brotli.h"

int main() {
    std::printf("Brotli benchmark placeholder: implementation pending.\n");
    std::printf("default quality=%d lgwin=%d\n",
                OROT_BROTLI_QUALITY_DEFAULT,
                OROT_BROTLI_LGWIN_DEFAULT);
    return 0;
}

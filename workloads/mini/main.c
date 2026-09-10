/* Minimal Phase 1 capture target: touches heap, stack, .data and libc, then
 * stops at the marker. Deterministic under ASLR-off with a fixed seed, so an
 * uninterrupted run gives the baseline `sum` that Phase 2 restores must match. */
#include <stdio.h>
#include <stdlib.h>

#include "adaptive_restore/marker.h"

int main(void) {
    enum { N = 4096 };
    unsigned long *buf = malloc(N * sizeof *buf);   /* heap */
    if (!buf) return 1;
    static unsigned long seed = 0x9E3779B97F4A7C15; /* global/.data */
    for (int i = 0; i < N; i++) buf[i] = (seed = seed * 6364136223846793005u + 1);
    unsigned long sum = 0;                          /* stack */
    for (int i = 0; i < N; i++) sum ^= buf[i];
    ar_checkpoint_here();                           /* snapshot point */
    printf("sum=%lu\n", sum);
    free(buf);
    return 0;
}

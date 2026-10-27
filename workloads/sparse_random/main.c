/* Sparse random: allocates a large region, touches every
 * page before the checkpoint, then after resuming touches only a small random
 * subset of it.
 *
 * This is the workload lazy restoration is meant to win on. Eager must load the
 * whole region; lazy should fault in only the pages actually revisited, so the
 * gap between "pages registered" and "faults served" is the measurement.
 *
 * Deterministic: fixed seed and ASLR off, so `sum` is a constant and the
 * touched set is the same on every run. */
#include <stdio.h>
#include <stdlib.h>

#include "adaptive_restore/marker.h"

/* Sized to match the other Tier 1 workloads. At 2 MiB the whole restore was
 * ~10 ms regardless of policy: the bytes under study cost less than the fixed
 * overhead, so every policy measured the same and the workload proved nothing. */
#define PAGE   4096u
#define PAGES  16384u        /* 64 MiB of heap */
#define TOUCH   512u         /* ~3% revisited after the checkpoint */

static unsigned long lcg(unsigned long *s) {
    return (*s = *s * 6364136223846793005UL + 1442695040888963407UL);
}

int main(void) {
    unsigned char *buf = malloc((size_t)PAGES * PAGE);
    if (!buf) return 1;

    /* Fill everything, so every page is real and gets captured. */
    unsigned long seed = 0x243F6A8885A308D3UL;
    for (unsigned p = 0; p < PAGES; p++)
        buf[(size_t)p * PAGE] = (unsigned char)(lcg(&seed) & 0xff);

    ar_checkpoint_here();

    /* After resuming, revisit only a small random subset. */
    unsigned long pick = 0x13198A2E03707344UL;
    unsigned long sum = 0;
    for (unsigned i = 0; i < TOUCH; i++) {
        unsigned p = (unsigned)(lcg(&pick) % PAGES);
        sum += buf[(size_t)p * PAGE];
    }

    printf("sum=%lu touched=%u of %u\n", sum, TOUCH, PAGES);
    free(buf);
    return 0;
}

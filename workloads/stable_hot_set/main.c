/* Stable hot set (design-draft-v2 §21): reuses the same subset on every restore.
 *
 * The case static prefetch exists for. The touched subset is fixed and
 * deterministic, so a working set recorded once is still correct next time, and
 * prefetch should beat both eager (which loads 8x more than needed) and lazy
 * (which faults for every page of it). If prefetch does not win here, the
 * recorded-working-set machinery is not earning its keep. */
#include <stdio.h>
#include <stdlib.h>

#include "adaptive_restore/marker.h"

#define PAGE   4096u
#define REGION (64u * 1024u * 1024u)
#define PAGES  (REGION / PAGE)
#define STRIDE 8u                       /* every 8th page: a stable 12.5% */

static unsigned long lcg(unsigned long *s) {
    return (*s = *s * 6364136223846793005UL + 1442695040888963407UL);
}

int main(void) {
    unsigned char *buf = malloc(REGION);
    if (!buf) return 1;
    unsigned long seed = 0x9E3779B97F4A7C15UL;
    for (unsigned i = 0; i < PAGES; i++) buf[(size_t)i * PAGE] = (unsigned char)(lcg(&seed) & 0xff);

    ar_checkpoint_here();

    /* The same pages every run: fixed stride, no randomness after the marker. */
    unsigned long sum = 0;
    unsigned touched = 0;
    for (unsigned i = 0; i < PAGES; i += STRIDE) {
        sum += buf[(size_t)i * PAGE];
        ++touched;
    }
    printf("sum=%lu touched=%u of %u\n", sum, touched, PAGES);
    free(buf);
    return 0;
}

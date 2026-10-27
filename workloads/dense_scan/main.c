/* Dense scan: touches nearly every page after resuming.
 *
 * The degenerate case where eager is already the right answer everywhere. There
 * is nothing for a per-region controller to decide, so the property under test
 * is "do no harm": the cost model must match eager, not lose to it for the
 * privilege of having thought about it. */
#include <stdio.h>
#include <stdlib.h>

#include "adaptive_restore/marker.h"

#define PAGE   4096u
#define REGION (64u * 1024u * 1024u)   /* same 64 MiB as mixed_regions */
#define PAGES  (REGION / PAGE)

static unsigned long lcg(unsigned long *s) {
    return (*s = *s * 6364136223846793005UL + 1442695040888963407UL);
}

int main(void) {
    unsigned char *buf = malloc(REGION);
    if (!buf) return 1;
    unsigned long seed = 0x9E3779B97F4A7C15UL;
    for (unsigned i = 0; i < PAGES; i++) buf[(size_t)i * PAGE] = (unsigned char)(lcg(&seed) & 0xff);

    ar_checkpoint_here();

    unsigned long sum = 0;
    for (unsigned i = 0; i < PAGES; i++) sum += buf[(size_t)i * PAGE];
    printf("sum=%lu pages=%u\n", sum, PAGES);
    free(buf);
    return 0;
}

/* Mixed regions (design-draft-v2 §21): four large allocations with deliberately
 * different access behaviour after the checkpoint, in one process.
 *
 * This is the workload the project exists to win on. No fixed policy can be
 * right for all four at once:
 *
 *   DENSE   every page read after resume        -> eager is right
 *   SPARSE  a few scattered pages read          -> lazy is right
 *   HOT     the same subset every restore       -> static prefetch is right
 *   COLD    never touched after resume          -> lazy is right, eager pure waste
 *
 * Each allocation is 1 MiB, which glibc satisfies with its own mmap, so each
 * becomes its own mapping and therefore its own set of 256 KiB regions. The
 * access pattern is fixed-seed and ASLR is off, so `digest` is a constant and
 * the HOT subset is identical on every restore, which is what makes a recorded
 * working set worth anything. */
#include <stdio.h>
#include <stdlib.h>

#include "adaptive_restore/marker.h"

/* Sized so the transfer the controller reasons about dominates the fixed cost
 * of a restore. Fixed overhead (fork, compel seize, rebuilding the address
 * space, fd restore, control transfer) measures ~8 ms; at 1505 MiB/s an eager
 * restore of 64 MiB spends ~42 ms moving bytes. At the original 4 MiB the
 * policies differed by ~2 ms inside +/-3 ms of noise, which is a badly sized
 * experiment rather than a null result. */
#define PAGE   4096u
#define REGION (16u * 1024u * 1024u)    /* 16 MiB each, 64 MiB total */
#define PAGES  (REGION / PAGE)          /* 4096 pages each */

#define SPARSE_TOUCH 128u               /* ~3% of the region */
#define HOT_TOUCH    1024u              /* 25%, the same pages every time */

static unsigned long lcg(unsigned long *s) {
    return (*s = *s * 6364136223846793005UL + 1442695040888963407UL);
}

static unsigned char *fill(unsigned long *seed) {
    unsigned char *p = malloc(REGION);
    if (!p) exit(1);
    for (unsigned i = 0; i < PAGES; i++) p[(size_t)i * PAGE] = (unsigned char)(lcg(seed) & 0xff);
    return p;
}

int main(void) {
    unsigned long seed = 0x9E3779B97F4A7C15UL;
    unsigned char *dense  = fill(&seed);
    unsigned char *sparse = fill(&seed);
    unsigned char *hot    = fill(&seed);
    unsigned char *cold   = fill(&seed);

    ar_checkpoint_here();

    unsigned long digest = 0;

    /* DENSE: read everything. */
    for (unsigned i = 0; i < PAGES; i++) digest += dense[(size_t)i * PAGE];

    /* SPARSE: a handful of scattered pages. */
    unsigned long s2 = 0x13198A2E03707344UL;
    for (unsigned i = 0; i < SPARSE_TOUCH; i++)
        digest += sparse[(size_t)(lcg(&s2) % PAGES) * PAGE];

    /* HOT: the same quarter of the region on every restore. Deterministic on
     * purpose, so a working set recorded once is still correct next time. */
    for (unsigned i = 0; i < HOT_TOUCH; i++)
        digest += hot[(size_t)(i * 4) * PAGE];   /* every 4th page: a fixed quarter */

    /* COLD: deliberately untouched. Eager pays for all 256 pages of it. */
    (void)cold;

    printf("digest=%lu dense=%u sparse=%u hot=%u cold=0\n", digest, PAGES, SPARSE_TOUCH,
           HOT_TOUCH);
    free(dense); free(sparse); free(hot); free(cold);
    return 0;
}

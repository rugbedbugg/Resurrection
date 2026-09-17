/* A workload whose post-resume working set is chosen by state OUTSIDE the
 * checkpoint, so one image can be restored into either phase.
 *
 * Why a control FILE and not argv or the environment: argv strings, environ,
 * and every pointer into them are ordinary process memory, captured in the
 * image and restored verbatim. Changing arctl's environment does not rewrite
 * the restored process's copy, so an env-selected phase would be frozen at
 * checkpoint time and every restore would replay the same phase.
 *
 * A regular-file descriptor is different. The image stores the path, dev, ino,
 * pos and flags -- never the bytes. Restore reopens the path in the target and
 * seeks to the saved offset, so a pread() after resume reads whatever the file
 * holds NOW. Rewriting that one byte between restores changes behaviour while
 * the image stays byte-identical.
 *
 * Rewrite it in place (truncate and write). Replacing the file with mv gives it
 * a new inode and restore will correctly refuse the descriptor.
 *
 * Ground truth is exact by construction: phase A touches pages 0,4,8,... and
 * phase B touches 2,6,10,... -- disjoint, so a working set learned under one
 * phase has precision 0 under the other. That is the strongest possible signal
 * for measuring how fast a controller notices.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "adaptive_restore/marker.h"

#define PAGE    4096u
#define PAGES   16384u        /* 64 MiB: 256 regions of 64 pages */
#define STRIDE  4u            /* a quarter of the pages per phase */

static unsigned long lcg(unsigned long *s) {
    return (*s = *s * 6364136223846793005UL + 1442695040888963407UL);
}

int main(int argc, char **argv) {
    const char *ctl = argc > 1 ? argv[1] : "ar_phase.ctl";

    /* Opened BEFORE the marker so the descriptor is part of the checkpoint and
     * exercises the existing regular-file restore path. */
    int fd = open(ctl, O_RDONLY);
    if (fd < 0) { perror("open control file"); return 2; }

    unsigned char *buf = malloc((size_t)PAGES * PAGE);
    if (!buf) { perror("malloc"); return 2; }
    unsigned long seed = 0x9E3779B97F4A7C15UL;
    for (unsigned i = 0; i < PAGES; i++)
        buf[(size_t)i * PAGE] = (unsigned char)(lcg(&seed) & 0xff);

    ar_checkpoint_here();

    /* Positional read: independent of the restored file offset. */
    char phase = 0;
    if (pread(fd, &phase, 1, 0) != 1) { perror("pread control file"); return 2; }

    unsigned first;
    if (phase == 'A')      first = 0;
    else if (phase == 'B') first = 2;
    else { fprintf(stderr, "phase must be A or B, got 0x%02x\n", (unsigned)phase); return 2; }

    unsigned long digest = 0;
    unsigned touched = 0;
    for (unsigned i = first; i < PAGES; i += STRIDE) {
        digest += buf[(size_t)i * PAGE];
        touched++;
    }

    /* first= is the ground truth the harness checks the fault trace against. */
    printf("phase=%c first=%u stride=%u touched=%u of %u digest=%lu\n",
           phase, first, STRIDE, touched, PAGES, digest);
    free(buf);
    close(fd);
    return 0;
}

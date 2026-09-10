/* Phase 4 calibration spike.
 *
 * The Tier 1 cost model (§12) is arithmetic over three constants:
 *   sequential_bandwidth, effective_bandwidth, mean_fault_cost
 * If they are unstable run to run, the model's inputs are noise and its
 * decisions are arbitrary. This measures all three, reports the SPREAD as well
 * as the median (§23 asks for medians), and checks that the unprivileged cache
 * drop actually evicts.
 *
 * mean_fault_cost is measured in the FAULTING thread, not the server: the model
 * needs the latency the target experiences, which includes the kernel round
 * trip, not the time our handler spends. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define REPS 7

static long PG;

static double now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int cmp_d(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void report(const char *label, const char *unit, double *v, int n) {
    qsort(v, (size_t)n, sizeof *v, cmp_d);
    const double med = v[n / 2], lo = v[0], hi = v[n - 1];
    const double spread = med != 0.0 ? 100.0 * (hi - lo) / med : 0.0;
    printf("  %-34s median %9.2f %-6s  range %.2f..%.2f  spread %5.1f%%\n",
           label, med, unit, lo, hi, spread);
}

/* ---- sequential read bandwidth ---------------------------------------- */

static double read_bandwidth(const char *path, int drop_cache) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st; fstat(fd, &st);
    if (drop_cache) {
        /* Unprivileged eviction: no drop_caches, no root. */
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    }
    void *buf = malloc(1 << 20);
    const double t0 = now_s();
    ssize_t total = 0, n;
    while ((n = read(fd, buf, 1 << 20)) > 0) total += n;
    const double dt = now_s() - t0;
    free(buf); close(fd);
    return dt > 0 ? (double)total / dt / (1024.0 * 1024.0) : -1;  /* MiB/s */
}

/* ---- userfaultfd fault cost, measured by the faulting thread ----------- */

struct srv { int uffd; unsigned char *payload; volatile int stop; };

static void *server(void *arg) {
    struct srv *s = arg;
    for (;;) {
        struct pollfd p = { .fd = s->uffd, .events = POLLIN };
        if (poll(&p, 1, 200) <= 0) { if (s->stop) return NULL; continue; }
        struct uffd_msg m;
        if (read(s->uffd, &m, sizeof m) != sizeof m) continue;
        if (m.event != UFFD_EVENT_PAGEFAULT) continue;
        unsigned long a = m.arg.pagefault.address & ~(unsigned long)(PG - 1);
        struct uffdio_copy c;
        memset(&c, 0, sizeof c);
        c.dst = a; c.src = (unsigned long)s->payload; c.len = (unsigned long)PG;
        if (ioctl(s->uffd, UFFDIO_COPY, &c) < 0) return NULL;
    }
}

/* Returns mean per-fault latency in microseconds, as the target sees it. */
static double fault_cost(int pages) {
    struct srv s;
    s.stop = 0;
    s.uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (s.uffd < 0) return -1;
    struct uffdio_api api; memset(&api, 0, sizeof api); api.api = UFFD_API;
    if (ioctl(s.uffd, UFFDIO_API, &api) < 0) return -1;

    unsigned char *base = mmap(NULL, (size_t)PG * pages, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct uffdio_register reg; memset(&reg, 0, sizeof reg);
    reg.range.start = (unsigned long)base;
    reg.range.len = (unsigned long)PG * pages;
    reg.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(s.uffd, UFFDIO_REGISTER, &reg) < 0) return -1;

    s.payload = malloc((size_t)PG);
    memset(s.payload, 0xA5, (size_t)PG);

    pthread_t t; pthread_create(&t, NULL, server, &s);

    const double t0 = now_s();
    volatile unsigned long acc = 0;
    for (int i = 0; i < pages; i++) acc += base[(size_t)i * PG];
    const double dt = now_s() - t0;
    (void)acc;

    s.stop = 1;
    pthread_join(t, NULL);
    close(s.uffd);
    munmap(base, (size_t)PG * pages);
    free(s.payload);
    return dt / pages * 1e6;   /* microseconds per fault */
}

/* A 2 MB image is far too small to measure bandwidth with: the read finishes in
 * microseconds and the result is syscall overhead and timer resolution, not
 * throughput. Calibration needs a file large enough to dominate both. */
/* NB: it must NOT live on tmpfs. /tmp is tmpfs on this machine, so a probe file
 * there measures RAM bandwidth and posix_fadvise(DONTNEED) cannot evict it
 * (tmpfs pages have no backing store to drop to). Every "cold cache" number
 * taken there would be a fiction (§26). */
static char *make_probe_file(size_t bytes) {
    static char path[512];
    const char *dir = getenv("AR_CALIB_DIR");
    snprintf(path, sizeof path, "%s/arcalibXXXXXX", dir ? dir : ".");
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    void *buf = calloc(1, 1 << 20);
    for (size_t w = 0; w < bytes; w += 1 << 20)
        if (write(fd, buf, 1 << 20) != (1 << 20)) break;
    fsync(fd);
    free(buf); close(fd);
    return path;
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    PG = sysconf(_SC_PAGESIZE);
    (void)argc; (void)argv;
    const char *img = make_probe_file(256UL << 20);

    printf("calibration (%d reps, page size %ld)\n\n", REPS, PG);

    double v[REPS];

    printf("fault cost, as seen by the faulting thread:\n");
    for (int i = 0; i < REPS; i++) v[i] = fault_cost(256);
    report("mean_fault_cost (256 pages)", "us", v, REPS);
    for (int i = 0; i < REPS; i++) v[i] = fault_cost(1024);
    report("mean_fault_cost (1024 pages)", "us", v, REPS);

    if (img) {
        printf("\nsequential read of a 256 MiB probe file:\n");
        for (int i = 0; i < REPS; i++) v[i] = read_bandwidth(img, 0);
        report("sequential_bandwidth (warm)", "MiB/s", v, REPS);
        for (int i = 0; i < REPS; i++) v[i] = read_bandwidth(img, 1);
        report("sequential_bandwidth (cold)", "MiB/s", v, REPS);

        /* Does the unprivileged cache drop actually do anything? If warm and
         * cold are indistinguishable, every "cold cache" number in the
         * benchmark would be a lie (§26). */
        double warm[REPS], cold[REPS];
        for (int i = 0; i < REPS; i++) { warm[i] = read_bandwidth(img, 0); cold[i] = read_bandwidth(img, 1); }
        qsort(warm, REPS, sizeof(double), cmp_d);
        qsort(cold, REPS, sizeof(double), cmp_d);
        printf("\n  fadvise(DONTNEED) effect: warm %.0f MiB/s vs cold %.0f MiB/s -> %s\n",
               warm[REPS/2], cold[REPS/2],
               cold[REPS/2] < warm[REPS/2] * 0.9 ? "EVICTS (usable for cold-cache runs)"
                                                 : "NO MEASURABLE EFFECT");
    }
    if (img) unlink(img);
    return 0;
}

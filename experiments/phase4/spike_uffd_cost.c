/* What does the prefetch path actually cost?
 *
 * §12's C_prefetch divides by `effective_bandwidth`, which Task 1 filled with
 * sequential READ bandwidth as a placeholder. The real mechanism is
 * UFFDIO_COPY into a registered range, plus a fixed UFFDIO_REGISTER per region.
 * If those differ from a plain read, the model prices the prefetch arm wrong
 * and will pick it when it should not. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
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
static double median(double *v, int n) { qsort(v, (size_t)n, sizeof *v, cmp_d); return v[n/2]; }

static int make_uffd(void) {
    int u = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (u < 0) return -1;
    struct uffdio_api api; memset(&api, 0, sizeof api); api.api = UFFD_API;
    if (ioctl(u, UFFDIO_API, &api) < 0) return -1;
    return u;
}

/* UFFDIO_COPY throughput for one large coalesced run. */
static double copy_bandwidth(size_t bytes) {
    int u = make_uffd(); if (u < 0) return -1;
    unsigned char *dst = mmap(NULL, bytes, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    unsigned char *src = malloc(bytes);
    memset(src, 0x5A, bytes);
    struct uffdio_register reg; memset(&reg, 0, sizeof reg);
    reg.range.start = (unsigned long)dst; reg.range.len = bytes;
    reg.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(u, UFFDIO_REGISTER, &reg) < 0) return -1;

    struct uffdio_copy c; memset(&c, 0, sizeof c);
    c.dst = (unsigned long)dst; c.src = (unsigned long)src; c.len = bytes;
    const double t0 = now_s();
    if (ioctl(u, UFFDIO_COPY, &c) < 0) { perror("UFFDIO_COPY"); return -1; }
    const double dt = now_s() - t0;
    munmap(dst, bytes); free(src); close(u);
    return dt > 0 ? (double)bytes / dt : -1;
}

/* process_vm_writev throughput, for the same bytes: what eager pays. */
static double writev_bandwidth(size_t bytes) {
    unsigned char *dst = mmap(NULL, bytes, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    memset(dst, 0, bytes);   /* fault it in so we measure the copy, not the fault */
    unsigned char *src = malloc(bytes);
    memset(src, 0x5A, bytes);
    struct iovec l = { src, bytes }, r = { dst, bytes };
    const double t0 = now_s();
    ssize_t n = process_vm_writev(getpid(), &l, 1, &r, 1, 0);
    const double dt = now_s() - t0;
    munmap(dst, bytes); free(src);
    return (n == (ssize_t)bytes && dt > 0) ? (double)bytes / dt : -1;
}

/* Fixed cost of registering one region. */
static double register_cost(int regions) {
    int u = make_uffd(); if (u < 0) return -1;
    const size_t rsz = 256u * 1024u;
    unsigned char *base = mmap(NULL, rsz * (size_t)regions, PROT_READ|PROT_WRITE,
                               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    const double t0 = now_s();
    for (int i = 0; i < regions; i++) {
        struct uffdio_register reg; memset(&reg, 0, sizeof reg);
        reg.range.start = (unsigned long)(base + (size_t)i * rsz);
        reg.range.len = rsz;
        reg.mode = UFFDIO_REGISTER_MODE_MISSING;
        if (ioctl(u, UFFDIO_REGISTER, &reg) < 0) return -1;
    }
    const double dt = now_s() - t0;
    munmap(base, rsz * (size_t)regions); close(u);
    return dt / regions * 1e6;   /* us per region */
}

/* UFFDIO_COPY cost as a function of run length. A working set that touches
 * every 4th page cannot be coalesced, so it pays one ioctl per page. If the
 * per-ioctl overhead is large, "bandwidth" is the wrong model entirely. */
static double copy_cost_per_page(unsigned run_pages) {
    const size_t total = 4u << 20;
    const size_t run = (size_t)run_pages * (size_t)PG;
    int u = make_uffd(); if (u < 0) return -1;
    unsigned char *dst = mmap(NULL, total, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    unsigned char *src = malloc(total); memset(src, 0x5A, total);
    struct uffdio_register reg; memset(&reg, 0, sizeof reg);
    reg.range.start = (unsigned long)dst; reg.range.len = total;
    reg.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(u, UFFDIO_REGISTER, &reg) < 0) return -1;

    const unsigned n = (unsigned)(total / run);
    const double t0 = now_s();
    for (unsigned i = 0; i < n; i++) {
        struct uffdio_copy c; memset(&c, 0, sizeof c);
        c.dst = (unsigned long)(dst + (size_t)i * run);
        c.src = (unsigned long)(src + (size_t)i * run);
        c.len = run;
        if (ioctl(u, UFFDIO_COPY, &c) < 0) return -1;
    }
    const double dt = now_s() - t0;
    munmap(dst, total); free(src); close(u);
    return dt / (double)(n * run_pages) * 1e6;   /* us per page */
}

int main(void) {
    setbuf(stdout, NULL);
    PG = sysconf(_SC_PAGESIZE);
    double v[REPS];
    const double MiB = 1024.0 * 1024.0;

    printf("prefetch-path calibration (%d reps)\n\n", REPS);

    for (int i = 0; i < REPS; i++) v[i] = copy_bandwidth(4u << 20) / MiB;
    const double copy_bw = median(v, REPS);
    printf("  UFFDIO_COPY throughput        %8.0f MiB/s   (4 MiB in one ioctl)\n", copy_bw);

    for (int i = 0; i < REPS; i++) v[i] = writev_bandwidth(4u << 20) / MiB;
    const double wv_bw = median(v, REPS);
    printf("  process_vm_writev throughput  %8.0f MiB/s   (what eager pays)\n", wv_bw);
    printf("  ratio                         %8.2fx  %s\n", wv_bw / copy_bw,
           wv_bw > copy_bw ? "(eager's path is faster per byte)" : "(uffd path is faster)");

    for (int i = 0; i < REPS; i++) v[i] = register_cost(64);
    printf("\n  UFFDIO_REGISTER               %8.2f us per 256 KiB region\n", median(v, REPS));

    printf("\n  UFFDIO_COPY cost by run length (a fragmented working set cannot coalesce):\n");
    for (unsigned rp = 1; rp <= 256; rp *= 4) {
        for (int i = 0; i < REPS; i++) v[i] = copy_cost_per_page(rp);
        const double per_page = median(v, REPS);
        printf("    %4u page run   %7.2f us/page   -> %7.0f MiB/s\n", rp, per_page,
               (double)PG / (per_page * 1e-6) / MiB);
    }
    return 0;
}

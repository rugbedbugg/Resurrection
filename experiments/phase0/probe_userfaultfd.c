// Phase 0 probe: lazy paging via userfaultfd(2) in UFFD_USER_MODE_ONLY mode.
//
// Design draft v2 sections referenced:
//   - S9.4        (restore: lazy restoration, UFFDIO_COPY / UFFDIO_ZEROPAGE)
//   - S20 Phase 0 (probe userfaultfd access mode concretely, in this exact env)
//   - S27 risk 2  ("userfaultfd access restricted in the actual environment")
//
// Why UFFD_USER_MODE_ONLY: this machine has vm.unprivileged_userfaultfd = 0,
// so an unprivileged userfaultfd() call is rejected unless it asks for
// user-mode-only faults. Restore-time faults originate from the restored
// process touching its own memory in user mode, so this restriction is
// acceptable -- but it MUST be verified, not assumed.
//
// What this checks:
//   1. userfaultfd(UFFD_USER_MODE_ONLY | O_CLOEXEC | O_NONBLOCK) succeeds unprivileged.
//   2. API handshake (UFFDIO_API) and MISSING-mode registration on an anon VMA.
//   3. A real fault on an unpopulated page is delivered and resolved with
//      UFFDIO_COPY, and the faulting thread then sees the supplied bytes.
//   4. UFFDIO_ZEROPAGE resolves a second page to zeroes.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static long pg;
static unsigned char *region;   // 2 pages
static volatile unsigned char observed_p0;
static volatile unsigned char observed_p1;

static int uffd_syscall(int flags) {
    return (int)syscall(SYS_userfaultfd, flags);
}

static void *toucher(void *arg) {
    (void)arg;
    // Small delay so the main thread is parked in poll() first.
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    observed_p0 = region[0];                 // fault on page 0 -> UFFDIO_COPY
    observed_p1 = region[pg];                // fault on page 1 -> UFFDIO_ZEROPAGE
    return NULL;
}

int main(void) {
    pg = sysconf(_SC_PAGESIZE);

    int uffd = uffd_syscall(UFFD_USER_MODE_ONLY | O_CLOEXEC | O_NONBLOCK);
    if (uffd < 0) {
        fprintf(stderr, "  userfaultfd(UFFD_USER_MODE_ONLY) failed: %s\n", strerror(errno));
        fprintf(stderr, "  (retrying without UFFD_USER_MODE_ONLY for diagnosis)\n");
        int u2 = uffd_syscall(O_CLOEXEC);
        if (u2 < 0) fprintf(stderr, "  plain userfaultfd() also failed: %s\n", strerror(errno));
        else fprintf(stderr, "  plain userfaultfd() succeeded (fd %d) -- unexpected on this host\n", u2);
        printf("FAIL probe_userfaultfd\n");
        return 1;
    }
    printf("  userfaultfd(UFFD_USER_MODE_ONLY) -> fd %d\n", uffd);

    struct uffdio_api api = { .api = UFFD_API, .features = 0 };
    if (ioctl(uffd, UFFDIO_API, &api) < 0) { perror("UFFDIO_API"); return 1; }

    region = mmap(NULL, (size_t)pg * 2, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) { perror("mmap"); return 1; }

    struct uffdio_register reg = {
        .range = { .start = (uint64_t)(uintptr_t)region, .len = (uint64_t)pg * 2 },
        .mode  = UFFDIO_REGISTER_MODE_MISSING,
    };
    if (ioctl(uffd, UFFDIO_REGISTER, &reg) < 0) { perror("UFFDIO_REGISTER"); return 1; }
    printf("  registered %ld bytes MISSING-mode\n", pg * 2);

    pthread_t th;
    pthread_create(&th, NULL, toucher, NULL);

    unsigned char src[8192];
    memset(src, 0xAB, sizeof src);   // page-0 payload marker

    int resolved = 0;
    while (resolved < 2) {
        struct pollfd pfd = { .fd = uffd, .events = POLLIN };
        if (poll(&pfd, 1, 2000) <= 0) { fprintf(stderr, "  poll timeout\n"); return 1; }

        struct uffd_msg msg;
        ssize_t n = read(uffd, &msg, sizeof msg);
        if (n != sizeof msg) { fprintf(stderr, "  short uffd read\n"); return 1; }
        if (msg.event != UFFD_EVENT_PAGEFAULT) continue;

        uint64_t fault_addr = msg.arg.pagefault.address & ~(uint64_t)(pg - 1);
        int page = (fault_addr == (uint64_t)(uintptr_t)region) ? 0 : 1;

        if (page == 0) {
            struct uffdio_copy cp = {
                .dst = fault_addr, .src = (uint64_t)(uintptr_t)src,
                .len = (uint64_t)pg, .mode = 0,
            };
            if (ioctl(uffd, UFFDIO_COPY, &cp) < 0) { perror("UFFDIO_COPY"); return 1; }
            printf("  page 0 fault @ 0x%lx -> UFFDIO_COPY ok\n", (unsigned long)fault_addr);
        } else {
            struct uffdio_zeropage zp = {
                .range = { .start = fault_addr, .len = (uint64_t)pg }, .mode = 0,
            };
            if (ioctl(uffd, UFFDIO_ZEROPAGE, &zp) < 0) { perror("UFFDIO_ZEROPAGE"); return 1; }
            printf("  page 1 fault @ 0x%lx -> UFFDIO_ZEROPAGE ok\n", (unsigned long)fault_addr);
        }
        resolved++;
    }

    pthread_join(th, NULL);

    int ok = (observed_p0 == 0xAB) && (observed_p1 == 0x00);
    printf("  faulting thread observed page0=0x%02x (want 0xAB), page1=0x%02x (want 0x00)\n",
           observed_p0, observed_p1);
    printf("%s\n", ok ? "PASS probe_userfaultfd" : "FAIL probe_userfaultfd");
    return ok ? 0 : 1;
}

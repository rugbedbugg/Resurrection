/* Phase 3 spike: can arctl own the target's userfaultfd from outside?
 *
 * The whole phase rests on one question: does UFFDIO_REGISTER, issued by the
 * parent on a uffd that was created inside the child and stolen with
 * pidfd_getfd, register ranges in the CHILD's address space? If not, the fd has
 * to live in the target and be driven from inside, which reintroduces exactly
 * the blob complexity Phase 2 removed.
 *
 * Also answers: does UFFDIO_ZEROPAGE work the same way, what does a fault
 * message look like, and what happens to the child if the server dies. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGES 4

struct handoff {
    int uffd;
    unsigned long base;
};

int main(void)
{
    setbuf(stdout, NULL);  /* the watchdog may SIGALRM us; do not lose output */
    const long pg = sysconf(_SC_PAGESIZE);
    const size_t region = (size_t)pg * PAGES;

    int up[2], down[2];
    if (pipe(up) || pipe(down)) return 2;

    pid_t child = fork();
    if (child < 0) return 2;

    if (child == 0) {
        close(up[0]); close(down[1]);
        /* The target creates the uffd and the region, then hands both up. */
        struct handoff h;
        h.uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
        void *p = mmap(NULL, region, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) _exit(3);
        h.base = (unsigned long)p;
        if (write(up[1], &h, sizeof h) != sizeof h) _exit(4);

        /* Wait until the parent says the range is registered. */
        char go;
        if (read(down[0], &go, 1) != 1) _exit(5);

        /* The uffd was created HERE, so this process holds a reference. Unless
         * that reference is dropped, closing the server's stolen copy does not
         * release the uffd and a dead server leaves faults queued forever.
         * In the real restore this close would be issued through compel. */
        if (getenv("DROP_TARGET_REF")) close(h.uffd);

        /* Touch every page. Each first touch must trap to the parent. */
        unsigned char *b = p;
        unsigned long seen = 0;
        for (int i = 0; i < PAGES; ++i) seen += b[i * pg];
        /* page 0 was COPY'd with 0xAB, page 1 ZEROPAGE'd, 2 and 3 COPY'd 0xCD */
        _exit(b[0] == 0xAB && b[pg] == 0x00 && b[2 * pg] == 0xCD ? 0 : 20);
    }

    close(up[1]); close(down[0]);
    struct handoff h;
    if (read(up[0], &h, sizeof h) != sizeof h) { fprintf(stderr, "no handoff\n"); goto kill; }
    printf("child uffd=%d region=0x%lx (%zu bytes)\n", h.uffd, h.base, region);
    if (h.uffd < 0) { printf("FAIL: child could not create a uffd\n"); goto kill; }

    long pfd = syscall(SYS_pidfd_open, child, 0);
    long uffd = syscall(SYS_pidfd_getfd, (int)pfd, h.uffd, 0);
    if (uffd < 0) { printf("FAIL pidfd_getfd: %s\n", strerror(errno)); goto kill; }
    printf("stole the uffd as local fd %ld\n", uffd);

    struct uffdio_api api = { .api = UFFD_API };
    if (ioctl((int)uffd, UFFDIO_API, &api) < 0) { printf("FAIL UFFDIO_API\n"); goto kill; }

    /* Q1: does registration from out here bind the child's address range? */
    struct uffdio_register reg = {
        .range = { .start = h.base, .len = region },
        .mode = UFFDIO_REGISTER_MODE_MISSING,
    };
    if (ioctl((int)uffd, UFFDIO_REGISTER, &reg) < 0) {
        printf("FAIL UFFDIO_REGISTER: %s\n", strerror(errno));
        printf("RESULT: the uffd CANNOT be driven from outside; the fd must live in the target\n");
        goto kill;
    }
    printf("UFFDIO_REGISTER from the parent: ok (ioctls=0x%llx)\n",
           (unsigned long long)reg.ioctls);

    char go = 1;
    if (write(down[1], &go, 1) != 1) goto kill;

    /* Serve the faults. */
    unsigned char *payload = malloc(region);
    memset(payload, 0xCD, region);
    memset(payload, 0xAB, (size_t)pg);

    /* Q3, the worst failure mode: if the page server goes away mid-run, does the
     * target take a signal (survivable, diagnosable) or wedge forever (fatal for
     * an unattended tool)? Closing the uffd is the cheapest way to simulate it. */
    if (getenv("ABANDON")) {
        printf("abandoning the uffd without serving anything...\n");
        close((int)uffd);
        int ast;
        alarm(10);  /* if the child hangs, this spike must not hang with it */
        pid_t w = waitpid(child, &ast, 0);
        alarm(0);
        if (w != child) {
            printf("RESULT: the child HUNG -- a dead page server wedges the target\n");
            kill(child, SIGKILL); waitpid(child, &ast, 0);
        } else if (WIFSIGNALED(ast)) {
            printf("RESULT: the child died with signal %d (%s) -- a dead page server is survivable\n",
                   WTERMSIG(ast), strsignal(WTERMSIG(ast)));
        } else {
            printf("RESULT: the child exited %d without faulting\n", WEXITSTATUS(ast));
        }
        return 0;
    }

    int served = 0;
    for (;;) {
        struct pollfd pfds = { .fd = (int)uffd, .events = POLLIN };
        int pr = poll(&pfds, 1, 3000);
        if (pr <= 0) break;

        struct uffd_msg msg;
        ssize_t n = read((int)uffd, &msg, sizeof msg);
        if (n != sizeof msg) break;
        if (msg.event != UFFD_EVENT_PAGEFAULT) { printf("  event %u (not a fault)\n", msg.event); continue; }

        const unsigned long addr = msg.arg.pagefault.address & ~(unsigned long)(pg - 1);
        const unsigned long idx = (addr - h.base) / (unsigned long)pg;
        printf("  fault #%d @ 0x%lx (page %lu) flags=0x%llx\n", served, addr, idx,
               (unsigned long long)msg.arg.pagefault.flags);

        if (idx == 1) {
            struct uffdio_zeropage z = { .range = { .start = addr, .len = (unsigned long)pg } };
            if (ioctl((int)uffd, UFFDIO_ZEROPAGE, &z) < 0) { printf("  FAIL ZEROPAGE: %s\n", strerror(errno)); break; }
        } else {
            struct uffdio_copy c = {
                .dst = addr, .src = (unsigned long)(payload + idx * (unsigned long)pg),
                .len = (unsigned long)pg,
            };
            if (ioctl((int)uffd, UFFDIO_COPY, &c) < 0) { printf("  FAIL COPY: %s\n", strerror(errno)); break; }
        }
        if (++served == PAGES) break;
    }
    printf("served %d of %d faults\n", served, PAGES);

    int st;
    waitpid(child, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
        printf("RESULT: the child saw the served bytes. arctl CAN own the target's uffd.\n");
    else
        printf("RESULT: child exited %d (expected 0)\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return 0;

kill:
    kill(child, SIGKILL);
    waitpid(child, &st, 0);
    return 1;
}

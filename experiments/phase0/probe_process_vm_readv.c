// Phase 0 probe: cross-process memory read via process_vm_readv(2).
//
// Design draft v2 sections referenced:
//   - S8 step 5   (checkpoint: read eligible page contents via process_vm_readv)
//   - S20 Phase 0 (verify process_vm_readv in the grading environment)
//
// What this checks:
//   1. A traced/stopped child's anonymous page can be read out of its address
//      space by the parent in one call.
//   2. The bytes read match what the child wrote (content fidelity).
//   3. A large multi-page read in a single iovec works (batching path for S9.3).
//
// The child publishes its buffer address to the parent through a pipe.

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define NPAGES 4

int main(void) {
    long pg = sysconf(_SC_PAGESIZE);
    size_t region = (size_t)pg * NPAGES;

    int tube[2];
    if (pipe(tube) < 0) { perror("pipe"); return 2; }

    pid_t child = fork();
    if (child < 0) { perror("fork"); return 2; }

    if (child == 0) {
        close(tube[0]);
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        void *buf = mmap(NULL, region, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buf == MAP_FAILED) { perror("child mmap"); _exit(3); }
        // Deterministic fill: byte i = (i * 31 + 7) & 0xff
        unsigned char *p = buf;
        for (size_t i = 0; i < region; ++i) p[i] = (unsigned char)((i * 31 + 7) & 0xff);
        uint64_t addr = (uint64_t)(uintptr_t)buf;
        write(tube[1], &addr, sizeof addr);
        close(tube[1]);
        raise(SIGSTOP);
        _exit(0);
    }

    close(tube[1]);
    uint64_t caddr = 0;
    if (read(tube[0], &caddr, sizeof caddr) != (ssize_t)sizeof caddr) {
        fprintf(stderr, "did not receive child addr\n"); return 4;
    }
    close(tube[0]);

    int status;
    waitpid(child, &status, 0);  // stops at SIGSTOP

    unsigned char *got = malloc(region);
    struct iovec local  = { .iov_base = got,  .iov_len = region };
    struct iovec remote = { .iov_base = (void *)(uintptr_t)caddr, .iov_len = region };

    ssize_t n = process_vm_readv(child, &local, 1, &remote, 1, 0);
    printf("  process_vm_readv returned %zd (expected %zu)\n", n, region);

    int match = (n == (ssize_t)region);
    if (match) {
        for (size_t i = 0; i < region; ++i) {
            if (got[i] != (unsigned char)((i * 31 + 7) & 0xff)) { match = 0;
                printf("  mismatch at byte %zu: got %02x\n", i, got[i]); break; }
        }
    }

    ptrace(PTRACE_CONT, child, 0, 0);
    waitpid(child, &status, 0);
    free(got);

    printf("  content fidelity across %d pages: %s\n", NPAGES, match ? "ok" : "FAILED");
    printf("%s\n", match ? "PASS probe_process_vm_readv" : "FAIL probe_process_vm_readv");
    return match ? 0 : 1;
}

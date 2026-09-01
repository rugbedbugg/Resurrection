// Phase 0 probe: deterministic address-space layout via ADDR_NO_RANDOMIZE.
//
// Design draft v2 sections referenced:
//   - S5.2        (address space must be reconstructable at saved virtual
//                  addresses -> ASLR must be disabled for the target)
//   - S20 Phase 0 (confirm the ASLR-disable mechanism works for the target class)
//
// What this checks:
//   1. personality(ADDR_NO_RANDOMIZE) can be set unprivileged.
//   2. After setting it and re-exec'ing, the main executable, a fresh mmap,
//      and the stack land at identical addresses across independent runs.
//      (If they do not, saved absolute addresses would be meaningless.)
//
// Usage: run with no args (driver). It re-execs itself as `<self> child`
// twice and compares the reported layout.

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_child(const char *self, uint64_t out[3]) {
    int tube[2];
    if (pipe(tube) < 0) { perror("pipe"); return -1; }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        dup2(tube[1], 1);
        close(tube[0]); close(tube[1]);
        char *argv[] = { (char *)self, "child", NULL };
        execv(self, argv);
        perror("execv");
        _exit(127);
    }
    close(tube[1]);
    char buf[128] = {0};
    read(tube[0], buf, sizeof buf - 1);
    close(tube[0]);
    int status; waitpid(pid, &status, 0);
    return sscanf(buf, "%lx %lx %lx", &out[0], &out[1], &out[2]) == 3 ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "child") == 0) {
        int volatile stackvar = 0;
        void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        printf("%lx %lx %lx\n",
               (unsigned long)(uintptr_t)&main,
               (unsigned long)(uintptr_t)m,
               (unsigned long)(uintptr_t)&stackvar);
        fflush(stdout);
        return 0;
    }

    int persona = personality(0xffffffff);
    if (personality((unsigned long)persona | ADDR_NO_RANDOMIZE) < 0) {
        perror("personality(ADDR_NO_RANDOMIZE)");
        printf("FAIL probe_aslr\n");
        return 1;
    }
    printf("  personality(ADDR_NO_RANDOMIZE) set (was 0x%x)\n", persona);

    char self[4096];
    ssize_t sn = readlink("/proc/self/exe", self, sizeof self - 1);
    if (sn < 0) { perror("readlink"); return 1; }
    self[sn] = 0;

    uint64_t a[3], b[3];
    if (run_child(self, a) < 0 || run_child(self, b) < 0) {
        fprintf(stderr, "  child runs failed\n"); return 1;
    }

    printf("  run A:  main=0x%lx  mmap=0x%lx  stack=0x%lx\n",
           (unsigned long)a[0], (unsigned long)a[1], (unsigned long)a[2]);
    printf("  run B:  main=0x%lx  mmap=0x%lx  stack=0x%lx\n",
           (unsigned long)b[0], (unsigned long)b[1], (unsigned long)b[2]);

    int ok = (a[0] == b[0]) && (a[1] == b[1]) && (a[2] == b[2]);
    printf("  layout identical across runs: %s\n", ok ? "yes" : "NO");
    printf("%s\n", ok ? "PASS probe_aslr" : "FAIL probe_aslr");
    return ok ? 0 : 1;
}

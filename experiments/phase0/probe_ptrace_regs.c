// Probe: ptrace attach + general-purpose register capture/restore.
//
// Checkpoint stops the target under ptrace and captures GP registers + IP.
// Restore writes them back.
//
// What this checks:
//   1. We can spawn a target and stop it under ptrace (ptrace_scope=1 => must be
//      our own descendant; this probe deliberately uses PTRACE_TRACEME).
//   2. PTRACE_GETREGSET(NT_PRSTATUS) returns a plausible register file
//      (RIP inside an executable mapping, RSP inside the stack mapping).
//   3. PTRACE_SETREGSET round-trips: we can write the register file back.
//
// Build: see Makefile in this directory.  Exit code 0 == probe passed.

#define _GNU_SOURCE
#include <elf.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

static int addr_in_maps(pid_t pid, uint64_t addr, const char *want_perm_substr) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        uint64_t lo, hi;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
        if (addr >= lo && addr < hi) {
            found = want_perm_substr ? (strstr(perms, want_perm_substr) != NULL) : 1;
            break;
        }
    }
    fclose(f);
    return found;
}

int main(void) {
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 2; }

    if (child == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) < 0) { perror("PTRACE_TRACEME"); _exit(3); }
        // Stop ourselves so the parent can inspect a well-defined state.
        raise(SIGSTOP);
        // Trivial deterministic work after resume.
        volatile uint64_t acc = 0;
        for (uint64_t i = 0; i < 1000; ++i) acc += i;
        _exit((int)(acc % 7));
    }

    int status;
    if (waitpid(child, &status, 0) < 0) { perror("waitpid"); return 2; }
    if (!WIFSTOPPED(status)) { fprintf(stderr, "child did not stop\n"); return 4; }

    struct user_regs_struct regs;
    struct iovec iov = { .iov_base = &regs, .iov_len = sizeof regs };
    if (ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &iov) < 0) {
        perror("PTRACE_GETREGSET"); kill(child, SIGKILL); return 5;
    }

    uint64_t rip = regs.rip, rsp = regs.rsp, rax = regs.rax;
    printf("  RIP = 0x%016" PRIx64 "  RSP = 0x%016" PRIx64 "  RAX = 0x%016" PRIx64 "\n",
           rip, rsp, rax);

    int rip_ok = addr_in_maps(child, rip, "x");
    int rsp_ok = addr_in_maps(child, rsp, "w");
    printf("  RIP in executable mapping: %s\n", rip_ok == 1 ? "yes" : "NO");
    printf("  RSP in writable mapping:   %s\n", rsp_ok == 1 ? "yes" : "NO");

    // SETREGSET round-trip: write the same register file back unchanged.
    if (ptrace(PTRACE_SETREGSET, child, (void *)NT_PRSTATUS, &iov) < 0) {
        perror("PTRACE_SETREGSET"); kill(child, SIGKILL); return 6;
    }
    printf("  PTRACE_SETREGSET round-trip: ok\n");

    ptrace(PTRACE_CONT, child, 0, 0);
    waitpid(child, &status, 0);

    int pass = (rip_ok == 1) && (rsp_ok == 1);
    printf("%s\n", pass ? "PASS probe_ptrace_regs" : "FAIL probe_ptrace_regs");
    return pass ? 0 : 1;
}

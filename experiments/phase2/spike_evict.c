/* Spike 3: the self-eviction boundary.
 *
 * Restore must tear down the stub's whole address space and rebuild the image's
 * in its place. compel drives syscalls by poking a stub at ictx->syscall_ip and
 * pointing rip at it, so the question is how little needs to stay mapped.
 *
 * Strategy: reserve one scratch executable page at an address the image will
 * never claim, point syscall_ip there, then unmap everything else and see
 * whether we can still drive syscalls. If yes, no parasite blob is needed. */
#define _GNU_SOURCE
#include <compel/infect.h>
#include <compel/log.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <elf.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define SCRATCH_ADDR 0x700000000000UL   /* far from any normal layout */
#define SCRATCH_SIZE 8192UL             /* page 0: syscall stub, page 1: stack */

static int log_level = COMPEL_LOG_ERROR;
static void spike_log(unsigned int l, const char *f, va_list a)
{ if ((int)l <= log_level) vfprintf(stderr, f, a); }

static int spike_open_proc(int pid, int mode, const char *fmt, ...)
{
    char path[256], rest[192];
    va_list a; va_start(a, fmt); vsnprintf(rest, sizeof rest, fmt, a); va_end(a);
    snprintf(path, sizeof path, "/proc/%d/%s", pid, rest);
    return open(path, mode);
}

struct vma { unsigned long start, end; char perms[8], tag[64]; };

static int read_vmas(pid_t pid, struct vma *v, int max)
{
    char path[64]; snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char line[512]; int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        v[n].tag[0] = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %63s",
                   &v[n].start, &v[n].end, v[n].perms, v[n].tag) < 3) continue;
        n++;
    }
    fclose(f);
    return n;
}

static int syscall_ok(struct parasite_ctl *ctl)
{
    long ret = 0;
    if (compel_syscall(ctl, __NR_getpid, &ret, 0,0,0,0,0,0) < 0) return 0;
    return ret > 0;
}

int main(void)
{
    compel_log_init(spike_log, log_level);
    int tube[2]; if (pipe(tube) < 0) return 2;
    pid_t child = fork(); if (child < 0) return 2;
    if (child == 0) {
        close(tube[0]); char r = 1;
        if (write(tube[1], &r, 1) != 1) _exit(3);
        close(tube[1]);
        for (;;) pause();
    }
    close(tube[1]); char r; if (read(tube[0], &r, 1) != 1) return 3; close(tube[0]);

    int st;
    if (compel_stop_task(child) < 0) { fprintf(stderr, "FAIL stop\n"); goto out; }
    struct parasite_ctl *ctl = compel_prepare_noctx(child, false);
    if (!ctl) { fprintf(stderr, "FAIL prepare\n"); goto out; }

    struct infect_ctx *ictx = compel_infect_ctx(ctl);
    ictx->open_proc = spike_open_proc;
    ictx->task_size = compel_task_size();
    ictx->log_fd = -1;

    struct vma vmas[256];
    int nv = read_vmas(child, vmas, 256);
    printf("target starts with %d mappings\n", nv);
    for (int i = 0; i < nv && !ictx->syscall_ip; i++)
        if (vmas[i].perms[2] == 'x') ictx->syscall_ip = vmas[i].start;

    /* 1. Reserve the scratch page using the original executable mapping. */
    long ret = 0;
    if (compel_syscall(ctl, __NR_mmap, &ret, SCRATCH_ADDR, 4096,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) < 0 ||
        (unsigned long)ret != SCRATCH_ADDR) {
        fprintf(stderr, "FAIL: could not reserve scratch page (ret=0x%lx)\n", (unsigned long)ret);
        goto cure;
    }
    printf("scratch page reserved at 0x%lx\n", SCRATCH_ADDR);

    /* 2. process_vm_writev into that fresh mapping (unknown #3). */
    unsigned char pattern[4096];
    for (int i = 0; i < 4096; i++) pattern[i] = (unsigned char)(i & 0xff);
    struct iovec liov = { pattern, sizeof pattern };
    struct iovec riov = { (void *)SCRATCH_ADDR, sizeof pattern };
    ssize_t w = process_vm_writev(child, &liov, 1, &riov, 1, 0);
    unsigned char back[4096];
    struct iovec lb = { back, sizeof back };
    ssize_t rr = process_vm_readv(child, &lb, 1, &riov, 1, 0);
    printf("process_vm_writev into a fresh mapping: %s (wrote %zd, read %zd, match %s)\n",
           w == 4096 ? "ok" : "FAILED", w, rr,
           (rr == 4096 && memcmp(pattern, back, 4096) == 0) ? "yes" : "NO");

    /* 3. Park the target's own registers inside the scratch page BEFORE evicting.
     *
     * compel_syscall saves the target regs, runs the syscall, then restores
     * them. The stub's rip still points into libc's pause(); the moment libc is
     * unmapped, restoring rip there faults and compel reports an unexpected
     * task interruption. Parking rip/rsp somewhere that survives the sweep is
     * what makes full eviction possible. For a real restore this costs nothing,
     * because the saved register state is going to be overwritten anyway. */
    {
        struct user_regs_struct regs;
        struct iovec riov2 = { &regs, sizeof regs };
        if (ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &riov2) != 0) {
            fprintf(stderr, "FAIL: GETREGSET\n"); goto cure;
        }
        regs.rip = SCRATCH_ADDR;
        regs.rsp = SCRATCH_ADDR + SCRATCH_SIZE - 256;
        riov2.iov_len = sizeof regs;
        if (ptrace(PTRACE_SETREGSET, child, (void *)NT_PRSTATUS, &riov2) != 0) {
            fprintf(stderr, "FAIL: SETREGSET\n"); goto cure;
        }
        printf("parked target rip/rsp inside the scratch page\n");
    }

    ictx->syscall_ip = SCRATCH_ADDR;
    compel_set_leader_ip(ctl, SCRATCH_ADDR);
    printf("compel_set_leader_ip -> scratch page\n");
    if (!syscall_ok(ctl)) { fprintf(stderr, "FAIL: syscall via scratch page\n"); goto cure; }
    printf("syscall via scratch page: ok\n");

    /* Which region holds TLS? glibc parks thread-local storage there and
     * fs_base points into it. */
    unsigned long fs_base = 0;
    {
        struct user_regs_struct rg;
        struct iovec ri = { &rg, sizeof rg };
        if (ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &ri) == 0) fs_base = rg.fs_base;
    }
    printf("target fs_base = 0x%lx\n", fs_base);

    nv = read_vmas(child, vmas, 256);
    for (int i = 0; i < nv; i++)
        if (fs_base >= vmas[i].start && fs_base < vmas[i].end)
            printf("fs_base lives in %lx-%lx %s %s  <-- TLS\n",
                   vmas[i].start, vmas[i].end, vmas[i].perms, vmas[i].tag);

    int unmapped = 0, skipped = 0, broke_after = -1;
    for (int i = 0; i < nv; i++) {
        if (vmas[i].start == SCRATCH_ADDR) continue;
        if (strstr(vmas[i].tag, "vsyscall")) { skipped++; continue; }  /* cannot be unmapped */
        if (getenv("SKIP_TLS") && fs_base >= vmas[i].start && fs_base < vmas[i].end) {
            printf("  SKIP  %lx-%lx (TLS)\n", vmas[i].start, vmas[i].end);
            skipped++; continue;
        }
        long ur = 0;
        printf("  unmap #%2d %lx-%lx %-5s %s ... ", unmapped + 1,
               vmas[i].start, vmas[i].end, vmas[i].perms, vmas[i].tag);
        fflush(stdout);
        if (compel_syscall(ctl, __NR_munmap, &ur,
                           vmas[i].start, vmas[i].end - vmas[i].start, 0,0,0,0) < 0) {
            printf("SYSCALL FAILED\n");
            broke_after = unmapped + 1; break;
        }
        unmapped++;
        if (!syscall_ok(ctl)) { printf("unmapped, but next syscall FAILED\n"); broke_after = unmapped; break; }
        printf("ok\n");
    }

    int left = read_vmas(child, vmas, 256);
    printf("unmapped %d mappings (%d skipped), %d remain\n", unmapped, skipped, left);
    for (int i = 0; i < left; i++)
        printf("   remaining: %lx-%lx %s %s\n", vmas[i].start, vmas[i].end, vmas[i].perms, vmas[i].tag);

    if (broke_after < 0) {
        printf("RESULT: syscalls STILL WORK with the address space evicted.\n");
        printf("        One reserved executable page is sufficient. No parasite blob needed.\n");
    } else {
        printf("RESULT: syscalls broke after unmap #%d. A parasite blob IS needed.\n", broke_after);
    }

cure:
    if (compel_cure(ctl) < 0) fprintf(stderr, "(cure failed, expected after eviction)\n");
out:
    kill(child, SIGKILL); waitpid(child, &st, 0);
    return 0;
}

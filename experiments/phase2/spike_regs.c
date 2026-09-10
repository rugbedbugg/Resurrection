/* Phase 2 spike 4: can the reserve be made fully self-contained, and can control
 * be transferred with PTRACE_SETREGSET alone?
 *
 * Spike 3 showed the TLS block must survive the eviction sweep, because fs_base
 * points into it. That is only acceptable if TLS can be MOVED into our reserved
 * area first -- otherwise a checkpoint image whose mappings collide with the
 * stub's TLS could not be restored. This probe relocates TLS, evicts everything
 * else, and then hands control to fresh code via PTRACE_SETREGSET + detach. */
#define _GNU_SOURCE
#include <asm/prctl.h>
#include <compel/infect.h>
#include <compel/log.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define RESERVE_ADDR 0x700000000000UL
#define RESERVE_SIZE (3 * 4096UL)   /* code | stack | tls */
#define TLS_OFF      (2 * 4096UL)

static int log_level = COMPEL_LOG_ERROR;
static void slog(unsigned int l, const char *f, va_list a)
{ if ((int)l <= log_level) vfprintf(stderr, f, a); }

static int sopen(int pid, int mode, const char *fmt, ...)
{
    char p[256], r[192]; va_list a;
    va_start(a, fmt); vsnprintf(r, sizeof r, fmt, a); va_end(a);
    snprintf(p, sizeof p, "/proc/%d/%s", pid, r);
    return open(p, mode);
}

struct vma { unsigned long start, end; char perms[8], tag[64]; };
static int read_vmas(pid_t pid, struct vma *v, int max)
{
    char p[64]; snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    char l[512]; int n = 0;
    while (n < max && fgets(l, sizeof l, f)) {
        v[n].tag[0] = 0;
        if (sscanf(l, "%lx-%lx %7s %*s %*s %*s %63s",
                   &v[n].start, &v[n].end, v[n].perms, v[n].tag) < 3) continue;
        n++;
    }
    fclose(f); return n;
}

int main(void)
{
    compel_log_init(slog, log_level);
    int tube[2]; if (pipe(tube) < 0) return 2;
    pid_t child = fork(); if (child < 0) return 2;
    if (child == 0) {
        close(tube[0]); char r = 1;
        if (write(tube[1], &r, 1) != 1) _exit(3);
        close(tube[1]);
        for (;;) pause();
    }
    close(tube[1]); char rd; if (read(tube[0], &rd, 1) != 1) return 3; close(tube[0]);

    int st;
    if (compel_stop_task(child) < 0) { fprintf(stderr, "FAIL stop\n"); goto out; }
    struct parasite_ctl *ctl = compel_prepare_noctx(child, false);
    if (!ctl) { fprintf(stderr, "FAIL prepare\n"); goto out; }
    struct infect_ctx *ictx = compel_infect_ctx(ctl);
    ictx->open_proc = sopen;
    ictx->task_size = compel_task_size();
    ictx->log_fd = -1;

    struct vma v[256];
    int nv = read_vmas(child, v, 256);
    for (int i = 0; i < nv && !ictx->syscall_ip; i++)
        if (v[i].perms[2] == 'x') ictx->syscall_ip = v[i].start;

    long ret = 0;
    if (compel_syscall(ctl, __NR_mmap, &ret, RESERVE_ADDR, RESERVE_SIZE,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) < 0 ||
        (unsigned long)ret != RESERVE_ADDR) {
        fprintf(stderr, "FAIL reserve\n"); goto cure;
    }
    printf("reserve at 0x%lx (%lu bytes)\n", RESERVE_ADDR, RESERVE_SIZE);
    ictx->syscall_ip = RESERVE_ADDR;
    compel_set_leader_ip(ctl, RESERVE_ADDR);

    /* Relocate TLS into the reserve so the original block is expendable. */
    if (compel_syscall(ctl, __NR_arch_prctl, &ret, ARCH_SET_FS,
                       RESERVE_ADDR + TLS_OFF, 0,0,0,0) < 0 || ret != 0) {
        fprintf(stderr, "FAIL: arch_prctl(ARCH_SET_FS) ret=%ld\n", ret); goto cure;
    }
    printf("arch_prctl(ARCH_SET_FS) returned 0\n");

    /* Does the relocation actually stick across a compel_syscall? compel saves
     * and restores the target's registers around each injected call, and
     * user_regs_struct carries fs_base, so a cached copy would silently undo it. */
    {
        struct user_regs_struct rg;
        struct iovec ri2 = { &rg, sizeof rg };
        ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &ri2);
        printf("  fs_base right after arch_prctl : 0x%llx\n", (unsigned long long)rg.fs_base);
        long dummy = 0;
        if (compel_syscall(ctl, __NR_getpid, &dummy, 0,0,0,0,0,0) < 0)
            fprintf(stderr, "  (getpid failed)\n");
        ri2.iov_len = sizeof rg;
        ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &ri2);
        printf("  fs_base after one compel_syscall: 0x%llx  %s\n",
               (unsigned long long)rg.fs_base,
               rg.fs_base == RESERVE_ADDR + TLS_OFF ? "(stuck)" : "(REVERTED by compel)");
    }

    /* Evict everything except the reserve and the unremovable [vsyscall]. */
    /* FINDING: compel restores its saved regs after every injected syscall, so
     * arch_prctl(ARCH_SET_FS) does not stick and the stub's original TLS block
     * cannot be relocated. It must stay mapped for as long as compel_syscall is
     * used. Identify it and spare it. */
    unsigned long fs_base = 0;
    {
        struct user_regs_struct rg;
        struct iovec ri3 = { &rg, sizeof rg };
        if (ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &ri3) == 0) fs_base = rg.fs_base;
    }

    nv = read_vmas(child, v, 256);
    int gone = 0;
    for (int i = 0; i < nv; i++) {
        if (v[i].start == RESERVE_ADDR) continue;
        if (strstr(v[i].tag, "vsyscall")) continue;
        if (fs_base >= v[i].start && fs_base < v[i].end) {
            printf("   sparing TLS %lx-%lx (load-bearing for compel)\n", v[i].start, v[i].end);
            continue;
        }
        long ur = 0;
        if (compel_syscall(ctl, __NR_munmap, &ur, v[i].start, v[i].end - v[i].start, 0,0,0,0) < 0) {
            fprintf(stderr, "FAIL: unmap %lx-%lx broke compel\n", v[i].start, v[i].end);
            goto cure;
        }
        gone++;
    }
    int left = read_vmas(child, v, 256);
    printf("evicted %d mappings, %d remain:\n", gone, left);
    for (int i = 0; i < left; i++)
        printf("   %lx-%lx %s %s\n", v[i].start, v[i].end, v[i].perms, v[i].tag);

    long pid_ret = 0;
    if (compel_syscall(ctl, __NR_getpid, &pid_ret, 0,0,0,0,0,0) < 0 || pid_ret != child) {
        fprintf(stderr, "FAIL: syscalls dead after full eviction\n"); goto cure;
    }
    printf("syscalls survive a FULL eviction (TLS relocated): ok\n");

    /* Control transfer: write exit_group(42) and jump to it with SETREGSET. */
    static const unsigned char code[] = {
        0xb8, 0xe7, 0x00, 0x00, 0x00,   /* mov eax, 231 (exit_group) */
        0xbf, 0x2a, 0x00, 0x00, 0x00,   /* mov edi, 42               */
        0x0f, 0x05                      /* syscall                   */
    };
    struct iovec li = { (void *)code, sizeof code };
    struct iovec ri = { (void *)RESERVE_ADDR, sizeof code };
    if (process_vm_writev(child, &li, 1, &ri, 1, 0) != (ssize_t)sizeof code) {
        fprintf(stderr, "FAIL: writing code\n"); goto cure;
    }

    struct user_regs_struct regs;
    struct iovec rv = { &regs, sizeof regs };
    if (ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &rv) != 0) goto cure;
    regs.rip = RESERVE_ADDR;
    regs.rsp = RESERVE_ADDR + 2 * 4096 - 256;
    /* The stub was blocked in pause(), a restartable syscall. On resume the
     * kernel rewinds rip by 2 to re-execute the syscall instruction, which
     * would land just below the reserve and fault. orig_rax = -1 tells the
     * kernel there is no syscall in progress to restart. */
    regs.orig_rax = (unsigned long long)-1;
    rv.iov_len = sizeof regs;
    if (ptrace(PTRACE_SETREGSET, child, (void *)NT_PRSTATUS, &rv) != 0) {
        fprintf(stderr, "FAIL: SETREGSET\n"); goto cure;
    }
    /* Verify what actually landed before blaming the transfer mechanism. */
    {
        unsigned char back[12] = {0};
        struct iovec lb = { back, sizeof back };
        struct iovec rb = { (void *)RESERVE_ADDR, sizeof back };
        ssize_t got = process_vm_readv(child, &lb, 1, &rb, 1, 0);
        printf("  code readback: %zd bytes, match=%s\n", got,
               (got == (ssize_t)sizeof code && memcmp(back, code, sizeof code) == 0) ? "yes" : "NO");
        struct user_regs_struct chk;
        struct iovec cv = { &chk, sizeof chk };
        ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &cv);
        printf("  regs before detach: rip=0x%llx rsp=0x%llx (want rip=0x%lx)\n",
               (unsigned long long)chk.rip, (unsigned long long)chk.rsp, RESERVE_ADDR);
    }

    /* PTRACE_DETACH leaves a SEIZE-d, INTERRUPT-ed task in a state where the
     * resume did not take; PTRACE_CONT resumes it properly. arctl stays the
     * tracer either way, so this costs nothing. */
    const char *how = getenv("USE_DETACH") ? "DETACH" : "CONT";
    int req = getenv("USE_DETACH") ? PTRACE_DETACH : PTRACE_CONT;
    if (ptrace(req, child, 0, 0) != 0) {
        fprintf(stderr, "FAIL: %s: %s\n", how, strerror(errno)); goto cure;
    }
    printf("  resumed with PTRACE_%s\n", how);

    if (waitpid(child, &st, 0) == child && WIFEXITED(st) && WEXITSTATUS(st) == 42) {
        printf("control transfer via SETREGSET + PTRACE_%s: ok (exit 42)\n", how);
        printf("RESULT: no parasite blob, no sigreturn frame needed for eager restore\n");
        return 0;
    }
    fprintf(stderr, "FAIL: transferred control but status was 0x%x\n", st);
    return 1;

cure:
    if (compel_cure(ctl) < 0) fprintf(stderr, "(cure failed)\n");
out:
    kill(child, SIGKILL); waitpid(child, &st, 0);
    return 1;
}

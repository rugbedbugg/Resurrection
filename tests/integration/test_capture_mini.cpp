#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "adaptive_restore/image/image_reader.hpp"

using namespace adaptive_restore::image;

// workload_mini is a fixed-seed LCG over 4096 words XOR-folded into `sum`, so
// with ASLR off its output is a constant. This is the Phase 1 baseline digest
// that Phase 2 restores must reproduce.
static const char* kBaseline = "sum=11791276921336954880\n";

static int run(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    return (rc != -1 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
}

static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main() {
    const std::string base = "/tmp/ar_mini_" + std::to_string(::getpid());
    const std::string img = base + ".arimg";

    const std::string cmd = std::string(ARCTL_PATH) + " checkpoint --verify --output " + img +
                            " -- " + MINI_PATH + " > " + base + ".log 2>&1";
    assert(run(cmd) == 0 && "arctl checkpoint --verify must succeed with 0 mismatches");

    auto ir = ImageReader::open(img);
    ir.verify();

    const auto regs = ir.registers();
    const auto maps = ir.mappings();
    bool rip_exec = false, rsp_stack = false, have_heap = false, have_libc_text = false;
    bool have_kernel_special = false;
    for (const auto& m : maps) {
        if (regs.rip >= m.start_vaddr && regs.rip < m.end_vaddr && (m.flags & MappingFlag::Exec))
            rip_exec = true;
        if (regs.rsp >= m.start_vaddr && regs.rsp < m.end_vaddr && (m.flags & MappingFlag::Write))
            rsp_stack = true;
        if (m.path.empty() && (m.flags & MappingFlag::Write) &&
            (m.flags & MappingFlag::ContentsSaved))
            have_heap = true;
        if (m.path.find("libc") != std::string::npos && (m.flags & MappingFlag::Exec))
            have_libc_text = true;
        if (m.flags & MappingFlag::KernelSpecial) {
            have_kernel_special = true;
            assert((m.flags & MappingFlag::ContentsSaved) == 0 &&
                   "kernel mappings must never carry contents");
        }
    }
    assert(rip_exec && "rip must land in an executable mapping");
    assert(rsp_stack && "rsp must land in a writable mapping");
    assert(have_heap && "an anonymous writable mapping must be captured");
    assert(have_libc_text && "libc text must be captured");
    assert(have_kernel_special && "vdso/vvar must be recorded");
    assert(!ir.regions().empty());

    // Every region indexes a non-empty, in-bounds slice of the page index, and
    // every raw page re-checksums against what the image stored.
    const auto pidx = ir.page_index();
    for (const auto& r : ir.regions()) {
        assert(r.page_index_hi > r.page_index_lo);
        assert(r.page_index_hi <= pidx.size());
    }
    std::uint64_t raw = 0, zero = 0, from_file = 0;
    for (const auto& p : pidx) {
        if (p.encoding == PageEncoding::FromFile) {
            // Content lives in the backing file, so page_bytes cannot serve it.
            ++from_file;
            assert(p.store_len == 0);
            continue;
        }
        const auto bytes = ir.page_bytes(p);  // throws on CRC mismatch
        assert(bytes.size() == ir.header().page_size);
        if (p.encoding == PageEncoding::Raw) ++raw; else ++zero;
    }
    assert(raw > 0 && zero > 0 && "the workload should yield both raw and zero pages");
    assert(from_file > raw && "most of a dynamically linked image is unmodified library text");

    // File-page referencing is what keeps the image small; guard it here so a
    // regression shows up as a failing test rather than a fatter image.
    {
        std::ifstream f(img, std::ios::binary | std::ios::ate);
        const auto size = static_cast<std::uint64_t>(f.tellg());
        const std::uint64_t mapped = pidx.size() * ir.header().page_size;
        assert(size < mapped / 4 &&
               "a referenced image must be far smaller than the address space it describes");
    }

    // The target's own stdout was redirected beside the image; checkpointing
    // must not perturb the run, so it has to match the uninterrupted baseline.
    assert(slurp(img + ".stdout") == kBaseline && "checkpointed run must match the baseline");

    // The captured fd table must include the redirected stdout as a real file.
    bool saw_stdout_fd = false;
    for (const auto& f : ir.fds())
        if (f.path == img + ".stdout") saw_stdout_fd = true;
    assert(saw_stdout_fd);

    const std::string ins =
        std::string(ARCTL_PATH) + " inspect --image " + img + " > " + base + ".txt";
    assert(run(ins) == 0);
    assert(slurp(base + ".txt").find("mappings (") != std::string::npos);

    for (const char* suffix : {".arimg", ".arimg.stdout", ".arimg.stderr", ".log", ".txt"})
        ::unlink((base + suffix).c_str());

    std::puts("test_capture_mini: ok");
    return 0;
}

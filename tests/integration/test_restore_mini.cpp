// Phase 2 exit gate: a checkpointed deterministic program resumes from the
// saved instruction and produces output identical to an uninterrupted run
// (design-draft-v2 §20 Phase 2, §25.2, §25.3).
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

// Fixed-seed LCG over 4096 words, ASLR off, so this is a constant. Established
// in Phase 1 and asserted there too; a restore must reproduce it exactly.
static const char* kBaseline = "sum=11791276921336954880\n";

static int run(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    return (rc != -1 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
}

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void truncate_file(const std::string& p) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
}

int main() {
    const std::string base = "/tmp/ar_restore_mini_" + std::to_string(::getpid());
    const std::string img = base + ".arimg";
    const std::string out = img + ".stdout";

    // --- checkpoint ---------------------------------------------------------
    assert(run(std::string(ARCTL_PATH) + " checkpoint --verify --output " + img + " -- " +
               MINI_PATH + " > " + base + ".log 2>&1") == 0);
    // The target ran to completion during checkpoint, so this is the baseline.
    assert(slurp(out) == kBaseline && "the checkpointed run must match the baseline");

    // --- restore, twice, from the same image (§25.2 repeated restore) -------
    for (int attempt = 0; attempt < 2; ++attempt) {
        // Clear the target's stdout first, or a stale value from the previous
        // run would make this pass without the restore writing anything.
        truncate_file(out);
        assert(slurp(out).empty());

        const int rc = run(std::string(ARCTL_PATH) + " restore --image " + img +
                           " --policy eager --verify > " + base + ".restore" +
                           std::to_string(attempt) + " 2>&1");
        assert(rc == 0 && "arctl restore --policy eager --verify must succeed");

        const std::string log = slurp(base + ".restore" + std::to_string(attempt));
        assert(log.find("0 mismatches") != std::string::npos &&
               "every restored page must verify against the image");
        assert(log.find("target     exited 0") != std::string::npos &&
               "the restored process must run to a clean exit");

        assert(slurp(out) == kBaseline &&
               "the restored process must reproduce the uninterrupted output exactly");
    }

    // --- the image is unchanged by being restored ---------------------------
    auto ir = ImageReader::open(img);
    ir.verify();
    assert(ir.registers().rip != 0);

    for (const char* suffix : {".arimg", ".arimg.stdout", ".arimg.stderr", ".log", ".restore0",
                               ".restore1"})
        ::unlink((base + suffix).c_str());

    std::puts("test_restore_mini: ok");
    return 0;
}

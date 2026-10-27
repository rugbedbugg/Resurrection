#pragma once

// compel's <common/compiler.h> defines min() and max() as function-like macros.
// Any libstdc++ header parsed afterwards breaks in confusing ways: std::min in
// <string_view> expands into nonsense and the errors point at the standard
// library rather than at compel. Include compel ONLY through this header.

// compel ships no extern "C" guards either, so its symbols would be mangled
// and fail to link from C++. Both problems are contained here.
extern "C" {
#include <compel/infect.h>
#include <compel/log.h>
}

#undef min
#undef max

// criu 4.2 gave the prepare calls a handle_rseq argument. Nothing here manages
// rseq state, so the only value this project passes is false. criu 4.1 and
// earlier take no argument at all. CMake probes the header and defines
// AR_COMPEL_PREPARE_TAKES_RSEQ when the argument exists.
#ifdef AR_COMPEL_PREPARE_TAKES_RSEQ
inline struct parasite_ctl* compel_prepare_noctx_compat(int pid) {
    return compel_prepare_noctx(pid, false);
}
#else
inline struct parasite_ctl* compel_prepare_noctx_compat(int pid) {
    return compel_prepare_noctx(pid);
}
#endif

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

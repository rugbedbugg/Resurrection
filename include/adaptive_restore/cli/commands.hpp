#pragma once
#include <span>
#include <string_view>

namespace adaptive_restore::cli {

// Each takes the argument list following the subcommand name and returns the
// process exit status.
int cmd_checkpoint(std::span<const std::string_view> args);
int cmd_inspect(std::span<const std::string_view> args);
int cmd_benchmark(std::span<const std::string_view> args);

#ifdef AR_HAVE_COMPEL
// Only built when compel is available; see cmake/FindCompel.cmake.
int cmd_restore(std::span<const std::string_view> args);
#endif

#ifdef AR_HAVE_SQLITE
// Only built when SQLite is available; see cmake/FindSQLite3Opt.cmake.
int cmd_profile(std::span<const std::string_view> args);
#endif

}  // namespace adaptive_restore::cli

#pragma once

#include <optional>
#include <string>

namespace forandsim {

// e.g. "Windows x64", "macOS arm64", "Linux x86_64" - recorded in the report's
// tool-provenance section so an acquisition can always be traced back to what
// it ran on.
std::string platformDescription();

// SHA-256 of the currently-running executable's own file on disk, so a report
// can be traced back to the exact binary that produced it (beyond just the
// version string, which doesn't change between rebuilds). std::nullopt if the
// executable's own path couldn't be resolved or read.
std::optional<std::string> currentExecutableSha256();

} // namespace forandsim

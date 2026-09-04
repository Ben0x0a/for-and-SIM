#pragma once

#include <chrono>
#include <string>

namespace forandsim {

// ISO-8601 UTC timestamp, e.g. "2026-09-04T16:00:00Z".
std::string isoTimestamp(std::chrono::system_clock::time_point tp);

} // namespace forandsim

#include "report_common.h"

#include <cstdio>
#include <ctime>

namespace forandsim::output {

std::string isoTimestamp(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tmBuf{};
#ifdef _WIN32
    gmtime_s(&tmBuf, &t);
#else
    gmtime_r(&t, &tmBuf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmBuf);
    return buf;
}

std::string atrHex(const std::vector<uint8_t>& atr) {
    std::string hex;
    char b[4];
    for (uint8_t byte : atr) {
        std::snprintf(b, sizeof(b), "%02X ", byte);
        hex += b;
    }
    if (!hex.empty()) hex.pop_back();
    return hex;
}

const char* chvResultString(ChvResult r) {
    switch (r) {
        case ChvResult::Correct: return "correct";
        case ChvResult::NotInitialized: return "not_initialized";
        case ChvResult::Incorrect: return "incorrect";
        case ChvResult::Blocked: return "blocked";
        default: return "error";
    }
}

} // namespace forandsim::output

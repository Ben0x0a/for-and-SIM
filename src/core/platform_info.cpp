#include "platform_info.h"

#include <fstream>
#include <vector>

#include "hashing.h"

#ifdef _WIN32
#include <windows.h>
#elif __APPLE__
#include <mach-o/dyld.h>
#include <climits>
#else
#include <climits>
#include <unistd.h>
#endif

namespace forandsim {

namespace {

const char* archName() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown-arch";
#endif
}

std::optional<std::string> currentExecutablePath() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return std::nullopt;
    return std::string(buf, n);
#elif __APPLE__
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return std::nullopt;
    return std::string(buf);
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::nullopt;
    return std::string(buf, n);
#endif
}

} // namespace

std::string platformDescription() {
#ifdef _WIN32
    std::string os = "Windows";
#elif __APPLE__
    std::string os = "macOS";
#else
    std::string os = "Linux";
#endif
    return os + " " + archName();
}

std::optional<std::string> currentExecutableSha256() {
    auto path = currentExecutablePath();
    if (!path) return std::nullopt;

    std::ifstream f(*path, std::ios::binary);
    if (!f) return std::nullopt;

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return sha256Hex(bytes);
}

} // namespace forandsim

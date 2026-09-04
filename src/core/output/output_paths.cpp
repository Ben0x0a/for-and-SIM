#include "output_paths.h"

#include <cctype>
#include <stdexcept>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace forandsim::output {

namespace {

std::string sanitize(const std::string& raw) {
    std::string name = raw.empty() ? "acquisition" : raw;
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') {
            out += c;
        } else {
            out += '_';
        }
    }
    return out.empty() ? "acquisition" : out;
}

} // namespace

OutputPaths computeOutputPaths(const std::string& outputDir, const std::string& caseIdentifier) {
    std::string safeName = sanitize(caseIdentifier);

    OutputPaths paths;
    paths.caseDir = outputDir + "/" + safeName;
    paths.zipFileName = safeName + ".zip";
    paths.zipPath = paths.caseDir + "/" + paths.zipFileName;
    paths.htmlPath = paths.caseDir + "/" + safeName + ".html";
    return paths;
}

bool pathExists(const std::string& path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

void ensureCaseDir(const std::string& caseDir) {
#ifdef _WIN32
    if (_mkdir(caseDir.c_str()) != 0 && !pathExists(caseDir)) {
        throw std::runtime_error("failed to create output directory '" + caseDir + "'");
    }
#else
    if (mkdir(caseDir.c_str(), 0755) != 0 && !pathExists(caseDir)) {
        throw std::runtime_error("failed to create output directory '" + caseDir + "'");
    }
#endif
}

} // namespace forandsim::output

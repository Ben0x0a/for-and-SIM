#pragma once

#include <string>

namespace forandsim::output {

struct OutputPaths {
    std::string caseDir;      // <outputDir>/<sanitized case id>
    std::string zipFileName;  // "<case>.zip" (no directory)
    std::string zipPath;      // caseDir + "/" + zipFileName
    std::string htmlPath;     // caseDir + "/<case>.html"
};

// Sanitizes caseIdentifier into a filesystem-safe folder/file name and lays
// out where results for this acquisition go: a dedicated subfolder inside
// outputDir, so multiple acquisitions in the same output directory don't mix
// their files together.
OutputPaths computeOutputPaths(const std::string& outputDir, const std::string& caseIdentifier);

bool pathExists(const std::string& path);

// Creates `caseDir` if it doesn't already exist (single level; `outputDir`
// itself must already exist). Throws std::runtime_error on failure.
void ensureCaseDir(const std::string& caseDir);

} // namespace forandsim::output

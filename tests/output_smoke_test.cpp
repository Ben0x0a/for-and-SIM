// Builds a synthetic AcquisitionResult (no hardware involved) and exercises
// the zip/HTML output layer end to end. Exits non-zero on any check failure.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "acquisition_engine.h"
#include "html_report.h"
#include "zip_writer.h"

using namespace forandsim;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

int main() {
    AcquisitionResult result;
    result.caseMetadata.caseIdentifier = "CASE-2026-001";
    result.caseMetadata.pieceNumber = "P1";
    result.caseMetadata.operatorName = "J. Examiner";
    result.caseMetadata.examinerNotes = "Synthetic smoke-test acquisition";
    result.startedAt = std::chrono::system_clock::now();
    result.finishedAt = result.startedAt + std::chrono::seconds(5);
    result.workstationHostname = "test-bench";
    result.workstationUser = "tester";
    result.readerName = "Fake Reader 0";
    result.atr = {0x3B, 0x9F, 0x94, 0x80, 0x1F};
    result.iccid = "8933010000000000001";
    result.mode = AcquisitionMode::FullDump;
    result.pinAttempted = true;
    result.pinResult = ChvResult::Correct;
    result.caseMetadata.authorizationConfirmed = true;
    result.verifyRequested = true;
    result.verifyPerformed = true;

    ExtractedFile iccidFile;
    iccidFile.path = "MF/ICCID";
    iccidFile.fileId = 0x2FE2;
    iccidFile.name = "ICCID";
    iccidFile.structure = apdu::FileStructure::Transparent;
    iccidFile.rawData = {0x98, 0x33, 0x01, 0x00};
    iccidFile.sha256 = "deadbeef";
    iccidFile.interpretedValue = result.iccid;
    result.files.push_back(iccidFile);

    ExtractedFile adnFile;
    adnFile.path = "MF/DF_TELECOM/ADN";
    adnFile.fileId = 0x6F3A;
    adnFile.name = "ADN";
    adnFile.structure = apdu::FileStructure::LinearFixed;
    adnFile.records = {{0x41, 0x42, 0x43}, {0x44, 0x45, 0x46}};
    adnFile.sha256 = "cafef00d";
    result.files.push_back(adnFile);

    result.log = {"Selecting MF", "Reading ICCID", "PIN verified", "Walk complete"};

    const std::string zipPath = "smoke_test_output.zip";
    const std::string htmlPath = "smoke_test_output.html";
    const std::string zipFileName = "smoke_test_output.zip";

    output::EvidenceZipResult zipInfo = output::writeEvidenceZip(result, zipPath);
    check(zipInfo.sha256.size() == 64, "zip result reports a 64-char SHA-256");

    std::ifstream zipCheck(zipPath, std::ios::binary);
    check(zipCheck.good(), "zip file was created");
    char magic[4] = {};
    zipCheck.read(magic, 4);
    check(magic[0] == 'P' && magic[1] == 'K', "zip file starts with PK local-file-header magic");

    check(std::ifstream(zipInfo.sidecarPath).good(), "zip sidecar hash file was created");

    output::writeHtmlReport(result, htmlPath, zipFileName, zipInfo);

    std::string html = readFile(htmlPath);
    check(!html.empty(), "html report was created");
    check(html.find("CASE-2026-001") != std::string::npos, "html report contains case identifier");
    check(html.find("8933010000000000001") != std::string::npos, "html report contains ICCID");
    check(html.find("MF/DF_TELECOM/ADN") != std::string::npos, "html report lists ADN file path");
    check(html.find(zipInfo.sha256) != std::string::npos, "html report contains the zip's SHA-256");
    check(html.find("For&SIM") != std::string::npos, "html report contains tool name");
    check(html.find("for-and-SIM") != std::string::npos, "html report contains repo link");

    std::remove(zipPath.c_str());
    std::remove(zipInfo.sidecarPath.c_str());
    std::remove(htmlPath.c_str());

    if (failures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("All output smoke-test checks passed.\n");
    return 0;
}

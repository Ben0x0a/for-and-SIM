// Builds a synthetic AcquisitionResult (no hardware involved) and exercises
// the zip/HTML output layer end to end. Exits non-zero on any check failure.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "acquisition_engine.h"
#include "html_report.h"
#include "miniz.h"
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

    ExtractedFile msisdnFile;
    msisdnFile.path = "MF/DF_TELECOM/MSISDN";
    msisdnFile.fileId = 0x6F40;
    msisdnFile.name = "MSISDN";
    msisdnFile.structure = apdu::FileStructure::LinearFixed;
    msisdnFile.records = {{0x51, 0x55, 0x21, 0x43, 0x65, 0xF7}}; // BCD digits only, for size
    msisdnFile.sha256 = "aabbccdd";
    msisdnFile.interpretedValue = "+15551234567"; // as decodeMsisdnRecord would produce
    result.files.push_back(msisdnFile);

    ExtractedFile spnFile;
    spnFile.path = "MF/DF_GSM/SPN";
    spnFile.fileId = 0x6F46;
    spnFile.name = "SPN";
    spnFile.structure = apdu::FileStructure::Transparent;
    spnFile.rawData = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // never provisioned by the carrier
    spnFile.sha256 = "00000000";
    result.files.push_back(spnFile);

    ExtractedFile kcFile;
    kcFile.path = "MF/DF_GSM/Kc";
    kcFile.fileId = 0x6F20;
    kcFile.name = "Kc";
    kcFile.structure = apdu::FileStructure::Transparent;
    kcFile.rawData = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    kcFile.sha256 = "feedface";
    kcFile.sensitive = true;
    result.files.push_back(kcFile);

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

    // Security-critical check: cryptographic key material (Kc) must never be
    // written into the evidence zip, even though it's listed in the manifest.
    {
        mz_zip_archive reader;
        memset(&reader, 0, sizeof(reader));
        check(mz_zip_reader_init_file(&reader, zipPath.c_str(), 0), "zip reopened for content check");
        int iccidIndex = mz_zip_reader_locate_file(&reader, "files/MF/ICCID.bin", nullptr, 0);
        check(iccidIndex >= 0, "non-sensitive file (ICCID) content IS present in the zip");
        int kcIndex = mz_zip_reader_locate_file(&reader, "files/MF/DF_GSM/Kc.bin", nullptr, 0);
        check(kcIndex < 0, "sensitive file (Kc) content is NOT present in the zip");
        mz_zip_reader_end(&reader);
    }

    output::writeHtmlReport(result, htmlPath, zipFileName, zipInfo);

    std::string html = readFile(htmlPath);
    check(!html.empty(), "html report was created");
    check(html.find("CASE-2026-001") != std::string::npos, "html report contains case identifier");
    check(html.find("8933010000000000001") != std::string::npos, "html report contains ICCID");
    check(html.find("MF/DF_TELECOM/ADN") != std::string::npos, "html report lists ADN file path");
    check(html.find(zipInfo.sha256) != std::string::npos, "html report contains the zip's SHA-256");
    check(html.find("For&SIM") != std::string::npos, "html report contains tool name");
    check(html.find("for-and-SIM") != std::string::npos, "html report contains repo link");
    check(html.find("Extraction results") != std::string::npos, "html report has Extraction results section");
    check(html.find("Acquisition results") != std::string::npos, "html report has Acquisition results section");
    check(html.find("Container information") != std::string::npos, "html report has Container information subsection");
    check(html.find("International Mobile Subscriber Identity") != std::string::npos,
          "html report's interpreted values include a plain-language IMSI definition");
    check(html.find("+15551234567") != std::string::npos,
          "html report shows the decoded MSISDN value");
    check(html.find("not provisioned (blank)") != std::string::npos,
          "html report distinguishes a blank/unprovisioned SPN from a decode failure");
    check(html.find("content withheld from disk") != std::string::npos,
          "html report flags the sensitive Kc file");

    std::remove(zipPath.c_str());
    std::remove(htmlPath.c_str());

    if (failures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("All output smoke-test checks passed.\n");
    return 0;
}

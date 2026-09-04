#include "zip_writer.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "hashing.h"
#include "miniz.h"
#include "nlohmann/json.hpp"
#include "report_common.h"
#include "version.h"

namespace forandsim::output {

namespace {

using json = nlohmann::json;

void addFile(mz_zip_archive& zip, const std::string& name, const std::vector<uint8_t>& data) {
    if (!mz_zip_writer_add_mem(&zip, name.c_str(), data.empty() ? "" : (const void*)data.data(),
                                data.size(), MZ_BEST_COMPRESSION)) {
        throw std::runtime_error("failed to add '" + name + "' to evidence zip");
    }
}

void addFile(mz_zip_archive& zip, const std::string& name, const std::string& text) {
    addFile(zip, name, std::vector<uint8_t>(text.begin(), text.end()));
}

std::vector<uint8_t> concatenatedContent(const ExtractedFile& f) {
    if (!f.rawData.empty() || f.records.empty()) return f.rawData;
    std::vector<uint8_t> all;
    for (auto& r : f.records) all.insert(all.end(), r.begin(), r.end());
    return all;
}

std::string buildMetaText(const AcquisitionResult& result) {
    std::ostringstream m;
    m << kToolName << " - SIM/USIM Forensic Acquisition Tool\n"
      << "Version: " << kToolVersion << "\n"
      << "Repository: " << kRepoWeb << "\n\n"
      << "This file's own SHA-256 is recorded in manifest.json under files[].\n"
      << "Note: the evidence zip's own SHA-256 cannot be embedded here (writing it into this\n"
      << "file would change the zip and invalidate that very hash); it is instead written to\n"
      << "a sidecar '<zip>.sha256' file next to the zip, and shown in the HTML report.\n\n"
      << "Case identifier:    " << result.caseMetadata.caseIdentifier << "\n"
      << "Piece/exhibit #:    " << result.caseMetadata.pieceNumber << "\n"
      << "Operator:           " << result.caseMetadata.operatorName << "\n"
      << "Authorization confirmed: " << (result.caseMetadata.authorizationConfirmed ? "yes" : "no") << "\n"
      << "Notes:              " << result.caseMetadata.examinerNotes << "\n\n"
      << "Acquisition started:  " << isoTimestamp(result.startedAt) << "\n"
      << "Acquisition finished: " << isoTimestamp(result.finishedAt) << "\n"
      << "Workstation:        " << result.workstationHostname << " (user: " << result.workstationUser << ")\n"
      << "Reader:             " << result.readerName << "\n\n"
      << "ATR:                " << atrHex(result.atr) << "\n"
      << "ICCID:              " << result.iccid << "\n"
      << "Acquisition mode:   " << (result.mode == AcquisitionMode::FullDump ? "full_dump" : "iccid_only") << "\n"
      << "PIN attempted:      " << (result.pinAttempted ? "yes" : "no") << "\n";
    if (result.chv1AttemptsBeforeVerify.has_value()) {
        m << "CHV1 attempts remaining (before verify): " << *result.chv1AttemptsBeforeVerify << "\n";
    }
    if (result.pinAttempted) {
        m << "PIN result:         " << chvResultString(result.pinResult) << "\n";
    }
    m << "\nRead-only acquisition: yes (no UPDATE BINARY/RECORD, INVALIDATE or REHABILITATE\n"
      << "command is ever issued; VERIFY CHV is the only card-state-affecting operation).\n"
      << "ICCID integrity re-read: " << (result.integrityRereadPerformed
             ? (result.integrityRereadMatches ? "MATCH" : "MISMATCH") : "not performed")
      << " (first " << result.iccidSha256First << ", reread " << result.iccidSha256Reread << ")\n";
    if (result.verifyRequested) {
        m << "Full verification pass: " << (result.verifyPerformed ? "performed" : "not completed")
          << ", " << result.verifyMismatches.size() << " mismatch(es)\n";
        for (auto& p : result.verifyMismatches) {
            m << "  MISMATCH: " << p << "\n";
        }
    }
    m << "\nTotal files acquired: " << result.files.size() << "\n";
    return m.str();
}

} // namespace

EvidenceZipResult writeEvidenceZip(const AcquisitionResult& result, const std::string& zipPath) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, zipPath.c_str(), 0)) {
        throw std::runtime_error("failed to create evidence zip at '" + zipPath + "'");
    }

    json values = json::object();
    json manifestFiles = json::array();

    for (const auto& f : result.files) {
        std::vector<uint8_t> content = concatenatedContent(f);
        addFile(zip, "files/" + f.path + ".bin", content);

        manifestFiles.push_back({
            {"path", f.path},
            {"file_id", f.fileId},
            {"name", f.name},
            {"sha256", f.sha256},
            {"size_bytes", content.size()},
            {"structure_unknown", f.structureUnknown},
            {"size_mismatch", f.sizeMismatch},
        });

        if (f.interpretedValue.has_value()) {
            values[f.path] = *f.interpretedValue;
        }
    }

    addFile(zip, "values.json", values.dump(2));

    std::string metaText = buildMetaText(result);
    std::string metaSha256 = sha256Hex(std::vector<uint8_t>(metaText.begin(), metaText.end()));
    addFile(zip, "for-and-sim-meta.txt", metaText);

    json manifest = {
        {"tool", {
            {"name", kToolName},
            {"version", kToolVersion},
            {"repository_web", kRepoWeb},
        }},
        {"case", {
            {"case_identifier", result.caseMetadata.caseIdentifier},
            {"piece_number", result.caseMetadata.pieceNumber},
            {"operator", result.caseMetadata.operatorName},
            {"notes", result.caseMetadata.examinerNotes},
            {"authorization_confirmed", result.caseMetadata.authorizationConfirmed},
        }},
        {"chain_of_custody", {
            {"started_at", isoTimestamp(result.startedAt)},
            {"finished_at", isoTimestamp(result.finishedAt)},
            {"workstation_hostname", result.workstationHostname},
            {"workstation_user", result.workstationUser},
            {"reader_name", result.readerName},
        }},
        {"card", {
            {"atr_hex", atrHex(result.atr)},
            {"iccid", result.iccid},
        }},
        {"acquisition", {
            {"mode", result.mode == AcquisitionMode::FullDump ? "full_dump" : "iccid_only"},
            {"pin_attempted", result.pinAttempted},
            {"pin_result", chvResultString(result.pinResult)},
            {"chv1_attempts_before_verify", result.chv1AttemptsBeforeVerify.has_value()
                 ? json(*result.chv1AttemptsBeforeVerify) : json(nullptr)},
        }},
        {"integrity", {
            {"read_only_acquisition", AcquisitionResult::kReadOnlyAcquisition},
            {"note", "No UPDATE BINARY/RECORD, INVALIDATE or REHABILITATE command is ever "
                     "issued. VERIFY CHV (PIN check) is the only card-state-affecting "
                     "operation performed; its retry counter is captured before the attempt."},
            {"iccid_sha256_first_read", result.iccidSha256First},
            {"iccid_sha256_reread", result.iccidSha256Reread},
            {"reread_performed", result.integrityRereadPerformed},
            {"reread_matches", result.integrityRereadMatches},
            {"full_verify_requested", result.verifyRequested},
            {"full_verify_performed", result.verifyPerformed},
            {"full_verify_mismatches", result.verifyMismatches},
        }},
        {"files", manifestFiles},
        {"meta_file", {
            {"name", "for-and-sim-meta.txt"},
            {"sha256", metaSha256},
        }},
        {"log", result.log},
    };
    addFile(zip, "manifest.json", manifest.dump(2));

    if (!mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        throw std::runtime_error("failed to finalize evidence zip");
    }
    mz_zip_writer_end(&zip);

    // The zip's own hash can only be computed after it is finalized on disk,
    // and can't be embedded inside itself without invalidating that hash -
    // so it's externalized as a sidecar file (same pattern as disk-image tools).
    std::ifstream zipIn(zipPath, std::ios::binary);
    if (!zipIn) {
        throw std::runtime_error("failed to reopen '" + zipPath + "' to hash it");
    }
    std::vector<uint8_t> zipBytes((std::istreambuf_iterator<char>(zipIn)),
                                   std::istreambuf_iterator<char>());
    EvidenceZipResult zipResult;
    zipResult.sha256 = sha256Hex(zipBytes);
    zipResult.sidecarPath = zipPath + ".sha256";

    std::string zipBasename = zipPath;
    if (auto pos = zipBasename.find_last_of("/\\"); pos != std::string::npos) {
        zipBasename = zipBasename.substr(pos + 1);
    }
    std::ofstream sidecar(zipResult.sidecarPath, std::ios::binary);
    sidecar << zipResult.sha256 << "  " << zipBasename << "\n";

    return zipResult;
}

} // namespace forandsim::output

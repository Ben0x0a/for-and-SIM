#include "acquisition_engine.h"

#include <array>
#include <cstdio>

#include "ef_catalog.h"
#include "file_walker.h"
#include "hashing.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 256
#endif
#endif

namespace forandsim {

namespace {

std::string decodeIccid(const std::vector<uint8_t>& data) {
    std::string iccid;
    for (uint8_t b : data) {
        uint8_t lo = b & 0x0F;
        uint8_t hi = (b >> 4) & 0x0F;
        if (lo <= 9) iccid += char('0' + lo);
        if (hi <= 9) iccid += char('0' + hi);
    }
    return iccid;
}

std::string decodeImsi(const std::vector<uint8_t>& data) {
    if (data.empty()) return {};
    uint8_t len = data[0];
    if (len == 0 || data.size() < size_t(1 + len)) return {};

    std::string imsi;
    imsi += char('0' + (data[1] & 0x0F)); // parity/identity digit
    for (size_t i = 2; i < size_t(1 + len); ++i) {
        uint8_t b = data[i];
        imsi += char('0' + (b & 0x0F));
        uint8_t hi = (b >> 4) & 0x0F;
        if (hi != 0x0F) {
            imsi += char('0' + hi);
        }
    }
    return imsi;
}

void applyKnownInterpretation(ExtractedFile& file) {
    if (file.name == "ICCID" && !file.rawData.empty()) {
        file.interpretedValue = decodeIccid(file.rawData);
    } else if (file.name == "IMSI" && !file.rawData.empty()) {
        file.interpretedValue = decodeImsi(file.rawData);
    }
}

std::string hostname() {
#ifdef _WIN32
    char buf[256];
    DWORD size = sizeof(buf);
    if (GetComputerNameA(buf, &size)) return std::string(buf, size);
    return "unknown-host";
#else
    char buf[HOST_NAME_MAX + 1] = {};
    if (gethostname(buf, sizeof(buf)) == 0) return buf;
    return "unknown-host";
#endif
}

std::string currentUser() {
#ifdef _WIN32
    char buf[256];
    DWORD size = sizeof(buf);
    if (GetUserNameA(buf, &size)) return std::string(buf, size - 1);
    return "unknown-user";
#else
    const char* u = getenv("USER");
    return u ? u : "unknown-user";
#endif
}

} // namespace

AcquisitionResult acquire(PcscTransport& transport,
                           const CaseMetadata& caseMetadata,
                           const std::optional<std::string>& pin,
                           bool verify,
                           const ProgressCallback& progress) {
    AcquisitionResult result;
    result.caseMetadata = caseMetadata;
    result.verifyRequested = verify;
    result.startedAt = std::chrono::system_clock::now();
    result.workstationHostname = hostname();
    result.workstationUser = currentUser();

    auto log = [&](const std::string& msg) {
        result.log.push_back(msg);
        if (progress) progress(msg);
    };

    if (!caseMetadata.authorizationConfirmed) {
        result.refusedUnauthorized = true;
        log("REFUSED: authorization to examine this exhibit was not confirmed; "
            "no connection to the card was made.");
        result.finishedAt = std::chrono::system_clock::now();
        return result;
    }

    result.atr = transport.atr();

    CardSession session(transport);

    log("Selecting MF");
    apdu::FileInfo mfInfo = session.selectFile(catalog::kMF);
    if (mfInfo.type != apdu::FileType::MF && mfInfo.type != apdu::FileType::DF) {
        log("WARNING: MF selection did not report MF/DF file type; continuing anyway");
    }

    log("Reading ICCID");
    apdu::FileInfo iccidInfo = session.selectFile(catalog::kEF_ICCID);
    if (iccidInfo.type == apdu::FileType::EF) {
        ExtractedFile iccidFile;
        iccidFile.name = "ICCID";
        iccidFile.path = "MF/ICCID";
        readSelectedEf(session, iccidInfo, iccidFile, iccidFile.path, progress);
        applyKnownInterpretation(iccidFile);
        result.iccid = iccidFile.interpretedValue.value_or("");
        result.iccidSha256First = iccidFile.sha256;
        log("ICCID = " + result.iccid + " (SHA-256 " + iccidFile.sha256 + ")");
        result.files.push_back(iccidFile);
    } else {
        log("WARNING: EF_ICCID not found or not readable");
    }

    if (!pin.has_value()) {
        log("No PIN supplied: stopping after ICCID acquisition");
        result.mode = AcquisitionMode::IccidOnly;
        result.finishedAt = std::chrono::system_clock::now();
        return result;
    }

    result.mode = AcquisitionMode::FullDump;
    result.pinAttempted = true;

    apdu::FileInfo mfBeforeVerify = session.selectFile(catalog::kMF);
    result.chv1AttemptsBeforeVerify = mfBeforeVerify.chv1AttemptsRemaining;
    if (result.chv1AttemptsBeforeVerify.has_value()) {
        log("CHV1 attempts remaining before verify: " +
            std::to_string(*result.chv1AttemptsBeforeVerify));
        if (*result.chv1AttemptsBeforeVerify >= 0 && *result.chv1AttemptsBeforeVerify <= 1) {
            log("WARNING: only " + std::to_string(*result.chv1AttemptsBeforeVerify) +
                " CHV1 attempt(s) left; a wrong PIN now will block the card. "
                "Double-check the PIN before proceeding.");
        }
    }

    log("Verifying CHV1 (this is the only write-capable operation this tool "
        "performs on the card: a wrong PIN decrements its retry counter)");
    ChvVerifyOutcome chv = session.verifyChv1(*pin);
    result.pinResult = chv.result;

    if (chv.result != ChvResult::Correct) {
        log("PIN verification failed or not applicable; stopping full acquisition");
        result.finishedAt = std::chrono::system_clock::now();
        return result;
    }
    log("PIN verified successfully; proceeding with full acquisition");

    session.selectFile(catalog::kMF);
    readKnownEfs(session, catalog::mfEfs(), {}, "MF", result.files, progress);

    session.selectFile(catalog::kMF);
    std::vector<uint16_t> mfKnown;
    for (auto& [id, name] : catalog::mfEfs()) mfKnown.push_back(id);
    probeUnknownEfs(session, mfKnown, {}, "MF", result.files, progress);

    log("Walking GSM DF tree (DF_TELECOM, DF_GSM, ...)");
    walkDfTree(session, catalog::gsmDfTree(), {}, "MF", result.files, progress);

    log("Probing for non-standard/hidden top-level DFs under MF");
    std::vector<uint16_t> knownTopDfs;
    for (auto& node : catalog::gsmDfTree()) knownTopDfs.push_back(node.id);
    session.selectFile(catalog::kMF);
    probeUnknownDfs(session, knownTopDfs, {}, "MF", result.files, progress);

    // NOTE: this pass covers the classic MF/DF_GSM/DF_TELECOM tree (legacy
    // GSM access, which every SIM/USIM answers to). AID-based SELECT of the
    // USIM ADF (3GPP TS 31.102) needs EF_DIR parsing + a CLA=0x00 SELECT and
    // is tracked as a follow-up (see catalog::usimAdfEfs(), currently unused).

    for (auto& file : result.files) {
        applyKnownInterpretation(file);
    }

    // Read-only integrity check: re-read ICCID after the full walk and
    // compare its hash to the very first read. This is the closest read-only
    // analogue of a "hash before/after" check on a SIM (there is no way to
    // hash the whole card the way one hashes a disk image).
    log("Re-reading ICCID for a post-acquisition integrity check");
    apdu::FileInfo iccidRecheck = selectPath(session, {catalog::kEF_ICCID});
    if (iccidRecheck.type == apdu::FileType::EF) {
        ExtractedFile recheck;
        readSelectedEf(session, iccidRecheck, recheck, "MF/ICCID", progress);
        result.iccidSha256Reread = recheck.sha256;
        result.integrityRereadPerformed = true;
        result.integrityRereadMatches = (recheck.sha256 == result.iccidSha256First);
        log("Integrity re-read: first SHA-256 " + result.iccidSha256First + ", reread SHA-256 " +
            result.iccidSha256Reread + " -> " +
            (result.integrityRereadMatches ? "MATCH" : "MISMATCH (card data changed during acquisition!)"));
    } else {
        log("WARNING: could not re-read ICCID for integrity check");
    }

    if (verify) {
        log("Starting full verification pass: re-reading every acquired file (~2x acquisition time)");
        result.verifyPerformed = true;
        size_t checked = 0;
        for (auto& file : result.files) {
            selectPath(session, file.dfPath);
            apdu::FileInfo info = session.selectFile(file.fileId);
            if (info.type != apdu::FileType::EF) {
                result.verifyMismatches.push_back(file.path);
                log("VERIFY WARNING: could not re-select " + file.path + " for verification");
                continue;
            }
            ExtractedFile recheck;
            readSelectedEf(session, info, recheck, file.path, {});
            ++checked;
            if (recheck.sha256 != file.sha256) {
                result.verifyMismatches.push_back(file.path);
                log("VERIFY MISMATCH: " + file.path + " (first SHA-256 " + file.sha256 +
                    ", re-read SHA-256 " + recheck.sha256 + ")");
            }
        }
        log("Verification pass complete: " + std::to_string(result.verifyMismatches.size()) +
            " mismatch(es) out of " + std::to_string(checked) + " file(s) re-read");
    }

    result.finishedAt = std::chrono::system_clock::now();
    return result;
}

std::optional<int> checkChv1AttemptsRemaining(PcscTransport& transport) {
    CardSession session(transport);
    apdu::FileInfo mfInfo = session.selectFile(catalog::kMF);
    return mfInfo.chv1AttemptsRemaining;
}

} // namespace forandsim

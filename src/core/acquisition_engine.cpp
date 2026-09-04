#include "acquisition_engine.h"

#include <algorithm>
#include <array>
#include <cstdio>

#include "ef_catalog.h"
#include "file_walker.h"
#include "hashing.h"
#include "platform_info.h"
#include "time_utils.h"

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

// EF_MSISDN record layout (3GPP TS 51.011 clause 10.5.5), from the end of the
// record backwards: Ext1 record id (1), CCP record id (1), BCD number (10,
// 0xFF-padded), TON/NPI (1), length-of-BCD-number-and-TON/NPI (1); everything
// before that is the (unused here) alpha identifier.
std::string decodeMsisdnRecord(const std::vector<uint8_t>& record) {
    if (record.size() < 14) return {};
    size_t n = record.size();
    uint8_t bcdLen = record[n - 14];     // includes the TON/NPI byte itself
    uint8_t tonNpi = record[n - 13];
    if (bcdLen == 0 || bcdLen == 0xFF) return {};

    size_t bcdBytes = bcdLen > 1 ? std::min<size_t>(bcdLen - 1, 10) : 0;
    std::string digits;
    for (size_t i = 0; i < bcdBytes; ++i) {
        uint8_t b = record[n - 12 + i];
        uint8_t lo = b & 0x0F, hi = (b >> 4) & 0x0F;
        if (lo <= 9) digits += char('0' + lo);
        if (hi <= 9) digits += char('0' + hi);
    }
    if (digits.empty()) return {};

    bool international = ((tonNpi >> 4) & 0x07) == 0x01; // type-of-number = international
    return international ? ("+" + digits) : digits;
}

// EF_SPN (3GPP TS 51.011 10.3.11): byte 1 is a display-condition bitmask,
// followed by the name in the SMS default (GSM 7-bit) alphabet stored
// unpacked (one byte per character, which matches ASCII for common Latin
// letters/digits), 0xFF-padded to the file's fixed length.
std::string decodeSpn(const std::vector<uint8_t>& data) {
    std::string name;
    for (size_t i = 1; i < data.size(); ++i) {
        if (data[i] == 0xFF) break;
        name += char(data[i]);
    }
    return name;
}

// EF_FPLMN (3GPP TS 51.011 10.3.13): a list of 3-byte PLMN (MCC/MNC) codes
// the card was rejected by, same MCC/MNC nibble encoding as EF_LOCI's LAI.
// 0xFFFFFF marks an unused slot. Forensically useful: hints at roaming/travel
// history distinct from the home network in EF_IMSI.
std::string decodeFplmnList(const std::vector<uint8_t>& data) {
    std::string joined;
    for (size_t i = 0; i + 3 <= data.size(); i += 3) {
        uint8_t b0 = data[i], b1 = data[i + 1], b2 = data[i + 2];
        if (b0 == 0xFF && b1 == 0xFF && b2 == 0xFF) continue; // unused slot

        uint8_t mcc1 = b0 & 0x0F, mcc2 = (b0 >> 4) & 0x0F;
        uint8_t mcc3 = b1 & 0x0F, mnc3 = (b1 >> 4) & 0x0F;
        uint8_t mnc1 = b2 & 0x0F, mnc2 = (b2 >> 4) & 0x0F;
        if (mcc1 > 9 || mcc2 > 9 || mcc3 > 9 || mnc1 > 9 || mnc2 > 9) continue;

        std::string mcc = {char('0' + mcc1), char('0' + mcc2), char('0' + mcc3)};
        std::string mnc = {char('0' + mnc1), char('0' + mnc2)};
        if (mnc3 <= 9) mnc += char('0' + mnc3);

        if (!joined.empty()) joined += ", ";
        joined += mcc + "-" + mnc;
    }
    return joined;
}

void applyKnownInterpretation(ExtractedFile& file) {
    if (file.name == "ICCID" && !file.rawData.empty()) {
        file.interpretedValue = decodeIccid(file.rawData);
    } else if (file.name == "IMSI" && !file.rawData.empty()) {
        file.interpretedValue = decodeImsi(file.rawData);
    } else if (file.name == "MSISDN") {
        for (auto& record : file.records) {
            std::string decoded = decodeMsisdnRecord(record);
            if (!decoded.empty()) {
                file.interpretedValue = decoded;
                break;
            }
        }
    } else if (file.name == "LOCI" && !file.rawData.empty()) {
        if (auto loci = decodeLoci(file.rawData)) {
            file.interpretedValue = "MCC=" + loci->mcc + " MNC=" + loci->mnc + " LAC=0x" + loci->lac;
        }
    } else if (file.name == "SPN" && !file.rawData.empty()) {
        std::string spn = decodeSpn(file.rawData);
        if (!spn.empty()) file.interpretedValue = spn;
    } else if (file.name == "FPLMN" && !file.rawData.empty()) {
        std::string fplmn = decodeFplmnList(file.rawData);
        file.interpretedValue = fplmn; // empty string is itself meaningful: no forbidden PLMNs stored
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

std::optional<LocationInfo> decodeLoci(const std::vector<uint8_t>& data) {
    // 3GPP TS 51.011 10.3.11: TMSI (4 bytes) + LAI (5 bytes: MCC/MNC 3 bytes,
    // LAC 2 bytes big-endian binary) + location update status (1+ bytes).
    if (data.size() < 9) return std::nullopt;

    uint8_t b0 = data[4], b1 = data[5], b2 = data[6];
    uint8_t mcc1 = b0 & 0x0F, mcc2 = (b0 >> 4) & 0x0F;
    uint8_t mcc3 = b1 & 0x0F, mnc3 = (b1 >> 4) & 0x0F; // mnc3 == 0xF if MNC has only 2 digits
    uint8_t mnc1 = b2 & 0x0F, mnc2 = (b2 >> 4) & 0x0F;

    if (mcc1 > 9 || mcc2 > 9 || mcc3 > 9 || mnc1 > 9 || mnc2 > 9) return std::nullopt;

    LocationInfo info;
    info.mcc = {char('0' + mcc1), char('0' + mcc2), char('0' + mcc3)};
    info.mnc = {char('0' + mnc1), char('0' + mnc2)};
    if (mnc3 <= 9) info.mnc += char('0' + mnc3);

    char lacHex[5];
    std::snprintf(lacHex, sizeof(lacHex), "%02X%02X", data[7], data[8]);
    info.lac = lacHex;
    return info;
}

AcquisitionResult acquire(PcscTransport& transport,
                           const CaseMetadata& caseMetadata,
                           const std::optional<std::string>& pin,
                           const AcquisitionOptions& options,
                           const ProgressCallback& progress) {
    AcquisitionResult result;
    result.caseMetadata = caseMetadata;
    result.verifyRequested = options.verify;
    result.startedAt = std::chrono::system_clock::now();
    result.workstationHostname = hostname();
    result.workstationUser = currentUser();
    result.platform = platformDescription();
    result.toolExeSha256 = currentExecutableSha256();

    auto log = [&](const std::string& msg) {
        std::string stamped = "[" + isoTimestamp(std::chrono::system_clock::now()) + "] " + msg;
        result.log.push_back(stamped);
        if (progress) progress(stamped);
    };

    if (!caseMetadata.authorizationConfirmed) {
        result.refusedUnauthorized = true;
        log("REFUSED: authorization to examine this exhibit was not confirmed; "
            "no connection to the card was made.");
        result.finishedAt = std::chrono::system_clock::now();
        return result;
    }

    try {
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
        readKnownEfs(session, catalog::mfEfs(), {}, "MF", result.files, options, progress);

        if (options.scanNonStandardFiles) {
            session.selectFile(catalog::kMF);
            std::vector<uint16_t> mfKnown;
            for (auto& [id, name] : catalog::mfEfs()) mfKnown.push_back(id);
            probeUnknownEfs(session, mfKnown, {}, "MF", result.files, options, progress);
        }

        log("Walking GSM DF tree (DF_TELECOM, DF_GSM, ...)");
        walkDfTree(session, catalog::gsmDfTree(), {}, "MF", result.files, options, progress);

        if (options.scanNonStandardFiles) {
            log("Probing for non-standard/hidden top-level DFs under MF");
            std::vector<uint16_t> knownTopDfs;
            for (auto& node : catalog::gsmDfTree()) knownTopDfs.push_back(node.id);
            session.selectFile(catalog::kMF);
            probeUnknownDfs(session, knownTopDfs, {}, "MF", result.files, options, progress);
        }

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

        if (options.verify) {
            log("Starting full verification pass: re-reading every acquired file (~2x acquisition time)");
            result.verifyPerformed = true;
            size_t checked = 0;
            for (auto& file : result.files) {
                checkCancellation(options);
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
    } catch (const AcquisitionCancelled&) {
        result.cancelled = true;
        log("Acquisition cancelled by operator; keeping " + std::to_string(result.files.size()) +
            " file(s) already read.");
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

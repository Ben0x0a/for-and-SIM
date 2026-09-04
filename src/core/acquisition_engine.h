#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "apdu.h"
#include "card_session.h"
#include "case_metadata.h"
#include "pcsc_transport.h"

namespace forandsim {

// Decoded from EF_LOCI (3GPP TS 51.011 clause 10.3.11): the last GSM/UMTS
// cell the card's baseband registered on. Classic EF_LOCI carries the
// Location Area (MCC/MNC/LAC) but not a Cell ID or LTE Tracking Area Code -
// those live in EF_EPSLOCI under the USIM ADF, which this tool cannot yet
// SELECT by AID (see the README's known limitations).
struct LocationInfo {
    std::string mcc; // Mobile Country Code
    std::string mnc; // Mobile Network Code
    std::string lac; // Location Area Code, as 4 hex digits
};

// Returns std::nullopt if `data` isn't a plausible EF_LOCI (too short).
std::optional<LocationInfo> decodeLoci(const std::vector<uint8_t>& data);

// One acquired GSM/USIM file, as it existed on the card.
struct ExtractedFile {
    std::string path;    // e.g. "MF/DF_GSM/IMSI"
    uint16_t fileId = 0;
    std::string name;    // catalog name, or "UNKNOWN" if not in the catalog
    // DF ids from MF down to this file's immediate parent DF (empty = child of
    // MF). Lets a verification pass re-select the exact same file later.
    std::vector<uint16_t> dfPath;
    apdu::FileStructure structure = apdu::FileStructure::Unknown;
    std::vector<uint8_t> rawData;              // transparent EF content
    std::vector<std::vector<uint8_t>> records; // linear-fixed/cyclic EF records
    std::string sha256;
    // Set for a handful of well-known EFs (ICCID, IMSI, MSISDN, SPN, ...) whose
    // content is a single decodable value rather than an opaque blob/file.
    std::optional<std::string> interpretedValue;
    // True if the SELECT response reported a file structure byte outside the
    // GSM 11.11 transparent/linear-fixed/cyclic set (e.g. a UICC BER-TLV EF);
    // in that case no content could be reliably read and rawData/records are empty.
    bool structureUnknown = false;
    // True if a READ BINARY/RECORD returned fewer bytes than the file's
    // declared size/record length — a truncated or inconsistent read.
    bool sizeMismatch = false;
    // True for cryptographic key material (EF_Kc/EF_KcGPRS) — see
    // catalog::isSensitiveKeyMaterial. rawData/records are still populated in
    // memory (so sha256 above is computed normally) but the output layer
    // (writeEvidenceZip) never writes this file's raw content to disk.
    bool sensitive = false;
};

enum class AcquisitionMode {
    IccidOnly,   // no PIN supplied: only ATR + ICCID are read
    FullDump,    // PIN verified: full MF/DF_GSM/DF_TELECOM/ADF_USIM walk
};

struct AcquisitionResult {
    CaseMetadata caseMetadata;
    std::chrono::system_clock::time_point startedAt;
    std::chrono::system_clock::time_point finishedAt;
    std::string workstationHostname;
    std::string workstationUser;
    std::string readerName;
    std::string platform;                    // e.g. "macOS arm64"
    std::optional<std::string> toolExeSha256; // hash of the running executable

    std::vector<uint8_t> atr;
    std::string iccid;

    AcquisitionMode mode = AcquisitionMode::IccidOnly;
    bool pinAttempted = false;
    ChvResult pinResult = ChvResult::Error;
    // CHV1 (PIN1) retry counter, read from the card *before* verification is
    // attempted, so an operator/log can see how many tries were left going
    // in. std::nullopt if it could not be determined.
    std::optional<int> chv1AttemptsBeforeVerify;

    // This tool never issues UPDATE BINARY/RECORD, INVALIDATE, REHABILITATE or
    // any other write command — see apdu.h/apdu.cpp, which simply does not
    // implement builders for them. The one unavoidable exception is VERIFY
    // CHV: presenting a PIN is inherently a card-state operation (it can
    // decrement the retry counter on failure), which is why the counter is
    // captured above before the attempt is made.
    static constexpr bool kReadOnlyAcquisition = true;

    // Post-acquisition integrity check: ICCID is re-read after the full walk
    // and its hash compared against the very first read, as a read-only
    // analogue of a "hash before/after" verification (there is no way to hash
    // "the whole card" the way one hashes a disk image).
    std::string iccidSha256First;
    std::string iccidSha256Reread;
    bool integrityRereadPerformed = false;
    bool integrityRereadMatches = false;

    // Optional full verification pass (opt-in, roughly doubles acquisition
    // time): every acquired file is re-selected and re-read, and its hash
    // compared against the first read. Unlike the ICCID-only integrity check
    // above, this covers every PIN-gated file actually walked.
    bool verifyRequested = false;
    bool verifyPerformed = false;
    std::vector<std::string> verifyMismatches; // paths whose re-read hash differed

    // True if acquire() refused to touch the card because
    // caseMetadata.authorizationConfirmed was false.
    bool refusedUnauthorized = false;

    // True if the acquisition was stopped early via AcquisitionOptions::cancel
    // (e.g. the GUI's Stop button). Whatever was read before the cancellation
    // point is still kept in `files`/`log`, but the walk is incomplete.
    bool cancelled = false;

    std::vector<ExtractedFile> files;
    std::vector<std::string> log;
};

using ProgressCallback = std::function<void(const std::string& message)>;

struct AcquisitionOptions {
    // Re-reads and hash-checks every acquired file after the walk completes
    // (see AcquisitionResult::verifyMismatches). Roughly doubles acquisition
    // time but is the only way to confirm nothing changed across every
    // PIN-gated file, not just ICCID. On by default.
    bool verify = true;

    // Brute-force probes non-standard EF/DF id ranges at every level to catch
    // hidden/undocumented files (see file_walker.h). This can be slow on a
    // slow reader, and cards that answer SELECT "successfully" for almost any
    // id (some test/simulator cards) can make it explore a very large number
    // of phantom files; disable it for a much faster catalog-only walk.
    bool scanNonStandardFiles = true;

    // If non-null and set to true from another thread, acquire() stops at the
    // next checkpoint (between files/probes) and returns with `cancelled` set,
    // keeping whatever was already read. Never left in a half-sent-APDU state.
    std::atomic<bool>* cancelRequested = nullptr;
};

// Runs a full acquisition: connects (transport must already be connected to
// the target reader/card), selects MF, reads ICCID, and — if `pin` has a
// value — verifies CHV1 and, on success, walks the GSM DF tree and the USIM
// ADF (if present), reading every catalog EF plus, if
// options.scanNonStandardFiles, a brute-force ID probe per DF to catch
// non-standard/hidden files.
// Refuses outright (returns immediately, without touching the card) if
// caseMetadata.authorizationConfirmed is false.
AcquisitionResult acquire(PcscTransport& transport,
                           const CaseMetadata& caseMetadata,
                           const std::optional<std::string>& pin,
                           const AcquisitionOptions& options = {},
                           const ProgressCallback& progress = {});

// Quick, read-only pre-flight check: selects MF and returns the CHV1 retry
// counter without ever attempting VERIFY CHV. Lets an operator see how many
// PIN attempts remain and decide whether to proceed, before committing to a
// guess that could block the card.
std::optional<int> checkChv1AttemptsRemaining(PcscTransport& transport);

} // namespace forandsim

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Known GSM SIM (3GPP TS 51.011) and USIM (3GPP TS 31.102) DF/EF identifiers.
namespace forandsim::catalog {

struct DfNode {
    uint16_t id;
    const char* name;
    std::vector<DfNode> children; // nested DFs, empty for leaf DFs
    std::vector<std::pair<uint16_t, const char*>> ownEfs; // EFs directly under this DF
};

// AID prefix (RID + application code) used to SELECT the USIM ADF via EF_DIR
// entries; the acquisition engine resolves the concrete AID from EF_DIR at
// runtime and falls back to this well-known 3GPP USIM AID if EF_DIR is absent.
extern const std::vector<uint8_t> kUsimAidFallback;

constexpr uint16_t kMF = 0x3F00;
constexpr uint16_t kEF_ICCID = 0x2FE2;
constexpr uint16_t kEF_DIR = 0x2F00;
constexpr uint16_t kEF_PL = 0x2F05;
constexpr uint16_t kDF_GSM = 0x7F20;

// EFs directly under MF (besides ICCID/EF_DIR/EF_PL, listed explicitly above).
std::vector<std::pair<uint16_t, const char*>> mfEfs();

// EFs directly under DF_GSM whose READ access condition is ALW (3GPP TS
// 51.011): readable with no PIN at all, unlike almost everything else under
// DF_GSM/DF_TELECOM. Currently just EF_PHASE, which a phone needs to read
// before it can even attempt to verify a PIN.
std::vector<std::pair<uint16_t, const char*>> alwEfsUnderDfGsm();

// The classic GSM DF tree (DF_TELECOM, DF_GSM and their sub-DFs/EFs), matching
// what a SIM (not USIM) filesystem exposes under MF.
std::vector<DfNode> gsmDfTree();

// EFs found directly under the USIM Application DF (ADF_USIM), selected by AID
// (see card_session.h's selectByAid() and acquisition_engine's EF_DIR-based AID
// discovery). Most share the same file IDs as their GSM DF_GSM/DF_TELECOM
// counterparts (3GPP TS 31.102 Annex), so the acquisition engine also reuses
// this list to probe an ADF once selected.
std::vector<std::pair<uint16_t, const char*>> usimAdfEfs();

// Looks up a human-readable name for a file id within a given catalog list;
// returns nullptr if unknown (still acquired, just labeled "UNKNOWN").
const char* nameFor(uint16_t fileId);

// True for catalog entries that hold cryptographic key material rather than
// subscriber data - currently EF_Kc/EF_KcGPRS (3GPP TS 51.011 10.3.9/10.3.20),
// the last GSM/GPRS ciphering session key the card computed. Ki (the card's
// long-term authentication key) is never readable via any EF at all, so this
// list only needs to cover keys that genuinely are exposed as files.
// Acquisition still detects and hashes these files (proving they exist and
// what they are, for the educational point of the exercise), but their raw
// bytes are never written to disk - see AcquisitionResult / writeEvidenceZip.
bool isSensitiveKeyMaterial(const std::string& fileName);

} // namespace forandsim::catalog

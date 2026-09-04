#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "acquisition_engine.h"
#include "card_session.h"
#include "time_utils.h"

namespace forandsim::output {

using forandsim::isoTimestamp;

std::string atrHex(const std::vector<uint8_t>& atr);
const char* chvResultString(ChvResult r);

// One of a fixed set of "identity" fields (ICCID, IMSI, MSISDN, SPN) always
// reported the same way regardless of acquisition mode, so a report is easy
// to scan/compare even when most of them are empty (e.g. no PIN supplied).
struct KeyResultField {
    std::string name;   // "ICCID", "IMSI", "MSISDN", "SPN", "MCC", "MNC", "LAC", "TAC", "CID"
    std::string value;  // decoded value, empty if unavailable
    std::string status; // "found" | "not read (no PIN)" | "not present on card" |
                         // "present on card, not decoded" | "not accessible"
    std::string path;   // file path if the underlying file was read, else empty
    std::string note;   // extra context, e.g. why a field can never be filled in
};

// Always returns one entry per known identity field, in a fixed order, so
// the report schema never changes shape between an ICCID-only run and a
// full PIN-verified one.
std::vector<KeyResultField> buildKeyResults(const AcquisitionResult& result);

} // namespace forandsim::output

#include "report_common.h"

#include <cstdio>

namespace forandsim::output {

std::string atrHex(const std::vector<uint8_t>& atr) {
    std::string hex;
    char b[4];
    for (uint8_t byte : atr) {
        std::snprintf(b, sizeof(b), "%02X ", byte);
        hex += b;
    }
    if (!hex.empty()) hex.pop_back();
    return hex;
}

const char* chvResultString(ChvResult r) {
    switch (r) {
        case ChvResult::Correct: return "correct";
        case ChvResult::NotInitialized: return "not_initialized";
        case ChvResult::Incorrect: return "incorrect";
        case ChvResult::Blocked: return "blocked";
        default: return "error";
    }
}

std::vector<KeyResultField> buildKeyResults(const AcquisitionResult& result) {
    static const char* kDirectFieldNames[] = {"ICCID", "IMSI", "MSISDN", "SPN", "FPLMN"};

    std::vector<KeyResultField> fields;
    for (const char* name : kDirectFieldNames) {
        KeyResultField field;
        field.name = name;

        const ExtractedFile* match = nullptr;
        for (const auto& f : result.files) {
            if (f.name == name) {
                match = &f;
                break;
            }
        }

        if (match) {
            field.path = match->path;
            if (match->interpretedValue.has_value() && !match->interpretedValue->empty()) {
                field.value = *match->interpretedValue;
                field.status = "found";
            } else {
                field.status = "present on card, not decoded";
            }
        } else if (field.name != "ICCID" && result.mode == AcquisitionMode::IccidOnly) {
            field.status = "not read (no PIN)";
        } else {
            field.status = "not present on card";
        }

        fields.push_back(std::move(field));
    }

    // MCC/MNC/LAC come from EF_LOCI (the last GSM/UMTS cell registered), not
    // from a file of their own - so they're derived here rather than looked
    // up by file name like the fields above.
    const ExtractedFile* loci = nullptr;
    for (const auto& f : result.files) {
        if (f.name == "LOCI") {
            loci = &f;
            break;
        }
    }
    std::optional<LocationInfo> location =
        loci && !loci->rawData.empty() ? decodeLoci(loci->rawData) : std::nullopt;

    auto lociField = [&](const char* name, const std::string& value) {
        KeyResultField field;
        field.name = name;
        if (loci) field.path = loci->path;
        if (location) {
            field.value = value;
            field.status = "found";
        } else if (result.mode == AcquisitionMode::IccidOnly) {
            field.status = "not read (no PIN)";
        } else {
            field.status = "not present on card";
        }
        fields.push_back(std::move(field));
    };
    lociField("MCC", location ? location->mcc : "");
    lociField("MNC", location ? location->mnc : "");
    lociField("LAC", location ? location->lac : "");

    // TAC (LTE tracking area code) and CID (cell id) live in EF_EPSLOCI under
    // the USIM ADF, which needs an AID-based SELECT this tool doesn't
    // implement yet (see the README's known limitations) - always reported
    // as unavailable, with the reason, rather than silently omitted.
    for (const char* name : {"TAC", "CID"}) {
        KeyResultField field;
        field.name = name;
        field.status = "not accessible";
        field.note = "Requires EF_EPSLOCI under the USIM ADF (AID-based SELECT), "
                     "which this tool does not implement.";
        fields.push_back(std::move(field));
    }

    return fields;
}

} // namespace forandsim::output

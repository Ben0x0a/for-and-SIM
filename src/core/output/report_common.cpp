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

namespace {

// Plain-language definition of each key-result field, shown in its Note
// column regardless of whether a value was actually found - so the report is
// self-explanatory without needing GLOSSARY.md open alongside it.
const char* definitionFor(const std::string& name) {
    if (name == "ICCID") {
        return "Integrated Circuit Card Identifier - the card's own serial number. "
               "Readable without a PIN.";
    }
    if (name == "IMSI") {
        return "International Mobile Subscriber Identity - identifies the subscriber "
               "to the network (country + operator + subscriber number). PIN-protected.";
    }
    if (name == "MSISDN") {
        return "The phone number associated with the card, if the operator wrote it "
               "(not always populated).";
    }
    if (name == "SPN") {
        return "Service Provider Name - the operator name the phone displays.";
    }
    if (name == "FPLMN") {
        return "Forbidden PLMN list - networks (MCC-MNC) the phone was rejected by; "
               "can hint at travel/roaming history.";
    }
    if (name == "MCC") {
        return "Mobile Country Code of the last GSM/UMTS cell the card registered on "
               "(from EF_LOCI).";
    }
    if (name == "MNC") {
        return "Mobile Network Code (operator) of the last GSM/UMTS cell the card "
               "registered on (from EF_LOCI).";
    }
    if (name == "LAC") {
        return "Location Area Code of the last GSM/UMTS cell the card registered on "
               "(from EF_LOCI).";
    }
    if (name == "TAC") {
        return "LTE Tracking Area Code (the 4G equivalent of a Location Area). Not "
               "accessible: requires EF_EPSLOCI under the USIM ADF (AID-based SELECT), "
               "which this tool does not implement.";
    }
    if (name == "CID") {
        return "Cell ID - the specific cell/base station sector, not just its Location/"
               "Tracking Area. Not accessible: neither EF_LOCI nor EF_EPSLOCI store "
               "this at all, regardless of tooling.";
    }
    return "";
}

} // namespace

std::vector<KeyResultField> buildKeyResults(const AcquisitionResult& result) {
    static const char* kDirectFieldNames[] = {"ICCID", "IMSI", "MSISDN", "SPN", "FPLMN"};

    std::vector<KeyResultField> fields;
    for (const char* name : kDirectFieldNames) {
        KeyResultField field;
        field.name = name;
        field.note = definitionFor(name);

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
        field.note = definitionFor(name);
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
    // as unavailable, with the reason (folded into their definition), rather
    // than silently omitted.
    for (const char* name : {"TAC", "CID"}) {
        KeyResultField field;
        field.name = name;
        field.status = "not accessible";
        field.note = definitionFor(name);
        fields.push_back(std::move(field));
    }

    return fields;
}

} // namespace forandsim::output

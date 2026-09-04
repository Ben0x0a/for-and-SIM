#pragma once

#include <string>

namespace forandsim {

// Case/exhibit metadata supplied by the operator before acquisition; carried
// through into the manifest and HTML report for chain-of-custody purposes.
struct CaseMetadata {
    std::string caseIdentifier;
    std::string pieceNumber; // exhibit / item number
    std::string operatorName;
    std::string examinerNotes;
    // Procedural safeguard: the operator must explicitly attest they are
    // authorized to examine this exhibit before any acquisition starts.
    // acquire() refuses to touch the card at all if this is false, and the
    // attestation itself is recorded in the manifest/meta/HTML report either way.
    bool authorizationConfirmed = false;
};

} // namespace forandsim

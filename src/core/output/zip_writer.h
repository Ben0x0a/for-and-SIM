#pragma once

#include <string>

#include "acquisition_engine.h"

namespace forandsim::output {

// Result of writing the evidence zip: its own SHA-256 (of the finalized zip
// file's bytes). Not embeddable inside the zip itself (that would change the
// zip and invalidate the very hash being recorded), so it's returned here and
// shown in the HTML report / manifest.json instead.
struct EvidenceZipResult {
    std::string sha256;
};

// Writes the acquisition evidence container to `zipPath`:
//   files/<DF path>/<EF name>.bin   raw content of every acquired EF (except
//                                    cryptographic key material - see
//                                    ExtractedFile::sensitive)
//   values.json                     fixed set of decoded identity values,
//                                    always present with an explicit status
//   manifest.json                   case metadata, chain of custody, tool
//                                    provenance, and a per-file SHA-256 hash list
//   for-and-sim-meta.txt            human-readable summary of the above (its
//                                    own hash is recorded in manifest.json)
// Throws std::runtime_error on failure.
EvidenceZipResult writeEvidenceZip(const AcquisitionResult& result, const std::string& zipPath);

} // namespace forandsim::output

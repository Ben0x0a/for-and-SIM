#pragma once

#include <string>

#include "acquisition_engine.h"

namespace forandsim::output {

// Result of writing the evidence zip: its own SHA-256 (of the finalized zip
// file's bytes) plus the sidecar path it was also written to.
struct EvidenceZipResult {
    std::string sha256;
    std::string sidecarPath; // "<zipPath>.sha256", a plain sha256sum-format line
};

// Writes the acquisition evidence container to `zipPath`:
//   files/<DF path>/<EF name>.bin   raw content of every acquired EF
//   values.json                     decoded values for the handful of EFs
//                                    that are pure values (ICCID, IMSI, ...)
//   manifest.json                   case metadata, chain of custody, tool
//                                    provenance, and a per-file SHA-256 hash list
//   for-and-sim-meta.txt            human-readable summary of the above (its
//                                    own hash is recorded in manifest.json)
// The zip's own hash cannot be embedded inside itself (it would invalidate
// the hash), so it is returned here and also written to a sidecar
// "<zipPath>.sha256" file, matching how disk-image tools externalize their
// container hash. Throws std::runtime_error on failure.
EvidenceZipResult writeEvidenceZip(const AcquisitionResult& result, const std::string& zipPath);

} // namespace forandsim::output

#pragma once

#include <string>

#include "acquisition_engine.h"
#include "zip_writer.h"

namespace forandsim::output {

// Renders a standalone (no external assets) HTML chain-of-custody report to
// `htmlPath`: case metadata, chain of custody (hashes, timestamps,
// workstation), tool provenance, the evidence zip's own hash, and the tree of
// extracted files/values. `zipFileName`/`zipInfo` come from writeEvidenceZip().
void writeHtmlReport(const AcquisitionResult& result, const std::string& htmlPath,
                      const std::string& zipFileName, const EvidenceZipResult& zipInfo);

} // namespace forandsim::output

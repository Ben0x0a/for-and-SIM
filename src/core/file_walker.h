#pragma once

#include <vector>

#include "acquisition_engine.h"
#include "card_session.h"
#include "ef_catalog.h"

namespace forandsim {

// Selects MF then each DF id in `path`, in order. Returns the FileInfo of the
// last selection (the target DF), or of MF itself if `path` is empty.
apdu::FileInfo selectPath(CardSession& session, const std::vector<uint16_t>& path);

// Reads the content of the EF that is *currently selected*, filling `out`.
// Flags out.structureUnknown / out.sizeMismatch on anomalies; `path` is only
// used to make the (optional) progress messages about those anomalies useful.
void readSelectedEf(CardSession& session, const apdu::FileInfo& info, ExtractedFile& out,
                     const std::string& path = {}, const ProgressCallback& progress = {});

// Selects and reads every (id, name) EF pair directly under the currently
// selected DF (reached via `parentIds`); appends one ExtractedFile per EF
// actually present. Logs a warning (rather than silently skipping) if a
// SELECT fails with a status word other than the ordinary "file not found",
// since that can indicate a CLA/class incompatibility instead of true absence.
void readKnownEfs(CardSession& session,
                   const std::vector<std::pair<uint16_t, const char*>>& efs,
                   const std::vector<uint16_t>& parentIds,
                   const std::string& parentPath,
                   std::vector<ExtractedFile>& out,
                   const ProgressCallback& progress);

// Recursively walks a GSM DF tree starting at `nodes` (children of the DF
// already selected via `parentIds`), reading catalog EFs and recursing into
// sub-DFs. `parentPath`/`parentIds` describe where `nodes` live relative to MF.
void walkDfTree(CardSession& session,
                 const std::vector<catalog::DfNode>& nodes,
                 const std::vector<uint16_t>& parentIds,
                 const std::string& parentPath,
                 std::vector<ExtractedFile>& out,
                 const ProgressCallback& progress);

// Brute-force probes EF ids in the standard GSM non-standard-EF ranges
// (0x4F00-0x4FFF, 0x6F00-0x6FFF) under the currently selected DF (reached via
// `parentIds`), skipping ids already present in `alreadyFound`. Catches
// non-standard/hidden elementary files a catalog-only walk would miss.
void probeUnknownEfs(CardSession& session,
                      const std::vector<uint16_t>& alreadyFound,
                      const std::vector<uint16_t>& parentIds,
                      const std::string& parentPath,
                      std::vector<ExtractedFile>& out,
                      const ProgressCallback& progress);

// Brute-force probes DF ids in the standard GSM DF-naming ranges
// (0x5F00-0x5FFF, 0x7F00-0x7FFF) under the currently selected DF (reached via
// `parentIds`), skipping ids already present in `alreadyFoundDfs` (the
// catalog's known children). Any hit is a DF the catalog doesn't know about -
// a hidden/undocumented directory - so it's fully explored recursively (its
// own EFs and sub-DFs are probed the same way).
void probeUnknownDfs(CardSession& session,
                      const std::vector<uint16_t>& alreadyFoundDfs,
                      const std::vector<uint16_t>& parentIds,
                      const std::string& parentPath,
                      std::vector<ExtractedFile>& out,
                      const ProgressCallback& progress);

} // namespace forandsim

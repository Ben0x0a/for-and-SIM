#pragma once

#include <vector>

#include "acquisition_engine.h"
#include "card_session.h"
#include "ef_catalog.h"

namespace forandsim {

// Thrown internally when AcquisitionOptions::cancelRequested is observed set;
// caught in acquire() to stop cleanly and finalize a partial result.
struct AcquisitionCancelled {};

// Throws AcquisitionCancelled if options.cancelRequested is set. Call this at
// natural checkpoints (between files/probes) in every loop below.
void checkCancellation(const AcquisitionOptions& options);

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
                   const AcquisitionOptions& options,
                   const ProgressCallback& progress);

// Recursively walks a GSM DF tree starting at `nodes` (children of the DF
// already selected via `parentIds`), reading catalog EFs and recursing into
// sub-DFs. `parentPath`/`parentIds` describe where `nodes` live relative to MF.
// If options.scanNonStandardFiles, also brute-force probes for undocumented
// EFs/DFs at every level (see probeUnknownEfs/probeUnknownDfs below).
void walkDfTree(CardSession& session,
                 const std::vector<catalog::DfNode>& nodes,
                 const std::vector<uint16_t>& parentIds,
                 const std::string& parentPath,
                 std::vector<ExtractedFile>& out,
                 const AcquisitionOptions& options,
                 const ProgressCallback& progress);

// Brute-force probes EF ids in the standard GSM non-standard-EF ranges
// (0x4F00-0x4FFF, 0x6F00-0x6FFF) under the currently selected DF (reached via
// `parentIds`), skipping ids already present in `alreadyFound`. Catches
// non-standard/hidden elementary files a catalog-only walk would miss.
// If an unexpectedly large number of ids come back as "valid EFs" (some
// test/simulator cards answer SELECT successfully for almost any id), logs a
// warning and stops early rather than treating the whole id space as files.
void probeUnknownEfs(CardSession& session,
                      const std::vector<uint16_t>& alreadyFound,
                      const std::vector<uint16_t>& parentIds,
                      const std::string& parentPath,
                      std::vector<ExtractedFile>& out,
                      const AcquisitionOptions& options,
                      const ProgressCallback& progress);

// Brute-force probes DF ids in the standard GSM DF-naming ranges
// (0x5F00-0x5FFF, 0x7F00-0x7FFF) under the currently selected DF (reached via
// `parentIds`), skipping ids already present in `alreadyFoundDfs` (the
// catalog's known children). Any hit is a DF the catalog doesn't know about -
// a hidden/undocumented directory - so it's fully explored recursively (its
// own EFs and sub-DFs are probed the same way), subject to three safety
// guards against a card that mishandles SELECT (seen in practice on at least
// one test card, which otherwise recurses into itself forever):
//   - cycle guard: an id already present in `parentIds` (i.e. an ancestor of
//     the current position) is never re-entered;
//   - depth guard: non-standard DF nesting stops after `depth` reaches 3;
//   - anomaly guard: if more than a handful of ids in one 512-id scan come
//     back as "valid DFs", the card is almost certainly mis-answering SELECT
//     rather than genuinely hiding that many directories, so probing stops
//     early with a warning instead of recursing into all of them.
void probeUnknownDfs(CardSession& session,
                      const std::vector<uint16_t>& alreadyFoundDfs,
                      const std::vector<uint16_t>& parentIds,
                      const std::string& parentPath,
                      std::vector<ExtractedFile>& out,
                      const AcquisitionOptions& options,
                      const ProgressCallback& progress,
                      int depth = 0);

} // namespace forandsim

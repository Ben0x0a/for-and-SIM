#include "file_walker.h"

#include <cstdio>

#include "hashing.h"

namespace forandsim {

apdu::FileInfo selectPath(CardSession& session, const std::vector<uint16_t>& path) {
    apdu::FileInfo info = session.selectFile(catalog::kMF);
    for (uint16_t id : path) {
        info = session.selectFile(id);
    }
    return info;
}

void readSelectedEf(CardSession& session, const apdu::FileInfo& info, ExtractedFile& out,
                     const std::string& path, const ProgressCallback& progress) {
    out.fileId = info.fileId;
    out.structure = info.structure;

    if (info.structure == apdu::FileStructure::Transparent) {
        out.rawData = session.readTransparent(info.size);
        out.sha256 = sha256Hex(out.rawData);
        if (out.rawData.size() != info.size) {
            out.sizeMismatch = true;
            if (progress) {
                progress("WARNING: " + path + " declared " + std::to_string(info.size) +
                          " bytes but only " + std::to_string(out.rawData.size()) + " were read");
            }
        }
        return;
    }

    if (info.structure != apdu::FileStructure::LinearFixed &&
        info.structure != apdu::FileStructure::Cyclic) {
        // Unrecognized structure byte (e.g. a UICC BER-TLV EF) - GSM 11.11
        // only defines transparent/linear-fixed/cyclic, so we have no way to
        // know how to segment this file's content. Flag it rather than
        // silently emitting an empty file that looks like a genuinely-empty EF.
        out.structureUnknown = true;
        if (progress) {
            char idHex[8];
            std::snprintf(idHex, sizeof(idHex), "%04X", info.fileId);
            progress("WARNING: " + path + " (" + idHex +
                      ") has an unrecognized file structure; content could not be read");
        }
        return;
    }

    // Linear-fixed / cyclic: one record at a time, 1-indexed.
    uint16_t numRecords = info.recordLength ? (info.size / info.recordLength) : 0;
    std::vector<uint8_t> concatenated;
    for (uint16_t r = 1; r <= numRecords; ++r) {
        std::vector<uint8_t> record = session.readRecord(uint8_t(r), info.recordLength);
        if (record.size() != info.recordLength) {
            out.sizeMismatch = true;
            if (progress) {
                progress("WARNING: " + path + " record " + std::to_string(r) + " expected " +
                          std::to_string(info.recordLength) + " bytes, got " +
                          std::to_string(record.size()));
            }
        }
        concatenated.insert(concatenated.end(), record.begin(), record.end());
        out.records.push_back(std::move(record));
    }
    out.sha256 = sha256Hex(concatenated);
}

void readKnownEfs(CardSession& session,
                   const std::vector<std::pair<uint16_t, const char*>>& efs,
                   const std::vector<uint16_t>& parentIds,
                   const std::string& parentPath,
                   std::vector<ExtractedFile>& out,
                   const ProgressCallback& progress) {
    for (auto& [id, name] : efs) {
        apdu::FileInfo info = session.selectFile(id);
        if (info.type != apdu::FileType::EF) {
            apdu::ApduResponse lastResp = session.lastResponse();
            bool cleanNotFound = (lastResp.sw1 == 0x6A && lastResp.sw2 == 0x82);
            if (!cleanNotFound && progress) {
                char idHex[8];
                std::snprintf(idHex, sizeof(idHex), "%04X", id);
                progress(std::string("WARNING: SELECT ") + idHex + " (" + name +
                          ") under " + parentPath + " returned unexpected status '" +
                          apdu::describeStatusWord(lastResp.sw1, lastResp.sw2) +
                          "' (possible CLA/class incompatibility, not necessarily absent)");
            }
            continue; // not present (or not selectable) on this card
        }
        ExtractedFile file;
        file.name = name;
        file.path = parentPath + "/" + name;
        file.dfPath = parentIds;
        readSelectedEf(session, info, file, file.path, progress);
        if (progress) {
            progress("Read " + file.path);
        }
        out.push_back(std::move(file));
    }
}

void walkDfTree(CardSession& session,
                 const std::vector<catalog::DfNode>& nodes,
                 const std::vector<uint16_t>& parentIds,
                 const std::string& parentPath,
                 std::vector<ExtractedFile>& out,
                 const ProgressCallback& progress) {
    for (const auto& node : nodes) {
        std::vector<uint16_t> nodePath = parentIds;
        nodePath.push_back(node.id);

        apdu::FileInfo dfInfo = selectPath(session, nodePath);
        if (dfInfo.type != apdu::FileType::DF && dfInfo.type != apdu::FileType::MF) {
            if (progress) {
                progress(std::string(node.name) + " not present on this card, skipping");
            }
            continue;
        }

        std::string nodePathStr = parentPath + "/" + node.name;
        if (progress) {
            progress("Entering " + nodePathStr);
        }

        readKnownEfs(session, node.ownEfs, nodePath, nodePathStr, out, progress);

        std::vector<uint16_t> foundEfs;
        for (auto& [id, name] : node.ownEfs) {
            foundEfs.push_back(id);
        }
        selectPath(session, nodePath); // re-enter DF (readKnownEfs left current EF selected)
        probeUnknownEfs(session, foundEfs, nodePath, nodePathStr, out, progress);

        std::vector<uint16_t> knownChildDfs;
        for (auto& child : node.children) knownChildDfs.push_back(child.id);
        selectPath(session, nodePath); // re-enter DF (probeUnknownEfs left current EF selected)
        probeUnknownDfs(session, knownChildDfs, nodePath, nodePathStr, out, progress);

        walkDfTree(session, node.children, nodePath, nodePathStr, out, progress);
    }
}

void probeUnknownDfs(CardSession& session,
                      const std::vector<uint16_t>& alreadyFoundDfs,
                      const std::vector<uint16_t>& parentIds,
                      const std::string& parentPath,
                      std::vector<ExtractedFile>& out,
                      const ProgressCallback& progress) {
    auto isKnown = [&](uint16_t id) {
        for (uint16_t f : alreadyFoundDfs) {
            if (f == id) return true;
        }
        return false;
    };

    for (uint32_t base : {0x5F00u, 0x7F00u}) {
        for (uint32_t offset = 0; offset <= 0xFF; ++offset) {
            uint16_t id = uint16_t(base + offset);
            if (isKnown(id)) continue;

            apdu::FileInfo info = session.selectFile(id);
            if (info.type != apdu::FileType::DF && info.type != apdu::FileType::MF) {
                selectPath(session, parentIds); // restore position before trying the next id
                continue;
            }

            char idHex[8];
            std::snprintf(idHex, sizeof(idHex), "%04X", id);
            std::string dfName = std::string("UNKNOWN_DF_") + idHex;
            std::string dfPath = parentPath + "/" + dfName;
            if (progress) {
                progress("Found non-standard DF " + dfPath + " - exploring it fully");
            }

            std::vector<uint16_t> dfIdPath = parentIds;
            dfIdPath.push_back(id);

            // Fully unknown DF: no catalog EFs to read, just probe everything.
            probeUnknownEfs(session, {}, dfIdPath, dfPath, out, progress);
            selectPath(session, dfIdPath);
            probeUnknownDfs(session, {}, dfIdPath, dfPath, out, progress);

            selectPath(session, parentIds); // back to the parent before continuing the scan
        }
    }
}

void probeUnknownEfs(CardSession& session,
                      const std::vector<uint16_t>& alreadyFound,
                      const std::vector<uint16_t>& parentIds,
                      const std::string& parentPath,
                      std::vector<ExtractedFile>& out,
                      const ProgressCallback& progress) {
    auto isKnown = [&](uint16_t id) {
        for (uint16_t f : alreadyFound) {
            if (f == id) return true;
        }
        return false;
    };

    for (uint32_t base : {0x4F00u, 0x6F00u}) {
        for (uint32_t offset = 0; offset <= 0xFF; ++offset) {
            uint16_t id = uint16_t(base + offset);
            if (isKnown(id)) continue;

            apdu::FileInfo info = session.selectFile(id);
            if (info.type != apdu::FileType::EF) {
                continue;
            }

            ExtractedFile file;
            char idHex[8];
            std::snprintf(idHex, sizeof(idHex), "%04X", id);
            file.name = std::string("UNKNOWN_") + idHex;
            file.path = parentPath + "/" + file.name;
            file.dfPath = parentIds;
            readSelectedEf(session, info, file, file.path, progress);
            if (progress) {
                progress("Found non-standard file " + file.path);
            }
            out.push_back(std::move(file));
        }
    }
}

} // namespace forandsim

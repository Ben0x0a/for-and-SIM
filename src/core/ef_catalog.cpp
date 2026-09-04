#include "ef_catalog.h"

#include <unordered_map>

namespace forandsim::catalog {

// 3GPP TS 31.102 well-known USIM application AID (RID 'A0 00 00 00 87', app code '1002').
const std::vector<uint8_t> kUsimAidFallback = {0xA0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x02};

std::vector<std::pair<uint16_t, const char*>> mfEfs() {
    return {
        {kEF_ICCID, "ICCID"},
        {kEF_DIR, "DIR"},
        {kEF_PL, "PL (Preferred Languages)"},
    };
}

std::vector<std::pair<uint16_t, const char*>> alwEfsUnderDfGsm() {
    return {
        {0x6FAE, "PHASE"},
    };
}

std::vector<DfNode> gsmDfTree() {
    return {
        DfNode{0x7F10, "DF_TELECOM", {
            DfNode{0x5F50, "DF_GRAPHICS", {}, {{0x4F20, "IMG"}}},
        }, {
            {0x6F3A, "ADN"}, {0x6F3B, "FDN"}, {0x6F3C, "SMS"}, {0x6F3D, "CCP"},
            {0x6F40, "MSISDN"}, {0x6F42, "SMSP"}, {0x6F43, "SMSS"}, {0x6F44, "LND"},
            {0x6F47, "SMSR"}, {0x6F49, "SDN"}, {0x6F4A, "EXT1"}, {0x6F4B, "EXT2"},
            {0x6F4C, "EXT3"}, {0x6F4D, "BDN"}, {0x6F4E, "EXT4"},
        }},
        DfNode{0x7F20, "DF_GSM", {
            DfNode{0x5F70, "DF_SoLSA", {}, {{0x4F30, "SAI"}, {0x4F31, "SLL"}}},
            DfNode{0x5F3C, "DF_MExE", {}, {
                {0x4F40, "MExE-ST"}, {0x4F41, "ORPK"}, {0x4F42, "ARPK"}, {0x4F43, "TPRPK"},
            }},
        }, {
            {0x6F05, "LP"}, {0x6F07, "IMSI"}, {0x6F20, "Kc"}, {0x6F30, "PLMNsel"},
            {0x6F31, "HPPLMN"}, {0x6F37, "ACMmax"}, {0x6F38, "SST"}, {0x6F39, "ACM"},
            {0x6F3E, "GID1"}, {0x6F3F, "GID2"}, {0x6F41, "PUCT"}, {0x6F45, "CBMI"},
            {0x6F46, "SPN"}, {0x6F48, "CBMID"}, {0x6F74, "BCCH"}, {0x6F78, "ACC"},
            {0x6F7B, "FPLMN"}, {0x6F7E, "LOCI"}, {0x6FAD, "AD"}, {0x6FAE, "PHASE"},
            {0x6FB1, "VGCS"}, {0x6FB2, "VGCSS"}, {0x6FB3, "VBS"}, {0x6FB4, "VBSS"},
            {0x6FB5, "eMLPP"}, {0x6FB6, "AAeM"}, {0x6FB7, "ECC"}, {0x6F50, "CBMIR"},
            {0x6F51, "NIA"}, {0x6F52, "KcGPRS"}, {0x6F53, "LOCIGPRS"}, {0x6F54, "SUME"},
            {0x6F60, "PLMNwAcT"}, {0x6F61, "OPLMNwAcT"}, {0x6F62, "HPLMNAcT"},
            {0x6F63, "CPBCCH"}, {0x6F64, "INVSCAN"},
        }},
        DfNode{0x7F22, "DF_IS-41", {}, {}},
        DfNode{0x7F23, "DF_FP-CTS", {}, {}},
    };
}

std::vector<std::pair<uint16_t, const char*>> usimAdfEfs() {
    return {
        {0x6F07, "IMSI"}, {0x6F05, "LI"}, {0x6F38, "UST"}, {0x6F56, "EST"},
        {0x6F31, "HPPLMN"}, {0x6F37, "ACMmax"}, {0x6F39, "ACM"}, {0x6F3E, "GID1"},
        {0x6F3F, "GID2"}, {0x6F46, "SPN"}, {0x6F41, "PUCT"}, {0x6F45, "CBMI"},
        {0x6F78, "ACC"}, {0x6F7B, "FPLMN"}, {0x6F7E, "LOCI"}, {0x6FAD, "AD"},
        {0x6F48, "CBMID"}, {0x6FB7, "ECC"}, {0x6F50, "CBMIR"}, {0x6F73, "PSLOCI"},
        {0x6F3B, "FDN"}, {0x6F3C, "SMS"}, {0x6F3D, "CCP"}, {0x6F40, "MSISDN"},
        {0x6F42, "SMSP"}, {0x6F43, "SMSS"}, {0x6F47, "SMSR"}, {0x6F49, "SDN"},
        {0x6F4A, "EXT1"}, {0x6F4B, "EXT2"}, {0x6F4C, "EXT3"}, {0x6F4D, "BDN"},
        {0x6F4E, "EXT4"}, {0x6F3A, "ADN"}, {0x6FC3, "PSC"}, {0x6FC4, "PNN"},
        {0x6FC5, "OPL"}, {0x6FC6, "MBDN"}, {0x6FC7, "EXT6"}, {0x6FC8, "MBI"},
        {0x6FC9, "MWIS"}, {0x6FCA, "CFIS"}, {0x6FCB, "EXT7"}, {0x6FCC, "SPDI"},
        {0x6FCD, "MMSN"}, {0x6FCE, "EXT8"}, {0x6FCF, "MMSICP"}, {0x6FD0, "MMSUP"},
        {0x6FD1, "MMSUCP"},
    };
}

const char* nameFor(uint16_t fileId) {
    static const std::unordered_map<uint16_t, const char*> kNames = [] {
        std::unordered_map<uint16_t, const char*> map;
        for (auto& [id, name] : mfEfs()) map[id] = name;
        for (auto& [id, name] : usimAdfEfs()) map[id] = name;
        std::vector<DfNode> stack = gsmDfTree();
        std::vector<DfNode> queue = stack;
        for (size_t i = 0; i < queue.size(); ++i) {
            map[queue[i].id] = queue[i].name;
            for (auto& [id, name] : queue[i].ownEfs) map[id] = name;
            for (auto& child : queue[i].children) queue.push_back(child);
        }
        return map;
    }();
    auto it = kNames.find(fileId);
    return it == kNames.end() ? nullptr : it->second;
}

bool isSensitiveKeyMaterial(const std::string& fileName) {
    return fileName == "Kc" || fileName == "KcGPRS";
}

} // namespace forandsim::catalog

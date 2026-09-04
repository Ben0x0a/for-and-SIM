#include "reader_discovery.h"

#include "apdu.h"
#include "card_session.h"
#include "ef_catalog.h"

namespace forandsim {

namespace {

std::string decodeIccidBytes(const std::vector<uint8_t>& data) {
    std::string iccid;
    for (uint8_t b : data) {
        uint8_t lo = b & 0x0F, hi = (b >> 4) & 0x0F;
        if (lo <= 9) iccid += char('0' + lo);
        if (hi <= 9) iccid += char('0' + hi);
    }
    return iccid;
}

} // namespace

std::vector<ReaderWithIccid> listReadersWithIccid(PcscTransport& transport) {
    std::vector<ReaderWithIccid> result;
    for (auto& name : transport.listReaders()) {
        ReaderWithIccid info;
        info.readerName = name;
        try {
            transport.connect(name);
            CardSession session(transport);
            session.selectFile(catalog::kMF);
            apdu::FileInfo iccidInfo = session.selectFile(catalog::kEF_ICCID);
            if (iccidInfo.type == apdu::FileType::EF) {
                std::vector<uint8_t> data = session.readTransparent(iccidInfo.size);
                std::string iccid = decodeIccidBytes(data);
                if (!iccid.empty()) info.iccid = iccid;
            }
        } catch (...) {
            // No card present, or the reader/card rejected the read - leave iccid empty.
        }
        result.push_back(std::move(info));
    }
    transport.disconnect();
    return result;
}

} // namespace forandsim

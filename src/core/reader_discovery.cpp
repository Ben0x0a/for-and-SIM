// Reader discovery implementation - see reader_discovery.h.
// Used by: src/cli/cli_app.cpp, src/core/reader_monitor.cpp.
// Depends on: pcsc_transport, card_session, ef_catalog, apdu.
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

CardPresence classifyCardPresence(DWORD eventState) {
    // UNKNOWN = reader name not recognised, UNAVAILABLE = state not known,
    // IGNORE = reader skipped: none of them say anything about a card.
    if (eventState & (SCARD_STATE_UNKNOWN | SCARD_STATE_UNAVAILABLE | SCARD_STATE_IGNORE)) {
        return CardPresence::Unknown;
    }
    if (eventState & SCARD_STATE_EMPTY) return CardPresence::Absent;
    if (eventState & SCARD_STATE_PRESENT) {
        if (eventState & SCARD_STATE_MUTE) return CardPresence::Mute;
        if (eventState & SCARD_STATE_EXCLUSIVE) return CardPresence::Exclusive;
        return CardPresence::Present;
    }
    return CardPresence::Unknown;
}

std::vector<ReaderStatus> queryReaderStatuses(PcscTransport& transport) {
    std::vector<ReaderStatus> result;
    // One query per reader rather than one batched call: a batched
    // SCardGetStatusChange fails as a whole if any single reader vanished in
    // the meantime, whereas this only marks that reader Unknown.
    for (auto& name : transport.listReaders()) {
        ReaderStatus status;
        status.readerName = name;
        if (auto state = transport.readerState(name)) {
            status.presence = classifyCardPresence(state->eventState);
            status.atr = state->atr;
            status.cardEventCount = uint16_t((state->eventState >> 16) & 0xFFFF);
        }
        result.push_back(std::move(status));
    }
    return result;
}

std::optional<std::string> readIccid(PcscTransport& transport, const std::string& readerName) {
    std::optional<std::string> iccid;
    try {
        transport.connect(readerName);
        CardSession session(transport);
        session.selectFile(catalog::kMF);
        apdu::FileInfo iccidInfo = session.selectFile(catalog::kEF_ICCID);
        if (iccidInfo.type == apdu::FileType::EF) {
            std::vector<uint8_t> data = session.readTransparent(iccidInfo.size);
            std::string decoded = decodeIccidBytes(data);
            if (!decoded.empty()) iccid = decoded;
        }
    } catch (...) {
        // No card present, or the reader/card rejected the read - leave iccid empty.
    }
    transport.disconnect();
    return iccid;
}

std::vector<ReaderWithIccid> listReadersWithIccid(PcscTransport& transport) {
    std::vector<ReaderWithIccid> result;
    for (auto& status : queryReaderStatuses(transport)) {
        ReaderWithIccid info;
        info.readerName = status.readerName;
        info.presence = status.presence;
        // Unknown is still tried: if the status query is not supported, this
        // falls back to the connect-and-read probe.
        if (status.presence == CardPresence::Present || status.presence == CardPresence::Unknown) {
            info.iccid = readIccid(transport, status.readerName);
        }
        result.push_back(std::move(info));
    }
    return result;
}

} // namespace forandsim

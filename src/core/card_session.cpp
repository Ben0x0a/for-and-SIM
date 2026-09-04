#include "card_session.h"

#include <algorithm>

namespace forandsim {

CardSession::CardSession(PcscTransport& transport) : transport_(transport) {}

std::vector<uint8_t> CardSession::transmitWithGetResponse(const std::vector<uint8_t>& command) {
    std::vector<uint8_t> raw = transport_.transmit(command);
    apdu::ApduResponse resp = apdu::parseResponse(raw);

    if (resp.hasMoreDataViaGetResponse()) {
        std::vector<uint8_t> getResp = transport_.transmit(apdu::buildGetResponse(resp.sw2));
        resp = apdu::parseResponse(getResp);
    }

    lastResponse_ = resp;
    return resp.data;
}

apdu::FileInfo CardSession::selectFile(uint16_t fileId) {
    std::vector<uint8_t> header = transmitWithGetResponse(apdu::buildSelect(fileId));

    if (!lastResponse_.ok() && lastResponse_.data.empty()) {
        // SELECT failed outright (e.g. file not found) with no header payload.
        apdu::FileInfo info;
        info.fileId = fileId;
        info.type = apdu::FileType::Unknown;
        return info;
    }

    auto info = apdu::parseFileInfo(header);
    if (!info) {
        apdu::FileInfo fallback;
        fallback.fileId = fileId;
        return fallback;
    }
    return *info;
}

std::vector<uint8_t> CardSession::readTransparent(uint16_t size) {
    std::vector<uint8_t> content;
    content.reserve(size);
    uint16_t offset = 0;
    while (offset < size) {
        uint8_t chunk = uint8_t(std::min<uint16_t>(size - offset, 0xFF));
        std::vector<uint8_t> raw = transport_.transmit(apdu::buildReadBinary(offset, chunk));
        lastResponse_ = apdu::parseResponse(raw);
        if (!lastResponse_.ok()) {
            break;
        }
        content.insert(content.end(), lastResponse_.data.begin(), lastResponse_.data.end());
        offset += chunk;
    }
    return content;
}

std::vector<uint8_t> CardSession::readRecord(uint8_t recordNumber, uint8_t recordLength) {
    std::vector<uint8_t> raw =
        transport_.transmit(apdu::buildReadRecord(recordNumber, recordLength));
    lastResponse_ = apdu::parseResponse(raw);
    return lastResponse_.data;
}

ChvVerifyOutcome CardSession::verifyChv1(const std::string& pin) {
    std::vector<uint8_t> chvBytes(pin.begin(), pin.end());
    std::vector<uint8_t> raw = transport_.transmit(apdu::buildVerifyChv(0x01, chvBytes));
    lastResponse_ = apdu::parseResponse(raw);

    // Status words per GSM 11.11 clause 9.4 (SW1=0x98 "security management"):
    //   98 02  no CHV initialized
    //   98 04  bad CHV, verification failed but attempts remain
    //   98 40  verification failed, no attempts left / CHV blocked
    // Unlike EMV cards, GSM 11.11 does not encode the exact retry counter in SW2.
    ChvVerifyOutcome outcome;
    if (lastResponse_.ok()) {
        outcome.result = ChvResult::Correct;
        return outcome;
    }
    if (lastResponse_.sw1 == 0x98 && lastResponse_.sw2 == 0x02) {
        outcome.result = ChvResult::NotInitialized;
        return outcome;
    }
    if (lastResponse_.sw1 == 0x98 && lastResponse_.sw2 == 0x04) {
        outcome.result = ChvResult::Incorrect;
        return outcome;
    }
    if (lastResponse_.sw1 == 0x98 && lastResponse_.sw2 == 0x40) {
        outcome.result = ChvResult::Blocked;
        outcome.attemptsRemaining = 0;
        return outcome;
    }
    outcome.result = ChvResult::Error;
    return outcome;
}

bool CardSession::selectByAid(const std::vector<uint8_t>& aid) {
    std::vector<uint8_t> raw = transport_.transmit(apdu::buildSelectAid(aid));
    lastResponse_ = apdu::parseResponse(raw);
    return lastResponse_.ok();
}

} // namespace forandsim

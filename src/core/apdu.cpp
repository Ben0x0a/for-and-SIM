#include "apdu.h"

namespace forandsim::apdu {

std::vector<uint8_t> buildSelect(uint16_t fileId) {
    return {CLA_GSM, INS_SELECT, 0x00, 0x00, 0x02,
            uint8_t(fileId >> 8), uint8_t(fileId & 0xFF)};
}

std::vector<uint8_t> buildSelectAid(const std::vector<uint8_t>& aid) {
    std::vector<uint8_t> command{0x00, INS_SELECT, 0x04, 0x0C, uint8_t(aid.size())};
    command.insert(command.end(), aid.begin(), aid.end());
    return command;
}

std::vector<uint8_t> buildGetResponse(uint8_t length) {
    return {CLA_GSM, INS_GET_RESPONSE, 0x00, 0x00, length};
}

std::vector<uint8_t> buildReadBinary(uint16_t offset, uint8_t length) {
    return {CLA_GSM, INS_READ_BINARY, uint8_t(offset >> 8), uint8_t(offset & 0xFF), length};
}

std::vector<uint8_t> buildReadRecord(uint8_t recordNumber, uint8_t length) {
    // P2 = 0x04: "absolute/current mode", read record recordNumber.
    return {CLA_GSM, INS_READ_RECORD, recordNumber, 0x04, length};
}

std::vector<uint8_t> buildVerifyChv(uint8_t chvNumber, const std::vector<uint8_t>& chv) {
    std::vector<uint8_t> apdu{CLA_GSM, INS_VERIFY_CHV, 0x00, chvNumber, 0x08};
    std::vector<uint8_t> padded(8, 0xFF);
    for (size_t i = 0; i < chv.size() && i < 8; ++i) {
        padded[i] = chv[i];
    }
    apdu.insert(apdu.end(), padded.begin(), padded.end());
    return apdu;
}

ApduResponse parseResponse(const std::vector<uint8_t>& raw) {
    ApduResponse resp;
    if (raw.size() < 2) {
        return resp;
    }
    resp.sw1 = raw[raw.size() - 2];
    resp.sw2 = raw[raw.size() - 1];
    resp.data.assign(raw.begin(), raw.end() - 2);
    return resp;
}

std::optional<FileInfo> parseFileInfo(const std::vector<uint8_t>& header) {
    // Minimum length to reach byte 15 (record length), 1-indexed as in GSM 11.11.
    if (header.size() < 15) {
        return std::nullopt;
    }
    FileInfo info;
    info.size = (uint16_t(header[2]) << 8) | header[3];
    info.fileId = (uint16_t(header[4]) << 8) | header[5];

    switch (header[6]) {
        case 0x01: info.type = FileType::MF; break;
        case 0x02: info.type = FileType::DF; break;
        case 0x04: info.type = FileType::EF; break;
        default: info.type = FileType::Rfu; break;
    }

    uint8_t status = header[11];
    // Bit 0 clear (ALIVE=0x00/ASLEEP mask) => invalidated, per GSM 11.11 9.2.1.1.
    info.invalidated = (status & 0x01) == 0;

    if (info.type == FileType::EF) {
        switch (header[13]) {
            case 0x00: info.structure = FileStructure::Transparent; break;
            case 0x01: info.structure = FileStructure::LinearFixed; break;
            case 0x03: info.structure = FileStructure::Cyclic; break;
            default: info.structure = FileStructure::Unknown; break;
        }
        if (header.size() > 14) {
            info.recordLength = header[14];
        }
    } else if (info.type == FileType::MF || info.type == FileType::DF) {
        // GSM 11.11 clause 9.2.1.2: byte 19 (index 18) is CHV1 status.
        if (header.size() > 18) {
            uint8_t chv1Status = header[18];
            bool initialized = (chv1Status & 0x80) != 0;
            info.chv1AttemptsRemaining = initialized ? int(chv1Status & 0x0F) : -1;
        }
    }

    return info;
}

const char* describeStatusWord(uint8_t sw1, uint8_t sw2) {
    if (sw1 == 0x90 && sw2 == 0x00) return "normal ending of command";
    if (sw1 == 0x91) return "normal ending with proactive command pending";
    if (sw1 == 0x9F) return "normal ending, response data available";
    if (sw1 == 0x67) return "incorrect length (P3)";
    if (sw1 == 0x69 || sw1 == 0x98) return "command not allowed / security status not satisfied";
    if (sw1 == 0x6A && sw2 == 0x82) return "file not found";
    if (sw1 == 0x6B) return "incorrect parameters P1/P2";
    if (sw1 == 0x6D) return "unknown instruction code";
    if (sw1 == 0x6E) return "unknown instruction class";
    if (sw1 == 0x94 && sw2 == 0x04) return "file inconsistent with command / no EF selected";
    if (sw1 == 0x98 && sw2 == 0x02) return "no CHV initialized";
    if (sw1 == 0x98 && sw2 == 0x04) return "CHV verification failed, attempts remain";
    if (sw1 == 0x98 && sw2 == 0x08) return "in contradiction with CHV status";
    if (sw1 == 0x98 && sw2 == 0x10) return "in contradiction with invalidation status";
    if (sw1 == 0x98 && sw2 == 0x40) return "CHV verification failed, no attempts left / blocked";
    return "unrecognized status word";
}

} // namespace forandsim::apdu

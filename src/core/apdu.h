#pragma once

#include <cstdint>
#include <optional>
#include <vector>

// GSM 11.11 / 3GPP TS 51.011 APDU instruction and status-word constants.
namespace forandsim::apdu {

constexpr uint8_t CLA_GSM = 0xA0;

constexpr uint8_t INS_SELECT = 0xA4;
constexpr uint8_t INS_STATUS = 0xF2;
constexpr uint8_t INS_READ_BINARY = 0xB0;
constexpr uint8_t INS_UPDATE_BINARY = 0xD6;
constexpr uint8_t INS_READ_RECORD = 0xB2;
constexpr uint8_t INS_UPDATE_RECORD = 0xDC;
constexpr uint8_t INS_VERIFY_CHV = 0x20;
constexpr uint8_t INS_CHANGE_CHV = 0x24;
constexpr uint8_t INS_GET_RESPONSE = 0xC0;

// File structure byte (from response to SELECT / GET RESPONSE, byte 14).
enum class FileStructure : uint8_t {
    Transparent = 0x00,
    LinearFixed = 0x01,
    Cyclic = 0x03,
    Unknown = 0xFF,
};

// File type byte (byte 7 of the SELECT response).
enum class FileType : uint8_t {
    Rfu = 0x00,
    MF = 0x01,
    DF = 0x02,
    EF = 0x04,
    Unknown = 0xFF,
};

struct ApduResponse {
    std::vector<uint8_t> data; // payload without SW1/SW2
    uint8_t sw1 = 0;
    uint8_t sw2 = 0;

    bool ok() const { return sw1 == 0x90 && sw2 == 0x00; }
    // 0x9F/0x61: normal ending, SW2 gives the number of bytes available via GET RESPONSE.
    bool hasMoreDataViaGetResponse() const { return sw1 == 0x9F || sw1 == 0x61; }
    uint16_t sw() const { return (uint16_t(sw1) << 8) | sw2; }
};

// Parsed "response to SELECT" / GET RESPONSE header (GSM 11.11 clause 9.2.1).
struct FileInfo {
    uint16_t fileId = 0;
    FileType type = FileType::Unknown;
    FileStructure structure = FileStructure::Unknown;
    uint16_t size = 0;       // bytes 3-4: file size (EF) or free space (DF/MF)
    uint8_t recordLength = 0; // byte 15, meaningful for linear-fixed/cyclic EFs
    bool invalidated = false; // derived from byte 12 status
    // CHV1 status, only populated for MF/DF selects with a long-enough header
    // (byte 19: bit8 = initialized, bits1-4 = remaining verify attempts).
    // -1 = CHV1 not initialized (no PIN set), nullopt = not present in this
    // response (e.g. selecting a plain EF).
    std::optional<int> chv1AttemptsRemaining;
};

std::vector<uint8_t> buildSelect(uint16_t fileId);

// SELECT by AID (ISO 7816-4 / ETSI TS 102.221): CLA=0x00 (not the classic GSM
// class), INS=SELECT, P1=0x04 "select by DF name", P2=0x0C "no response data"
// (so no FCP template to parse - success is just SW=9000). Used to enter an
// application ADF such as the USIM, found via its AID in EF_DIR.
std::vector<uint8_t> buildSelectAid(const std::vector<uint8_t>& aid);

std::vector<uint8_t> buildGetResponse(uint8_t length);
std::vector<uint8_t> buildReadBinary(uint16_t offset, uint8_t length);
std::vector<uint8_t> buildReadRecord(uint8_t recordNumber, uint8_t length);
std::vector<uint8_t> buildVerifyChv(uint8_t chvNumber, const std::vector<uint8_t>& chv);

// Parses a raw PC/SC response buffer (payload + trailing SW1 SW2) into an ApduResponse.
ApduResponse parseResponse(const std::vector<uint8_t>& raw);

// Interprets the header bytes returned by SELECT/GET RESPONSE into a FileInfo.
// Returns std::nullopt if the header is too short to be a valid GSM file header.
std::optional<FileInfo> parseFileInfo(const std::vector<uint8_t>& header);

// Human-readable description of a status word, for logs/manifest.
const char* describeStatusWord(uint8_t sw1, uint8_t sw2);

} // namespace forandsim::apdu

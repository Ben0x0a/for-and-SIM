#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "apdu.h"
#include "pcsc_transport.h"

namespace forandsim {

enum class ChvResult {
    Correct,
    NotInitialized,
    Incorrect,   // wrong PIN, attempts remain
    Blocked,     // no attempts left
    Error,
};

struct ChvVerifyOutcome {
    ChvResult result;
    int attemptsRemaining = -1; // -1 if unknown
};

// High-level GSM/USIM card operations built on top of PcscTransport + apdu::.
// Not thread-safe; one CardSession per acquisition run.
class CardSession {
public:
    explicit CardSession(PcscTransport& transport);

    // SELECT a file by its 2-byte GSM file ID, relative to the currently
    // selected DF (or absolute if selecting MF 0x3F00 first).
    // Returns the parsed FileInfo; throws on transmit failure, but a
    // "file not found" status is returned as ok()==false via lastStatus().
    apdu::FileInfo selectFile(uint16_t fileId);

    // Status word of the most recent selectFile()/verifyChv1() call.
    apdu::ApduResponse lastResponse() const { return lastResponse_; }

    // Reads the full contents of the currently-selected transparent EF.
    std::vector<uint8_t> readTransparent(uint16_t size);

    // Reads one record of the currently-selected linear-fixed/cyclic EF.
    std::vector<uint8_t> readRecord(uint8_t recordNumber, uint8_t recordLength);

    // Verifies CHV1 (PIN1). `pin` is the ASCII digits, e.g. "1234".
    ChvVerifyOutcome verifyChv1(const std::string& pin);

    // SELECTs an application by its AID (e.g. the USIM ADF found via EF_DIR),
    // using CLA=0x00 rather than the classic GSM class. Returns true on
    // success (SW=9000); check lastResponse() for the failure status word.
    bool selectByAid(const std::vector<uint8_t>& aid);

private:
    std::vector<uint8_t> transmitWithGetResponse(const std::vector<uint8_t>& command);

    PcscTransport& transport_;
    apdu::ApduResponse lastResponse_;
};

} // namespace forandsim

// Reader discovery: which PC/SC readers exist, whether each holds a card, and
// the card's ICCID.
// Used by: src/cli/cli_app.cpp (--list-readers), src/core/reader_monitor.cpp
// (GUI background polling).
// Depends on: pcsc_transport.h, card_session.h, ef_catalog.h, apdu.h.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pcsc_transport.h"

namespace forandsim {

// Card state of a reader, derived from SCardGetStatusChange flags - known
// without connecting to the card.
enum class CardPresence {
    Absent,    // reader is empty
    Present,   // card inserted and answered reset; may be connected to
    Mute,      // card inserted but did not answer reset (dirty/broken/upside down)
    Exclusive, // card held in exclusive mode by another application
    Unknown,   // the PC/SC service could not report the state
};

// Maps SCARD_STATE_* flags to a CardPresence.
CardPresence classifyCardPresence(DWORD eventState);

struct ReaderStatus {
    std::string readerName;
    CardPresence presence = CardPresence::Unknown;
    std::vector<uint8_t> atr;
    // Card insertion/removal counter (upper 16 bits of dwEventState), on
    // platforms that report one - pcsc-lite documents it; 0 where absent.
    // Lets a card swapped between two polls be told apart from the old one.
    uint16_t cardEventCount = 0;
};

// Lists every reader with its card state. Fast: never connects to a card.
std::vector<ReaderStatus> queryReaderStatuses(PcscTransport& transport);

// Connects to `readerName` and does a read-only ICCID read (SELECT MF,
// SELECT EF_ICCID, READ BINARY, decode) - never a PIN. nullopt if there is
// no card or it rejected the read. Leaves `transport` disconnected.
std::optional<std::string> readIccid(PcscTransport& transport, const std::string& readerName);

struct ReaderWithIccid {
    std::string readerName;
    CardPresence presence = CardPresence::Unknown;
    std::optional<std::string> iccid; // nullopt if no card present or ICCID unreadable
};

// Lists every PC/SC reader and, for each one that may hold a card, its
// ICCID. Lets a caller show "ICCID — reader name" instead of just the reader
// name, which is the whole point when more than one reader/card is plugged
// in at once. Readers reported empty are not connected to, so they cost
// nothing. Leaves `transport` disconnected when done.
std::vector<ReaderWithIccid> listReadersWithIccid(PcscTransport& transport);

} // namespace forandsim

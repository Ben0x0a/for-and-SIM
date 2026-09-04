#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pcsc_transport.h"

namespace forandsim {

struct ReaderWithIccid {
    std::string readerName;
    std::optional<std::string> iccid; // nullopt if no card present or ICCID unreadable
};

// Lists every PC/SC reader and, for each, attempts a quick read-only ICCID
// read (SELECT MF, SELECT EF_ICCID, READ BINARY, decode) - never a PIN. Lets
// a caller show "ICCID — reader name" instead of just the reader name, which
// is the whole point when more than one reader/card is plugged in at once.
// Leaves `transport` disconnected when done.
std::vector<ReaderWithIccid> listReadersWithIccid(PcscTransport& transport);

} // namespace forandsim

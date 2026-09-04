#include "pcsc_transport.h"

#include <cstdio>

namespace forandsim {

std::string PcscError::toHex(long code) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08lX", (unsigned long)code);
    return buf;
}

PcscTransport::PcscTransport() {
    LONG rv = SCardEstablishContext(SCARD_SCOPE_SYSTEM, nullptr, nullptr, &context_);
    if (rv != SCARD_S_SUCCESS) {
        throw PcscError("failed to establish PC/SC context", rv);
    }
    contextValid_ = true;
}

PcscTransport::~PcscTransport() {
    if (connected_) {
        SCardDisconnect(card_, SCARD_LEAVE_CARD);
    }
    if (contextValid_) {
        SCardReleaseContext(context_);
    }
}

std::vector<std::string> PcscTransport::listReaders() const {
    DWORD size = 0;
    LONG rv = SCardListReaders(context_, nullptr, nullptr, &size);
    if (rv == (LONG)SCARD_E_NO_READERS_AVAILABLE) {
        return {};
    }
    if (rv != SCARD_S_SUCCESS) {
        throw PcscError("failed to query reader list size", rv);
    }

    std::vector<char> buffer(size);
    rv = SCardListReaders(context_, nullptr, buffer.data(), &size);
    if (rv != SCARD_S_SUCCESS) {
        throw PcscError("failed to list readers", rv);
    }

    std::vector<std::string> readers;
    const char* p = buffer.data();
    while (*p) {
        readers.emplace_back(p);
        p += readers.back().size() + 1;
    }
    return readers;
}

void PcscTransport::connect(const std::string& readerName) {
    if (connected_) {
        disconnect();
    }
    LONG rv = SCardConnect(context_, readerName.c_str(), SCARD_SHARE_SHARED,
                            SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1, &card_, &activeProtocol_);
    if (rv != SCARD_S_SUCCESS) {
        throw PcscError("failed to connect to reader '" + readerName + "'", rv);
    }
    connected_ = true;
}

void PcscTransport::disconnect() {
    if (connected_) {
        SCardDisconnect(card_, SCARD_LEAVE_CARD);
        connected_ = false;
    }
}

std::vector<uint8_t> PcscTransport::atr() const {
    if (!connected_) {
        throw std::runtime_error("PcscTransport::atr() called before connect()");
    }
    DWORD state = 0, protocol = 0, atrLen = 32;
    std::vector<uint8_t> atrBuf(atrLen);
    DWORD readerLen = 0;
    LONG rv = SCardStatus(card_, nullptr, &readerLen, &state, &protocol, atrBuf.data(), &atrLen);
    if (rv == (LONG)SCARD_E_INSUFFICIENT_BUFFER) {
        atrBuf.resize(atrLen);
        rv = SCardStatus(card_, nullptr, &readerLen, &state, &protocol, atrBuf.data(), &atrLen);
    }
    if (rv != SCARD_S_SUCCESS) {
        throw PcscError("failed to read ATR", rv);
    }
    atrBuf.resize(atrLen);
    return atrBuf;
}

std::vector<uint8_t> PcscTransport::transmit(const std::vector<uint8_t>& apdu) const {
    if (!connected_) {
        throw std::runtime_error("PcscTransport::transmit() called before connect()");
    }

    const SCARD_IO_REQUEST* sendPci =
        (activeProtocol_ == SCARD_PROTOCOL_T1) ? SCARD_PCI_T1 : SCARD_PCI_T0;

    std::vector<uint8_t> response(258); // max short APDU response + SW1 SW2
    DWORD responseLen = (DWORD)response.size();

    LONG rv = SCardTransmit(card_, sendPci, apdu.data(), (DWORD)apdu.size(), nullptr,
                             response.data(), &responseLen);
    if (rv != SCARD_S_SUCCESS) {
        throw PcscError("APDU transmit failed", rv);
    }
    response.resize(responseLen);
    return response;
}

} // namespace forandsim

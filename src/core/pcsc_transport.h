#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winscard.h>
#else
#include <PCSC/winscard.h>
#include <PCSC/wintypes.h>
#endif

namespace forandsim {

// Thrown for any PC/SC-level failure (context, reader, connect, transmit).
class PcscError : public std::runtime_error {
public:
    PcscError(const std::string& what, long code)
        : std::runtime_error(what + " (0x" + toHex(code) + ")"), code_(code) {}
    long code() const { return code_; }

private:
    static std::string toHex(long code);
    long code_;
};

// Raw card state of one reader, as reported by SCardGetStatusChange.
struct ReaderState {
    DWORD eventState = 0;     // SCARD_STATE_* flags; pcsc-lite also packs a
                              // card insertion/removal counter in the upper 16 bits
    std::vector<uint8_t> atr; // empty when no card, or not reported
};

// Thin RAII wrapper around a PC/SC context + connection to a single reader/card.
class PcscTransport {
public:
    PcscTransport();
    ~PcscTransport();

    PcscTransport(const PcscTransport&) = delete;
    PcscTransport& operator=(const PcscTransport&) = delete;

    // Names of all readers currently visible to the PC/SC subsystem.
    std::vector<std::string> listReaders() const;

    // Current card state of one reader, WITHOUT connecting to it or sending
    // the card any APDU: answered by the PC/SC service from its own state, so
    // it returns immediately. nullopt if the service could not report it
    // (e.g. the reader was unplugged between listReaders() and this call).
    std::optional<ReaderState> readerState(const std::string& readerName) const;

    // Connects to the card in the given reader (shared mode, T=0 protocol).
    void connect(const std::string& readerName);

    void disconnect();

    bool isConnected() const { return connected_; }

    // Raw ATR bytes of the connected card.
    std::vector<uint8_t> atr() const;

    // Sends one APDU and returns the raw response (including trailing SW1 SW2).
    std::vector<uint8_t> transmit(const std::vector<uint8_t>& apdu) const;

private:
    SCARDCONTEXT context_ = 0;
    SCARDHANDLE card_ = 0;
    DWORD activeProtocol_ = 0;
    bool contextValid_ = false;
    bool connected_ = false;
};

} // namespace forandsim

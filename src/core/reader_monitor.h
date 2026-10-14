// ReaderMonitor: background thread that keeps an up-to-date list of PC/SC
// readers, their card state, and each card's ICCID, so a UI can show a card
// the moment it is detected and fill in its ICCID once read. Also defines
// pickReader(), the pure policy deciding which reader a UI should select.
// Used by: src/gui/main_gui.cpp, tests/reader_monitor_test.cpp.
// Depends on: reader_discovery.h, pcsc_transport.h.
#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "reader_discovery.h"

namespace forandsim {

// How often readers and card states are re-checked. A status check never
// touches a card, so a short interval costs nothing but a local IPC call.
constexpr std::chrono::milliseconds kReaderPollInterval{1000};

// ICCID read attempts per card insertion. A card can refuse the first read
// while it is still being reset, or while the OS probes it right after
// insertion; the cap stops a card that never answers from being hammered.
constexpr int kMaxIccidReadsPerInsertion = 3;

enum class IccidState {
    NotApplicable, // no readable card (Absent / Mute / Exclusive)
    Pending,       // card detected, ICCID not read yet
    Read,
    Unreadable,    // every attempt failed
};

struct MonitoredReader {
    std::string readerName;
    CardPresence presence = CardPresence::Unknown;
    IccidState iccidState = IccidState::NotApplicable;
    std::string iccid; // set when iccidState == Read
};

struct ReaderSnapshot {
    bool scanned = false; // false until the first poll has completed
    std::vector<MonitoredReader> readers;
    std::optional<std::string> error; // PC/SC service unavailable, etc.
};

class ReaderMonitor {
public:
    // `cardIoMutex` must be held by every other code path in the process that
    // talks to a card (acquisition, PIN check). The monitor only try-locks it
    // and skips ICCID reads while it is taken, because in SCARD_SHARE_SHARED
    // mode its SELECTs would otherwise interleave with theirs on the same card
    // and silently change the file they are reading.
    explicit ReaderMonitor(std::mutex& cardIoMutex);
    ~ReaderMonitor(); // stops and joins the thread

    ReaderMonitor(const ReaderMonitor&) = delete;
    ReaderMonitor& operator=(const ReaderMonitor&) = delete;

    void start();

    // Forgets every cached ICCID and polls immediately.
    void rescan();

    // Copies the latest snapshot into `out` and returns true if it changed
    // since the previous call; returns false (leaving `out` alone) otherwise.
    bool takeSnapshotIfChanged(ReaderSnapshot& out);

private:
    struct CardCacheEntry {
        std::vector<uint8_t> atr;
        uint16_t cardEventCount = 0;
        int reads = 0; // since the reader was last seen empty
        bool done = false;
        std::optional<std::string> iccid;
    };

    void loop();
    void pollOnce(bool forgetCache);
    void publish(const std::vector<ReaderStatus>& statuses, std::optional<std::string> error);

    std::mutex& cardIoMutex_;
    std::map<std::string, CardCacheEntry> cache_; // monitor thread only

    std::mutex mutex_; // guards everything below
    std::condition_variable wake_;
    bool stop_ = false;
    bool rescanRequested_ = false;
    bool changed_ = false;
    ReaderSnapshot snapshot_;

    std::thread thread_;
};

// Index of the reader a UI should select, or -1 if there are none.
// - A reader the user picked explicitly stays selected while it is plugged in.
// - Otherwise the current selection stays if it holds a card, so the
//   selection never hops between two cards on its own.
// - Otherwise the first reader with a readable card is selected: with a
//   built-in reader listed first and the SIM in a USB reader, the USB one wins.
int pickReader(const std::vector<MonitoredReader>& readers, const std::string& currentName,
               bool userPicked);

} // namespace forandsim

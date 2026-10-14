// ReaderMonitor and pickReader implementation - see reader_monitor.h.
// Used by: src/gui/main_gui.cpp, tests/reader_monitor_test.cpp.
// Depends on: reader_discovery, pcsc_transport.
#include "reader_monitor.h"

#include <exception>

namespace forandsim {

namespace {

bool mayHoldReadableCard(CardPresence p) {
    // Unknown is included so a platform whose status query fails still gets
    // the connect-and-read probe (the pre-monitor behaviour).
    return p == CardPresence::Present || p == CardPresence::Unknown;
}

} // namespace

ReaderMonitor::ReaderMonitor(std::mutex& cardIoMutex) : cardIoMutex_(cardIoMutex) {}

ReaderMonitor::~ReaderMonitor() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void ReaderMonitor::start() {
    if (!thread_.joinable()) thread_ = std::thread(&ReaderMonitor::loop, this);
}

void ReaderMonitor::rescan() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rescanRequested_ = true;
    }
    wake_.notify_all();
}

bool ReaderMonitor::takeSnapshotIfChanged(ReaderSnapshot& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!changed_) return false;
    out = snapshot_;
    changed_ = false;
    return true;
}

void ReaderMonitor::loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    bool forget = true;
    while (!stop_) {
        lock.unlock();
        pollOnce(forget);
        lock.lock();
        wake_.wait_for(lock, kReaderPollInterval, [this] { return stop_ || rescanRequested_; });
        forget = rescanRequested_;
        rescanRequested_ = false;
    }
}

void ReaderMonitor::pollOnce(bool forgetCache) {
    if (forgetCache) cache_.clear();

    std::vector<ReaderStatus> statuses;
    try {
        // A fresh PC/SC context every poll: on Windows the smart-card service
        // may only start once a reader is plugged in, and a context opened
        // before that stays unusable - reusing one would never see the reader.
        PcscTransport transport;
        statuses = queryReaderStatuses(transport);

        // Drop cache entries of unplugged readers and removed cards, and
        // invalidate entries whose card changed (different ATR, or a new
        // insertion counted by the platform) without being seen removed.
        std::map<std::string, CardCacheEntry> kept;
        for (auto& s : statuses) {
            if (s.presence == CardPresence::Absent) continue;
            CardCacheEntry entry;
            auto it = cache_.find(s.readerName);
            if (it != cache_.end()) entry = it->second;
            if (entry.atr != s.atr || entry.cardEventCount != s.cardEventCount) {
                // `reads` is kept on purpose: if a platform bumped the counter
                // on each of our own connects, resetting it would re-read the
                // card forever.
                entry.atr = s.atr;
                entry.cardEventCount = s.cardEventCount;
                entry.done = false;
                entry.iccid.reset();
            }
            kept[s.readerName] = entry;
        }
        cache_ = std::move(kept);

        // Publish card presence BEFORE any ICCID read, so a UI shows "card
        // detected" right away instead of after the (slower) card I/O.
        publish(statuses, std::nullopt);

        for (auto& s : statuses) {
            if (!mayHoldReadableCard(s.presence)) continue;
            CardCacheEntry& entry = cache_[s.readerName];
            if (entry.done) continue;

            std::unique_lock<std::mutex> io(cardIoMutex_, std::try_to_lock);
            if (!io.owns_lock()) break; // a card is in use (acquisition / PIN check): next poll

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_ || rescanRequested_) return;
            }
            entry.iccid = readIccid(transport, s.readerName);
            ++entry.reads;
            entry.done = entry.iccid.has_value() || entry.reads >= kMaxIccidReadsPerInsertion;
            io.unlock();
            publish(statuses, std::nullopt);
        }
    } catch (const std::exception& e) {
        publish({}, std::string(e.what()));
    }
}

void ReaderMonitor::publish(const std::vector<ReaderStatus>& statuses,
                            std::optional<std::string> error) {
    ReaderSnapshot snap;
    snap.scanned = true;
    snap.error = std::move(error);
    for (auto& s : statuses) {
        MonitoredReader r;
        r.readerName = s.readerName;
        r.presence = s.presence;
        if (mayHoldReadableCard(s.presence)) {
            auto it = cache_.find(s.readerName);
            if (it == cache_.end() || !it->second.done) {
                r.iccidState = IccidState::Pending;
            } else if (it->second.iccid) {
                r.iccidState = IccidState::Read;
                r.iccid = *it->second.iccid;
            } else {
                r.iccidState = IccidState::Unreadable;
            }
        }
        snap.readers.push_back(std::move(r));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = std::move(snap);
    changed_ = true;
}

int pickReader(const std::vector<MonitoredReader>& readers, const std::string& currentName,
               bool userPicked) {
    // Unknown presence (status query unsupported) only counts as a card once
    // the fallback connect-and-read found an ICCID.
    auto readableCard = [](const MonitoredReader& r) {
        return r.presence == CardPresence::Present ||
               (r.presence == CardPresence::Unknown && r.iccidState == IccidState::Read);
    };
    // Mute / Exclusive cards are still cards: keep them selected rather than
    // hop away, so the user sees why that one cannot be read.
    auto anyCard = [&](const MonitoredReader& r) {
        return readableCard(r) || r.presence == CardPresence::Mute ||
               r.presence == CardPresence::Exclusive;
    };

    int current = -1;
    for (int i = 0; i < (int)readers.size(); ++i) {
        if (readers[i].readerName == currentName) current = i;
    }
    if (current >= 0 && (userPicked || anyCard(readers[current]))) return current;
    for (int i = 0; i < (int)readers.size(); ++i) {
        if (readableCard(readers[i])) return i;
    }
    if (current >= 0) return current;
    return readers.empty() ? -1 : 0;
}

} // namespace forandsim

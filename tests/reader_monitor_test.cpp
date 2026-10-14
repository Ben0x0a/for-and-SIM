// Checks the hardware-free parts of reader discovery: SCARD_STATE_* flag
// classification and the pickReader() selection policy (notably the
// "built-in empty reader listed first, SIM in the USB reader" case).
// Exits non-zero on any check failure.
// Depends on: src/core/reader_discovery.h, src/core/reader_monitor.h.
#include <cstdio>

#include "reader_monitor.h"

using namespace forandsim;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

MonitoredReader reader(const char* name, CardPresence presence,
                       IccidState iccidState = IccidState::NotApplicable) {
    MonitoredReader r;
    r.readerName = name;
    r.presence = presence;
    r.iccidState = iccidState;
    return r;
}

} // namespace

int main() {
    check(classifyCardPresence(SCARD_STATE_EMPTY) == CardPresence::Absent, "EMPTY -> Absent");
    check(classifyCardPresence(SCARD_STATE_PRESENT) == CardPresence::Present, "PRESENT -> Present");
    check(classifyCardPresence(SCARD_STATE_PRESENT | SCARD_STATE_INUSE) == CardPresence::Present,
          "PRESENT|INUSE (shared) -> Present");
    check(classifyCardPresence(SCARD_STATE_PRESENT | SCARD_STATE_MUTE) == CardPresence::Mute,
          "PRESENT|MUTE -> Mute");
    check(classifyCardPresence(SCARD_STATE_PRESENT | SCARD_STATE_EXCLUSIVE) ==
              CardPresence::Exclusive,
          "PRESENT|EXCLUSIVE -> Exclusive");
    check(classifyCardPresence(SCARD_STATE_UNAVAILABLE) == CardPresence::Unknown,
          "UNAVAILABLE -> Unknown");
    check(classifyCardPresence(0) == CardPresence::Unknown, "no flags -> Unknown");
    check(classifyCardPresence(0x00030000 | SCARD_STATE_PRESENT) == CardPresence::Present,
          "event counter in upper bits is ignored");

    const std::vector<MonitoredReader> builtinEmptyUsbCard = {
        reader("Built-in", CardPresence::Absent),
        reader("USB", CardPresence::Present, IccidState::Pending),
    };
    check(pickReader(builtinEmptyUsbCard, "", false) == 1,
          "first start: the reader holding a card is selected, not slot 0");
    check(pickReader(builtinEmptyUsbCard, "Built-in", false) == 1,
          "auto-selected empty reader yields to a newly detected card");
    check(pickReader(builtinEmptyUsbCard, "Built-in", true) == 0,
          "an explicit user pick is kept even without a card");

    const std::vector<MonitoredReader> twoCards = {
        reader("A", CardPresence::Present, IccidState::Read),
        reader("B", CardPresence::Present, IccidState::Read),
    };
    check(pickReader(twoCards, "B", false) == 1, "selection does not hop between two cards");

    const std::vector<MonitoredReader> unknownStates = {
        reader("A", CardPresence::Unknown, IccidState::Unreadable),
        reader("B", CardPresence::Unknown, IccidState::Read),
    };
    check(pickReader(unknownStates, "A", false) == 1,
          "status query unsupported: falls back to the reader whose ICCID was read");

    const std::vector<MonitoredReader> noCards = {
        reader("A", CardPresence::Absent),
        reader("B", CardPresence::Absent),
    };
    check(pickReader(noCards, "B", false) == 1, "no card anywhere: current selection kept");
    check(pickReader(noCards, "gone", true) == 0, "unplugged selection: first reader");
    check(pickReader({}, "A", true) == -1, "no readers: nothing selected");

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all reader monitor checks passed\n");
    return 0;
}

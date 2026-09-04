# Glossary

Terms and abbreviations used throughout For&SIM, its code, and its output. Grouped by
topic rather than alphabetically, since most of them only make sense next to their
neighbors.

## The card itself

- **SIM** (Subscriber Identity Module) - the original GSM-era smart card standard
  (3GPP TS 51.011 / formerly GSM 11.11) that identifies a subscriber to a mobile
  network and stores network/phonebook/SMS data.
- **USIM** (Universal SIM) - the 3G/4G/5G evolution of the SIM, defined by 3GPP TS
  31.102. It runs as an *application* on a UICC rather than being the whole card.
- **UICC** (Universal Integrated Circuit Card) - the physical/logical smart card
  platform that can host one or more applications (a USIM, an ISIM for VoLTE, etc.).
  In casual usage "SIM card" today usually means "UICC running a USIM".
- **ICC** (Integrated Circuit Card) - the generic smart-card term; SIM/USIM/UICC are
  all specific kinds of ICC.
- **ATR** (Answer To Reset) - the first bytes the card sends after power-up,
  identifying its communication parameters (protocol, timing, historical bytes).
  Captured verbatim in every acquisition as a card "fingerprint".

## The filesystem on the card

SIM/USIM cards expose their data as a small tree-structured filesystem, navigated
with `SELECT` commands, much like folders and files on a disk:

- **MF** (Master File) - the root of the filesystem, id `0x3F00`.
- **DF** (Dedicated File) - a "directory": a file that contains other files (DFs or
  EFs) rather than data. `DF_GSM` and `DF_TELECOM` are the two standard top-level
  DFs under MF on a classic SIM.
- **EF** (Elementary File) - a "file" that actually holds data: either **transparent**
  (a flat byte blob, like `EF_ICCID`), **linear-fixed** (a table of fixed-size
  records, like `EF_ADN` phonebook entries), or **cyclic** (a fixed-size ring buffer
  of records, like call-cost counters).
- **ADF** (Application Dedicated File) - a DF that hosts a specific application (e.g.
  the USIM application), selected by its **AID** rather than a plain file id.
- **AID** (Application Identifier) - a longer, globally-unique id (RID + PIX) used to
  `SELECT` an application (ADF) rather than a simple 2-byte file id.
- **File id** - every MF/DF/EF has a 2-byte identifier (e.g. `0x6F07` for `EF_IMSI`);
  For&SIM's file catalog (`ef_catalog.h`) maps these ids to human-readable names.

## Values commonly found on the card

- **ICCID** (Integrated Circuit Card Identifier) - the card's own serial number.
  Readable without any PIN; the only thing For&SIM extracts in "no PIN" mode.
- **IMSI** (International Mobile Subscriber Identity) - identifies the subscriber to
  the network (country + operator + subscriber number). PIN-protected.
- **MSISDN** - the phone number associated with the card, if the operator wrote it
  (not always populated).
- **SPN** (Service Provider Name) - the operator name the phone displays.
- **PLMN** (Public Land Mobile Network) - a network identifier; `EF_PLMNsel`/
  `EF_FPLMN`/`EF_HPLMN` list preferred/forbidden/home networks.
- **LOCI** (Location Information) - the last known cell/location area the card
  registered on - potentially forensically significant.
- **ADN/FDN/SDN/BDN** - phonebook-type EFs: Abbreviated Dialling Numbers, Fixed
  Dialling Numbers, Service Dialling Numbers, Barred Dialling Numbers.
- **SMS/SMSP/SMSS/SMSR** - stored text messages and related SMS parameters/status.
- **ACC/ECC** - Access Control Class and Emergency Call Codes.

## PIN / security

- **CHV1 / CHV2** (Card Holder Verification 1/2) - the technical names for what a
  phone UI calls "PIN1" and "PIN2". CHV1 gates most user data; this tool only ever
  touches CHV1.
- **PUK** (PIN Unblock Key) - unblocks a CHV that's hit 0 remaining attempts. For&SIM
  never uses a PUK - if a card is blocked, it stays blocked.
- **Retry counter / attempts remaining** - the card tracks how many wrong CHV1
  guesses are left (usually starting at 3); it hits 0 and blocks the card if
  exhausted. For&SIM reads this counter before ever attempting a PIN.

## Talking to the card

- **APDU** (Application Protocol Data Unit) - the message format for every
  command/response exchanged with a smart card: `CLA INS P1 P2 [Lc data] [Le]` out,
  `[data] SW1 SW2` back.
  - **CLA** - instruction class (which command set; this tool uses the classic GSM
    class `0xA0`).
  - **INS** - the instruction itself (`SELECT`, `READ BINARY`, `VERIFY CHV`, ...).
  - **P1/P2** - instruction-specific parameters.
  - **Lc/Le** - length of data sent / length of data expected back.
  - **SW1/SW2** - the 2-byte status word ending every response (`90 00` = success;
    others indicate errors, security conditions, or "more data available").
- **PC/SC** (Personal Computer/Smart Card) - the standard cross-platform API for
  talking to a smart-card reader (`WinSCard` on Windows, `PCSC-lite` on Linux/macOS).
  For&SIM's `pcsc_transport.*` wraps this.
- **CCID** (Chip Card Interface Device) - the standard USB class most smart-card
  readers implement, meaning the OS's built-in driver usually just works with no
  vendor driver install.
- **T=0 / T=1** - the two ISO 7816 byte-transmission protocols a card can use; SIMs
  traditionally use T=0.
- **BER-TLV** - a tag-length-value binary encoding some newer UICC files use instead
  of the classic fixed GSM 11.11 layout; For&SIM doesn't decode these (see the
  `structure_unknown` flag in the report).

## Integrity / evidence handling

- **SHA-256** - the cryptographic hash algorithm used throughout For&SIM to fingerprint
  every extracted file and the evidence container itself, so any later change is
  detectable.
- **Chain of custody** - the documented, unbroken record of who handled evidence and
  when; here, the timestamps/workstation/hashes recorded in the manifest and report.
- **CoC** - common shorthand for "chain of custody".

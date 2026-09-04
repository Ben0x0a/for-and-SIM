# How SIM/USIM storage actually works

This is the "wait, what's a file?" explainer. It's aimed at someone comfortable
with computers in general but new to smart cards specifically. See
[GLOSSARY.md](GLOSSARY.md) for quick abbreviation lookups and
[REPORT.md](REPORT.md) for what For&SIM's output means field by field.

## It's a tiny computer, not a memory card

A SIM isn't a passive storage chip you can just read bytes off of, the way you'd
read a USB drive. It's a small computer: a CPU, a bit of RAM, and persistent
storage, running its own operating system (a "smart card OS"). The only way to
get anything out of it is to *ask* it, one command at a time, and wait for an
answer - there's no direct memory access from the outside at all. That's the
whole reason a tool like For&SIM exists: it's a conversation with the card, not
a disk image.

## The conversation: APDUs

Every single interaction is one **APDU** (Application Protocol Data Unit) out,
and one response back. An outgoing APDU looks like:

```
CLA  INS  P1  P2  [Lc  data]  [Le]
```

- **CLA** - which command set you're speaking (For&SIM mostly uses the classic
  GSM class, `0xA0`).
- **INS** - the actual instruction: `SELECT`, `READ BINARY`, `READ RECORD`,
  `VERIFY CHV`, and so on. There is no "list files" or "read whole card"
  instruction - you can only read what you already know the address of.
- **P1/P2** - parameters specific to that instruction.
- **Lc/data** - "length of data I'm sending" plus that data, for commands that
  send something (like a PIN).
- **Le** - "how many bytes I expect back."

The response is whatever data was requested, followed by a 2-byte **status
word** (`SW1 SW2`) - `90 00` means "it worked"; anything else is some flavor of
error, security rejection, or "there's more data, ask again." For&SIM's
`apdu.h`/`apdu.cpp` are nothing more than functions that build these exact byte
sequences and interpret the status words that come back.

## The filesystem model: MF / DF / EF

Since you can't "list files," how do you know what to ask for? The card
exposes a small, tree-shaped filesystem, standardized decades ago (ISO 7816-4,
then GSM 11.11 / 3GPP TS 51.011 for the SIM-specific layout) and every
compliant card follows it:

```
MF (3F00)                       <- the root, "Master File"
├── EF_ICCID (2FE2)             <- a file directly under the root
├── EF_DIR   (2F00)
├── EF_PL    (2F05)
├── DF_TELECOM (7F10)           <- a "directory"
│   ├── EF_ADN (6F3A)           <- phonebook entries
│   ├── EF_SMS (6F3C)           <- stored text messages
│   └── DF_GRAPHICS (5F50)      <- a directory nested inside a directory
│       └── EF_IMG (4F20)
└── DF_GSM (7F20)
    ├── EF_IMSI (6F07)
    ├── EF_Kc   (6F20)
    └── ...
```

- **MF** (Master File) - the root. Every SIM has exactly one, at a fixed
  address `0x3F00`.
- **DF** (Dedicated File) - despite the name, this is a *directory*: a
  container for other DFs/EFs, not something with content of its own.
- **EF** (Elementary File) - an actual *file* with content. This is where real
  data lives - a name, a number, a key, a list of records.

Every MF/DF/EF has a fixed 2-byte address (`0x6F07` for `EF_IMSI`, etc.),
defined by the standard - these aren't arbitrary, they're the same on every
compliant card. For&SIM's `ef_catalog.h` is just that standard's address book,
translated into code: id → human-readable name → where it lives in the tree.

To read anything, you first `SELECT` it (tell the card "make this the current
file"), then send a read command against whatever's currently selected -
`READ BINARY` for a flat file, or `READ RECORD` for a file organized as a table
of fixed-size rows (like a phonebook, one row per contact). You can't jump
straight to `EF_IMSI` from nowhere: you select `MF`, then `DF_GSM`, then
`EF_IMSI`, in that order, the same way you'd `cd` into nested folders before
opening a file (this is exactly what `file_walker.cpp`'s `selectPath()` does).

## Why "PIN-protected" is a per-file property, not a card-wide switch

Here's the part that surprises people: the PIN doesn't "unlock the card." It
unlocks specific commands *on specific files*. Every file has an **access
condition** attached to each operation it supports (mainly READ and UPDATE):

- `ALW` - always allowed, no PIN needed. A handful of files are like this on
  purpose - `EF_ICCID` (the card's own serial number), `EF_DIR` (what
  applications are installed), `EF_PL`/`EF_PHASE` (basic info a phone needs
  before it can even prompt for a PIN).
- `CHV1` - needs PIN1 verified first. This is the overwhelming majority of
  actually-interesting files: IMSI, phonebook, SMS, and so on.
- `CHV2`, `ADM`, `NEV` - other tiers (second PIN, administrative-only, never
  from outside) that For&SIM doesn't interact with.

`SELECT` itself is (almost) never access-controlled - you can navigate the
tree freely. It's the *read* that gets rejected with a security status word if
the file needs a PIN you haven't verified. That's why, in For&SIM's "no PIN"
mode, it can still successfully `SELECT` and read a small set of `ALW` files
(see the README's "What it does" section) but gets a rejection on everything
else - the tree is visible either way, only the *contents* are gated, file by
file.

## SIM vs USIM: two filesystems, one card

An older "SIM" *is* this MF/DF/EF tree directly. A modern "USIM" card
additionally runs the tree above as one *application* among possibly several,
each identified by a globally-unique **AID** (Application Identifier) rather
than a plain 2-byte id. To get into the USIM application specifically, you
`SELECT` it by AID (a different, longer form of `SELECT`), which then exposes
its own tree of USIM-specific files (3GPP TS 31.102) - some genuinely new
(like `EF_EPSLOCI`, the LTE location record), some duplicates of the classic
GSM ones for backward compatibility. For&SIM discovers the USIM's AID by
reading `EF_DIR` (which lists installed applications) and falls back to the
well-known standard USIM AID if that fails - see `acquisition_engine.cpp`'s
`discoverUsimAid()`.

## Putting it together

When you run a full acquisition, For&SIM is doing exactly the manual process
above, just automated and logged: select MF, read the always-open files,
verify the PIN, then repeatedly select-and-read down the classic tree
(`file_walker.cpp`'s `walkDfTree()`), then (if present) select the USIM
application by AID and do the same for its tree. Every single `SELECT`/`READ`
in that whole process is what ends up as one line in the acquisition log.

# For&SIM

A SIM/USIM forensic acquisition tool for digital forensics education, originally built
for a classroom setting but usable by anyone who wants to learn how SIM/USIM forensic
acquisition works.

Repository: [https://github.com/Ben0x0a/for-SIM](https://github.com/Ben0x0a/for-SIM)

> **Educational tool — not for real cases.** This project was built for teaching how
> SIM/USIM forensic acquisition works. It has not been validated against real forensic
> tooling standards (e.g. NIST CFTT) and should not be used to examine evidence in an
> actual investigation.

## What it does

- Connects to a SIM/USIM card through any standard PC/SC USB smart-card reader.
- Always reads the ICCID first, without a PIN.
- If a PIN (CHV1) is supplied and verified, performs a full acquisition: walks the
  classic GSM DF tree (`DF_TELECOM`, `DF_GSM` and their sub-DFs) reading every
  catalog elementary file, plus a brute-force probe of the non-standard EF-id
  ranges (`0x4Fxx`/`0x6Fxx`) *and* the DF-id ranges (`0x5Fxx`/`0x7Fxx`) at every
  level, recursively exploring any undocumented DF it finds.
- Produces per acquisition:
  - `<case>.zip` - the evidence container: raw bytes of every acquired file under
    `files/`, a `values.json` with decoded values for the handful of EFs that are
    pure values (ICCID, IMSI), a `manifest.json` with case metadata, tool
    provenance, chain of custody and a SHA-256 per file, and a human-readable
    `for-and-sim-meta.txt` summary of all of the above (itself hashed and recorded
    in the manifest).
  - `<case>.zip.sha256` - a sidecar file with the SHA-256 of the zip itself. This
    can't live inside the zip (writing it in would change the zip and invalidate
    the hash), so it's external, same as disk-imaging tools do.
  - `<case>.html` - a standalone, self-contained chain-of-custody report (case
    info, timestamps, workstation, tool version/repo, the zip's own hash,
    per-file hashes, extracted-file tree, full acquisition log).

  See [docs/REPORT.md](docs/REPORT.md) for a field-by-field explanation of
  everything in the zip and the HTML report, and
  [docs/GLOSSARY.md](docs/GLOSSARY.md) for every abbreviation used (SIM/USIM,
  MF/DF/EF, ICCID/IMSI, CHV/PUK, APDU/SW1-SW2, ATR, PC/SC, and more).

## Read-only guarantee

This tool never issues an `UPDATE BINARY`, `UPDATE RECORD`, `INVALIDATE` or
`REHABILITATE` command — those APDU builders simply don't exist in `apdu.cpp`.
The one unavoidable exception is `VERIFY CHV` (PIN check): presenting a PIN is
inherently a card-state operation, since a wrong PIN decrements the card's own
retry counter. To make that safe and transparent:

- a `--check-pin` pre-flight (CLI flag, or the GUI's "Check PIN status" button) reads
  the CHV1 retry counter **without ever verifying**, so an operator can see how many
  attempts remain and decide whether to proceed, before committing to a guess;
- the same retry counter is read and logged again immediately before the actual
  verify, with a loud warning if only 0-1 attempts remain;
- after the full acquisition, ICCID is re-read and its hash compared against the
  very first read, as a lightweight always-on integrity check;
- a full verification pass — **on by default**, opt out with `--no-verify` (CLI) or
  by unchecking "Verify" (GUI) — re-reads **every** acquired file and diffs its hash
  against the first read: the real read-only analogue of a "hash before/after"
  check, since there's no way to hash "the whole card" the way one hashes a disk
  image. It roughly doubles acquisition time, same tradeoff as a verify pass in any
  imaging tool.
- all of the above are recorded in the manifest, `for-and-sim-meta.txt`, the HTML
  report, and the acquisition log.

Two more safeguards, unrelated to the PIN:

- **Authorization gate**: acquisition refuses to even connect to the card unless
  the operator explicitly attests authorization (`--confirm-authorized` / the GUI
  checkbox). This refusal is itself recorded if it happens.
- **Anomaly flags**: a file with an unrecognized structure byte, or a read that
  returned fewer bytes than declared, is flagged (`structure_unknown` /
  `size_mismatch` in the manifest, a warning in the log and HTML report) instead
  of silently appearing as an empty/complete file. A `SELECT` failure with a
  status word other than the ordinary "file not found" is also logged as a
  warning, since it can indicate a CLA/class incompatibility rather than genuine
  absence.

## Building

Requires CMake 3.16+ and a C++17 compiler.

```
cmake -S . -B build -DFORANDSIM_BUILD_GUI=ON
cmake --build build --config Release
```

- `FORANDSIM_BUILD_GUI` (default `ON`) requires SDL2; if SDL2 isn't found, the
  build automatically falls back to a CLI-only binary with a warning.
- On Windows the build uses the static CRT (`/MT`) so the shipped `.exe` needs no
  Visual C++ Redistributable install — students only need the single `.exe` file
  (plus SDL2 if it's dynamically linked; use a static SDL2 triplet, e.g. via
  vcpkg's `sdl2:x64-windows-static`, to keep it a single file, as CI does).
- macOS/Linux dev machines can compile-check the PC/SC code without a physical
  reader: macOS ships `PCSC.framework` (Windows-API-compatible headers) and Linux
  needs `libpcsclite-dev`.

Run the output-layer smoke test (no hardware needed):

```
ctest --test-dir build --output-on-failure
```

## Usage

For a full walkthrough (build → find your reader → check the PIN safely → acquire →
read the report), see [docs/TUTORIAL.md](docs/TUTORIAL.md).

### GUI (default, no arguments)

```
forandsim
```

![For&SIM GUI](docs/screenshots/gui-overview.png)

Check "I am authorized to examine this exhibit", fill in case identifier,
piece/exhibit number, operator, notes and output directory, pick a reader,
optionally click "Check PIN status" first, then either enter the PIN or check
"Extract without PIN (ICCID only)", and click Start. "Verify" is checked by
default (uncheck it for a quicker but less thoroughly confirmed acquisition).

### CLI (headless)

```
forandsim --list-readers
forandsim --check-pin --reader "ACS ACR38U-CCID 0"
forandsim --reader "ACS ACR38U-CCID 0" --case CASE-2026-014 --piece P1 \
          --operator "J. Examiner" --output ./out --confirm-authorized --pin 1234
forandsim --reader "ACS ACR38U-CCID 0" --case CASE-2026-014 --piece P1 \
          --operator "J. Examiner" --output ./out --confirm-authorized --no-pin --no-verify
```

## Ethics note

Only use this tool on SIM/USIM cards you are authorized to examine (test cards
provided for a course/lab, or cards you own). Entering a wrong PIN decrements the
card's retry counter and can permanently block a real card without its PUK. This
tool is for learning how SIM/USIM forensic acquisition works — do not use it on
real evidence.

## Known limitations

- AID-based `SELECT` of the USIM Application DF (3GPP TS 31.102, needs `EF_DIR`
  parsing and a `CLA=0x00` select) is not implemented yet; the classic
  MF/DF_GSM/DF_TELECOM tree that every SIM/USIM answers to is what gets walked
  (see `catalog::usimAdfEfs()`, currently unused).
- The brute-force probes only scan the conventional EF (`0x4Fxx`/`0x6Fxx`) and DF
  (`0x5Fxx`/`0x7Fxx`) ranges per level, not the full 16-bit id space; a full scan
  would be far slower for little additional coverage. This does mean a file
  deliberately hidden outside those conventional ranges would be missed.
- If a `SCardTransmit` fails mid-walk (card removed, reader glitch), the whole
  acquisition throws and nothing is written to disk — a partial acquisition is
  not currently preserved with an "incomplete" flag.
- Workstation timestamps are the local system clock, not validated against any
  trusted time source (acceptable for an offline educational tool, but worth
  knowing if this were ever used for real casework).

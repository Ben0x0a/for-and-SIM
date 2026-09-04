# Understanding the report

Every acquisition produces four things. This document explains every field found in
each of them. See [GLOSSARY.md](GLOSSARY.md) for what the abbreviations mean.

```
<case>.zip            evidence container
<case>.zip.sha256     sidecar hash of the zip above
<case>.html           human-readable report (open in any browser)
```

Inside the zip:

```
files/<DF path>/<name>.bin   raw bytes of every acquired file
values.json                  decoded values for "pure value" files
manifest.json                machine-readable record of everything below
for-and-sim-meta.txt         human-readable summary of the manifest
```

## The HTML report, section by section

- **Case information** - what you typed in before acquiring: case identifier,
  piece/exhibit number, operator, and whether authorization was confirmed (see
  below). "Authorization confirmed = No" means the acquisition was refused outright
  and no data was collected.
- **Acquisition refused** (only shown if authorization wasn't confirmed) - states
  plainly that the tool never connected to the card.
- **Tool provenance** - which build of For&SIM produced this report (name, version,
  source repository link), so a report can always be traced back to the exact tool
  that generated it.
- **Evidence container** - the zip's filename and its own SHA-256. This hash is
  *not* inside the zip (writing it in would change the zip's bytes and invalidate
  the very hash being recorded), so it's shown here and in the `.sha256` sidecar
  file next to the zip. Verify it with `shasum -a 256 -c <case>.zip.sha256`.
- **Chain of custody** - when the acquisition started/finished, which workstation
  (hostname + logged-in user) and which reader performed it, the card's raw ATR, its
  ICCID, the acquisition mode (`ICCID only` vs `Full dump`), and - if a PIN was
  used - the verification result and how many CHV1 attempts were left *before* the
  attempt was made.
- **Read-only / integrity guarantee** - explains that no write command is ever
  issued (see the main README's "Read-only guarantee" section), reports the ICCID
  re-read check (first read vs. a second read taken after the whole acquisition -
  a "hash before/after" style check using the one file readable both with and
  without a PIN), and, if requested, the full verification pass result: every file
  re-read and hash-compared, with any mismatches listed.
- **Extracted files** - one row per file actually read off the card: its path in the
  filesystem, its 2-byte file id, its structure (transparent/linear-fixed/cyclic),
  size, SHA-256, any decoded value (ICCID/IMSI), and a **Flags** column noting
  `structure unknown` (the file's structure byte wasn't one GSM 11.11 defines, so
  its content couldn't be reliably segmented - see BER-TLV in the glossary) or
  `size mismatch` (a read returned fewer bytes than the file declared, meaning the
  content shown for it may be incomplete).
- **Acquisition log** - the full, timestamped-by-order sequence of every step taken:
  every SELECT, every warning (unexpected status words, non-standard files found,
  CHV1 attempt counts, verification results). This is the most detailed record and
  is worth reading end to end for anything unusual.

## `manifest.json`

The same information as the HTML report, structured for machine parsing:

- `tool` - name, version, repository URL of the build that produced this acquisition.
- `case` - case identifier, piece number, operator, notes, and
  `authorization_confirmed` (boolean - see the README's "Ethics note").
- `chain_of_custody` - ISO-8601 UTC timestamps, workstation hostname/user, reader name.
- `card` - `atr_hex` (space-separated hex bytes) and `iccid`.
- `acquisition` - `mode` (`"iccid_only"` or `"full_dump"`), `pin_attempted`,
  `pin_result` (`correct` / `incorrect` / `blocked` / `not_initialized` / `error`),
  and `chv1_attempts_before_verify` (the retry counter read *before* the PIN was
  tried; `null` if it couldn't be determined).
- `integrity` - `read_only_acquisition` (always `true`), the ICCID re-read hashes and
  whether they matched, and, if requested, `full_verify_performed` and
  `full_verify_mismatches` (a list of file paths whose re-read hash differed from
  the first read - should normally be empty).
- `files` - one object per acquired file: `path`, `file_id` (decimal; the HTML report
  shows the same value in hex), `name`, `sha256`, `size_bytes`, `structure_unknown`,
  `size_mismatch`.
- `meta_file` - the name and SHA-256 of `for-and-sim-meta.txt`, so that file's
  integrity is also checkable from the manifest.
- `log` - the full acquisition log as a plain array of strings, in order.

## `values.json`

A flat map of file path to decoded value, for the handful of files whose entire
content is one human-meaningful value rather than a structure worth keeping as raw
bytes - currently ICCID and IMSI. Every acquired file still gets its raw bytes under
`files/` regardless; this is a convenience view on top of that.

## `for-and-sim-meta.txt`

A plain-text rendering of most of `manifest.json`, meant to be readable without any
tooling (e.g. printed, or opened in Notepad). It intentionally does **not** include
the zip's own hash - see "Evidence container" above for why that's impossible to
embed - and says so explicitly.

## `files/` layout inside the zip

Each acquired file is stored at `files/<its filesystem path>.bin`, e.g.
`files/MF/DF_GSM/IMSI.bin`. For linear-fixed/cyclic files, all records are
concatenated in record order into that one `.bin` file (record boundaries aren't
marked in the raw bytes - `manifest.json`'s `size_bytes` combined with the file's
known record structure, per 3GPP TS 51.011/31.102, lets you split them back out).

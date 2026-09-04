# Understanding the report

Every acquisition produces two things. This document explains every field found in
each of them. See [GLOSSARY.md](GLOSSARY.md) for what the abbreviations mean.

Both live in `<output dir>/<case>/` - a dedicated subfolder per case, so
running several acquisitions into the same output directory never mixes their
files together:

```
<output dir>/<case>/<case>.zip     evidence container
<output dir>/<case>/<case>.html    human-readable report (open in any browser)
```

Inside the zip:

```
files/<DF path>/<name>.bin   raw bytes of every acquired file (except sensitive
                              key material - see "Sensitive values" below)
values.json                  fixed set of decoded identity values, always present
manifest.json                machine-readable record of everything below
for-and-sim-meta.txt         human-readable summary of the manifest
```

## The HTML report

A table of contents at the top links every section below. Sections in order:

### Case information

What you typed in before acquiring: case identifier, piece/exhibit number,
operator, and whether authorization was confirmed. "Authorization confirmed =
No" means the acquisition was refused outright and no data was collected (see
the next section, only shown in that case).

### Acquisition refused

Only shown if authorization wasn't confirmed - states plainly that the tool
never connected to the card.

### Tool provenance

Which build of For&SIM produced this report: name, version, source repository
link, the **platform** it ran on (e.g. `macOS arm64`, `Windows x64`), and the
**SHA-256 of the executable itself** - so a report can be traced back to the
exact binary that produced it, not just a version string that doesn't change
between rebuilds.

### Acquisition results

What the acquisition actually produced, in two subsections:

- **Container information** - the zip's filename and its own SHA-256. This
  hash is *not* inside the zip (writing it in would change the zip's bytes and
  invalidate the very hash being recorded), so it's recorded here and in
  `manifest.json` instead - there's no separate sidecar file.
- **Extracted files** - one row per file actually read off the card: its path in
  the filesystem, its 2-byte file id, its structure (transparent/linear-fixed/
  cyclic), size, SHA-256, any decoded value, and a **Flags** column noting:
  - `structure unknown` - the file's structure byte wasn't one GSM 11.11
    defines, so its content couldn't be reliably segmented (see BER-TLV in the
    glossary).
  - `size mismatch` - a read returned fewer bytes than the file declared,
    meaning the content shown for it may be incomplete.
  - `cryptographic key material - content withheld from disk` - this file
    (`EF_Kc`/`EF_KcGPRS`) was detected and hashed, but its raw content was
    deliberately never written anywhere on disk - see the README's "Sensitive
    values" section. The SHA-256 shown is still real and can be used to
    confirm/match the key without the key itself ever leaving the card.

### Extraction results

A fixed set of identity fields (ICCID, IMSI, MSISDN, SPN, FPLMN, MCC, MNC, LAC,
TAC, CID), always listed in the same order regardless of acquisition mode, each
with a Value, a Status, and a Note that defines what the field actually means
(so this table is self-explanatory without GLOSSARY.md open alongside it).
Status is one of:

- `found` - decoded successfully; value shown.
- `present on card, not decoded` - the file was read but this tool doesn't
  (yet) decode its content into a value.
- `not read (no PIN)` - acquisition was ICCID-only; this field needs a PIN.
- `not present on card` - a full acquisition was done but this file/value
  wasn't found or the card doesn't have it.
- `not accessible` (TAC/CID only) - these require `EF_EPSLOCI` under the USIM
  ADF, which needs an AID-based `SELECT` this tool doesn't implement (see the
  README's known limitations); the Note column explains why.

MCC/MNC/LAC come from `EF_LOCI` (last registered GSM/UMTS cell); TAC (LTE
tracking area) and CID (cell id) are not present in that classic file at all.

### Chain of custody & integrity

Merges the "who/when/where" record with the read-only guarantees, since they're
both about trusting what happened during acquisition:

- When the acquisition started/finished, which workstation (hostname + logged-in
  user) and which reader performed it, the card's raw ATR, its ICCID, the
  acquisition mode (`ICCID only` vs `Full dump`), whether the USIM ADF was
  selected (only shown in full-dump mode - "yes" means USIM-specific EFs were
  also walked, "no" means either this card has no USIM application or its AID
  couldn't be found), and whether the operator cancelled it early (partial
  results).
- If a PIN was used: a redacted note that a PIN was entered (`****** —
  educational purpose, PIN value not disclosed`), the verification result, and
  how many CHV1 attempts were left *before* the attempt was made. Nothing
  PIN-related appears at all if no PIN was entered.
- That no write command is ever issued (see the main README's "Read-only
  guarantee" section), the ICCID re-read check (first read vs. a second read
  taken after the whole acquisition - a "hash before/after" style check using
  the one file readable both with and without a PIN), and, if requested, the
  full verification pass result: every file re-read and hash-compared, with any
  mismatches listed.

### Acquisition log

The full, timestamped sequence of every step taken: every SELECT, every warning
(unexpected status words, non-standard files found, CHV1 attempt counts,
verification results). This is the most detailed record and is worth reading
end to end for anything unusual.

## `manifest.json`

The same information as the HTML report, structured for machine parsing:

- `tool` - `name`, `version`, `repository_web`, `platform`, and
  `executable_sha256` (hash of the running executable; `null` if it couldn't
  be determined) of the build that produced this acquisition.
- `case` - case identifier, piece number, operator, notes, and
  `authorization_confirmed` (boolean - see the README's "Ethics note").
- `chain_of_custody` - ISO-8601 UTC timestamps, workstation hostname/user, reader name.
- `card` - `atr_hex` (space-separated hex bytes) and `iccid`.
- `acquisition` - `mode` (`"iccid_only"` or `"full_dump"`), `usim_adf_selected`
  (only meaningful in full-dump mode), `cancelled` (true if the operator
  stopped it early), `pin_attempted`, `pin_value` (the fixed redacted string if
  `pin_attempted`, else `null` - the real PIN is never recorded anywhere),
  `pin_result` (`correct` / `incorrect` / `blocked` / `not_initialized` /
  `error`), and `chv1_attempts_before_verify` (the retry counter read *before*
  the PIN was tried; `null` if it couldn't be determined).
- `integrity` - `read_only_acquisition` (always `true`), the ICCID re-read hashes and
  whether they matched, and, if requested, `full_verify_performed` and
  `full_verify_mismatches` (a list of file paths whose re-read hash differed from
  the first read - should normally be empty).
- `files` - one object per acquired file: `path`, `file_id` (decimal; the HTML report
  shows the same value in hex), `name`, `sha256`, `size_bytes`, `structure_unknown`,
  `size_mismatch`, `sensitive` and `content_withheld` (both true only for
  `EF_Kc`/`EF_KcGPRS` - `size_bytes`/`sha256` are still real, but the file's raw
  bytes are not in the zip under `files/`).
- `meta_file` - the name and SHA-256 of `for-and-sim-meta.txt`, so that file's
  integrity is also checkable from the manifest.
- `log` - the full acquisition log as a plain array of strings, in order.

## `values.json`

```json
{
  "key_results": {
    "ICCID": {"value": "8933...", "status": "found", "path": "MF/ICCID"},
    "IMSI":  {"value": "", "status": "not read (no PIN)", "path": null},
    "TAC":   {"value": "", "status": "not accessible", "path": null}
  }
}
```

One entry per field described in the HTML report's "Extraction results" section
above (`value`, `status`, and `path` - the file path it came from, or `null` if
there wasn't one). Every non-sensitive acquired file still gets its raw bytes
under `files/` regardless of whether it made it into this fixed list.

## `for-and-sim-meta.txt`

A plain-text rendering of most of `manifest.json`, meant to be readable without any
tooling (e.g. printed, or opened in Notepad). It intentionally does **not** include
the zip's own hash - see "Container information" above for why that's impossible to
embed - and says so explicitly. If any cryptographic key material was found, it
says how many files and reiterates that their content wasn't written to disk.

## `files/` layout inside the zip

Each acquired file is stored at `files/<its filesystem path>.bin`, e.g.
`files/MF/DF_GSM/IMSI.bin` - **except** `EF_Kc`/`EF_KcGPRS`, which are hashed
and listed in `manifest.json` but never get a `.bin` entry at all. For
linear-fixed/cyclic files, all records are concatenated in record order into
that one `.bin` file (record boundaries aren't marked in the raw bytes -
`manifest.json`'s `size_bytes` combined with the file's known record structure,
per 3GPP TS 51.011/31.102, lets you split them back out).

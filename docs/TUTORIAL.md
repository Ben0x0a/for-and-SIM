# Tutorial: your first acquisition

This walks through building For&SIM, doing a "no PIN" acquisition, then a full
PIN-verified one, and reading the result. See [GLOSSARY.md](GLOSSARY.md) for any
term that isn't explained inline, and [REPORT.md](REPORT.md) for the full field
reference once you have a report in front of you.

## 0. What you need

- A standard USB PC/SC smart-card reader (any CCID-class reader; no vendor driver
  needed on a normal Windows/macOS/Linux install).
- A SIM/USIM card you are authorized to test with (a course-provided test card, an
  old personal card, or a blank/test UICC). **Do not use this on a card that isn't
  yours to examine.**
- CMake 3.16+ and a C++17 compiler.

## 1. Build it

```
git clone https://github.com/Ben0x0a/for-SIM.git
cd for-SIM
cmake -S . -B build -DFORANDSIM_BUILD_GUI=ON
cmake --build build --config Release
```

If SDL2 isn't installed, the build automatically falls back to a CLI-only binary
(you'll see a CMake warning) - the GUI is a convenience, not a requirement. See the
main [README.md](../README.md#building) for platform-specific notes (Windows static
CRT build, macOS/Linux dependencies).

Sanity-check the build without any hardware:

```
ctest --test-dir build --output-on-failure
./build/forandsim --help
```

## 2. Plug in the reader and find it

Insert your reader, then the card, and run:

```
./build/forandsim --list-readers
```

You should see one line per connected reader, e.g.:

```
ACS ACR38U-CCID 0
```

If you see "No PC/SC readers found", check the reader is plugged in and (on Linux)
that `pcscd` is running. If two readers are connected, both are listed - you pick
which one to use with `--reader "<exact name>"` (CLI) or the dropdown (GUI).

## 3. A quick, PIN-free look: ICCID only

Before risking a PIN attempt, it's worth confirming the reader/card talk to each
other at all. Reading the ICCID never requires a PIN:

```
./build/forandsim --reader "ACS ACR38U-CCID 0" --case DEMO-001 --piece P1 \
  --operator "Your Name" --output ./out --confirm-authorized --no-pin
```

- `--confirm-authorized` is mandatory - it's your attestation that you're allowed to
  examine this card. Without it the tool refuses to even connect (see the README's
  "Read-only guarantee" section for why this and other safeguards exist).
- `--no-pin` means "don't even try a PIN, just read what's publicly available"
  (currently: just the ICCID).

You'll get `./out/DEMO-001.zip`, `./out/DEMO-001.zip.sha256`, and
`./out/DEMO-001.html`. Open the `.html` file in a browser - with no PIN, most
sections will be short (one extracted file: ICCID), which is expected.

## 4. Check the PIN before risking it

If your card has a PIN and you know it, don't just try it blind - a wrong guess
decrements the card's retry counter, and 3 wrong guesses (the usual default) blocks
the card until someone enters its PUK. Check first:

```
./build/forandsim --check-pin --reader "ACS ACR38U-CCID 0"
```

This only performs a `SELECT`, never a `VERIFY CHV` - it cannot cause a wrong-PIN
lockout. You'll see something like:

```
CHV1 attempts remaining: 3
```

or, if the card has no PIN set at all:

```
CHV1 is not initialized (no PIN set on this card).
```

If the count is 1 or 0, stop and think - do not guess.

## 5. A full acquisition

Once you're confident in the PIN:

```
./build/forandsim --reader "ACS ACR38U-CCID 0" --case DEMO-001 --piece P1 \
  --operator "Your Name" --output ./out --confirm-authorized --pin 1234
```

(Verification is on by default - it re-reads every file afterward to confirm
nothing changed; add `--no-verify` if you want a faster, less-confirmed run.)

Watch the console output: it prints every step as it happens (`[forandsim] ...`
lines) - selecting MF, verifying the PIN, walking `DF_TELECOM`/`DF_GSM`, probing for
non-standard files, the verification pass, and finally the paths to your output
files plus the zip's own SHA-256.

This can take anywhere from a few seconds to a couple of minutes: the brute-force
probing (looking for files the standard catalog doesn't know about) and the
verification pass both mean many APDU round-trips to the card.

## 6. Doing the same thing in the GUI

Run `./build/forandsim` with no arguments. Fill in the form top to bottom:

1. Pick your reader from the dropdown (click "Refresh" if it's not listed).
2. Check **"I am authorized to examine this exhibit"** - the Start button stays
   disabled until you do.
3. Fill in case identifier, piece/exhibit number, operator, and optionally notes.
4. Set the output directory: type a path, click "Browse..." for a native folder
   picker, or drag and drop a folder onto the window.
5. Optionally click **"Check PIN status"** first (same as step 4 above).
6. Either type the PIN, or check **"Extract without PIN (ICCID only)"**.
7. Leave **"Verify"** checked (it's on by default) unless you want a quicker run.
8. Click **Start acquisition** and watch the log panel at the bottom.
9. When it's done, click **"Open output folder"**.

## 7. Reading the result

Open `<case>.html` in any browser. Work through it top to bottom:

- **Case information / Tool provenance / Evidence container** - confirms who ran
  this, with what tool version, and gives you the zip's hash to verify later.
- **Chain of custody** - when, where, on what workstation, with what reader.
- **Read-only / integrity guarantee** - shows the PIN attempt count *before* it was
  used, and the verification pass result (should say every file matched).
- **Extracted files** - every file read off the card, its hash, and its decoded
  value where applicable (ICCID, IMSI). Check the **Flags** column for
  `structure unknown` or `size mismatch` - these mean that particular file's
  content might be incomplete or non-standard, and are worth a closer look.
- **Acquisition log** - the full blow-by-blow; read it if anything above looks odd.

For exactly what every field/section/JSON key means, see
[REPORT.md](REPORT.md).

## 8. Verifying the evidence container later

Anyone who receives just the `.zip` and its `.sha256` sidecar can confirm nothing
was altered since acquisition:

```
shasum -a 256 -c DEMO-001.zip.sha256
```

Unzip it, and every individual file's hash is listed in `manifest.json` (and in
human-readable form in `for-and-sim-meta.txt`) if you want to check a single
extracted file rather than the whole container.

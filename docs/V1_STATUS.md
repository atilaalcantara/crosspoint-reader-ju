# CrossPoint V1 — status and user workflow

**Target:** XTEINK X4 Pro. Reader, Highlights + Notes, and offline Study V1 are
implemented in the main firmware. This document consolidates the shipped host
and simulator scope; it does not claim physical-device validation.

**Hardware validation pending — X4 Pro currently unavailable.**

## What is included

- EPUB library/reader with pagination, reader menu, settings, and reading
  progress.
- Text highlights with word-range selection, navigation, removal, and an
  optional UTF-8 note per highlight. Notes use the existing on-screen keyboard.
- Offline Study decks with parent/subdeck navigation, front/back, Again/Hard/
  Good/Easy ratings, a deterministic local scheduler, and restart checkpoints.
- A local Python `.apkg` → `.cps` converter. Anki v3 packages require the host
  `zstd` executable; legacy SQLite packages use Python's standard library.

## Add a Study deck

### On the computer

1. In Anki Desktop, select the deck or parent/subtree and export an **Anki Deck
   Package (`.apkg`)**. Scheduling data is not needed. For the standard modern
   Anki v3 export, keep **Support older Anki versions** unchecked and install
   `zstd` if it is not already on `PATH`. Alternatively, check that option to
   export a legacy SQLite package without the `zstd` requirement.
2. Convert locally from the firmware repository:

   ```bash
   python3 tools/study-converter/convert.py deck.apkg deck.cps
   python3 tools/study-converter/convert.py deck.apkg deck.cps --deck "Parent::Subdeck"
   ```

   The second form selects that named deck and its descendants. Review the
   converter's skipped-card/media summary before copying the result. The
   firmware does not import APKG files or Anki scheduling data.

### Copy to the X4 Pro SD card

Study packages live as direct files in `/study/`; a subfolder beneath `/study/`
is not scanned.

- **USB MSC:** on Home, choose **File Transfer → USB Drive**. This mode exposes
  the SD card itself at its root, not a separate transfer partition. Create
  `/study/` on the card if needed, copy the `.cps` there, then safely eject the
  card in the host operating system. The firmware returns to Home after the
  host disconnects.
- **Wi-Fi File Transfer:** choose **File Transfer → Join Network** or **Create
  Hotspot**, open the URL shown on the device, then use **Files** to create
  `/study/` and upload the `.cps`. The web server has no authentication; use a
  trusted private network or the device hotspot. See [`webserver.md`](webserver.md).

Opening **Study** creates `/study/` if it does not exist, then scans its direct
entries for `.cps` files. After copying, return to Home and enter Study again;
the catalog is rebuilt on each entry, so no polling or separate refresh action
is required. If the SD path cannot be created/opened, the screen reports an SD
error with a Back action. An empty folder explains the `.cps` and Anki-convert
workflow and also provides Back.

The Study package is immutable. Opening it creates sidecars beside it:

```text
/study/<name>.cps       # package, content only
/study/<name>.state     # per-card local scheduler state
/study/<name>.session   # resumable card/face checkpoint
```

## Other V1 data and architecture

- **Reader/library:** `src/activities/library/`, `src/activities/reader/`,
  `lib/Epub/`; file/cache conventions are documented in [`file-formats.md`](file-formats.md).
- **Highlights + Notes:** sidecars live under
  `/.crosspoint/highlights/`; identity uses the book path, and offsets are
  visible text codepoints rather than page numbers. Details and limits:
  [`highlights-v1-format.md`](highlights-v1-format.md) and
  [`HIGHLIGHTS_NOTES_STATUS.md`](HIGHLIGHTS_NOTES_STATUS.md).
- **Study:** `.cps` v1 package plus separate `.state` and `.session` contracts:
  [`study-v1-format.md`](study-v1-format.md),
  [`STUDY_V1_STATUS.md`](STUDY_V1_STATUS.md), and
  [`STUDY_CONVERTER.md`](STUDY_CONVERTER.md).
- **Touch/UI:** new list screens use `UiListActivity` and FreeInkUI; see
  [`contributing/touch-and-ui.md`](contributing/touch-and-ui.md).

Study scheduling is local and intentionally not FSRS/Anki-compatible. Images,
audio, and LaTeX are not displayed by Study V1. Advanced template filters,
AnkiWeb/sync, and a converter GUI remain out of scope.

## Validation completed without hardware

- Real APKG v3 ENARE inspection/conversion: **949 notes, 1,039 source cards;
  1,036 converted, 3 malformed Cloze cards skipped explicitly**. The full CPS
  and the 30-card Virologia subtree were byte-deterministic and accepted by the
  C++ `StudyPackage` parser.
- Converter tests: **9/9**; host CTest: **420/420**.
- `pio run -e simulator_x4`: **PASS**.
- `pio run -e x4pro`: **PASS** (build only; no flash).
- Integrated SDL smoke used one SD root containing the EPUB fixture, the
  Highlights + Notes sidecar, and the ENARE `.cps` package. It exercised Reader
  entry, a long-press/drag highlight, save/list/navigation/removal, note entry
  and cancel, simulator-process restart, Study subdeck/session/rating, return
  to Reader, and reopening the highlight list. The simulated host file copy was
  placed into `/study/` between simulator runs; this confirms fresh catalog
  discovery on Study entry, not the physical USB transport. The initial Library
  index was built while the fixture EPUB was at the SD root, before `/study/` was
  added: the current SDL storage shim cannot seek back into a directory handle
  during recursive LibraryBuilder resume, so this smoke does not validate a
  recursive library rebuild with `/study/` present.
- The simulator fixture also verified automatic creation of a missing `/study/`
  directory and the visible SD error for a conflicting non-directory path.
- Before/after screenshots are saved under ignored build artifacts in
  `build/study-ux/before/`, `build/study-ux/after/`, and integrated-flow captures
  in `build/onboarding-v1/screenshots/`.

## Physical X4 Pro checklist

- [ ] **Reader:** open EPUB, turn pages, open menu, verify refresh and ghosting.
- [ ] **Highlights:** long-press/drag precision, GT911 hit targets, navigate and
      remove a highlight.
- [ ] **Notes:** on-screen keyboard entry, editing, cancellation, and SD save.
- [ ] **Study:** navigate ENARE parent/subdecks, scroll a long card, reveal,
      exercise all four ratings, exit/reopen, and restart/checkpoint recovery.
- [ ] **USB MSC:** connect the host, copy `.cps` to `/study/`, safely eject, and
      confirm the new deck appears after opening Study.
- [ ] **Stability:** monitor heap/PSRAM and SD behavior during a longer session.

**Hardware validation pending — X4 Pro currently unavailable.**

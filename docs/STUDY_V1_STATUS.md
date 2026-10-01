# Study V1 status

## UX and navigation

- `Study` is a permanent Home menu item, translated in English and PT-BR.
- The deck browser ensures `/study/` exists on entry, then scans direct `.cps` files; invalid packages are skipped independently. Rows are flattened in hierarchy order with `>` prefixes for descendants and show the subtree card count. Selecting any row studies that deck plus its descendants; re-entering Study rescans after file transfer, so no polling/refresh action is needed.
- Session uses front → Show answer → back/context → Again / Hard / Good / Easy → next card. Cards are loaded one at a time. Card text wraps in a scrollable viewport; its scroll indicator stays above a reserved action band so long content cannot cover the ratings.
- Empty `/study/`, invalid-only catalog, empty deck, unreadable card, changed package identity, damaged state and end-of-session have explicit screens/handling.

## Input mapping

- Deck browser: tap a row or Confirm opens the subtree; Up/Down or swipes navigate/scroll; empty and invalid catalogs include a visible touchable Back row.
- Session front: tap **Show answer** or Confirm reveals the back.
- Session back: tap one of four large rating rows, or Up/Down to select a rating and Confirm to record it. Vertical touch swipes page long card text without moving the selected rating.
- Back exits to the deck list. The session checkpoint preserves the current card and face for re-entry.

## Scheduler and order

Local deterministic day scheduler only; not FSRS/Anki compatible. Main pass visits New and due Review cards in ascending package index. Again sets Learning due today and is included once in the trailing retry pass; Again in that retry sets Review due tomorrow. Initial intervals: Hard 1 day, Good 3 days, Easy 5 days. Reviewed cards: Hard `max(previous + 1, round(previous × 1.2))`, Good `max(previous + 1, previous × 2)`, Easy `max(previous + 2, previous × 3)`. UTC day derives from `time(nullptr) / 86400`; if time is unavailable, day zero is used. Intervals saturate at `UINT32_MAX`.

## Persistence and recovery

- `.cps` package remains immutable. `<basename>.state` stores 80 bytes/card: two 40-byte generations with per-slot CRC; save writes and flushes the inactive slot, so a torn slot leaves the previous value readable.
- Initial state is built in `.state.tmp`, then validated and promoted. Valid `.state.bak` / `.state.tmp` can recover missing/corrupt canonical state. State is bound to package CRC and card count; incompatible state is reported and not silently reset.
- `<basename>.session` stores package/deck identity, phase, next/current index, card ID and front/back face. It is saved through `.tmp`/`.bak`; a valid backup or orphan temp can be promoted after interruption.

## Desktop converter

- `tools/study-converter/convert.py` converts legacy SQLite and modern Anki v3 `.apkg` packages to `.cps`; `--deck` selects a named subtree. IDs/output are deterministic and replacement is atomic. Modern v3 conversion requires the `zstd` executable; older SQLite packages use Python's standard library only.
- V3 selection follows package metadata and reads `collection.anki21b` plus normalized `notetypes`/`fields`/`templates`/`decks` tables. Its `collection.anki2` may be a valid SQLite compatibility decoy and is never preferred over the v3 collection. The compressed protobuf media manifest is counted; media itself remains omitted.
- Basic fields, `FrontSide`, simple conditionals, multi-card notes and basic non-nested Cloze are rendered; unsupported templates/Cloze are counted and warned.
- **Real Anki APKG validation: completed.** One real v3 package had 949 notes and 1,039 source cards; 1,036 cards converted and 3 cards with no matching Cloze deletions were skipped explicitly. The generated 285,575-byte full package and 9,246-byte Virologia subtree both opened in the C++ parser. Full-package and subtree output were byte-identical across repeated conversions.
- HTML becomes plain text. Media and LaTeX are identified/omitted with summary or placeholders; no media is transferred into firmware.
- README: `tools/study-converter/README.md`; technical contract, export steps and support matrix: `docs/STUDY_CONVERTER.md`.

## Simulator verification

- Manual-automation smoke used deterministic `.cps` fixtures and the SDL simulator input script. Captured deck list, front, back, each rating selection, Again retry, session complete, reopen after process restart, empty catalog, invalid-only catalog, PT-BR, Unicode and multiline cards.
- Converter end-to-end smoke generated an `.apkg` fixture, converted 7 cards, passed the C++ `StudyPackage` checker, copied the `.cps` to an isolated simulated SD, then navigated decks and studied front/back. After rating Again and restarting the simulator process, Study resumed at card 2.
- Real APKG end-to-end simulator smoke used an isolated SD: opened the parent/subdeck list, selected `2. Virologia` (30 cards), revealed a card with Portuguese accents, rated Hard, advanced to card 2 and restarted the process. The same card/face resumed from the `.session` checkpoint; a second visit opened the long-named 265-card subdeck and rendered its front/back. The 11-row hierarchy remained navigable without row overflow.
- Integrated V1 smoke used one simulator SD root for `test_reader_menu_progress.epub`, its Highlights + Notes sidecar, and ENARE `.cps`: created two highlights with touch long-press/drag, saved a keyboard note, canceled an edit without changing the note, restarted and confirmed it persisted, removed a highlight from Reader, copied the `.cps` into `/study/`, studied Virologia, then returned to Reader and reopened the remaining highlight list. Study state, checkpoint, highlights and notes occupied separate paths.
- With `/study/` absent, opening Study created the directory and showed the onboarding empty state. Copying the package into that same simulated SD and re-entering Study populated the browser without a refresh button. A conflicting non-directory `/study` path showed the SD error and a touchable Back row.
- UX/UI polish baseline and after captures use the real ENARE package in `build/study-ux/before/` and `build/study-ux/after/` (build artifacts, not tracked). The browser groups rows by package, indents subdecks, wraps long names to three lines and keeps subtree counts right-aligned. The answer view suppresses a separate front preview when the answer starts with the full front, reserves all four rating rows and scrolls overflow by vertical swipe. The old “Card N” label was removed because it displayed the package-wide index, not session progress; the header now identifies the selected deck without inventing a progress count.
- On display only, Study removes U+FE0F (emoji-presentation selector), collapses excessive template blank lines to one paragraph gap and inserts UTF-8-safe line breaks into unbroken runs over 200 bytes for the bounded wrapping helper. The immutable `.cps` text is unchanged. Verified PT-BR labels include “Estudar”, “Mostrar resposta”, “De novo”, “Difícil”, “Bom” and “Fácil”.
- The real 941-byte ENARE answer was read across multiple swipe pages; the rating band remained visible on each page and the one-card smoke reached the completed-session screen with the deck name.
- A mixed valid + invalid package directory still displayed and opened the valid package.
- Recovery smoke corrupted canonical `.state` with a valid `.state.bak`, removed `.session` with a valid `.session.tmp`, and restored corrupt canonical from `.session.bak`. A precedence case supplied different valid `.session.bak`, `.session.tmp`, and `.session.bad` records beside a corrupt canonical; the simulator resumed the temporary's Card 3/back record.
- State file inspection after the four-rating smoke showed Again retry followed by Good for card 101, Hard for 102, Good for 103 and Easy for 104; saved slot generations/IDs were valid.
- Additional touch-script smoke tapped the root deck, Show answer and a rating row.
- Session keeps one current card (front/back up to 4 KiB each), a display viewport line, package offset/deck index (8 bytes/card) and fixed state/checkpoint handles. A lookup reads the card ID and state; a rating writes one 40-byte state slot plus flush and a 48-byte `.session` checkpoint transaction.
- Host suite: **420/420 CTest cases pass**, including the Python converter-to-C++ parser integration and the previous Study/Highlights + Notes tests. The converter unittest command runs 9 Python cases.
- Builds: `pio run -e simulator_x4` and `pio run -e x4pro` pass.

## Limitations and physical validation

- The SD checkpoint/state recovery paths (including backup/temp precedence) are exercised on simulator SD across process restarts; electrical power-loss timing is not modeled.
- The simulator does not validate GT911 behavior, actual SDMMC timing, e-ink waveform, ghosting or battery effects.
- USB MSC is stubbed as unsupported in the simulator. The integrated smoke placed `.cps` into the same SD root between simulator runs; this validates fresh catalog discovery after returning to Study, not the physical USB transport.
- Visible glyph coverage depends on installed firmware fonts; the host smoke verifies PT-BR accents and Latin multiline text, not every Unicode glyph or emoji.
- U+FE0F no longer reaches the glyph renderer in Study, while the original CPS bytes remain intact. A real card also contains U+2192 (right arrow), which the current simulator font does not provide; broader symbol/emoji font coverage is not part of this polish.
- The simulator confirms content pagination and UI state transitions, but not partial-refresh artifacts, ghosting, waveform timing, or GT911 precision on the physical panel.
- **Hardware validation pending — X4 Pro currently unavailable.**
- Converter GUI, advanced Anki filters/add-ons, nested Cloze, firmware media, FSRS and synchronization remain out of scope. APKG v3 conversion additionally depends on the host `zstd` utility.

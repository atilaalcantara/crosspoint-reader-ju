# Study V1 package and state format

**Status:** version 1 is implemented by the host-testable reader in `src/study/`; the local desktop converter is `tools/study-converter/convert.py`. Integers are unsigned/signed little-endian as specified; no C/C++ struct is written directly. Maximum package size is bounded by the 32-bit offsets (4 GiB minus one byte), while current firmware policy caps packages at 100,000 cards and 1,024 decks.

## Location and discovery contract

- Package suffix: `.cps` (CrossPoint Study), placed directly in `/study/` on SD. No recursive directory walk is part of this format; package files may contain many decks in a hierarchy.
- `Study::discoverFromSd` lists up to 512 direct files in `/study/`, filters the suffix case-insensitively, and attempts each independently. Missing `/study/` or no matching files is an empty catalog. A malformed file records an error and does not suppress other valid packages; reaching the 512-file cap reports `LimitExceeded` instead of presenting a potentially incomplete catalog as complete.
- Package-local deck IDs and card IDs are namespaced by the package identity. The package identity is `(file size, payload CRC-32)`; state sidecar also records card count and must not be reused when the package identity changes.
- Mutable state path: `/study/<package-basename>.state`. It must never be written into the `.cps` package.

## `.cps` V1 binary layout

### Header: 40 bytes

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | magic bytes `CPSTUDY\0` |
| 8 | 2 | version = 1 |
| 10 | 2 | header size = 40 |
| 12 | 4 | exact file size in bytes |
| 16 | 4 | deck count, 1..1,024 |
| 20 | 4 | card count, 0..100,000 |
| 24 | 4 | deck table offset; must equal 40 |
| 28 | 4 | card offset-index offset; must equal `40 + deckCount × 108` |
| 32 | 4 | card-data offset; must equal `cardIndexOffset + cardCount × 4` |
| 36 | 4 | CRC-32 of every byte from offset 40 through EOF (IEEE polynomial `0xEDB88320`, init/final XOR `0xFFFFFFFF`) |

No unknown versions or alternate header sizes are accepted. V1 parsers reject unknown versions rather than guessing. A future incompatible format increments the version and defines migration/dual-read policy explicitly.

### Deck table: 108 bytes per deck

| Relative offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `deckId`, nonzero `uint32`; unique within package |
| 4 | 4 | `parentDeckId`; zero means root, otherwise references a deck in this package |
| 8 | 2 | `nameLength`, 1..96 bytes |
| 10 | 2 | flags, must be zero in V1 |
| 12 | 96 | UTF-8 name then zero padding |

The table may be in any order. Parent references must exist; self-parenting and cycles are invalid. Nesting is limited to the deck count and cycle checked. `deckId` is package-local; identical IDs in different `.cps` files are distinct.

### Card offset index: 4 bytes per card

The index is an array of absolute `uint32` offsets into the card-data area. Index order is canonical card order and card IDs must be strictly increasing in this order (therefore nonzero and unique). It is **4 bytes on disk per card**. The loader keeps one offset and the corresponding deck ID in RAM per card (**8 bytes/card**): approximately 8 KB for 1,000 cards, 80 KB for 10,000, and 800 KB for 100,000, excluding vector capacity and deck metadata. At 100,000 cards offset/index validation stays below the 32-bit file size ceiling; firmware implementation must account for available heap/PSRAM before opening unusually large packages.

Cards need not be grouped by deck. Membership is stored with each card record; selecting a parent/subtree enumerates membership while preserving file/index order. `DeckInfo::cardCount` and `subtreeCardCount` are computed at open.

### Card record: 16-byte prefix + UTF-8 content

| Relative offset | Size | Field |
|---:|---:|---|
| 0 | 8 | nonzero stable `cardId`, strictly ascending across the package |
| 8 | 4 | `deckId`, must reference a deck in this package |
| 12 | 2 | `frontLength`, 0..4,096 bytes |
| 14 | 2 | `backLength`, 0..4,096 bytes |
| 16 | frontLength | front UTF-8 bytes |
| 16 + frontLength | backLength | back UTF-8 bytes |

Both faces may be empty; converters should ordinarily omit cards whose two faces are empty. Maximum combined text is 8,192 bytes. The two lengths delimit exact byte spans; NUL terminators are not stored. UTF-8 must be shortest-form valid Unicode scalar encoding (no surrogate scalars or values above U+10FFFF). Text is plain text: LF (`0A`) is the line break. There is no escaping layer; literal `\`, `<`, `>`, `&` and quotes are ordinary characters. V1 text rejects NUL and control characters other than LF and TAB; CR line endings must be normalized to LF by the converter. HTML, CSS, image/audio references, LaTeX, and arbitrary template syntax have no execution semantics.

## Validation and failure behavior

`Study::Package::open` validates exact file size, offsets/table arithmetic without overflow, payload CRC, deck IDs/names/padding/parent graph, all card offsets/record bounds, UTF-8 and face limits before making the catalog available. Card records must be contiguous in index order from `dataOffset` to EOF. It reads/validates one bounded card text at a time and retains no card bodies. `loadCard` re-reads by offset and validates the text again, so a mutated/truncated source fails rather than returning partial data.

Invalid magic/version/header, checksum, deck, card, UTF-8, bounds, memory allocation, or I/O return a typed `Study::Error`. A failed package open leaves no partial deck/card catalog. `discoverFromSd` keeps valid packages and exposes invalid filenames/errors separately. Unknown flags/reserved bytes are rejected in V1. Unrecognized record kinds cannot occur: V1 has fixed deck/card record structures only. The caller should keep the package read handle stable for the session; `loadCard` checks bounds and current-record UTF-8 again but does not rescan the complete package CRC on every card access.

## Optional image sidecar (`.media/index.bin`)

For `/study/foo.cps`, images live in `/study/foo.media/` and the optional manifest is
`/study/foo.media/index.bin`. The 20-byte header contains ASCII `CPSMEDIA` (8 bytes),
little-endian `uint16` version 1 at offset 8, reserved zero `uint16` at 10,
the `.cps` payload CRC-32 `uint32` at 12, and record count `uint32` at 16.
Each record contains a little-endian `uint64` card ID, `uint8` face (0 front,
1 back), reserved zero `uint8`, little-endian `uint16` filename byte count
(1..64), then exactly that many ASCII bytes. Records are ordered by card ID,
face, then image occurrence within each face; at most four images per face.
The only accepted filename is 32 lowercase SHA-256 hex digits followed by
`.png` (36 bytes), with no path separators. Images are direct children of
the `.media` directory. The index must have exactly the declared records
and no trailing bytes; its size and count are bounded (at most 8 records per
package card limit). Missing, mismatched-CRC, malformed or unreadable sidecars
and images leave the Study text usable. The reader scans the index on demand
with fixed-size buffers, retaining filenames only for the current card. PNG
decoding streams through the HAL-backed decoder into the existing framebuffer;
media fits inside the card area, reserving text scroll space and the actions.

## `.state` V1

State uses separate magic `CPSSTAT\0`, version `uint16=1`, header length `uint16=32`, package CRC `uint32`, card count `uint32`, record size `uint32=80`, generation `uint32`, CRC-32 over header bytes 0..27 at offset 28. Header is followed by two alternating 40-byte slots per card, positionally aligned with the package index. Each slot has a `uint32` generation, the 32-byte payload below shifted by four bytes, and a CRC-32 trailer over the preceding 36 slot bytes. The newest valid generation is authoritative; an all-zero slot denotes the initial New state. Writes target the inactive slot and flush it, so a torn update leaves the previous slot available.

| Slot-relative offset | Size | Field |
|---:|---:|---|
| 0 | 4 | slot generation, nonzero for a written slot |
| 4 | 8 | card ID, must match `.cps` at that index |
| 12 | 4 | due day (`int32`, UTC day; zero when new) |
| 16 | 4 | interval days (`uint32`) |
| 20 | 4 | last review day (`int32`, -1 when never reviewed) |
| 24 | 4 | repetitions |
| 28 | 4 | lapses |
| 32 | 1 | status: New=0, Learning=1, Review=2, Suspended=3 |
| 33 | 1 | last rating: None=0, Again=1, Hard=2, Good=3, Easy=4 |
| 34 | 2 | reserved, zero |
| 36 | 4 | CRC-32 of slot bytes 0..35 |

Two all-zero slots resolve to New using the card ID from the package. A valid slot must have valid CRC, generation, enums, zero reserved bytes and matching card ID. If one slot is invalid and the other valid, the valid slot is returned; if both nonzero slots are invalid, load fails with a typed error and must not silently overwrite them. A package CRC/card-count mismatch rejects the whole state file, preventing scheduling from attaching to replaced content.

`StateStore::initialize` and `save` operate on a `WritableRandomAccess` abstraction. `PersistentState` creates a new state file through `.tmp`, validates it, then renames it to `.state`; it can promote valid `.tmp`/`.bak` copies when the canonical file is missing/corrupt. Per-card updates write only the inactive fixed-size slot. FAT rename/remove is not claimed to be atomic.

## `.session` V1 checkpoint

`/study/<package-basename>.session` is a 48-byte record with magic `CPSSESS\0`, version/length, package CRC, deck ID, phase (Main/Again/Done/Cram), face (Front/Back), next index, current index/card ID, card count and CRC-32. "Study again" from Done starts a Cram pass that revisits every card once in deck order (ratings still apply, so Again answers retry in the Again pass); without Cram a restart would land straight back on Done because nothing is due. It is written through `.tmp` and `.bak`; startup validates package identity, indexes, current-card membership, current-card scheduling eligibility and checksum. Recovery preference is valid canonical, then valid temporary (in-progress replacement), then validated backup, then a validated `.bad` quarantine. A failed rename leaves the validated source at its current name for another recovery attempt; invalid canonicals are quarantined so a fresh checkpoint can be saved. A Done checkpoint is re-evaluated against current due state on reopen. Invalid checkpoints are ignored/rebuilt without resetting per-card state.

## V1 scheduler behavior

The local day is UTC epoch days (`time(nullptr) / 86400`; unavailable clock uses day zero). Main pass visits eligible cards in ascending package index: New cards and due Review cards. A first-pass Again sets Learning/due-today and is retried once in the Again pass. Again during the retry moves the card to Review due tomorrow, preventing an endless loop. Hard uses one day for a new card, otherwise `max(previous+1, round(previous×1.2))`; Good uses 3 days for a new card, otherwise `max(previous+1, 2×previous)`; Easy uses 5 days for a new card, otherwise `max(previous+2, 3×previous)`. Intervals saturate at `UINT32_MAX`; these deterministic intervals are local behavior, not FSRS/Anki compatibility.

## Implementation and measurements

- Reader/model: `src/study/StudyPackage.{h,cpp}`, `StudyState.{h,cpp}`.
- Host fixtures/tests: `test/support/StudyFixtureBuilder.h`, `test/study_v1/StudyV1Test.cpp`.
- `Study::Catalog` accepts package candidates independently: invalid input is retained with its error and does not invalidate successful packages. SD directory enumeration and UI are intentionally deferred to the next phase.
- Offset/deck-membership index is 8 bytes/card in RAM and 4 bytes/card on disk; state is 80 bytes/card on disk (plus 32-byte header). Tests generate a 10,000-card package and measure open-time bound and index size. No physical-device RAM/latency measurement is claimed by host tests.

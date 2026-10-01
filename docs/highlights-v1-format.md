# Highlight sidecar, version 2

This is the on-card format for EPUB text highlights and their optional plain
text notes. It is separate from derived EPUB layout caches and contains no UI
state, page numbers or touch coordinates.

## Identity and location

- Sidecar: `/.crosspoint/highlights/highlight_<fnv1a64(bookPath)>.bin`.
- The exact book path is stored in the header and compared on load, so a hash
  collision is rejected rather than associating one book's records with another.
- The current identity is the card filepath. Neither v1 nor the note-bearing v2
  fingerprints EPUB contents; replacing an EPUB in place can leave anchors
  pointing into changed text. This remains a known limitation.

## Byte layout

All integers are unsigned little-endian and strings are UTF-8 byte sequences.

```text
Header
  4 bytes  magic = "HLT1"
  u8       format version = 2
  u16      record count (0..64)
  u16      book-path byte length (1..1024)
  bytes    exact book path

Repeated record (record count times)
  u32      stable record id (non-zero, unique within the book)
  u16      EPUB spine item index
  u32      start visible-codepoint offset
  u32      end visible-codepoint offset (exclusive; start < end)
  u16      quote byte length (1..512)
  bytes    selected quote in UTF-8
  u16      note byte length (0..512)
  bytes    optional note in UTF-8; zero length means no note

Trailer
  u32      CRC-32/IEEE of every preceding byte
```

Offsets are zero-based Unicode codepoint indices in the visible text counted
inside the spine item's `<body>`, matching the parser's `visibleTextOffset`
domain. They are not UTF-8 byte offsets, page numbers, coordinates or rendered
word ordinals. The quote is retained as a validation aid and recovery clue; the
interval plus spine is the location. Rendered words overlap the interval when
`wordStart < endOffset && wordEnd > startOffset`; zero-length synthetic words
are never covered.

When a word's NFC form has fewer codepoints than its EPUB source, the parser
maps normalized token boundaries back to source boundaries. If such a token
would itself need a hyphenation split, the layout keeps it whole rather than
creating an inexact anchor boundary; this uncommon NFD case can therefore wrap
less aggressively than ordinary NFC text.

## Reader/cache relationship

`ParsedText` supplies source-codepoint spans to each cached `TextBlock` word.
The section cache version is 49; version 48 and earlier are derived data and
are rebuilt when opened. Highlight sidecars are not in the EPUB cache directory, so
cache clearing/re-pagination does not erase highlights. Store changes do not
rewrite book/section cache files.

## Reader list and navigation

The reader menu (including the toolbar's More panel) opens a per-book Highlights
list, including when the collection is empty. Rows are sorted by
`spineIndex/startOffset/endOffset/id`, so creation order does not affect the
reading order. The list displays a UTF-8-safe preview of at most 88 codepoints,
a chapter title only when the existing TOC maps that spine item, and a small
`Note` subtitle when the highlight has a note. The full quote, note and anchor
stay in the sidecar; the child list references the reader's already-loaded
collection and materializes only bounded row previews.

Selecting a row returns its spine and `startOffset` through the reader's
existing `ProgressChangeResult` path. `Section::getPageForVisibleTextOffset`
resolves the page from the existing per-page visible-offset LUT; exact page
boundaries choose the first matching page. The LUT is read once in order
(O(number of pages), with no new persistent index). If a partial section does
not cover a distant target, the reader resumes its existing incremental build
until the target offset is reached, then renders the page and its normal
word-offset-based highlight overlay. Canceling the list returns to the same
reader position; selecting a row updates normal reader progress on render.

Long-pressing a list row opens `Open` plus `Add note` or `View / edit note`.
The existing `KeyboardEntryActivity` provides text input. Confirm persists the
complete highlight collection before the list updates; Back/cancel leaves the
original note untouched. Saving an empty field clears the note. Rows with a note
add a translated `Note` subtitle beside the chapter title when present.

The simulator smoke test opened `test_reader_menu_progress.epub` from an
isolated temporary SD root, seeded `sections/0.bin` with the obsolete v48 marker,
and reopened it. The reader logged the unsupported v48 cache, rendered the
fixture after rebuilding, and the replacement section began with version 49.

## Read/write safety

- Missing canonical and backup means an empty collection.
- Unknown versions, mismatched path, duplicate/invalid ids, invalid interval,
  excessive lengths/count, truncation, trailing bytes and CRC mismatch fail the
  load without replacing the caller's current collection.
- Version 1 records are still accepted. They ended after the quote field, so
  they load with an empty note; the next successful save rewrites the collection
  as version 2.
- Notes are limited to 512 UTF-8 bytes each. Malformed UTF-8, embedded NUL and
  over-limit notes are rejected before save and make a v2 sidecar fail load.
  Saving an empty note clears it while keeping the highlight valid.
- Writes go to `<sidecar>.tmp`, close the file, rotate the existing canonical
  to `<sidecar>.bak`, promote the temporary, then remove the backup. If the
  canonical is absent/corrupt and a complete backup validates, load restores it.
- FAT rename sequences are not a multi-operation transaction. A failure can
  leave the backup for next load; an incomplete temporary is ignored.
- V1 data is migrated in memory on load without rewriting the sidecar. A later
  edit/save rewrites the complete collection as v2 using the existing staging,
  backup and CRC recovery path.

## Measurement (macOS host)

The repeatable `ChapterHtmlSlimParserTest.ReportsTextPageCacheOffsetOverheadForRepeatableSample`
serializes one laid-out page containing 64 English words into the actual Page /
TextBlock cache representation. It compares the produced bytes with the exact
version-48 equivalent by subtracting the newly serialized per-token deltas and
per-block base:

| Sample | Words | TextBlocks | v48-equivalent bytes | v49 bytes | Added | Overhead |
|---|---:|---:|---:|---:|---:|---:|
| 64-word synthetic page | 64 | 16 | 1,662 | 1,982 | 320 B | 19.25% |

This is a deterministic page-cache sample, not a complete EPUB section. The
structural disk cost is exactly 4 bytes per word plus 4 bytes per TextBlock;
the relative percentage for a full section will be smaller because its header,
images and other records do not change. In RAM, the resident TextBlock arena
adds 4 bytes per word and each TextBlock object adds the 4-byte base. During
layout only, `ParsedText` retains a further 2-byte source-span length per token.
For the sample that is 448 bytes of additional peak working data
(4×64 arena + 4×16 bases + 2×64 parse metadata), aside from allocator capacity
rounding. Only NFD tokens allocate an additional temporary source-boundary map,
bounded by that token's source codepoint count (at most 200 `uint32_t` entries
for the parser's bounded token input, released at the end of `addWord`). The
same bytes are required by ESP32-C3 and ESP32-S3; the Pro has a larger heap, but
the new page metadata is not moved to PSRAM. Physical heap headroom and
full-book growth still need measurement on hardware.

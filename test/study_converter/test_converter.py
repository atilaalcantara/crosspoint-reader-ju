#!/usr/bin/env python3
"""Controlled APKG fixtures and independent C++ StudyPackage integration."""

from __future__ import annotations

import argparse
import importlib.util
import json
import sqlite3
import struct
import subprocess
import shutil
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CONVERTER_PATH = ROOT / "tools" / "study-converter" / "convert.py"
SPEC = importlib.util.spec_from_file_location("study_converter", CONVERTER_PATH)
assert SPEC and SPEC.loader
converter = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = converter
SPEC.loader.exec_module(converter)

CHECKER: Path | None = None


def make_collection(path: Path, *, empty: bool = False, duplicate_deck_ids: bool = False) -> None:
    connection = sqlite3.connect(path)
    connection.executescript(
        """
        CREATE TABLE col (id INTEGER PRIMARY KEY, models TEXT, decks TEXT);
        CREATE TABLE notes (id INTEGER PRIMARY KEY, mid INTEGER, flds TEXT, tags TEXT);
        CREATE TABLE cards (id INTEGER PRIMARY KEY, nid INTEGER, did INTEGER, ord INTEGER, odid INTEGER);
        """
    )
    decks = {
        "1": {"id": 1, "name": "Default", "dyn": 0},
        "10": {"id": 10, "name": "Veterinária", "dyn": 0},
        "11": {"id": 11, "name": "Veterinária::Virologia", "dyn": 0},
        "12": {"id": 12, "name": "Veterinária::Clínica", "dyn": 0},
        "99": {"id": 99, "name": "Filtered", "dyn": 1},
    }
    if duplicate_deck_ids:
        decks["11"]["id"] = 10
    models = {
        "100": {
            "id": 100,
            "name": "Basic",
            "type": 0,
            "flds": [{"name": "Front"}, {"name": "Back"}],
            "tmpls": [{"ord": 0, "name": "Card 1", "qfmt": "<div>{{Front}}</div>",
                       "afmt": "{{FrontSide}}<hr id=answer><p>{{Back}}</p>"}],
        },
        "101": {
            "id": 101,
            "name": "Multi card",
            "type": 0,
            "flds": [{"name": "Question"}, {"name": "Answer"}, {"name": "Extra"}],
            "tmpls": [
                {"ord": 0, "name": "Forward", "qfmt": "{{Question}}",
                 "afmt": "{{FrontSide}}<br>{{Answer}}{{#Extra}}<br><b>{{Extra}}</b>{{/Extra}}"},
                {"ord": 1, "name": "Reverse", "qfmt": "Reverse: {{Answer}}",
                 "afmt": "{{FrontSide}}<hr>{{Question}}"},
            ],
        },
        "102": {
            "id": 102,
            "name": "Cloze",
            "type": 1,
            "flds": [{"name": "Text"}, {"name": "Extra"}],
            "tmpls": [{"ord": 0, "name": "Cloze", "qfmt": "{{cloze:Text}}",
                       "afmt": ("{{FrontSide}}<br>{{cloze:Text}}{{#Extra}}<br>{{Extra}}{{/Extra}}"
                                "{{#c1}}<br>First cloze{{/c1}}{{#c2}}<br>Second cloze{{/c2}}" )}],
        },
        "103": {
            "id": 103,
            "name": "Unsupported filter",
            "type": 0,
            "flds": [{"name": "Front"}],
            "tmpls": [{"ord": 0, "name": "Bad", "qfmt": "{{furigana:Front}}", "afmt": "{{Front}}"}],
        },
    }
    connection.execute("INSERT INTO col VALUES(1, ?, ?)", (json.dumps(models), json.dumps(decks)))
    if not empty:
        connection.executemany(
            "INSERT INTO notes VALUES(?, ?, ?, ?)",
            [
                (201, 100, "Frente &amp; ação<br>linha dois\x1fVerso &amp; café", "ptbr marked"),
                (202, 101, "<b>Pergunta um</b>\x1fResposta um\x1fExtra <i>detalhe</i>", ""),
                (203, 102, "A {{c1::raiva::doença}} é causada por {{c2::Lyssavirus}}.\x1fNota extra", "cloze"),
                (204, 103, "não converter silenciosamente\x1f", ""),
                (205, 100, "<p>Figura<br>com mídia <img src=\"0\" alt=\"&amp;lt; placa\"></p> [sound:1]"
                 "\x1fResposta com mídia [sound:1]", "media"),
                (206, 100, "\x1fFrente vazia não deve criar card", "empty-front"),
                (207, 102, "{{c1::externo {{c2::aninhado}}}}\x1f", "nested-cloze"),
                (208, 100, "equação [latex]x^2[/latex] e inline [latex]y[/latex]\x1fResultado", "latex"),
            ],
        )
        connection.executemany(
            "INSERT INTO cards VALUES(?, ?, ?, ?, ?)",
            [(2001, 201, 11, 0, 0), (2002, 202, 12, 0, 0), (2003, 203, 11, 0, 0),
             (2004, 202, 12, 1, 0), (2005, 203, 11, 1, 0), (2006, 204, 12, 0, 0),
             (2007, 205, 11, 0, 0), (2008, 206, 12, 0, 0), (2009, 207, 11, 0, 0),
             (2010, 208, 12, 0, 0)],
        )
    connection.commit()
    connection.close()


def make_apkg(path: Path, *, empty: bool = False, duplicate_deck_ids: bool = False,
              collection_name: str = "collection.anki2") -> None:
    with tempfile.TemporaryDirectory(prefix="apkg-fixture-") as tmp:
        db = Path(tmp) / "collection.sqlite"
        make_collection(db, empty=empty, duplicate_deck_ids=duplicate_deck_ids)
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as package:
            package.write(db, collection_name)
            package.writestr("media", json.dumps({"0": "plate.png", "1": "sound.mp3"}))
            package.writestr("0", b"fixture image placeholder")
            package.writestr("1", b"fixture audio placeholder")


def protobuf_varint(value: int) -> bytes:
    output = bytearray()
    while value >= 0x80:
        output.append((value & 0x7F) | 0x80)
        value >>= 7
    output.append(value)
    return bytes(output)


def protobuf_bytes(field: int, value: bytes) -> bytes:
    return protobuf_varint((field << 3) | 2) + protobuf_varint(len(value)) + value


def make_modern_apkg(path: Path, decoy_path: Path, *, meta: bytes | None = b"\x08\x03") -> None:
    connection = sqlite3.connect(":memory:")
    connection.executescript(
        """
        CREATE TABLE col (id INTEGER PRIMARY KEY, models TEXT, decks TEXT);
        CREATE TABLE notes (id INTEGER PRIMARY KEY, mid INTEGER, flds TEXT, tags TEXT);
        CREATE TABLE cards (id INTEGER PRIMARY KEY, nid INTEGER, did INTEGER, ord INTEGER, odid INTEGER);
        CREATE TABLE notetypes (id INTEGER PRIMARY KEY, name TEXT, config BLOB);
        CREATE TABLE fields (ntid INTEGER, ord INTEGER, name TEXT, config BLOB);
        CREATE TABLE templates (ntid INTEGER, ord INTEGER, name TEXT, mtime_secs INTEGER, usn INTEGER, config BLOB);
        CREATE TABLE decks (id INTEGER PRIMARY KEY, name TEXT, mtime_secs INTEGER, usn INTEGER, common BLOB, kind BLOB);
        """
    )
    connection.execute("INSERT INTO col VALUES(1, '', '')")
    connection.execute("INSERT INTO notetypes VALUES(55, 'Basic', ?)", (b"\x08\x00",))
    connection.execute("INSERT INTO notetypes VALUES(56, 'Cloze', ?)", (protobuf_bytes(1, b"\x08\x01"),))
    connection.executemany(
        "INSERT INTO fields VALUES(55, ?, ?, X'')", [(0, "Front"), (1, "Back")]
    )
    connection.execute("INSERT INTO fields VALUES(56, 0, 'Text', X'')")
    template_config = protobuf_bytes(1, b"{{Front}}") + protobuf_bytes(
        2, b"{{FrontSide}}<br>{{Back}}"
    )
    connection.execute(
        "INSERT INTO templates VALUES(55, 0, 'Card 1', 0, 0, ?)", (template_config,)
    )
    cloze_config = protobuf_bytes(1, b"{{cloze:Text}}") + protobuf_bytes(
        2, b"{{FrontSide}}<br>{{cloze:Text}}"
    )
    connection.execute(
        "INSERT INTO templates VALUES(56, 0, 'Cloze', 0, 0, ?)", (cloze_config,)
    )
    normal_kind = protobuf_bytes(1, b"\x08\x01")
    connection.executemany(
        "INSERT INTO decks VALUES(?, ?, 0, 0, X'', ?)",
        [(1, "Modern", normal_kind), (2, "Modern\x1fVirologia", normal_kind)],
    )
    connection.executemany(
        "INSERT INTO notes VALUES(?, ?, ?, '')",
        [(101, 55, "Frente\x1fResposta"), (102, 56, "Pergunta {{c1::resposta}}")],
    )
    connection.executemany(
        "INSERT INTO cards VALUES(?, ?, 2, 0, 0)", [(1001, 101), (1002, 102)]
    )
    database = connection.serialize()
    connection.close()

    if not shutil.which("zstd"):
        raise unittest.SkipTest("zstd executable unavailable for modern APKG fixture")
    compressed_db = subprocess.run(["zstd", "-q", "-c"], input=database, capture_output=True, check=True).stdout
    media_entry = protobuf_bytes(1, b"sample.jpg") + protobuf_bytes(2, b"\x04") + protobuf_bytes(3, bytes(20))
    media_manifest = protobuf_bytes(1, media_entry)
    compressed_media = subprocess.run(
        ["zstd", "-q", "-c"], input=media_manifest, capture_output=True, check=True
    ).stdout
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as package:
        if meta is not None:
            package.writestr("meta", meta)
        package.write(decoy_path, "collection.anki2")
        package.writestr("collection.anki21b", compressed_db)
        package.writestr("media", compressed_media)
        package.writestr("0", b"fixture image bytes")


def read_cps(path: Path) -> tuple[list[dict], list[dict]]:
    data = path.read_bytes()
    header = struct.unpack_from("<8sHH7I", data)
    deck_count, card_count, deck_offset, index_offset, data_offset = header[4:9]
    decks = []
    for index in range(deck_count):
        offset = deck_offset + index * converter.DECK_BYTES
        deck_id, parent_id, name_size, _flags = struct.unpack_from("<IIHH", data, offset)
        name = data[offset + 12 : offset + 12 + name_size].decode("utf-8")
        decks.append({"id": deck_id, "parent": parent_id, "name": name})
    cards = []
    for index in range(card_count):
        offset = struct.unpack_from("<I", data, index_offset + index * 4)[0]
        card_id, deck_id, front_size, back_size = struct.unpack_from("<QIHH", data, offset)
        cursor = offset + 16
        front = data[cursor : cursor + front_size].decode("utf-8")
        back = data[cursor + front_size : cursor + front_size + back_size].decode("utf-8")
        cards.append({"id": card_id, "deck": deck_id, "front": front, "back": back})
    assert data_offset <= len(data)
    return decks, cards


class ConverterTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="study-converter-test-")
        self.root = Path(self.temp.name)
        self.apkg = self.root / "fixture.apkg"
        self.cps = self.root / "fixture.cps"
        make_apkg(self.apkg)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def check_cpp(self, path: Path, expected_decks: int, expected_cards: int) -> str:
        if CHECKER is None:
            self.skipTest("C++ StudyPackage checker was not supplied")
        result = subprocess.run([str(CHECKER), str(path)], text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith(f"OK {expected_decks} {expected_cards} "), result.stdout)
        return result.stdout

    def test_apkg_templates_multiple_cards_cloze_media_and_cpp_parser(self) -> None:
        report = converter.convert_apkg(self.apkg, self.cps)
        self.assertEqual(report.decks, 4)
        self.assertEqual(report.cards_written, 7)
        self.assertEqual(report.cards_skipped["unsupported-template-or-cloze"], 2)
        self.assertEqual(report.cards_skipped["empty-front"], 1)
        self.assertEqual(report.cloze_cards_written, 2)
        self.assertEqual(report.cloze_cards_skipped, 1)
        self.assertGreater(report.latex_references, 0)
        self.assertEqual(report.media_manifest_entries, 2)
        self.assertGreater(report.image_references, 0)
        self.assertGreater(report.sound_references, 0)

        decks, cards = read_cps(self.cps)
        self.assertEqual([card["id"] for card in cards], [2001, 2002, 2003, 2004, 2005, 2007, 2010])
        by_id = {card["id"]: card for card in cards}
        self.assertEqual(by_id[2001]["front"], "Frente & ação\nlinha dois")
        self.assertIn("Verso & café", by_id[2001]["back"])
        self.assertEqual(by_id[2002]["front"], "Pergunta um")
        self.assertIn("Extra detalhe", by_id[2002]["back"])
        self.assertIn("Reverse: Resposta um", by_id[2004]["front"])
        self.assertEqual(by_id[2003]["front"], "A [doença] é causada por Lyssavirus.")
        self.assertIn("A raiva é causada por Lyssavirus.", by_id[2003]["back"])
        self.assertIn("First cloze", by_id[2003]["back"])
        self.assertEqual(by_id[2005]["front"], "A raiva é causada por […].")
        self.assertIn("Second cloze", by_id[2005]["back"])
        self.assertNotIn("[sound:", by_id[2007]["front"] + by_id[2007]["back"])
        self.assertIn("&lt; placa", by_id[2007]["front"])
        self.assertIn("[LaTeX omitted]", by_id[2010]["front"])
        self.assertEqual(decks[1]["parent"], 0)
        self.assertIn("Virologia", {deck["name"] for deck in decks})
        output = self.check_cpp(self.cps, 4, 7)
        self.assertIn("DECK 2 0", output)

    def test_deck_filter_includes_selected_subtree_and_is_deterministic(self) -> None:
        report = converter.convert_apkg(self.apkg, self.cps, "Veterinária::Virologia")
        first = self.cps.read_bytes()
        second_path = self.root / "second.cps"
        converter.convert_apkg(self.apkg, second_path, "Veterinária::Virologia")
        self.assertEqual(first, second_path.read_bytes())
        decks, cards = read_cps(self.cps)
        self.assertEqual(len(decks), 1)
        self.assertEqual(decks[0]["name"], "Virologia")
        self.assertEqual([card["id"] for card in cards], [2001, 2003, 2005, 2007])
        self.assertEqual(report.cards_written, 4)
        self.check_cpp(self.cps, 1, 4)

    def test_empty_deck_package_is_valid(self) -> None:
        empty_apkg = self.root / "empty.apkg"
        empty_cps = self.root / "empty.cps"
        make_apkg(empty_apkg, empty=True)
        report = converter.convert_apkg(empty_apkg, empty_cps)
        self.assertEqual(report.cards_written, 0)
        self.check_cpp(empty_cps, 4, 0)

    def test_invalid_zip_sqlite_and_zstd_variant_fail_without_replacing_output(self) -> None:
        self.cps.write_bytes(b"sentinel")
        bad = self.root / "bad.apkg"
        bad.write_bytes(b"not a zip")
        with self.assertRaises(converter.ConversionError):
            converter.convert_apkg(bad, self.cps)
        self.assertEqual(self.cps.read_bytes(), b"sentinel")

        malformed = self.root / "malformed.apkg"
        with zipfile.ZipFile(malformed, "w") as package:
            package.writestr("collection.anki2", b"not sqlite")
        with self.assertRaises(converter.ConversionError):
            converter.convert_apkg(malformed, self.cps)
        self.assertEqual(self.cps.read_bytes(), b"sentinel")

        missing_collection = self.root / "missing-collection.apkg"
        with zipfile.ZipFile(missing_collection, "w") as package:
            package.writestr("media", "{}")
        with self.assertRaisesRegex(converter.ConversionError, "sem collection"):
            converter.convert_apkg(missing_collection, self.cps)
        self.assertEqual(self.cps.read_bytes(), b"sentinel")

        modern = self.root / "zstd.apkg"
        with zipfile.ZipFile(modern, "w") as package:
            package.writestr("collection.anki21b", b"zstandard bytes")
        with self.assertRaisesRegex(converter.ConversionError, "Zstandard"):
            converter.convert_apkg(modern, self.cps)
        self.assertEqual(self.cps.read_bytes(), b"sentinel")

    def test_sqlite_container_named_anki21_is_supported_when_plain_sqlite(self) -> None:
        modern_name = self.root / "anki21.apkg"
        modern_cps = self.root / "anki21.cps"
        make_apkg(modern_name, collection_name="collection.anki21")
        report = converter.convert_apkg(modern_name, modern_cps)
        self.assertEqual(report.cards_written, 7)
        self.check_cpp(modern_cps, 4, 7)

    def test_collection_apkg_requires_explicit_deck_filter(self) -> None:
        collection_apkg = self.root / "collection.apkg"
        collection_cps = self.root / "collection.cps"
        make_apkg(collection_apkg)
        with self.assertRaisesRegex(converter.ConversionError, "--deck"):
            converter.convert_apkg(collection_apkg, collection_cps)
        report = converter.convert_apkg(collection_apkg, collection_cps, "Veterinária::Virologia")
        self.assertEqual(report.cards_written, 4)
        self.check_cpp(collection_cps, 1, 4)

    def test_duplicate_source_deck_ids_are_rejected_without_partial_output(self) -> None:
        duplicate_apkg = self.root / "duplicate-decks.apkg"
        duplicate_cps = self.root / "duplicate-decks.cps"
        make_apkg(duplicate_apkg, duplicate_deck_ids=True)
        duplicate_cps.write_bytes(b"preserve prior output")
        with self.assertRaisesRegex(converter.ConversionError, "IDs de deck duplicados"):
            converter.convert_apkg(duplicate_apkg, duplicate_cps)
        self.assertEqual(duplicate_cps.read_bytes(), b"preserve prior output")

    def test_v3_apkg_prefers_modern_collection_and_maps_normalized_metadata(self) -> None:
        modern_apkg = self.root / "modern.apkg"
        modern_cps = self.root / "modern.cps"
        decoy = self.root / "decoy.sqlite"
        make_collection(decoy)
        make_modern_apkg(modern_apkg, decoy)

        report = converter.convert_apkg(modern_apkg, modern_cps)
        self.assertEqual(report.decks, 2)
        self.assertEqual(report.cards_written, 2)
        self.assertEqual(report.media_manifest_entries, 1)
        self.assertEqual(report.cloze_cards_written, 1)
        decks, cards = read_cps(modern_cps)
        self.assertEqual([(deck["name"], deck["parent"]) for deck in decks], [("Modern", 0), ("Virologia", 1)])
        self.assertEqual(cards[0], {"id": 1001, "deck": 2, "front": "Frente", "back": "Frente\nResposta"})
        self.assertEqual(cards[1], {"id": 1002, "deck": 2, "front": "Pergunta […]", "back": "Pergunta […]\nPergunta resposta"})
        self.check_cpp(modern_cps, 2, 2)

        duplicate_version = self.root / "modern-duplicate-version.apkg"
        duplicate_version_cps = self.root / "modern-duplicate-version.cps"
        make_modern_apkg(duplicate_version, self.root / "decoy.sqlite", meta=b"\x08\x02\x08\x03")
        self.assertEqual(converter.convert_apkg(duplicate_version, duplicate_version_cps).cards_written, 2)
        self.check_cpp(duplicate_version_cps, 2, 2)

    def test_modern_collection_is_not_silently_misread_when_meta_is_missing_or_conflicting(self) -> None:
        if not shutil.which("zstd"):
            self.skipTest("zstd executable unavailable for modern APKG fixture")
        decoy = self.root / "decoy.sqlite"
        make_collection(decoy)
        missing_meta = self.root / "modern-missing-meta.apkg"
        missing_meta_cps = self.root / "modern-missing-meta.cps"
        make_modern_apkg(missing_meta, decoy, meta=None)
        self.assertEqual(converter.convert_apkg(missing_meta, missing_meta_cps).cards_written, 2)
        self.check_cpp(missing_meta_cps, 2, 2)

        conflicting_meta = self.root / "modern-conflicting-meta.apkg"
        conflicting_cps = self.root / "modern-conflicting-meta.cps"
        make_modern_apkg(conflicting_meta, decoy, meta=b"\x08\x02")
        conflicting_cps.write_bytes(b"preserve prior output")
        with self.assertRaisesRegex(converter.ConversionError, "incompatível"):
            converter.convert_apkg(conflicting_meta, conflicting_cps)
        self.assertEqual(conflicting_cps.read_bytes(), b"preserve prior output")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checker", type=Path)
    parser.add_argument("--emit-e2e", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    global CHECKER
    CHECKER = args.checker
    if args.emit_e2e:
        args.emit_e2e.mkdir(parents=True, exist_ok=True)
        apkg = args.emit_e2e / "fixture.apkg"
        make_apkg(apkg)
        report = converter.convert_apkg(apkg, args.emit_e2e / "study" / "converted.cps")
        print(f"Generated {report.cards_written} Study cards in {args.emit_e2e / 'study' / 'converted.cps'}")
        return 0
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ConverterTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())

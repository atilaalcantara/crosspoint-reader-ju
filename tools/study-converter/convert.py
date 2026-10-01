#!/usr/bin/env python3
"""Convert a compatible legacy SQLite Anki .apkg to CrossPoint Study V1."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib
from collections import Counter
from dataclasses import dataclass, field
from html.parser import HTMLParser
from pathlib import Path
from typing import BinaryIO, Iterable

MAGIC = b"CPSTUDY\0"
VERSION = 1
HEADER_BYTES = 40
DECK_BYTES = 108
MAX_DECKS = 1024
MAX_CARDS = 100_000
MAX_DECK_NAME_BYTES = 96
MAX_FACE_BYTES = 4096
MAX_NOTE_FIELDS_BYTES = 64 * 1024
MAX_TEMPLATE_BYTES = 64 * 1024
MAX_COLLECTION_BYTES = 128 * 1024 * 1024
MAX_ARCHIVE_BYTES = 512 * 1024 * 1024
MAX_ARCHIVE_ENTRIES = 50_000
MAX_TEMPLATE_DEPTH = 24
SQLITE_MAGIC = b"SQLite format 3\0"
FIELD_SEPARATOR = "\x1f"
SOUND_RE = re.compile(r"\[sound:[^\]]*\]", re.IGNORECASE)
LATEX_RE = re.compile(r"\[latex\].*?\[/latex\]|\[\$\$\].*?\[/\$\$\]|\[\$\].*?\[/\]", re.IGNORECASE | re.DOTALL)


class ConversionError(Exception):
    """A package, collection, or conversion failure suitable for the CLI."""


class UnsupportedTemplate(ConversionError):
    """A template feature outside the explicitly supported subset."""


def _is_cloze_model(model: object) -> bool:
    if not isinstance(model, dict):
        return False
    try:
        return int(model.get("type", 0) or 0) == 1
    except (TypeError, ValueError):
        return False


@dataclass
class Report:
    decks: int = 0
    cards_written: int = 0
    cards_skipped: Counter[str] = field(default_factory=Counter)
    media_manifest_entries: int = 0
    image_references: int = 0
    sound_references: int = 0
    latex_references: int = 0
    cloze_cards_written: int = 0
    cloze_cards_skipped: int = 0
    warnings: list[str] = field(default_factory=list)

    def warn(self, message: str) -> None:
        if len(self.warnings) < 32:
            self.warnings.append(message)


class PlainTextExtractor(HTMLParser):
    """Strip markup while retaining readable block/line boundaries."""

    BLOCK_TAGS = {
        "address", "article", "blockquote", "div", "dl", "fieldset", "figcaption", "figure", "footer",
        "h1", "h2", "h3", "h4", "h5", "h6", "header", "li", "ol", "p", "pre", "section", "table",
        "tr", "ul",
    }
    SUPPRESSED_TAGS = {"audio", "script", "style", "video"}

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.parts: list[str] = []
        self.suppressed_depth = 0
        self.media_images = 0
        self.media_audio = 0
        self.latex_references = 0

    def _break(self) -> None:
        if self.parts and not self.parts[-1].endswith("\n"):
            self.parts.append("\n")

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        tag = tag.lower()
        if self.suppressed_depth:
            if tag in self.SUPPRESSED_TAGS:
                self.suppressed_depth += 1
            return
        if tag in self.SUPPRESSED_TAGS:
            if tag in {"audio", "video"}:
                self.media_audio += 1
            self.suppressed_depth = 1
        elif tag == "img":
            self.media_images += 1
            alt = next((value for key, value in attrs if key.lower() == "alt" and value), None)
            # HTMLParser normalizes character references in attribute values too.
            self.parts.append(f" {alt.strip()} " if alt and alt.strip() else " [image omitted] ")
        elif tag == "source":
            self.media_audio += 1
        elif tag == "br":
            self._break()
        elif tag in self.BLOCK_TAGS:
            self._break()

    def handle_endtag(self, tag: str) -> None:
        tag = tag.lower()
        if self.suppressed_depth:
            if tag in self.SUPPRESSED_TAGS:
                self.suppressed_depth -= 1
            return
        if tag in self.BLOCK_TAGS:
            self._break()

    def handle_data(self, data: str) -> None:
        if not self.suppressed_depth and data:
            data, latex_count = LATEX_RE.subn("[LaTeX omitted]", data)
            self.latex_references += latex_count
            cleaned, count = SOUND_RE.subn("", data)
            self.media_audio += count
            self.parts.append(cleaned)

    def text(self) -> str:
        # HTMLParser already decodes entities in text nodes. Avoid decoding a
        # second time: a literal &amp;lt; in a field must remain the text &lt;.
        raw = "".join(self.parts).replace("\r\n", "\n").replace("\r", "\n")
        lines = [re.sub(r"[\t\f\v ]+", " ", line).strip() for line in raw.split("\n")]
        while lines and not lines[0]:
            lines.pop(0)
        while lines and not lines[-1]:
            lines.pop()
        return "\n".join(lines)


def html_to_text(value: str) -> tuple[str, int, int, int]:
    parser = PlainTextExtractor()
    try:
        parser.feed(value)
        parser.close()
    except (AssertionError, ValueError) as exc:
        raise ConversionError(f"HTML malformado em campo/template: {exc}") from exc
    return parser.text(), parser.media_images, parser.media_audio, parser.latex_references


def _matching_section(template: str, start: int, field_name: str, depth: int) -> tuple[int, int]:
    if depth >= MAX_TEMPLATE_DEPTH:
        raise UnsupportedTemplate("condicionais de template aninhadas demais")
    token_re = re.compile(r"\{\{\s*([#^/])\s*([^{}]+?)\s*\}\}")
    stack = [field_name]
    for match in token_re.finditer(template, start):
        marker, name = match.group(1), match.group(2).strip()
        if marker in "#^":
            stack.append(name)
            if len(stack) > MAX_TEMPLATE_DEPTH:
                raise UnsupportedTemplate("condicionais de template aninhadas demais")
        else:
            if not stack or name != stack[-1]:
                raise UnsupportedTemplate(f"fechamento condicional inesperado: {name}")
            stack.pop()
            if not stack:
                return match.start(), match.end()
    raise UnsupportedTemplate(f"condicional sem fechamento: {field_name}")


def _cloze_expand(value: str, ordinal: int, reveal: bool) -> tuple[str, bool]:
    output: list[str] = []
    pos = 0
    found_ordinal = False
    while True:
        start = value.find("{{c", pos)
        if start < 0:
            output.append(value[pos:])
            break
        output.append(value[pos:start])
        end = value.find("}}", start + 3)
        if end < 0:
            raise UnsupportedTemplate("cloze sem fechamento")
        token = value[start + 2 : end]
        match = re.fullmatch(r"c(\d+(?:,\d+)*)::(.*)", token, re.DOTALL)
        if not match:
            raise UnsupportedTemplate("sintaxe cloze não suportada")
        numbers = [int(number) for number in match.group(1).split(",")]
        body = match.group(2)
        if "{{" in body or "}}" in body:
            raise UnsupportedTemplate("cloze aninhado não é suportado nesta versão")
        answer, separator, hint = body.partition("::")
        if ordinal + 1 in numbers:
            found_ordinal = True
            output.append(answer if reveal else f"[{hint if separator and hint else '…'}]")
        else:
            output.append(answer)
        pos = end + 2
    return "".join(output), found_ordinal


def render_template(
    template: str,
    fields: dict[str, str],
    specials: dict[str, str],
    *,
    front_side: str | None,
    cloze_ordinal: int | None,
    reveal_cloze: bool,
    cloze_seen: set[int],
    depth: int = 0,
) -> str:
    if depth > MAX_TEMPLATE_DEPTH:
        raise UnsupportedTemplate("templates condicionais aninhados demais")
    output: list[str] = []
    pos = 0
    while pos < len(template):
        start = template.find("{{", pos)
        if start < 0:
            output.append(template[pos:])
            break
        output.append(template[pos:start])
        end = template.find("}}", start + 2)
        if end < 0:
            raise UnsupportedTemplate("template contém {{ sem fechamento }}")
        token = template[start + 2 : end].strip()
        if token.startswith(("#", "^")):
            inverted = token[0] == "^"
            name = token[1:].strip()
            if not name or any(char.isspace() for char in name):
                raise UnsupportedTemplate(f"condicional inválida: {token}")
            body_start = end + 2
            body_end, section_end = _matching_section(template, body_start, name, depth)
            value = fields.get(name, specials.get(name, ""))
            present = bool(value.strip())
            if present != inverted:
                output.append(
                    render_template(
                        template[body_start:body_end], fields, specials, front_side=front_side,
                        cloze_ordinal=cloze_ordinal, reveal_cloze=reveal_cloze,
                        cloze_seen=cloze_seen, depth=depth + 1,
                    )
                )
            pos = section_end
            continue
        if token.startswith("/"):
            raise UnsupportedTemplate(f"fechamento condicional inesperado: {token}")
        if token == "FrontSide":
            if front_side is None:
                raise UnsupportedTemplate("{{FrontSide}} só pode aparecer no template verso")
            output.append(front_side)
        elif token.startswith("cloze:"):
            if cloze_ordinal is None:
                raise UnsupportedTemplate("filtro cloze usado fora de um modelo Cloze")
            field_name = token[len("cloze:") :]
            if field_name not in fields:
                raise UnsupportedTemplate(f"campo cloze desconhecido: {field_name}")
            expanded, found = _cloze_expand(fields[field_name], cloze_ordinal, reveal_cloze)
            output.append(expanded)
            if found:
                cloze_seen.add(cloze_ordinal)
        elif token.startswith("text:"):
            field_name = token[len("text:") :]
            if field_name not in fields:
                raise UnsupportedTemplate(f"filtro text para campo desconhecido: {field_name}")
            output.append(fields[field_name])
        elif ":" in token:
            raise UnsupportedTemplate(f"filtro de template não suportado: {token.split(':', 1)[0]}")
        else:
            name = token.strip()
            if name not in fields and name not in specials:
                raise UnsupportedTemplate(f"campo/template desconhecido: {name}")
            output.append(fields.get(name, specials.get(name, "")))
        pos = end + 2
    return "".join(output)


def _load_json(value: object, description: str) -> dict:
    if not isinstance(value, str):
        raise ConversionError(f"collection.anki2: {description} não é JSON textual")
    try:
        result = json.loads(value)
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise ConversionError(f"collection.anki2: JSON inválido em {description}") from exc
    if not isinstance(result, dict):
        raise ConversionError(f"collection.anki2: {description} deve ser um objeto JSON")
    return result


def _deck_records(decks_json: dict, selected_name: str | None) -> tuple[list[dict], dict[int, int], dict[int, str]]:
    records: list[dict] = []
    source_to_output: dict[int, int] = {}
    source_to_name: dict[int, str] = {}
    seen_source_ids: set[int] = set()
    for key, raw in decks_json.items():
        if not isinstance(raw, dict):
            raise ConversionError(f"registro de deck inválido em col.decks[{key!r}]")
        try:
            source_id = int(raw.get("id", key))
        except (TypeError, ValueError):
            raise ConversionError(f"ID de deck inválido em col.decks[{key!r}]")
        name = raw.get("name")
        if source_id <= 0 or not isinstance(name, str) or not name:
            raise ConversionError(f"ID/nome de deck inválido em col.decks[{key!r}]")
        if source_id in seen_source_ids:
            raise ConversionError(f"IDs de deck duplicados em col.decks[{key!r}]")
        seen_source_ids.add(source_id)
        try:
            is_dynamic = int(raw.get("dyn", 0) or 0) != 0
        except (TypeError, ValueError):
            raise ConversionError(f"flag dyn inválida no deck {name!r}")
        if is_dynamic:
            continue
        records.append({"source_id": source_id, "full_name": name, "dyn": False})
    names = [item["full_name"] for item in records]
    if len(names) != len(set(names)):
        raise ConversionError("a collection contém nomes de deck duplicados")
    source_ids = [item["source_id"] for item in records]
    if len(source_ids) != len(set(source_ids)):
        raise ConversionError("a coleção contém IDs de deck duplicados")
    if selected_name:
        if selected_name not in names:
            raise ConversionError(f"deck não encontrado: {selected_name}")
        records = [item for item in records if item["full_name"] == selected_name or item["full_name"].startswith(selected_name + "::")]
    if not records:
        raise ConversionError("nenhum deck normal foi encontrado na coleção")
    if len(records) > MAX_DECKS:
        raise ConversionError(f"a seleção tem {len(records)} decks; Study V1 aceita no máximo {MAX_DECKS}")

    records.sort(key=lambda item: item["full_name"])
    full_names = {item["full_name"]: item["source_id"] for item in records}
    for output_id, item in enumerate(records, 1):
        full_name = item["full_name"]
        parent_name = full_name.rpartition("::")[0]
        parent_source = full_names.get(parent_name, 0)
        if selected_name and full_name == selected_name:
            parent_source = 0
        item["id"] = output_id
        item["parent_source"] = parent_source
        item["name"] = full_name.rsplit("::", 1)[-1]
        encoded = item["name"].encode("utf-8", errors="strict")
        if not encoded or len(encoded) > MAX_DECK_NAME_BYTES:
            raise ConversionError(f"nome de deck fora do limite Study V1: {full_name!r}")
        if any(byte == 0 or byte == 13 or (byte < 32 and byte not in (9, 10)) for byte in encoded):
            raise ConversionError(f"nome de deck contém caracteres de controle: {full_name!r}")
        source_to_output[item["source_id"]] = output_id
        source_to_name[item["source_id"]] = full_name
    for item in records:
        item["parent_id"] = source_to_output.get(item["parent_source"], 0)
    return records, source_to_output, source_to_name


def _template_context(model: dict, fields: dict[str, str], tags: str, deck_name: str, template: dict) -> tuple[dict, str]:
    specials = dict(fields)
    specials.update(
        {
            "Tags": tags,
            "Type": str(model.get("name", "")),
            "Deck": deck_name,
            "Subdeck": deck_name.rsplit("::", 1)[-1],
            "Card": str(template.get("name", "")),
        }
    )
    return specials, str(model.get("name", ""))


def _choose_template(model: dict, ordinal: int) -> dict:
    templates = model.get("tmpls")
    if not isinstance(templates, list) or not templates:
        raise UnsupportedTemplate("modelo sem templates de cartão")
    def checked(template: dict) -> dict:
        for key in ("qfmt", "afmt"):
            value = template.get(key, "")
            if not isinstance(value, str) or len(value.encode("utf-8")) > MAX_TEMPLATE_BYTES:
                raise UnsupportedTemplate("template ausente ou excede o limite de 64 KiB")
        return template

    if int(model.get("type", 0)) == 1:
        if not isinstance(templates[0], dict):
            raise UnsupportedTemplate("template Cloze inválido")
        return checked(templates[0])
    for position, template in enumerate(templates):
        if isinstance(template, dict) and int(template.get("ord", position)) == ordinal:
            return checked(template)
    raise UnsupportedTemplate(f"template ordinal {ordinal} inexistente")


def _render_card(model: dict, template: dict, raw_fields: str, tags: str, deck_name: str,
                 ordinal: int) -> tuple[str, str, int, int, int, bool]:
    fields_meta = model.get("flds")
    if not isinstance(fields_meta, list):
        raise UnsupportedTemplate("modelo sem lista de campos")
    values = raw_fields.split(FIELD_SEPARATOR)
    names = [str(field_info.get("name", "")) for field_info in fields_meta if isinstance(field_info, dict)]
    if len(names) != len(fields_meta) or len(values) != len(names):
        raise UnsupportedTemplate("quantidade de campos da nota não corresponde ao modelo")
    fields = dict(zip(names, values))
    is_cloze = int(model.get("type", 0)) == 1
    ordinal = int(ordinal)
    if ordinal < 0:
        raise UnsupportedTemplate("ordinal negativo de cartão")
    specials, _ = _template_context(model, fields, tags, deck_name, template)
    if is_cloze:
        cloze_numbers: set[int] = set()
        for value in fields.values():
            for match in re.finditer(r"\{\{c(\d+(?:,\d+)*)::", value):
                cloze_numbers.update(int(part) for part in match.group(1).split(","))
        for number in cloze_numbers:
            specials[f"c{number}"] = "1" if number == ordinal + 1 else ""
    cloze_seen: set[int] = set()
    question = render_template(
        str(template.get("qfmt", "")), fields, specials, front_side=None,
        cloze_ordinal=ordinal if is_cloze else None, reveal_cloze=False, cloze_seen=cloze_seen,
    )
    if is_cloze and ordinal not in cloze_seen:
        raise UnsupportedTemplate("ordinal cloze não corresponde a nenhuma exclusão")
    front_seen = set(cloze_seen)
    answer_seen: set[int] = set()
    answer = render_template(
        str(template.get("afmt", "")), fields, specials, front_side=question,
        cloze_ordinal=ordinal if is_cloze else None, reveal_cloze=True, cloze_seen=answer_seen,
    )
    if is_cloze and ordinal not in front_seen:
        raise UnsupportedTemplate("cloze sem exclusão no lado pergunta")
    front_text, front_images, front_audio, front_latex = html_to_text(question)
    back_text, back_images, back_audio, back_latex = html_to_text(answer)
    return (front_text, back_text, front_images + back_images, front_audio + back_audio,
            front_latex + back_latex, is_cloze)


def _read_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while offset < len(data) and shift < 70:
        byte = data[offset]
        offset += 1
        if shift == 63 and byte > 1:
            raise ConversionError("protobuf Anki contém varint fora do limite uint64")
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7
    raise ConversionError("protobuf Anki truncado ou com varint inválido")


def _protobuf_fields(data: bytes) -> Iterable[tuple[int, int, int | bytes]]:
    offset = 0
    while offset < len(data):
        tag, offset = _read_varint(data, offset)
        number, wire = tag >> 3, tag & 7
        if number == 0:
            raise ConversionError("protobuf Anki contém campo zero")
        if wire == 0:
            value, offset = _read_varint(data, offset)
            yield number, wire, value
        elif wire == 1:
            if offset + 8 > len(data):
                raise ConversionError("protobuf Anki truncado")
            yield number, wire, data[offset : offset + 8]
            offset += 8
        elif wire == 2:
            length, offset = _read_varint(data, offset)
            if length > len(data) - offset:
                raise ConversionError("protobuf Anki truncado")
            yield number, wire, data[offset : offset + length]
            offset += length
        elif wire == 5:
            if offset + 4 > len(data):
                raise ConversionError("protobuf Anki truncado")
            yield number, wire, data[offset : offset + 4]
            offset += 4
        else:
            raise ConversionError("protobuf Anki usa wire type não suportado")


def _package_version(archive: zipfile.ZipFile) -> int:
    matches = [info for info in archive.infolist() if info.filename == "meta"]
    if not matches:
        # collection.anki21b is specific to modern v3 exports; do not allow a
        # valid legacy collection-isca to win merely because meta is missing.
        return 3 if any(info.filename == "collection.anki21b" for info in archive.infolist()) else 1
    if len(matches) != 1 or matches[0].file_size > 4096:
        raise ConversionError("metadados de versão do .apkg inválidos")
    metadata = archive.read(matches[0])
    package_version: int | None = None
    for number, wire, value in _protobuf_fields(metadata):
        if number == 1 and wire == 0:
            package_version = int(value)
    if package_version is None:
        raise ConversionError("metadados de versão do .apkg não contêm a versão")
    return package_version


def _copy_zstd_member(archive: zipfile.ZipFile, info: zipfile.ZipInfo, destination: Path,
                      maximum_size: int) -> int:
    executable = shutil.which("zstd")
    if executable is None:
        raise ConversionError("pacotes Anki modernos exigem o utilitário zstd no PATH")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix="study-zstd-", suffix=".zst", delete=False) as compressed:
        compressed_path = Path(compressed.name)
        try:
            with archive.open(info) as source:
                while True:
                    chunk = source.read(256 * 1024)
                    if not chunk:
                        break
                    compressed.write(chunk)
            compressed.flush()
            os.fsync(compressed.fileno())
            with destination.open("wb") as output:
                process = subprocess.Popen(
                    [executable, "-d", "-q", "-c", "--", str(compressed_path)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                try:
                    if process.stdout is None:
                        raise ConversionError("zstd não criou saída de descompressão")
                    total = 0
                    while True:
                        chunk = process.stdout.read(256 * 1024)
                        if not chunk:
                            break
                        total += len(chunk)
                        if total > maximum_size:
                            raise ConversionError(f"conteúdo Zstandard excede o limite de {maximum_size} bytes")
                        output.write(chunk)
                    stderr = process.stderr.read() if process.stderr else b""
                    status = process.wait()
                    if status:
                        raise ConversionError(
                            f"falha ao descompactar conteúdo Zstandard: {stderr.decode('utf-8', 'replace').strip()}"
                        )
                    output.flush()
                    os.fsync(output.fileno())
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.wait()
                    if process.stdout is not None:
                        process.stdout.close()
                    if process.stderr is not None:
                        process.stderr.close()
            return total
        finally:
            try:
                compressed_path.unlink()
            except OSError:
                pass


def _decode_media_v3(data: bytes) -> int:
    count = 0
    for number, wire, entry in _protobuf_fields(data):
        if number != 1 or wire != 2 or not isinstance(entry, bytes):
            continue
        name = ""
        for entry_number, entry_wire, value in _protobuf_fields(entry):
            if entry_number == 1 and entry_wire == 2 and isinstance(value, bytes):
                try:
                    name = value.decode("utf-8", errors="strict")
                except UnicodeDecodeError as exc:
                    raise ConversionError("nome UTF-8 inválido no manifesto de mídia") from exc
        if not name:
            raise ConversionError("entrada sem nome no manifesto de mídia")
        count += 1
    return count


def _parse_media_manifest(archive: zipfile.ZipFile, package_version: int) -> int:
    matches = [info for info in archive.infolist() if info.filename == "media"]
    if not matches:
        return 0
    if len(matches) != 1 or matches[0].file_size > 4 * 1024 * 1024:
        raise ConversionError("manifest de mídia duplicado ou grande demais")
    if package_version >= 3:
        with tempfile.TemporaryDirectory(prefix="study-media-manifest-") as temporary_dir:
            decoded_path = Path(temporary_dir) / "media.pb"
            _copy_zstd_member(archive, matches[0], decoded_path, 4 * 1024 * 1024)
            return _decode_media_v3(decoded_path.read_bytes())
    try:
        manifest = json.loads(archive.read(matches[0]).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError, zipfile.BadZipFile) as exc:
        raise ConversionError("manifest de mídia inválido") from exc
    if not isinstance(manifest, dict):
        raise ConversionError("manifest de mídia deve ser um objeto JSON")
    return len(manifest)


def _select_collection(archive: zipfile.ZipFile) -> tuple[zipfile.ZipInfo, bool]:
    entries = archive.infolist()
    if len(entries) > MAX_ARCHIVE_ENTRIES:
        raise ConversionError(f"ZIP tem mais de {MAX_ARCHIVE_ENTRIES} entradas")
    if sum(info.file_size for info in entries) > MAX_ARCHIVE_BYTES:
        raise ConversionError("conteúdo descompactado do ZIP excede o limite de 512 MiB")
    by_name: dict[str, list[zipfile.ZipInfo]] = {}
    for info in entries:
        by_name.setdefault(info.filename, []).append(info)
    package_version = _package_version(archive)
    if package_version >= 3:
        if "collection.anki21b" not in by_name or len(by_name["collection.anki21b"]) != 1:
            raise ConversionError("pacote Anki v3 sem collection.anki21b única")
        info = by_name["collection.anki21b"][0]
        if info.file_size > MAX_COLLECTION_BYTES:
            raise ConversionError("banco SQLite comprimido excede o limite de 128 MiB")
        return info, True
    if "collection.anki21b" in by_name:
        raise ConversionError("meta do .apkg é incompatível com a presença de collection.anki21b")
    for name in ("collection.anki2", "collection.anki21"):
        if name not in by_name:
            continue
        if len(by_name[name]) != 1:
            raise ConversionError(f"ZIP contém múltiplas entradas {name}")
        info = by_name[name][0]
        if info.file_size > MAX_COLLECTION_BYTES:
            raise ConversionError("banco SQLite excede o limite de 128 MiB")
        with archive.open(info) as stream:
            if stream.read(len(SQLITE_MAGIC)) != SQLITE_MAGIC:
                continue
        return info, False
    if "collection.anki21b" in by_name:
        raise ConversionError(
            "este .apkg usa collection.anki21b (Zstandard), que não pode ser aberto apenas com a biblioteca padrão; "
            "reexporte no Anki Desktop marcando ‘Support older Anki versions’"
        )
    raise ConversionError(".apkg sem collection.anki2/collection.anki21 SQLite compatível")


def _copy_collection(archive: zipfile.ZipFile, info: zipfile.ZipInfo, destination: Path) -> None:
    total = 0
    with archive.open(info) as source, destination.open("wb") as output:
        while True:
            chunk = source.read(256 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > MAX_COLLECTION_BYTES:
                raise ConversionError("collection SQLite ultrapassou o limite de 128 MiB")
            output.write(chunk)
        output.flush()
        os.fsync(output.fileno())
    with destination.open("rb") as check:
        if check.read(len(SQLITE_MAGIC)) != SQLITE_MAGIC:
            raise ConversionError(f"{info.filename} não contém SQLite válido")


def _columns(connection: sqlite3.Connection, table: str) -> set[str]:
    return {str(row[1]) for row in connection.execute(f'PRAGMA table_info("{table}")')}


def _collection_metadata(connection: sqlite3.Connection) -> tuple[dict, dict]:
    if not {"models", "decks"}.issubset(_columns(connection, "col")):
        raise ConversionError("schema SQLite sem campos col.models/col.decks suportados")
    row = connection.execute("SELECT models, decks FROM col ORDER BY id LIMIT 1").fetchone()
    if row is None:
        raise ConversionError("collection SQLite sem registro col")
    models = {} if row[0] in (None, "") else _load_json(row[0], "col.models")
    decks = {} if row[1] in (None, "") else _load_json(row[1], "col.decks")
    if models and decks:
        return models, decks
    return _normalized_collection_metadata(connection)


def _normalized_collection_metadata(connection: sqlite3.Connection) -> tuple[dict, dict]:
    tables = {str(row[0]) for row in connection.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    if not {"notetypes", "fields", "templates", "decks"}.issubset(tables):
        raise ConversionError("collection sem metadados de modelos/decks compatíveis")
    for table, required in {
        "notetypes": {"id", "name", "config"},
        "fields": {"ntid", "ord", "name"},
        "templates": {"ntid", "ord", "name", "config"},
        "decks": {"id", "name", "kind"},
    }.items():
        if not required.issubset(_columns(connection, table)):
            raise ConversionError(f"schema moderno incompatível na tabela {table}")

    fields_by_model: dict[int, list[dict]] = {}
    for model_id, ordinal, name in connection.execute("SELECT ntid, ord, name FROM fields ORDER BY ntid, ord"):
        fields_by_model.setdefault(int(model_id), []).append({"ord": int(ordinal), "name": str(name)})
    templates_by_model: dict[int, list[dict]] = {}
    for model_id, ordinal, name, config in connection.execute(
        "SELECT ntid, ord, name, config FROM templates ORDER BY ntid, ord"
    ):
        qfmt = ""
        afmt = ""
        for number, wire, value in _protobuf_fields(bytes(config or b"")):
            if number in (1, 2) and wire == 2 and isinstance(value, bytes):
                try:
                    decoded = value.decode("utf-8", errors="strict")
                except UnicodeDecodeError as exc:
                    raise ConversionError("template moderno não está em UTF-8") from exc
                if number == 1:
                    qfmt = decoded
                else:
                    afmt = decoded
        templates_by_model.setdefault(int(model_id), []).append(
            {"ord": int(ordinal), "name": str(name), "qfmt": qfmt, "afmt": afmt}
        )

    models: dict[str, dict] = {}
    for model_id, name, config in connection.execute("SELECT id, name, config FROM notetypes"):
        model_type = 0
        for number, wire, value in _protobuf_fields(bytes(config or b"")):
            if number == 1 and wire == 0:
                model_type = int(value)
                break
            if number == 1 and wire == 2 and isinstance(value, bytes):
                for inner_number, inner_wire, inner_value in _protobuf_fields(value):
                    if inner_number == 1 and inner_wire == 0:
                        model_type = int(inner_value)
                        break
                if model_type:
                    break
        model_templates = templates_by_model.get(int(model_id), [])
        # Cloze note types declare cloze rendering in their card template.
        if not model_type and any("{{cloze:" in str(t.get("qfmt", "")) for t in model_templates):
            model_type = 1
        models[str(int(model_id))] = {
            "id": int(model_id), "name": str(name), "type": model_type,
            "flds": fields_by_model.get(int(model_id), []),
            "tmpls": model_templates,
        }

    decks: dict[str, dict] = {}
    for deck_id, name, kind in connection.execute("SELECT id, name, kind FROM decks NOT INDEXED"):
        is_filtered = False
        for number, wire, value in _protobuf_fields(bytes(kind or b"")):
            if number == 2 and wire == 2 and isinstance(value, bytes):
                is_filtered = True
        full_name = str(name).replace("\x1f", "::")
        decks[str(int(deck_id))] = {"id": int(deck_id), "name": full_name, "dyn": int(is_filtered)}
    return models, decks


def _write_deck_record(output: BinaryIO, deck: dict, id_map: dict[int, int]) -> None:
    name = deck["name"].encode("utf-8")
    record = struct.pack("<IIHH", deck["id"], id_map.get(deck["parent_source"], 0), len(name), 0)
    output.write(record)
    output.write(name)
    output.write(bytes(96 - len(name)))


def _crc_stream(file: BinaryIO, offset: int, length: int) -> int:
    file.seek(offset)
    crc = 0
    remaining = length
    while remaining:
        chunk = file.read(min(256 * 1024, remaining))
        if not chunk:
            raise ConversionError("arquivo temporário truncado durante CRC")
        crc = zlib.crc32(chunk, crc)
        remaining -= len(chunk)
    return crc & 0xFFFFFFFF


def validate_cps_file(path: Path) -> tuple[int, int, int]:
    """Independent Python structural check before atomic replacement."""
    with path.open("rb") as file:
        header = file.read(HEADER_BYTES)
        if len(header) != HEADER_BYTES:
            raise ConversionError("validação interna: header truncado")
        magic, version, header_size, file_size, deck_count, card_count, deck_offset, index_offset, data_offset, crc = struct.unpack(
            "<8sHH7I", header
        )
        actual_size = path.stat().st_size
        if (magic != MAGIC or version != VERSION or header_size != HEADER_BYTES or file_size != actual_size
                or deck_count < 1 or deck_count > MAX_DECKS or card_count > MAX_CARDS
                or deck_offset != HEADER_BYTES or index_offset != deck_offset + deck_count * DECK_BYTES
                or data_offset != index_offset + card_count * 4 or data_offset > file_size):
            raise ConversionError("validação interna: header/offsets inválidos")
        if _crc_stream(file, HEADER_BYTES, file_size - HEADER_BYTES) != crc:
            raise ConversionError("validação interna: CRC inválido")
        file.seek(deck_offset)
        deck_ids: set[int] = set()
        parents: dict[int, int] = {}
        for _ in range(deck_count):
            record = file.read(DECK_BYTES)
            if len(record) != DECK_BYTES:
                raise ConversionError("validação interna: tabela de decks truncada")
            deck_id, parent_id, name_len, flags = struct.unpack_from("<IIHH", record)
            if not deck_id or deck_id in deck_ids or not 0 < name_len <= MAX_DECK_NAME_BYTES or flags:
                raise ConversionError("validação interna: registro de deck inválido")
            try:
                name = record[12 : 12 + name_len].decode("utf-8", errors="strict")
            except UnicodeDecodeError as exc:
                raise ConversionError("validação interna: nome de deck UTF-8 inválido") from exc
            if not name or any(byte == 0 or byte == 13 or (byte < 32 and byte not in (9, 10)) for byte in record[12 : 12 + name_len]):
                raise ConversionError("validação interna: nome de deck contém controle inválido")
            if any(record[12 + name_len :]):
                raise ConversionError("validação interna: padding do deck inválido")
            deck_ids.add(deck_id)
            parents[deck_id] = parent_id
        for deck_id, parent_id in parents.items():
            if parent_id and parent_id not in deck_ids:
                raise ConversionError("validação interna: deck pai ausente")
            seen = {deck_id}
            while parent_id:
                if parent_id in seen:
                    raise ConversionError("validação interna: ciclo na hierarquia")
                seen.add(parent_id)
                parent_id = parents[parent_id]

        file.seek(index_offset)
        offsets_raw = file.read(card_count * 4)
        if len(offsets_raw) != card_count * 4:
            raise ConversionError("validação interna: índice de cards truncado")
        offsets = struct.unpack(f"<{card_count}I", offsets_raw) if card_count else ()
        previous_id = 0
        expected_offset = data_offset
        for offset in offsets:
            if offset != expected_offset:
                raise ConversionError("validação interna: offsets não contíguos")
            file.seek(offset)
            prefix = file.read(16)
            if len(prefix) != 16:
                raise ConversionError("validação interna: prefixo de card truncado")
            card_id, deck_id, front_len, back_len = struct.unpack("<QIHH", prefix)
            if not card_id > previous_id or deck_id not in deck_ids or front_len > MAX_FACE_BYTES or back_len > MAX_FACE_BYTES:
                raise ConversionError("validação interna: ID/deck/limite de card inválido")
            front = file.read(front_len)
            back = file.read(back_len)
            if len(front) != front_len or len(back) != back_len:
                raise ConversionError("validação interna: conteúdo de card truncado")
            try:
                front.decode("utf-8", errors="strict")
                back.decode("utf-8", errors="strict")
            except UnicodeDecodeError as exc:
                raise ConversionError("validação interna: texto UTF-8 inválido") from exc
            if any(byte == 0 or byte == 13 or (byte < 32 and byte not in (9, 10)) for byte in front + back):
                raise ConversionError("validação interna: controle inválido no texto")
            previous_id = card_id
            expected_offset += 16 + front_len + back_len
        if expected_offset != file_size:
            raise ConversionError("validação interna: conteúdo extra/truncado após último card")
        return deck_count, card_count, crc


def _convert_collection(connection: sqlite3.Connection, archive_media_count: int, deck_filter: str | None,
                        destination: Path) -> Report:
    models_json, decks_json = _collection_metadata(connection)
    decks, deck_id_map, deck_names = _deck_records(decks_json, deck_filter)
    if not {"id", "nid", "did", "ord"}.issubset(_columns(connection, "cards")):
        raise ConversionError("schema cards sem id/nid/did/ord")
    note_columns = _columns(connection, "notes")
    if not {"id", "mid", "flds"}.issubset(note_columns):
        raise ConversionError("schema notes sem id/mid/flds")
    tags_expr = "notes.tags" if "tags" in note_columns else "''"
    cards_cols = _columns(connection, "cards")
    odid_expr = "cards.odid" if "odid" in cards_cols else "0"
    query = f"""SELECT cards.id, cards.nid, cards.did, cards.ord, {odid_expr},
                      notes.mid, notes.flds, {tags_expr}
                 FROM cards JOIN notes ON notes.id = cards.nid
                ORDER BY cards.id"""

    models: dict[int, dict] = {}
    for key, model in models_json.items():
        if not isinstance(model, dict):
            continue
        try:
            model_id = int(model.get("id", key))
        except (TypeError, ValueError):
            continue
        if model_id <= 0:
            continue
        models[model_id] = model
    if not models:
        raise ConversionError("a coleção não contém modelos de nota válidos")

    report = Report(decks=len(decks), media_manifest_entries=archive_media_count)
    offset_records: list[int] = []
    previous_card_id = 0
    with tempfile.TemporaryFile(mode="w+b") as payload:
        cursor = connection.execute(query)
        for row in cursor:
            card_id, note_id, deck_id, ordinal, original_deck_id, model_id, raw_fields, raw_tags = row
            try:
                if not all(isinstance(value, int) and not isinstance(value, bool)
                           for value in (card_id, deck_id, ordinal, model_id)):
                    raise ValueError("ID/ordinal não-inteiro")
                if original_deck_id is not None and not isinstance(original_deck_id, int):
                    raise ValueError("ID de deck original não-inteiro")
                card_id = int(card_id)
                deck_id = int(deck_id)
                ordinal = int(ordinal)
                original_deck_id = int(original_deck_id or 0)
                model_id = int(model_id)
            except (TypeError, ValueError):
                report.cards_skipped["invalid-card-row"] += 1
                continue
            if card_id <= previous_card_id or card_id <= 0:
                raise ConversionError("IDs de card não são positivos/únicos em ordem crescente")
            previous_card_id = card_id
            assigned_deck = deck_id if deck_id in deck_id_map else original_deck_id
            output_deck_id = deck_id_map.get(assigned_deck)
            if output_deck_id is None:
                continue
            model = models.get(model_id)
            if model is None or not isinstance(raw_fields, str):
                report.cards_skipped["missing-note-model-or-fields"] += 1
                if _is_cloze_model(model):
                    report.cloze_cards_skipped += 1
                continue
            if len(raw_fields.encode("utf-8")) > MAX_NOTE_FIELDS_BYTES:
                report.cards_skipped["note-fields-over-limit"] += 1
                if _is_cloze_model(model):
                    report.cloze_cards_skipped += 1
                report.warn(f"note {note_id} ignorada: campos excedem 64 KiB")
                continue
            try:
                template = _choose_template(model, ordinal)
                deck_name = deck_names[assigned_deck]
                front, back, images, audio, latex_count, was_cloze = _render_card(
                    model, template, raw_fields, str(raw_tags or ""), deck_name, ordinal
                )
            except (UnsupportedTemplate, ValueError, TypeError) as exc:
                report.cards_skipped["unsupported-template-or-cloze"] += 1
                if _is_cloze_model(model):
                    report.cloze_cards_skipped += 1
                report.warn(f"card {card_id} ignorado: {exc}")
                continue
            report.image_references += images
            report.sound_references += audio
            report.latex_references += latex_count
            if not front:
                report.cards_skipped["empty-front"] += 1
                if was_cloze:
                    report.cloze_cards_skipped += 1
                report.warn(f"card {card_id} ignorado: Anki não gera cartão com frente vazia")
                continue
            if len(front.encode("utf-8")) > MAX_FACE_BYTES or len(back.encode("utf-8")) > MAX_FACE_BYTES:
                report.cards_skipped["face-over-limit"] += 1
                if was_cloze:
                    report.cloze_cards_skipped += 1
                report.warn(f"card {card_id} ignorado: frente/verso excede 4 KiB")
                continue
            if any(byte == 0 or byte == 13 or (byte < 32 and byte not in (9, 10))
                   for byte in front.encode("utf-8") + back.encode("utf-8")):
                report.cards_skipped["control-character"] += 1
                if was_cloze:
                    report.cloze_cards_skipped += 1
                report.warn(f"card {card_id} ignorado: caractere de controle não é permitido em Study V1")
                continue
            if not front and not back:
                report.cards_skipped["empty-card"] += 1
                if was_cloze:
                    report.cloze_cards_skipped += 1
                continue
            if len(offset_records) >= MAX_CARDS:
                raise ConversionError(f"a seleção excede o limite Study V1 de {MAX_CARDS} cards")
            front_bytes = front.encode("utf-8")
            back_bytes = back.encode("utf-8")
            offset_records.append(payload.tell())
            payload.write(struct.pack("<QIHH", card_id, output_deck_id, len(front_bytes), len(back_bytes)))
            payload.write(front_bytes)
            payload.write(back_bytes)
            if was_cloze:
                report.cloze_cards_written += 1
        report.cards_written = len(offset_records)
        index_offset = HEADER_BYTES + len(decks) * DECK_BYTES
        data_offset = index_offset + len(offset_records) * 4
        payload_size = payload.tell()
        file_size = data_offset + payload_size
        if file_size > 0xFFFFFFFF:
            raise ConversionError(".cps excede o limite de offsets uint32")

        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary_path: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(mode="w+b", prefix=f".{destination.name}.", suffix=".tmp",
                                             dir=destination.parent, delete=False) as output:
                temporary_path = Path(output.name)
                output.write(bytes(HEADER_BYTES))
                for deck in decks:
                    _write_deck_record(output, deck, deck_id_map)
                for relative_offset in offset_records:
                    output.write(struct.pack("<I", data_offset + relative_offset))
                payload.seek(0)
                while True:
                    chunk = payload.read(256 * 1024)
                    if not chunk:
                        break
                    output.write(chunk)
                output.seek(0)
                output.write(struct.pack("<8sHH7I", MAGIC, VERSION, HEADER_BYTES, file_size, len(decks),
                                         report.cards_written, HEADER_BYTES, index_offset, data_offset, 0))
                output.flush()
                crc = _crc_stream(output, HEADER_BYTES, file_size - HEADER_BYTES)
                output.seek(36)
                output.write(struct.pack("<I", crc))
                output.flush()
                os.fsync(output.fileno())
            validate_cps_file(temporary_path)
            os.replace(temporary_path, destination)
            temporary_path = None
            try:
                dir_fd = os.open(destination.parent, os.O_RDONLY)
                try:
                    os.fsync(dir_fd)
                finally:
                    os.close(dir_fd)
            except OSError:
                pass
        finally:
            if temporary_path is not None:
                try:
                    temporary_path.unlink()
                except OSError:
                    pass
    return report


def convert_apkg(input_path: Path, output_path: Path, deck_filter: str | None = None) -> Report:
    if not input_path.is_file():
        raise ConversionError(f"arquivo de entrada não encontrado: {input_path}")
    if input_path.suffix.lower() != ".apkg":
        raise ConversionError("a entrada deve ser um pacote de deck .apkg (coleções .colpkg não são aceitas)")
    if input_path.resolve() == output_path.resolve():
        raise ConversionError("entrada e saída não podem ser o mesmo arquivo")
    if output_path.suffix.lower() != ".cps":
        raise ConversionError("a extensão de saída deve ser .cps")
    if input_path.name.lower() == "collection.apkg" and not deck_filter:
        raise ConversionError("collection.apkg contém a coleção inteira; especifique --deck para selecionar uma subtree")
    try:
        with zipfile.ZipFile(input_path, "r") as archive:
            collection, is_zstd = _select_collection(archive)
            package_version = _package_version(archive)
            media_count = _parse_media_manifest(archive, package_version)
            with tempfile.TemporaryDirectory(prefix="study-apkg-") as temporary_dir:
                database_path = Path(temporary_dir) / "collection.sqlite"
                if is_zstd:
                    _copy_zstd_member(archive, collection, database_path, MAX_COLLECTION_BYTES)
                    with database_path.open("rb") as check:
                        if check.read(len(SQLITE_MAGIC)) != SQLITE_MAGIC:
                            raise ConversionError("collection.anki21b não descompacta para SQLite")
                else:
                    _copy_collection(archive, collection, database_path)
                uri = database_path.resolve().as_uri() + "?mode=ro"
                connection = sqlite3.connect(uri, uri=True)
                try:
                    connection.create_collation(
                        "unicase", lambda left, right: (left.lower() > right.lower()) - (left.lower() < right.lower())
                    )
                    connection.execute("PRAGMA query_only=ON")
                    connection.execute("BEGIN")
                    return _convert_collection(connection, media_count, deck_filter, output_path)
                except sqlite3.DatabaseError as exc:
                    raise ConversionError(f"SQLite da coleção está inválido ou incompatível: {exc}") from exc
                finally:
                    connection.close()
    except zipfile.BadZipFile as exc:
        raise ConversionError("arquivo .apkg não é um ZIP válido") from exc
    except UnicodeError as exc:
        raise ConversionError("texto/metadata SQLite contém UTF-8 inválido") from exc
    except (OSError, RuntimeError, zipfile.LargeZipFile) as exc:
        raise ConversionError(f"falha ao ler .apkg: {exc}") from exc


def _report(report: Report, output: Path) -> None:
    print(f"Decks incluídos: {report.decks}")
    print(f"Cards convertidos: {report.cards_written}")
    print(f"Cards ignorados: {sum(report.cards_skipped.values())}")
    print(f"Cards Cloze incluídos/ignorados: {report.cloze_cards_written}/{report.cloze_cards_skipped}")
    for reason, count in sorted(report.cards_skipped.items()):
        print(f"  {reason}: {count}")
    print(f"Mídias detectadas no manifesto: {report.media_manifest_entries} (não copiadas)")
    print(f"Referências omitidas no conteúdo renderizado: imagens={report.image_references}, áudio={report.sound_references}")
    print(f"Blocos LaTeX omitidos: {report.latex_references}")
    for warning in report.warnings:
        print(f"Aviso: {warning}")
    if len(report.warnings) >= 32:
        print("Aviso: demais avisos foram agrupados no resumo de ignorados.")
    print(f"Saída: {output}")


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Converte um .apkg SQLite compatível para pacote textual Study V1 .cps (offline).",
        epilog=("Suporte: collection.anki2 / collection.anki21 SQLite, fields/templates simples, FrontSide, "
                "condicionais e cloze básico não aninhado. collection.anki21b/Zstandard, filtros avançados e mídia "
                "não são suportados pelo firmware Study V1."),
    )
    parser.add_argument("input", type=Path, help="arquivo .apkg exportado do Anki Desktop")
    parser.add_argument("output", type=Path, help="arquivo Study V1 .cps a criar/substituir atomicamente")
    parser.add_argument("--deck", help="nome exato do deck (inclua :: para subdeck); inclui seus descendentes")
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    args = build_argument_parser().parse_args(argv)
    try:
        report = convert_apkg(args.input, args.output, args.deck)
    except ConversionError as exc:
        print(f"Erro: {exc}", file=sys.stderr)
        return 2
    _report(report, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Compare Python and .NET Study V1 output card-by-card for the same APKG."""

from __future__ import annotations

import argparse
import importlib.util
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "test" / "study_converter" / "test_converter.py"
SPEC = importlib.util.spec_from_file_location("converter_tests", TESTS)
assert SPEC and SPEC.loader
test_module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = test_module
SPEC.loader.exec_module(test_module)
python_converter = test_module.converter


def read_cps(path: Path) -> tuple[dict[int, dict], dict[int, dict]]:
    decks, cards = test_module.read_cps(path)
    return {deck["id"]: deck for deck in decks}, {card["id"]: card for card in cards}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apkg", nargs="?", type=Path, help="APKG real; omitted for the checked-in synthetic fixture builder")
    parser.add_argument("--fixture", choices=("legacy", "modern", "modern-missing-meta"), default="legacy", help="fixture synthetic when APKG is omitted")
    parser.add_argument("--dotnet-dll", type=Path, default=ROOT / "tools/study-converter-dotnet/src/StudyConverter.Cli/bin/Release/net10.0/StudyConverter.Cli.dll")
    parser.add_argument("--checker", type=Path, default=ROOT / "build/host-tests/study_v1/study_package_check")
    parser.add_argument("--deck", help="optional full deck name/subtree")
    args = parser.parse_args()
    if not args.dotnet_dll.is_file():
        parser.error(f"CLI .NET não encontrado: {args.dotnet_dll}; rode dotnet build primeiro")

    with tempfile.TemporaryDirectory(prefix="study-differential-") as temp_name:
        temp = Path(temp_name)
        apkg = args.apkg
        if apkg is None:
            apkg = temp / "fixture.apkg"
            if args.fixture == "modern":
                decoy = temp / "decoy.sqlite"
                test_module.make_collection(decoy)
                test_module.make_modern_apkg(apkg, decoy)
            elif args.fixture == "modern-missing-meta":
                decoy = temp / "decoy.sqlite"
                test_module.make_collection(decoy)
                test_module.make_modern_apkg(apkg, decoy, meta=None)
            else:
                test_module.make_apkg(apkg)
        py_cps, dotnet_cps = temp / "python.cps", temp / "dotnet.cps"
        py_report = python_converter.convert_apkg(apkg, py_cps, args.deck)
        command = ["dotnet", str(args.dotnet_dll), str(apkg), str(dotnet_cps)]
        if args.deck:
            command.append(args.deck)
        run = subprocess.run(command, text=True, capture_output=True)
        print(".NET summary:", run.stdout.strip())
        if run.returncode != 0:
            print(".NET error:", run.stderr.strip() or run.stdout.strip())
            return 2

        py_decks, py_cards = read_cps(py_cps)
        net_decks, net_cards = read_cps(dotnet_cps)
        net_summary = run.stdout.splitlines()[0]
        match = re.search(r"converted: (\d+); skipped: (\d+)", net_summary)
        net_converted, net_skipped = (int(match.group(1)), int(match.group(2))) if match else (-1, -1)
        py_skipped = sum(py_report.cards_skipped.values())
        print(f"Python cards={len(py_cards)} skipped={py_skipped}; .NET cards={len(net_cards)} skipped={net_skipped}")
        if py_decks != net_decks:
            print("DECK DIFFERENCE")
            print(json.dumps({"python": py_decks, "dotnet": net_decks}, ensure_ascii=False, indent=2, sort_keys=True))
        missing = sorted(py_cards.keys() - net_cards.keys())
        extra = sorted(net_cards.keys() - py_cards.keys())
        different = [card_id for card_id in sorted(py_cards.keys() & net_cards.keys())
                     if any(py_cards[card_id][key] != net_cards[card_id][key] for key in ("deck", "front", "back"))]
        for card_id in missing:
            print(f"MISSING .NET {card_id}: " + json.dumps(py_cards[card_id], ensure_ascii=False, sort_keys=True))
        for card_id in extra:
            print(f"EXTRA .NET {card_id}: " + json.dumps(net_cards[card_id], ensure_ascii=False, sort_keys=True))
        for card_id in different:
            print(f"CONTENT DIFF {card_id}: " + json.dumps({"python": py_cards[card_id], "dotnet": net_cards[card_id]}, ensure_ascii=False, sort_keys=True))
        python_bytes, dotnet_bytes = py_cps.read_bytes(), dotnet_cps.read_bytes()
        print(f"CPS SHA-256 Python={hashlib.sha256(python_bytes).hexdigest()} .NET={hashlib.sha256(dotnet_bytes).hexdigest()}")
        if args.checker.is_file():
            for label, cps in (("Python", py_cps), (".NET", dotnet_cps)):
                checked = subprocess.run([str(args.checker), str(cps), "--open-only"], text=True, capture_output=True)
                if checked.returncode:
                    print(f"StudyPackageCheck rejeitou {label}: {checked.stderr.strip()}")
                    return 3
                print(f"StudyPackageCheck {label}: {checked.stdout.splitlines()[0]}")
        if not missing and not extra and not different and py_decks == net_decks and py_skipped == net_skipped and python_bytes == dotnet_bytes:
            print("PARITY: CPS bytes, card/deck records, and skipped counts match exactly.")
            return 0
        print(f"DIVERGENCE: missing={len(missing)} extra={len(extra)} content={len(different)}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

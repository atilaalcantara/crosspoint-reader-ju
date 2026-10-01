#!/usr/bin/env python3
"""Prepare the simulator's dedicated, guarded SD sandbox."""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = (REPO / "build").resolve()
SD_ROOT = BUILD / "simulator-test-sd"
MARKER = ".crosspoint-simulator-test-sandbox"


def validate_root(*, create: bool) -> None:
    if SD_ROOT == Path("/") or SD_ROOT == Path.home() or SD_ROOT == REPO:
        raise SystemExit(f"Recusando caminho inseguro: {SD_ROOT}")
    if SD_ROOT.parent.resolve() != BUILD or SD_ROOT.name != "simulator-test-sd":
        raise SystemExit(f"Raiz inesperada; nenhuma alteração feita: {SD_ROOT}")
    if SD_ROOT.is_symlink():
        raise SystemExit(f"A raiz não pode ser symlink: {SD_ROOT}")

    if not SD_ROOT.exists():
        if not create:
            raise SystemExit("Sandbox ainda não existe; rode primeiro sem --reset.")
        SD_ROOT.mkdir(parents=True)
        (SD_ROOT / MARKER).write_text("Managed by tools/simulator-test/prepare_sd.py\n", encoding="utf-8")
        return

    marker = SD_ROOT / MARKER
    if not SD_ROOT.is_dir() or not marker.is_file() or marker.is_symlink():
        raise SystemExit(
            f"A pasta existe mas não tem o marcador de sandbox esperado: {SD_ROOT}. "
            "Nada foi apagado. Escolha outra cópia/backup ou inspecione-a manualmente."
        )


def reset_contents() -> None:
    validate_root(create=False)
    for child in SD_ROOT.iterdir():
        if child.name == MARKER:
            continue
        if child.is_symlink() or child.is_file():
            child.unlink()
        elif child.is_dir():
            shutil.rmtree(child)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reset", action="store_true", help="limpa somente a sandbox marcada do simulador")
    parser.add_argument("--cps", type=Path, help="copia um pacote .cps para /study/")
    parser.add_argument("--cps-name", help="nome opcional para o CPS instalado, por exemplo enare.cps")
    parser.add_argument("--epub", type=Path, help="copia um EPUB para /books/")
    parser.add_argument("--snapshot", type=Path, help="cria uma cópia integral em um destino ainda inexistente")
    parser.add_argument("--restore-snapshot", type=Path, help="restaura snapshot sob build/snapshots para esta sandbox")
    args = parser.parse_args()

    validate_root(create=not args.reset)
    if args.reset:
        reset_contents()
        print(f"Sandbox resetada (somente seu conteúdo): {SD_ROOT}")

    if args.restore_snapshot is not None:
        snapshot = args.restore_snapshot.expanduser().resolve()
        snapshots_root = (BUILD / "snapshots").resolve()
        if snapshot == snapshots_root or snapshots_root not in snapshot.parents:
            raise SystemExit(f"Snapshot precisa estar dentro de {snapshots_root}: {snapshot}")
        snapshot_marker = snapshot / MARKER
        if not snapshot.is_dir() or not snapshot_marker.is_file() or snapshot_marker.is_symlink():
            raise SystemExit(f"Snapshot sem marcador válido; nenhuma cópia feita: {snapshot}")
        if any(item.is_symlink() for item in snapshot.rglob("*")):
            raise SystemExit("Snapshot contém symlink; restauração recusada.")
        validate_root(create=False)
        if not args.reset:
            raise SystemExit("Restauração exige --reset para limpar antes somente a sandbox marcada.")
        for child in snapshot.iterdir():
            if child.name == MARKER:
                continue
            target = SD_ROOT / child.name
            if child.is_dir():
                shutil.copytree(child, target)
            else:
                shutil.copy2(child, target)
        print(f"Snapshot restaurado: {snapshot} -> {SD_ROOT}")

    if args.cps_name and (Path(args.cps_name).name != args.cps_name or not args.cps_name.lower().endswith(".cps")):
        raise SystemExit("--cps-name deve ser apenas um nome de arquivo com extensão .cps")

    for source, folder, suffix in ((args.cps, "study", ".cps"), (args.epub, "books", ".epub")):
        if source is None:
            continue
        source = source.expanduser().resolve()
        if not source.is_file() or source.suffix.lower() != suffix:
            raise SystemExit(f"Arquivo de entrada inválido (esperado {suffix}): {source}")
        destination_name = args.cps_name if folder == "study" and args.cps_name else source.name
        destination = SD_ROOT / folder / destination_name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        print(f"Copiado: {source} -> {destination} (/{folder}/{destination.name} no firmware)")

    if args.snapshot is not None:
        destination = args.snapshot.expanduser().absolute()
        if destination.exists() or destination.is_symlink():
            raise SystemExit(f"Snapshot não criado: destino já existe: {destination}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(SD_ROOT, destination, symlinks=True)
        print(f"Snapshot criado: {destination}")

    print(f"Raiz SD simulada: {SD_ROOT}")
    print(f"Abrir no Finder: open \"{SD_ROOT}\"")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

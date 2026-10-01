#!/usr/bin/env bash
set -euo pipefail

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cd "$PROJECT"

# Initializes the fixed sandbox if it does not yet exist. Existing marked data
# is preserved; an unexpected/unmarked directory is refused by prepare_sd.py.
python3 tools/simulator-test/prepare_sd.py

SD_ROOT="$PROJECT/build/simulator-test-sd"
if [[ "$SD_ROOT" == "/" || "$SD_ROOT" == "$HOME" || ! -f "$SD_ROOT/.crosspoint-simulator-test-sandbox" ]]; then
  printf 'Recusando raiz SD inesperada: %s\n' "$SD_ROOT" >&2
  exit 1
fi
mkdir -p "$SD_ROOT/books" "$SD_ROOT/study"

# A convenient folder at the repository root points to the very same persistent
# SD used by the existing test helper. Never replace a user's existing path.
SD_LINK="$PROJECT/simulator-sd"
if [[ -L "$SD_LINK" ]]; then
  if [[ ! -d "$SD_LINK" || "$(cd "$SD_LINK" && pwd -P)" != "$(cd "$SD_ROOT" && pwd -P)" ]]; then
    printf 'simulator-sd aponta para outra pasta; nada foi alterado: %s\n' "$SD_LINK" >&2
    exit 1
  fi
elif [[ -e "$SD_LINK" ]]; then
  printf 'simulator-sd já existe e não será substituída: %s\n' "$SD_LINK" >&2
  exit 1
else
  ln -s "build/simulator-test-sd" "$SD_LINK"
fi

STRICT=0
if [[ "${1:-}" == "--strict-hardware" ]]; then
  STRICT=1
fi

if [[ "$STRICT" == "1" ]]; then
  # Aproximação honesta do X4 Pro: o simulador roda com a CPU/RAM do Mac, então
  # limita apenas as métricas de heap que o firmware consulta (decisão de
  # buffers PNG, pré-carregamento de fontes, etc.) e ativa o abort de
  # close()-sem-open (paridade com o assert do hardware). Não emula clock,
  # SDMMC, e-ink nem PSRAM de verdade: passar aqui reduz o risco, não prova.
  printf 'Modo restrito: heap simulado 200 KB livres / 64 KB por alocação.\n'
  printf 'Compilando e abrindo o simulador X4 Pro. Coloque arquivos em: %s\n' "$SD_LINK"
  exec env CROSSPOINT_SIM_SD="$SD_ROOT" CROSSPOINT_SIM_FREE_HEAP=200000 CROSSPOINT_SIM_MAX_ALLOC_HEAP=65536 \
    pio run -c platformio.sim.ini -e simulator_x4 -t run_simulator
fi

printf 'Compilando e abrindo o simulador X4 Pro. Coloque arquivos em: %s\n' "$SD_LINK"
exec env CROSSPOINT_SIM_SD="$SD_ROOT" pio run -c platformio.sim.ini -e simulator_x4 -t run_simulator

#!/usr/bin/env bash
set -euo pipefail

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
cd "$PROJECT"

if ! command -v pio >/dev/null 2>&1; then
  printf 'PlatformIO (pio) não encontrado no PATH.\n' >&2
  exit 1
fi

printf 'Compilando firmware para XTEINK X4 Pro (ESP32-S3)...\n'
pio run -e x4pro

SOURCE="$PROJECT/.pio/build/x4pro/firmware.bin"
if [[ ! -s "$SOURCE" ]]; then
  printf 'Build terminou sem produzir um firmware.bin válido: %s\n' "$SOURCE" >&2
  exit 1
fi

OUTPUT_DIR="$PROJECT/firmware-x4pro"
mkdir -p "$OUTPUT_DIR"
TEMP="$(mktemp "$OUTPUT_DIR/.firmware.bin.XXXXXX")"
trap 'rm -f "$TEMP"' EXIT
cp "$SOURCE" "$TEMP"
chmod 644 "$TEMP"
mv -f "$TEMP" "$OUTPUT_DIR/firmware.bin"
trap - EXIT

printf '\nFirmware X4 Pro pronto para copiar para a raiz do SD:\n%s\n' "$OUTPUT_DIR/firmware.bin"
ls -lh "$OUTPUT_DIR/firmware.bin"
shasum -a 256 "$OUTPUT_DIR/firmware.bin"
printf 'No aparelho, selecione esse arquivo em Configurações > Atualizar firmware pelo SD.\n'

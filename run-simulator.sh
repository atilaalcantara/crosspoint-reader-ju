#!/usr/bin/env bash
set -euo pipefail

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
exec bash "$PROJECT/tools/simulator-test/run.sh" "$@"

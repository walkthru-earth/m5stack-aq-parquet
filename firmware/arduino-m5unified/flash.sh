#!/usr/bin/env bash
set -euo pipefail

trial_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
port="${1:-}"
# shellcheck source=dependencies.lock
source "${trial_dir}/dependencies.lock"

if [[ -z "${port}" ]]; then
  printf 'usage: %s /dev/cu.usbmodem...\n' "$0" >&2
  exit 2
fi

"${trial_dir}/arduino-cli.sh" upload \
  --fqbn "${CORES3_FQBN}" \
  --port "${port}" \
  --input-dir "${trial_dir}/build"

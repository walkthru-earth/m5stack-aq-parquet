#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
backup_dir="${repo_root}/backup"
image="${backup_dir}/cores3-flash-$(date -u +%Y%m%dT%H%M%SZ).bin"

mkdir -p "${backup_dir}"
# Keep the transport at esptool's default speed. Global options precede the
# subcommand under esptool 5, so `pixi run backup --port ...` works.
exec esptool --chip esp32s3 "$@" read-flash 0 0x1000000 "${image}"

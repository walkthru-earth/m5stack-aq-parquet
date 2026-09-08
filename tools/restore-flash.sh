#!/usr/bin/env bash
set -euo pipefail

image="${1:-}"
if [[ -z "${image}" ]]; then
  printf 'usage: %s backup/<image>.bin [esptool global options]\n' "$0" >&2
  exit 2
fi
shift

if [[ ! -f "${image}" ]]; then
  printf 'restore image does not exist: %s\n' "${image}" >&2
  exit 2
fi

# Keep the transport at esptool's default speed.
exec esptool --chip esp32s3 "$@" write-flash 0 "${image}"

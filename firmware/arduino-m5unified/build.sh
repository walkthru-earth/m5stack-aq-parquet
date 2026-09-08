#!/usr/bin/env bash
set -euo pipefail

trial_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${trial_dir}/build"
# shellcheck source=dependencies.lock
source "${trial_dir}/dependencies.lock"

# Verify the committed codec implementation, not a floating downloaded library.
printf '%s  %s\n' \
  "${LZ4_C_SHA256}" "${trial_dir}/vendor/lz4/lz4.c" \
  "${LZ4_H_SHA256}" "${trial_dir}/vendor/lz4/lz4.h" \
  "${LZ4_LICENSE_SHA256}" "${trial_dir}/vendor/lz4/LICENSE" | shasum -a 256 -c -

mkdir -p "${build_dir}"
"${trial_dir}/arduino-cli.sh" compile \
  --fqbn "${CORES3_FQBN}" \
  --warnings all \
  --build-path "${build_dir}" \
  --export-binaries \
  "${trial_dir}/bringup"

grep -qx 'CONFIG_SPIRAM_MODE_QUAD=y' "${build_dir}/sdkconfig"
grep -qx '# CONFIG_SPIRAM_MODE_OCT is not set' "${build_dir}/sdkconfig"
grep -qx 'CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y' "${build_dir}/sdkconfig"
grep -qx 'CONFIG_PARTITION_TABLE_CUSTOM=y' "${build_dir}/sdkconfig"

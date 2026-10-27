#!/usr/bin/env bash
# Builds the release tester and copies its FOTA package to an output folder.
# Usage: ./build_fota.sh [output dir] [board]
set -euo pipefail

out="${1:-/home/olof-johnsson/Documents/repos/halloween-2026/build}"
board="${2:-promicro_nrf52840/nrf52840}"

cd "$(dirname "$0")"
source ../../common/ncs_env.sh

out="${out/#\~/$HOME}"
build_dir="build"

west build -b "$board" -p auto -d "$build_dir" .

mkdir -p "$out"
cp "$build_dir/dfu_application.zip" "$out/release-tester-dfu.zip"
echo "FOTA package: $out/release-tester-dfu.zip"

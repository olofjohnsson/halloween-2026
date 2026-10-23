#!/usr/bin/env bash
# Builds one station and copies its FOTA package to an output folder.
# Usage: ./build_fota.sh <station 1-5> [output dir] [board] [extra conf file]
# Example: ./build_fota.sh 2 ~/fota
set -euo pipefail

station="${1:?usage: $0 <station 1-5> [output dir] [board] [extra conf file]}"
out="${2:-/home/olof-johnsson/Documents/repos/halloween-2026/build}"
board="${3:-promicro_nrf52840/nrf52840}"
conf="${4:-station_${station}.conf}"

cd "$(dirname "$0")"
source ../common/ncs_env.sh

out="${out/#\~/$HOME}"
build_dir="build/station_${station}"

west build -b "$board" -p auto -d "$build_dir" . -- -DEXTRA_CONF_FILE="$conf"

mkdir -p "$out"
cp "$build_dir/dfu_application.zip" "$out/station-${station}-dfu.zip"
echo "FOTA package: $out/station-${station}-dfu.zip"

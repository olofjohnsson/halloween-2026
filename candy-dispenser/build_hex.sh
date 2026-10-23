#!/usr/bin/env bash
# Builds the candy dispenser (nRF7002 DK) and copies the hex files to an output folder.
# The dispenser has no MCUboot/FOTA, so both cores are flashed over SWD.
# Usage: ./build_hex.sh [output dir]
# Example: ./build_hex.sh ~/some/folder
set -euo pipefail

out="${1:-/home/olof-johnsson/Documents/repos/halloween-2026/build}"

cd "$(dirname "$0")"
source ../common/ncs_env.sh

out="${out/#\~/$HOME}"
build_dir="build/script"

west build -b nrf7002dk/nrf5340/cpuapp -p auto -d "$build_dir" . -- \
    -DDTC_OVERLAY_FILE=boards/nrf7002dk_nrf5340_cpuapp.overlay

mkdir -p "$out"
cp "$build_dir/candy-dispenser/zephyr/zephyr.hex" "$out/candy-dispenser-app.hex"
cp "$build_dir/ipc_radio/zephyr/zephyr.hex" "$out/candy-dispenser-netcore.hex"
echo "Hex files copied to $out"

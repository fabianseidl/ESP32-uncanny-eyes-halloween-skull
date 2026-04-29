#!/usr/bin/env bash
# Compile, upload, and monitor the FIRST ESP32 board listed by arduino-cli.
# Run this in one terminal; run flash_board2.sh in another to drive both eyes.
set -euo pipefail

FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=huge_app"

cd "$(dirname "$0")"

PORT=$(arduino-cli board list | awk '/ESP32 Family Device/{print $1; exit}')
if [ -z "${PORT:-}" ]; then
  echo "No ESP32 Family Device found. Plug a board in and try again." >&2
  exit 1
fi

echo "Board 1 -> $PORT"
arduino-cli compile --fqbn "$FQBN" .
arduino-cli upload  --fqbn "$FQBN" -p "$PORT" .
exec arduino-cli monitor -p "$PORT" -c baudrate=115200

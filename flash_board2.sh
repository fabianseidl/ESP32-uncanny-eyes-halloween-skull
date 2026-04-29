#!/usr/bin/env bash
# Compile, upload, and monitor the SECOND ESP32 board listed by arduino-cli.
# Run flash_board1.sh in another terminal first (or in parallel) to drive
# both eyes.
set -euo pipefail

FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=huge_app"

cd "$(dirname "$0")"

PORT=$(arduino-cli board list | awk '/ESP32 Family Device/{n++; if(n==2){print $1; exit}}')
if [ -z "${PORT:-}" ]; then
  echo "Second ESP32 Family Device not found. Plug both boards in and try again." >&2
  exit 1
fi

echo "Board 2 -> $PORT"
arduino-cli compile --fqbn "$FQBN" .
arduino-cli upload  --fqbn "$FQBN" -p "$PORT" .
exec arduino-cli monitor -p "$PORT" -c baudrate=115200

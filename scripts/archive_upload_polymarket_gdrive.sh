#!/usr/bin/env bash
set -euo pipefail

# Daily job:
#   1. compress yesterday's raw JSONL into .jsonl.zst
#   2. upload it to Google Drive with rclone
#   3. delete local raw/archive files only after successful upload

DATA_ROOT="${DATA_ROOT:-$HOME/polymarket-data}"
RAW_DIR="${RAW_DIR:-$DATA_ROOT/raw}"
ARCHIVE_DIR="${ARCHIVE_DIR:-$DATA_ROOT/archive}"
LOG_DIR="${LOG_DIR:-$DATA_ROOT/logs}"
RCLONE_REMOTE="${RCLONE_REMOTE:-gdrive:/trading_data/polymarket}"
DATE_TZ="${DATE_TZ:-UTC}"
KEEP_RAW_AFTER_UPLOAD="${KEEP_RAW_AFTER_UPLOAD:-0}"
KEEP_ARCHIVE_AFTER_UPLOAD="${KEEP_ARCHIVE_AFTER_UPLOAD:-0}"
ZSTD_LEVEL="${ZSTD_LEVEL:-6}"

mkdir -p "$RAW_DIR" "$ARCHIVE_DIR" "$LOG_DIR"

if [[ $# -ge 1 ]]; then
  DAY="$1"
else
  DAY="$(TZ="$DATE_TZ" date -v-1d +%F 2>/dev/null || TZ="$DATE_TZ" date -d 'yesterday' +%F)"
fi

RAW_FILE="$RAW_DIR/polymarket-market-$DAY.jsonl"
ARCHIVE_FILE="$ARCHIVE_DIR/polymarket-market-$DAY.jsonl.zst"
REMOTE_DIR="$RCLONE_REMOTE/$DAY"

if [[ ! -s "$RAW_FILE" ]]; then
  echo "no raw file to archive: $RAW_FILE"
  exit 0
fi

if [[ ! -s "$ARCHIVE_FILE" ]]; then
  TMP_ARCHIVE="$ARCHIVE_FILE.tmp"
  rm -f "$TMP_ARCHIVE"
  zstd -T0 "-$ZSTD_LEVEL" -o "$TMP_ARCHIVE" "$RAW_FILE"
  mv "$TMP_ARCHIVE" "$ARCHIVE_FILE"
fi

rclone mkdir "$REMOTE_DIR"
rclone copy "$ARCHIVE_FILE" "$REMOTE_DIR" --checksum --transfers 1 --checkers 4 --retries 5 --low-level-retries 20
rclone check "$ARCHIVE_FILE" "$REMOTE_DIR/$(basename "$ARCHIVE_FILE")" --one-way --size-only

if [[ "$KEEP_RAW_AFTER_UPLOAD" == "0" ]]; then
  rm -f "$RAW_FILE"
fi

if [[ "$KEEP_ARCHIVE_AFTER_UPLOAD" == "0" ]]; then
  rm -f "$ARCHIVE_FILE"
fi

echo "uploaded polymarket archive for $DAY to $REMOTE_DIR"

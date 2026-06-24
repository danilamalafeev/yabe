# Polymarket Daily Recorder on a VPS

This setup records Polymarket CLOB market websocket packets for the current day,
compresses yesterday's JSONL file with Zstandard, and uploads it to Google Drive
through `rclone`.

## Server

Use a cheap Ubuntu 24.04 VPS with enough local disk for a few days of buffer.
For a first collector, 1-2 vCPU, 1-2 GB RAM, and 25-40 GB disk is enough.

## Install

```bash
sudo apt update
sudo apt install -y git python3 python3-venv rclone zstd tmux

mkdir -p ~/polymarket-recorder ~/polymarket-data/{raw,archive,logs}
python3 -m venv ~/pm-venv
source ~/pm-venv/bin/activate
pip install -r requirements-polymarket-recorder.txt
```

Copy this repository's `scripts/record_polymarket_ws.py` and
`scripts/archive_upload_polymarket_gdrive.sh` to `~/polymarket-recorder/`.
Also copy `requirements-polymarket-recorder.txt` before installing dependencies,
or run `pip install websockets` directly on the server.

Create an asset id list:

```bash
nano ~/polymarket-recorder/asset_ids.txt
```

One token asset id per line:

```text
123...
456...
```

Configure Google Drive once:

```bash
rclone config
```

Create a remote named `gdrive`.

## Run Collector With Systemd

```bash
sudo nano /etc/systemd/system/polymarket-recorder.service
```

```ini
[Unit]
Description=Polymarket websocket JSONL recorder
After=network-online.target
Wants=network-online.target

[Service]
User=ubuntu
WorkingDirectory=/home/ubuntu/polymarket-recorder
Environment=PYTHONUNBUFFERED=1
ExecStart=/home/ubuntu/pm-venv/bin/python /home/ubuntu/polymarket-recorder/record_polymarket_ws.py --asset-ids-file /home/ubuntu/polymarket-recorder/asset_ids.txt --output-dir /home/ubuntu/polymarket-data/raw
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

If your VPS user is not `ubuntu`, replace `ubuntu` in the unit file.

Start it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now polymarket-recorder
sudo systemctl status polymarket-recorder
journalctl -u polymarket-recorder -f
```

## Daily Compress and Upload

Install the cron job:

```bash
chmod +x ~/polymarket-recorder/archive_upload_polymarket_gdrive.sh
crontab -e
```

Run at 00:15 UTC every day:

```cron
15 0 * * * DATA_ROOT=/home/ubuntu/polymarket-data RCLONE_REMOTE=gdrive:/trading_data/polymarket /home/ubuntu/polymarket-recorder/archive_upload_polymarket_gdrive.sh >> /home/ubuntu/polymarket-data/logs/archive.log 2>&1
```

The script only removes the local raw/archive files after `rclone copy` and
`rclone check` succeed.

## Manual Test

Record 10 packets:

```bash
~/pm-venv/bin/python ~/polymarket-recorder/record_polymarket_ws.py \
  --asset-ids-file ~/polymarket-recorder/asset_ids.txt \
  --output-dir ~/polymarket-data/raw \
  --max-messages 10
```

Archive and upload a specific date:

```bash
DATA_ROOT=~/polymarket-data RCLONE_REMOTE=gdrive:/trading_data/polymarket \
  ~/polymarket-recorder/archive_upload_polymarket_gdrive.sh 2026-04-29
```

Check size:

```bash
du -sh ~/polymarket-data/raw ~/polymarket-data/archive
rclone size gdrive:/trading_data/polymarket
```

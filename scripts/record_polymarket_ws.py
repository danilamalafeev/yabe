#!/usr/bin/env python3
"""Record Polymarket CLOB market websocket messages to local JSONL files.

The script intentionally stores raw websocket payloads inside a small receive
envelope instead of trying to normalize them online. That keeps collection
robust and preserves the original exchange message for later parsing.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import signal
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo


DEFAULT_URL = "wss://ws-subscriptions-clob.polymarket.com/ws/market"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Record Polymarket market websocket messages as JSONL.")
    parser.add_argument("--url", default=DEFAULT_URL, help="Polymarket websocket URL.")
    parser.add_argument("--asset-id", dest="asset_ids", action="append", default=[], help="Token asset id to subscribe to.")
    parser.add_argument("--asset-ids-file", type=Path, help="File with one asset id per line. Commas are also accepted.")
    parser.add_argument("--output-dir", type=Path, default=Path("data/polymarket/raw"), help="Directory for daily JSONL files.")
    parser.add_argument("--date-tz", default="UTC", help="Timezone used for daily file rotation, e.g. UTC.")
    parser.add_argument("--heartbeat-seconds", type=float, default=10.0, help="Text PING interval expected by Polymarket.")
    parser.add_argument("--reconnect-min-seconds", type=float, default=1.0)
    parser.add_argument("--reconnect-max-seconds", type=float, default=60.0)
    parser.add_argument("--log-every", type=int, default=10_000, help="Print progress every N received JSON packets.")
    parser.add_argument("--max-messages", type=int, default=0, help="Stop after N JSON messages. Useful for smoke tests.")
    return parser.parse_args()


def read_asset_ids(args: argparse.Namespace) -> list[str]:
    asset_ids: list[str] = []
    asset_ids.extend(args.asset_ids)

    if args.asset_ids_file:
        text = args.asset_ids_file.read_text()
        for raw_line in text.splitlines():
            line = raw_line.split("#", 1)[0].strip()
            if not line:
                continue
            asset_ids.extend(part.strip() for part in line.split(",") if part.strip())

    seen: set[str] = set()
    deduped: list[str] = []
    for asset_id in asset_ids:
        if asset_id in seen:
            continue
        seen.add(asset_id)
        deduped.append(asset_id)

    if not deduped:
        raise SystemExit("ERROR: provide at least one --asset-id or --asset-ids-file entry")
    return deduped


class DailyJsonlWriter:
    def __init__(self, output_dir: Path, tz_name: str) -> None:
        self.output_dir = output_dir
        self.tz = ZoneInfo(tz_name)
        self.current_date = ""
        self.file = None

    def _date_key(self) -> str:
        return datetime.now(self.tz).strftime("%Y-%m-%d")

    def _ensure_open(self) -> None:
        date_key = self._date_key()
        if self.file is not None and date_key == self.current_date:
            return

        self.close()
        self.output_dir.mkdir(parents=True, exist_ok=True)
        path = self.output_dir / f"polymarket-market-{date_key}.jsonl"
        self.file = path.open("a", buffering=1)
        self.current_date = date_key
        print(f"writing {path}", flush=True)

    def write_json(self, payload: object) -> None:
        self._ensure_open()
        assert self.file is not None
        self.file.write(json.dumps(payload, separators=(",", ":"), ensure_ascii=False))
        self.file.write("\n")

    def close(self) -> None:
        if self.file is not None:
            self.file.close()
            self.file = None


async def heartbeat(ws, interval_seconds: float, stop_event: asyncio.Event) -> None:
    while not stop_event.is_set():
        await asyncio.sleep(interval_seconds)
        if stop_event.is_set():
            break
        await ws.send("PING")


async def record_forever(args: argparse.Namespace) -> None:
    try:
        import websockets
    except ModuleNotFoundError as exc:
        raise SystemExit("ERROR: install dependency first: pip install websockets") from exc

    asset_ids = read_asset_ids(args)
    writer = DailyJsonlWriter(args.output_dir, args.date_tz)
    stop_event = asyncio.Event()

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        with contextlib.suppress(NotImplementedError):
            loop.add_signal_handler(sig, stop_event.set)

    reconnect_seconds = args.reconnect_min_seconds
    messages = 0

    try:
        while not stop_event.is_set():
            try:
                async with websockets.connect(args.url, ping_interval=None, close_timeout=5) as ws:
                    subscribe_msg = {"assets_ids": asset_ids, "type": "market"}
                    await ws.send(json.dumps(subscribe_msg))
                    print(f"subscribed to {len(asset_ids)} asset ids", flush=True)
                    reconnect_seconds = args.reconnect_min_seconds

                    heartbeat_task = asyncio.create_task(heartbeat(ws, args.heartbeat_seconds, stop_event))
                    try:
                        async for raw_msg in ws:
                            if stop_event.is_set():
                                break
                            if isinstance(raw_msg, bytes):
                                raw_text = raw_msg.decode("utf-8", errors="replace")
                            else:
                                raw_text = raw_msg

                            try:
                                exchange_payload = json.loads(raw_text)
                            except json.JSONDecodeError:
                                # Polymarket heartbeat replies are not data packets.
                                if raw_text.strip().upper() in {"PONG", "PING"}:
                                    continue
                                exchange_payload = {"raw_text": raw_text}

                            envelope = {
                                "recv_ts_ns": time.time_ns(),
                                "recv_time": datetime.now(timezone.utc).isoformat(timespec="microseconds"),
                                "source": "polymarket_clob_market_ws",
                                "payload": exchange_payload,
                            }
                            writer.write_json(envelope)
                            messages += 1

                            if args.log_every and messages % args.log_every == 0:
                                print(f"recorded {messages:,} JSON packets", flush=True)
                            if args.max_messages and messages >= args.max_messages:
                                stop_event.set()
                                break
                    finally:
                        heartbeat_task.cancel()
                        with contextlib.suppress(asyncio.CancelledError):
                            await heartbeat_task
            except Exception as exc:
                if stop_event.is_set():
                    break
                print(f"websocket error: {exc}; reconnecting in {reconnect_seconds:.1f}s", file=sys.stderr, flush=True)
                await asyncio.sleep(reconnect_seconds)
                reconnect_seconds = min(args.reconnect_max_seconds, reconnect_seconds * 2)
    finally:
        writer.close()


def main() -> int:
    args = parse_args()
    asyncio.run(record_forever(args))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

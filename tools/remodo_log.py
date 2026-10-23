#!/usr/bin/env python3
"""Keep an ESPHome device's log in daily files. Reconnects forever (reboots, OTA, Wi-Fi drops).

    pip install aioesphomeapi
    python remodo_log.py esphome-remodo.local --dir logs [--keep-awake]

Each line gets the date and time of arrival, so a failure hours later can be located.
One file per day: logs/remodo-2026-10-23.log. Stop with Ctrl+C.
"""
import argparse
import asyncio
import contextlib
import sys
from datetime import datetime
from pathlib import Path

from aioesphomeapi import APIClient
from aioesphomeapi.log_parser import parse_log_message
from aioesphomeapi.log_runner import async_run


async def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("address", help="device name or IP, e.g. esphome-remodo.local")
    ap.add_argument("--port", type=int, default=6053)
    ap.add_argument("--noise-psk", help="API encryption key, if the device uses one")
    ap.add_argument("--dir", default="logs", help="output folder (default: logs)")
    ap.add_argument("--keep-awake", action="store_true",
                    help="Windows: keep the PC from sleeping while the logger runs (the display may still turn off)")
    args = ap.parse_args()

    if args.keep_awake and sys.platform == "win32":
        import ctypes
        # ES_CONTINUOUS | ES_SYSTEM_REQUIRED; released automatically when this process ends
        awake_ok = ctypes.windll.kernel32.SetThreadExecutionState(0x80000001) != 0

    out = Path(args.dir)
    out.mkdir(parents=True, exist_ok=True)
    current = {"day": None, "file": None}

    def write(line: str) -> None:
        now = datetime.now().astimezone()
        day = now.strftime("%Y-%m-%d")
        if current["day"] != day:
            if current["file"]:
                current["file"].close()
            current["file"] = open(out / f"remodo-{day}.log", "a", encoding="utf-8", buffering=1)
            current["day"] = day
        current["file"].write(f"{now:%Y-%m-%d %H:%M:%S}.{now.microsecond // 1000:03d} {line}\n")

    def on_log(msg) -> None:
        text = msg.message.decode("utf8", "backslashreplace")
        for line in parse_log_message(text, "", strip_ansi_escapes=True):
            write(line)

    client = APIClient(args.address, args.port, password=None, noise_psk=args.noise_psk, keepalive=10)
    write(f"--- logger started for {args.address}")
    if args.keep_awake and sys.platform == "win32":
        write(f"--- keep-awake {'on' if awake_ok else 'FAILED'}")
    stop = await async_run(client, on_log, on_connect=lambda: write("--- connected to the device"),
                           subscribe_states=False)
    try:
        await asyncio.Event().wait()
    finally:
        write("--- logger stopped")
        await stop()


if __name__ == "__main__":
    with contextlib.suppress(KeyboardInterrupt):
        asyncio.run(main())

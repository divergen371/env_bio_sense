#!/usr/bin/env python3
"""Show only Web/SD/watchdog diagnostic lines from the device serial port."""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

import serial


PATTERN = re.compile(
    r"FileIndexTask|file_index\.refresh|web\.files|web\.flush|async_tcp|"
    r"watchdog|Task WDT|Guru Meditation|abort|rst:|Rebooting|WiFi|"
    r"\bAP\b|throttling|TaskStack|StorageTask|Minimum remaining stack|"
    r"IoProbe.*(?:active|done|owner)",
    re.IGNORECASE,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/cu.usbmodem101")
    parser.add_argument("--baud", default=115200, type=int)
    parser.add_argument(
        "--output",
        type=Path,
        help="Optional file for the filtered lines only.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output = args.output.open("a", encoding="utf-8") if args.output else None
    print(f"Monitoring {args.port} at {args.baud} baud; Ctrl+C to stop.")
    try:
        while True:
            try:
                with serial.Serial(args.port, args.baud, timeout=1) as device:
                    print("Connected.")
                    while True:
                        raw = device.readline()
                        if not raw:
                            continue
                        line = raw.decode("utf-8", errors="replace").rstrip()
                        if not PATTERN.search(line):
                            continue
                        print(line, flush=True)
                        if output:
                            output.write(line + "\n")
                            output.flush()
            except serial.SerialException as error:
                print(f"Serial disconnected ({error}); reconnecting…", file=sys.stderr)
                time.sleep(1)
    except KeyboardInterrupt:
        print("\nStopped.")
        return 0
    finally:
        if output:
            output.close()


if __name__ == "__main__":
    raise SystemExit(main())

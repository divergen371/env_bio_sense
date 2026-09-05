#!/usr/bin/env python3
"""Validate or extract env_bio_sense PPG Binary Format v1 files."""

from __future__ import annotations

import argparse
import csv
import json
import struct
import sys
import zlib
from pathlib import Path
from typing import BinaryIO, Optional

FILE_HEADER_SIZE = 64
BLOCK_HEADER_SIZE = 28
FOOTER_SIZE = 40
SAMPLE_SIZE = 8
FILE_FLAGS = 0x00000007
MAX_VALUE = 0x0003FFFF


class PpgError(Exception):
    pass


def crc32(data: bytes, seed: int = 0) -> int:
    return zlib.crc32(data, seed) & 0xFFFFFFFF


def fnv1a64(text: str) -> int:
    value = 0xCBF29CE484222325
    for byte in text.encode("ascii"):
        value ^= byte
        value = (value * 0x00000100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def read_exact(stream: BinaryIO, size: int, label: str) -> bytes:
    value = stream.read(size)
    if len(value) != size:
        raise PpgError(f"truncated {label}: expected {size}, got {len(value)}")
    return value


def validate_raw(path: Path, csv_path: Optional[Path] = None) -> dict:
    file_size = path.stat().st_size
    with path.open("rb") as stream:
        header = read_exact(stream, FILE_HEADER_SIZE, "file header")
        if header[:4] != b"PPG1":
            raise PpgError("invalid file magic")
        version, header_size, flags = struct.unpack_from("<HHI", header, 4)
        if version != 1:
            raise PpgError(f"unsupported version {version}")
        if header_size != FILE_HEADER_SIZE or flags != FILE_FLAGS:
            raise PpgError("invalid header size or flags")
        (
            start_unix_us,
            sample_rate_hz,
            sample_size,
            channel_mask,
            sample_average,
            reserved0,
            pulse_width_us,
            adc_range_na,
            red_current_x10,
            ir_current_x10,
            expected_samples,
            block_payload_max,
            session_hash,
        ) = struct.unpack_from("<QHHHBBHHHHIIQ", header, 12)
        if sample_rate_hz == 0 or sample_size != SAMPLE_SIZE:
            raise PpgError("invalid sample rate or sample size")
        if channel_mask != 3 or sample_average == 0 or reserved0 != 0:
            raise PpgError("invalid channel/averaging/reserved field")
        if header[52:60] != bytes(8):
            raise PpgError("reserved header bytes are not zero")
        if struct.unpack_from("<I", header, 60)[0] != crc32(header[:60]):
            raise PpgError("file header CRC mismatch")

        csv_file = csv_path.open("w", newline="", encoding="utf-8") if csv_path else None
        csv_writer = csv.writer(csv_file) if csv_file else None
        if csv_writer:
            csv_writer.writerow(("sample_index", "timestamp_unix_us", "red", "ir"))

        blocks = 0
        samples = 0
        next_index = 0
        stream_crc = 0
        discontinuities = []
        footer = None
        try:
            while stream.tell() < file_size:
                marker = read_exact(stream, 4, "record marker")
                stream.seek(-4, 1)
                if marker == b"END1":
                    raw_footer = read_exact(stream, FOOTER_SIZE, "footer")
                    magic, footer_version, footer_size = struct.unpack_from(
                        "<4sHH", raw_footer, 0
                    )
                    if magic != b"END1" or footer_version != 1 or footer_size != FOOTER_SIZE:
                        raise PpgError("invalid footer identity")
                    if struct.unpack_from("<I", raw_footer, 36)[0] != crc32(raw_footer[:36]):
                        raise PpgError("footer CRC mismatch")
                    (
                        end_unix_us,
                        footer_blocks,
                        footer_samples,
                        dropped_samples,
                        fifo_overflows,
                        footer_stream_crc,
                    ) = struct.unpack_from("<QIIII I", raw_footer, 8)
                    if stream.tell() != file_size:
                        raise PpgError("trailing bytes after footer")
                    if (footer_blocks, footer_samples, footer_stream_crc) != (
                        blocks,
                        samples,
                        stream_crc,
                    ):
                        raise PpgError("footer counters or stream CRC mismatch")
                    footer = {
                        "end_unix_us": end_unix_us,
                        "block_count": footer_blocks,
                        "sample_count": footer_samples,
                        "dropped_samples": dropped_samples,
                        "fifo_overflows": fifo_overflows,
                        "stream_crc32": f"{footer_stream_crc:08X}",
                    }
                    break
                if marker != b"BLK1":
                    raise PpgError(f"invalid block magic at offset {stream.tell()}")
                raw_block = read_exact(stream, BLOCK_HEADER_SIZE, "block header")
                (
                    _magic,
                    block_index,
                    first_index,
                    sample_count,
                    block_sample_size,
                    payload_size,
                    payload_crc,
                    header_crc,
                ) = struct.unpack("<4sIIHHIII", raw_block)
                if header_crc != crc32(raw_block[:24]):
                    raise PpgError(f"block {blocks} header CRC mismatch")
                if block_index != blocks:
                    raise PpgError(f"block sequence: expected {blocks}, got {block_index}")
                if block_sample_size != SAMPLE_SIZE or payload_size != sample_count * SAMPLE_SIZE:
                    raise PpgError(f"block {blocks} has invalid payload geometry")
                if sample_count == 0 or payload_size > block_payload_max:
                    raise PpgError(f"block {blocks} exceeds payload contract")
                if blocks and first_index < next_index:
                    raise PpgError(f"block {blocks} overlaps previous logical samples")
                if first_index > next_index:
                    discontinuities.append(
                        {"after_index": next_index - 1, "missing_samples": first_index - next_index}
                    )
                payload = read_exact(stream, payload_size, f"block {blocks} payload")
                if crc32(payload) != payload_crc:
                    raise PpgError(f"block {blocks} payload CRC mismatch")
                stream_crc = crc32(payload, stream_crc)
                if csv_writer:
                    for offset in range(sample_count):
                        red, ir = struct.unpack_from("<II", payload, offset * SAMPLE_SIZE)
                        if red > MAX_VALUE or ir > MAX_VALUE:
                            raise PpgError(f"block {blocks} sample has non-zero unused bits")
                        logical_index = first_index + offset
                        timestamp = start_unix_us + logical_index * 1_000_000 // sample_rate_hz
                        csv_writer.writerow((logical_index, timestamp, red, ir))
                else:
                    for red, ir in struct.iter_unpack("<II", payload):
                        if red > MAX_VALUE or ir > MAX_VALUE:
                            raise PpgError(f"block {blocks} sample has non-zero unused bits")
                blocks += 1
                samples += sample_count
                next_index = first_index + sample_count
        finally:
            if csv_file:
                csv_file.close()

    if footer is None:
        raise PpgError("missing footer")
    return {
        "valid": True,
        "path": str(path),
        "file_size": file_size,
        "header": {
            "version": version,
            "start_unix_us": start_unix_us,
            "sample_rate_hz": sample_rate_hz,
            "sample_average": sample_average,
            "pulse_width_us": pulse_width_us,
            "adc_range_na": adc_range_na,
            "red_led_current_x10_ma": red_current_x10,
            "ir_led_current_x10_ma": ir_current_x10,
            "expected_samples": expected_samples,
            "block_payload_max": block_payload_max,
            "session_id_hash": f"{session_hash:016X}",
        },
        "footer": footer,
        "discontinuities": discontinuities,
    }


def validate_session(directory: Path, csv_path: Optional[Path]) -> dict:
    raw_path = directory / "raw.ppg"
    metadata_path = directory / "metadata.json"
    environment_path = directory / "environment.csv"
    for path in (raw_path, metadata_path, environment_path):
        if not path.is_file():
            raise PpgError(f"missing session artifact: {path.name}")
    result = validate_raw(raw_path, csv_path)
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    session_id = metadata.get("session_id", "")
    if not isinstance(session_id, str) or fnv1a64(session_id) != int(
        result["header"]["session_id_hash"], 16
    ):
        raise PpgError("metadata session_id does not match raw header")
    quality = metadata.get("quality", {})
    if quality.get("block_count") != result["footer"]["block_count"]:
        raise PpgError("metadata block_count mismatch")
    if quality.get("stored_samples") != result["footer"]["sample_count"]:
        raise PpgError("metadata stored_samples mismatch")
    raw_meta = metadata.get("files", {}).get("raw", {})
    if raw_meta.get("size_bytes") != result["file_size"]:
        raise PpgError("metadata raw size mismatch")
    if raw_meta.get("stream_crc32") != result["footer"]["stream_crc32"]:
        raise PpgError("metadata stream CRC mismatch")
    environment = environment_path.read_bytes()
    if not environment.startswith(b"record_id,phase,") or not environment.endswith(b"\n"):
        raise PpgError("environment.csv is truncated or has an invalid header")
    result["session"] = {
        "session_id": session_id,
        "completion": metadata.get("completion"),
        "environment_rows": max(0, environment.count(b"\n") - 1),
    }
    return result


def make_golden(path: Path) -> None:
    session_id = "20260906T000000Z"
    start_us = 1_788_652_800_123_456
    header = bytearray(FILE_HEADER_SIZE)
    struct.pack_into("<4sHHIQHHHBBHHHHIIQ", header, 0, b"PPG1", 1, 64, FILE_FLAGS,
                     start_us, 100, 8, 3, 4, 0, 411, 4096, 64, 64, 0, 4096,
                     fnv1a64(session_id))
    struct.pack_into("<I", header, 60, crc32(header[:60]))
    payload = struct.pack("<IIII", 0x12345, 0x23456, 0x3FFFF, 0x05678)
    block = bytearray(BLOCK_HEADER_SIZE)
    struct.pack_into("<4sIIHHII", block, 0, b"BLK1", 0, 0, 2, 8,
                     len(payload), crc32(payload))
    struct.pack_into("<I", block, 24, crc32(block[:24]))
    footer = bytearray(FOOTER_SIZE)
    struct.pack_into("<4sHHQIIII I", footer, 0, b"END1", 1, 40,
                     start_us + 20_000, 1, 2, 0, 0, crc32(payload))
    struct.pack_into("<I", footer, 36, crc32(footer[:36]))
    path.write_bytes(bytes(header) + bytes(block) + payload + bytes(footer))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", type=Path,
                        help="raw.ppg file or completed session directory")
    parser.add_argument("--csv", type=Path, help="extract samples to CSV")
    parser.add_argument("--make-golden", type=Path,
                        help="write a two-sample conformance file")
    args = parser.parse_args()
    if args.make_golden:
        make_golden(args.make_golden)
        print(json.dumps({"created": str(args.make_golden)}, ensure_ascii=False))
        return 0
    if args.path is None:
        parser.error("path is required unless --make-golden is used")
    try:
        result = validate_session(args.path, args.csv) if args.path.is_dir() \
            else validate_raw(args.path, args.csv)
    except (OSError, ValueError, json.JSONDecodeError, PpgError) as error:
        print(json.dumps({"valid": False, "error": str(error)}, ensure_ascii=False))
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())

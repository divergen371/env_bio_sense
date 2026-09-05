"""Generate deterministic gzip-compressed PROGMEM assets for the Web UI."""

from __future__ import annotations

import gzip
from pathlib import Path


ASSETS = (
    ("INDEX_HTML", "index.html"),
    ("APP_CSS", "app.css"),
    ("APP_JS", "app.js"),
)


def byte_array(data: bytes) -> str:
    rows = []
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        rows.append("    " + ", ".join(f"0x{value:02X}" for value in chunk) + ",")
    return "\n".join(rows)


def generate(project_dir: Path) -> None:
    source_dir = project_dir / "web"
    output = project_dir / "include" / "generated" / "web_assets.h"
    sections = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "#include <cstddef>",
        "",
        "namespace web_assets {",
        "",
    ]
    for symbol, filename in ASSETS:
        source = (source_dir / filename).read_bytes()
        compressed_bytes = bytearray(
            gzip.compress(source, compresslevel=9, mtime=0)
        )
        # Python releases differ in the gzip OS byte when mtime is zero.
        # Normalize it so system Python and PlatformIO's Python emit the
        # exact same firmware input.
        compressed_bytes[9] = 255
        compressed = bytes(compressed_bytes)
        sections.extend(
            [
                f"static const uint8_t {symbol}_GZ[] PROGMEM = {{",
                byte_array(compressed),
                "};",
                f"static constexpr size_t {symbol}_GZ_SIZE = sizeof({symbol}_GZ);",
                "",
            ]
        )
    sections.extend(["} // namespace web_assets", ""])
    generated = "\n".join(sections).encode("ascii")
    if not output.exists() or output.read_bytes() != generated:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(generated)
        print(f"Generated {output.relative_to(project_dir)}")


try:
    Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.
except NameError:
    env = None

if env is not None:
    generate(Path(env.subst("$PROJECT_DIR")))
elif __name__ == "__main__":
    generate(Path(__file__).resolve().parents[1])

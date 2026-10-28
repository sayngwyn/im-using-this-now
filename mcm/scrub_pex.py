"""Strip personal details from compiled Skyrim Papyrus scripts.

The compiler writes the source file name, the Windows user name and the
computer name into every .pex header. This sets the user and computer to
the author name and keeps only the source's file name. Skyrim's .pex is
big-endian (Fallout 4's is little-endian). Nothing after the header holds
file offsets, so the strings may change length.
Usage: python scrub_pex.py <scripts dir> <author>
"""
import struct
import sys
from pathlib import Path

MAGIC = 0xFA57C0DE
HEADER = 4 + 1 + 1 + 2 + 8  # magic, major, minor, game id, compile time


def wstr(data, pos):
    n = struct.unpack_from(">H", data, pos)[0]
    return data[pos + 2:pos + 2 + n].decode("latin1"), pos + 2 + n


def pack(s):
    raw = s.encode("latin1")
    return struct.pack(">H", len(raw)) + raw


def scrub(path, author):
    data = path.read_bytes()
    if struct.unpack_from(">I", data, 0)[0] != MAGIC:
        sys.exit(f"not a Skyrim pex: {path}")
    source, pos = wstr(data, HEADER)
    _user, pos = wstr(data, pos)
    _machine, pos = wstr(data, pos)
    name = source.replace("/", "\\").rsplit("\\", 1)[-1]
    path.write_bytes(data[:HEADER] + pack(name) + pack(author) + pack(author) + data[pos:])
    return name


if __name__ == "__main__":
    root, author = Path(sys.argv[1]), sys.argv[2]
    for pex in sorted(root.rglob("*.pex")):
        print(f"{pex.name}: {scrub(pex, author)}")

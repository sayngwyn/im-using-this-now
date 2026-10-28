"""Writes ImUsingThisNow.esp for the MCM edition: an ESL-flagged plugin with
one start-game-enabled quest carrying the menu script (ImUsingThisNow_MCM)
and a player alias with SkyUI's SKI_PlayerLoadGameAlias, the standard
SkyUI MCM setup. No masters beyond Skyrim.esm and no SkyUI master: the
scripts are attached by name and SkyUI supplies its own at runtime.

Usage: python make_plugin.py <output.esp>
"""
import struct
import sys

FORM_VERSION = 44  # Skyrim SE
QUEST_ID = 0x01000800  # first ESL-safe id, local to this plugin (index 01)
PLAYER_REF = 0x00000014


def zstr(text):
    return text.encode("cp1252") + b"\0"


def wstr(text):
    data = text.encode("cp1252")
    return struct.pack("<H", len(data)) + data


def sub(tag, data):
    return tag.encode("ascii") + struct.pack("<H", len(data)) + data


def record(tag, flags, form_id, data):
    return tag.encode("ascii") + struct.pack("<IIIIHH", len(data), flags, form_id, 0, FORM_VERSION, 0) + data


def group(label, data):
    return b"GRUP" + struct.pack("<I4sIIHH", 24 + len(data), label.encode("ascii"), 0, 0, FORM_VERSION, 0) + data


def script(name):
    # name, status (local), no properties
    return wstr(name) + struct.pack("<BH", 0, 0)


def vmad():
    data = struct.pack("<hhH", 5, 2, 1) + script("ImUsingThisNow_MCM")
    # Quest fragment section: unknown (2), no fragments, empty file name.
    data += struct.pack("<bH", 2, 0) + wstr("")
    # One alias (id 0) carrying SkyUI's load-game script.
    data += struct.pack("<H", 1)
    data += struct.pack("<HhI", 0, 0, QUEST_ID)  # object: unused, alias id, quest
    data += struct.pack("<hhH", 5, 2, 1) + script("SKI_PlayerLoadGameAlias")
    return data


def quest():
    data = sub("EDID", zstr("ImUsingThisNowMCMQuest"))
    data += sub("VMAD", vmad())
    data += sub("FULL", zstr("I'm Using This Now MCM"))
    # Flags: Start Game Enabled (0x1) | Run Once (0x100); priority 0.
    data += sub("DNAM", struct.pack("<HBBII", 0x0101, 0, 0, 0, 0))
    data += sub("ANAM", struct.pack("<I", 1))  # next alias id
    data += sub("ALST", struct.pack("<I", 0))
    data += sub("ALID", zstr("PlayerAlias"))
    data += sub("FNAM", struct.pack("<I", 0))
    data += sub("ALFR", struct.pack("<I", PLAYER_REF))
    data += sub("VTCK", struct.pack("<I", 0))
    data += sub("ALED", b"")
    return record("QUST", 0, QUEST_ID, data)


def header(num_records):
    # 1.70, not 1.71: games before 1.6.1130 refuse 1.71 plugins.
    data = sub("HEDR", struct.pack("<fII", 1.70, num_records, 0x801))
    data += sub("CNAM", zstr("sayngwyn"))
    data += sub("SNAM", zstr("Menu for I'm Using This Now (needs SkyUI)."))
    data += sub("MAST", zstr("Skyrim.esm"))
    data += sub("DATA", struct.pack("<Q", 0))
    return record("TES4", 0x200, 0, data)  # 0x200 = light (ESL) flag


def main():
    out = sys.argv[1]
    body = group("QUST", quest())
    with open(out, "wb") as f:
        f.write(header(2) + body)  # 1 group + 1 record


if __name__ == "__main__":
    main()

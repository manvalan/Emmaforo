#!/usr/bin/env python3
"""Turns a dump of the "powerlog" partition into CSV, oldest row first.

Usage: power_log.py powerlog.bin [out.csv]
The layout is the Record struct in src/power_debug.cpp.
"""
import csv
import struct
import sys

RECORD = struct.Struct("<IIHHhHHHHBBBBBBBBBB")
TAG = 0xA7
CELL_MAH = 900
UNKNOWN = 0xFFFF
COLUMNS = ("seq,uptime_s,type,mv,min_mv,soc_x10,rate_centi,drain_ma,charge_ma,vbus_mv,charge_state,"
           "power_good,charging,gauge,fault,r,g,b,lamp_on,blink,ble,wifi,resting,audio,sound,audio_ms,"
           "sleep_permille,wakeups,autonomy_min").split(",")


def checksum(raw):
    total = 0
    for byte in raw[:-1]:
        total = (total * 31 + byte) & 0xFF
    return total


def rows(data):
    for offset in range(0, len(data) - RECORD.size + 1, RECORD.size):
        raw = data[offset:offset + RECORD.size]
        fields = RECORD.unpack(raw)
        if fields[0] >> 24 != TAG or fields[-1] != checksum(raw):
            continue
        yield decode(fields)


def decode(fields):
    (seq, uptime, mv, soc, rate, sleep, wakeups, autonomy, audio_ms,
     sag, charge, vbus, flags, status, r, g, b, sound, _check) = fields
    bit = lambda mask: int(flags & mask != 0)
    # Integer division towards zero, as the firmware prints it.
    drain = int(-rate * CELL_MAH / 10000) if rate < 0 else 0
    return [seq & 0xFFFFFF, uptime, "audio" if sound else "periodic", mv, mv - sag,
            "" if soc == UNKNOWN else soc, rate, drain, charge * 50, vbus * 100, status & 7,
            bit(0x02), bit(0x01), int(status & 0x08 != 0), int(status & 0x10 != 0), r, g, b,
            bit(0x04), bit(0x08), bit(0x10), bit(0x20), bit(0x40), bit(0x80), sound, audio_ms,
            sleep, wakeups, "" if autonomy == UNKNOWN else autonomy]


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as source:
        table = sorted(rows(source.read()), key=lambda row: row[0])
    out = open(sys.argv[2], "w", newline="") if len(sys.argv) == 3 else sys.stdout
    writer = csv.writer(out)
    writer.writerow(COLUMNS)
    writer.writerows(table)
    if out is not sys.stdout:
        out.close()
    print(f"{len(table)} rows", file=sys.stderr)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Decode PADSCOPE SysEx dumps (make PADSCOPE=1) pasted as hex, one or more
messages per line, spaces optional, from stdin.

Message: F0 00 20 29 7D <type> <part> <payload> F7
  type 1 capture:   part 0 = pad, stale lo7, stale hi7, 128 x 12-bit pad samples
                    part 1 = 128 x 12-bit neighbour-max samples
  type 2 idle stats: part 0 = 64 x min, part 1 = 64 x max (ADC channel order)
12-bit values are two bytes: (v >> 7) & 0x1F, v & 0x7F.

Prints for each capture a header line and CSV t_ms,pad,neighbour_max
(t_ms -8..119, 0 = trigger sample), and for idle stats a table of
channel, pad index, min, max. A channel with no idle samples shows "-".
"""
import re
import sys

PRE = 8
ADC_MAP = [
    11, 51, 12, 52, 13, 53, 14, 54, 15, 55, 16, 56, 17, 57, 18, 58,
    21, 61, 22, 62, 23, 63, 24, 64, 25, 65, 26, 66, 27, 67, 28, 68,
    31, 71, 32, 72, 33, 73, 34, 74, 35, 75, 36, 76, 37, 77, 38, 78,
    41, 81, 42, 82, 43, 83, 44, 84, 45, 85, 46, 86, 47, 87, 48, 88,
]


def unpack12(b):
    if len(b) % 2:
        raise ValueError("odd payload length")
    return [(b[i] << 7) | b[i + 1] for i in range(0, len(b), 2)]


def messages(text):
    for line in text.splitlines():
        line = line.split("#")[0]
        h = re.sub(r"(?i)0x|[^0-9a-f]", "", line)
        if len(h) % 2:
            sys.stderr.write("warning: odd hex digit count, line skipped\n")
            continue
        data = bytes.fromhex(h)
        cur = None
        for x in data:
            if x == 0xF0:
                cur = bytearray()
            elif x == 0xF7:
                if cur is not None:
                    yield bytes(cur)
                cur = None
            elif cur is not None:
                cur.append(x)


def main():
    pending = {}
    for m in messages(sys.stdin.read()):
        if len(m) < 6 or m[:3] != b"\x00\x20\x29" or m[3] != 0x7D:
            sys.stderr.write("warning: not a padscope message, skipped\n")
            continue
        typ, part, payload = m[4], m[5], m[6:]
        pending[(typ, part)] = payload
        if typ == 1 and (1, 0) in pending and (1, 1) in pending and part == 1:
            p0, p1 = pending.pop((1, 0)), pending.pop((1, 1))
            pad = p0[0]
            stale = p0[1] | (p0[2] << 7)
            pads = unpack12(p0[3:])
            nbs = unpack12(p1)
            print("# capture pad=%d stale_ticks=%d samples=%d" % (pad, stale, len(pads)))
            print("t_ms,pad,neighbour_max")
            for i, (a, b) in enumerate(zip(pads, nbs)):
                print("%d,%d,%d" % (i - PRE, a, b))
        elif typ == 2 and (2, 0) in pending and (2, 1) in pending and part == 1:
            mins = unpack12(pending.pop((2, 0)))
            maxs = unpack12(pending.pop((2, 1)))
            print("# idle stats (values below 400 only)")
            print("channel,pad,min,max")
            for c, (lo, hi) in enumerate(zip(mins, maxs)):
                pad = ADC_MAP[c] if c < len(ADC_MAP) else -1
                if lo > hi:
                    print("%d,%d,-,-" % (c, pad))
                else:
                    print("%d,%d,%d,%d" % (c, pad, lo, hi))


if __name__ == "__main__":
    main()

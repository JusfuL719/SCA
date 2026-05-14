#!/usr/bin/env python3
"""Drift check: every HvLog/HvLogHex code in SCA/*.c must appear in
LOG_DECODER.txt. SCA-scoped twin of PEX/tools/check_log_decoder.py — same
logic, different ROOT so the two HV trees can drift independently.

Exits 0 if clean, 1 with diff if drifted. Wired into build_sca_hv.sh
ahead of the EDK2 build so a missing entry kills the build instead of
shipping unmappable codes into the ring buffer.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DECODER = ROOT / "LOG_DECODER.txt"

HV_CALL = re.compile(r'HvLog(?:Hex)?\(([^;]*?)\)', re.DOTALL)
CODE_LIT = re.compile(r'"([A-Z][A-Z0-9_]{1,5})(?:\\n)?"')
DEC_KEY = re.compile(r'^\s*([A-Z][A-Z0-9_]{1,5})\s*=', re.MULTILINE)


def main() -> int:
    src_codes: dict[str, list[str]] = {}
    for c in sorted(ROOT.glob("*.c")):
        text = c.read_text()
        line_starts = [0]
        for i, ch in enumerate(text):
            if ch == "\n":
                line_starts.append(i + 1)

        def lineno(off: int) -> int:
            lo, hi = 0, len(line_starts) - 1
            while lo < hi:
                mid = (lo + hi + 1) // 2
                if line_starts[mid] <= off:
                    lo = mid
                else:
                    hi = mid - 1
            return lo + 1

        for m in HV_CALL.finditer(text):
            args = m.group(1)
            ln = lineno(m.start())
            for cm in CODE_LIT.finditer(args):
                src_codes.setdefault(cm.group(1), []).append(f"{c.name}:{ln}")

    decoded = set(DEC_KEY.findall(DECODER.read_text()))

    missing = sorted(c for c in src_codes if c not in decoded)
    orphan  = sorted(c for c in decoded if c not in src_codes)

    if not missing and not orphan:
        print(f"LOG_DECODER OK — {len(src_codes)} codes / {len(decoded)} entries")
        return 0

    if missing:
        print("MISSING from LOG_DECODER.txt (emitted but not documented):")
        for c in missing:
            sites = src_codes[c][:3]
            tail = "" if len(src_codes[c]) <= 3 else f"  (+{len(src_codes[c]) - 3} more)"
            print(f"  {c:<8} {', '.join(sites)}{tail}")
    if orphan:
        print("ORPHANED in LOG_DECODER.txt (documented but never emitted):")
        for c in orphan:
            print(f"  {c}")
    return 1


if __name__ == "__main__":
    sys.exit(main())

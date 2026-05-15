#!/usr/bin/env python3
"""Static VGUI surface discovery + vtable mapping for SCA plan phases.

Produces:
  - output/canon/vgui_candidates_<date>.json
  - output/canon/vgui_candidates_<date>.md
  - output/canon/vgui_vtable_map_<date>.json
  - output/canon/vgui_probe_trace_<date>.md
  - output/canon/vgui_dryrun_verdict_<date>.md
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import struct
from collections import Counter, defaultdict

import capstone
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

# r5apex_dx12 section RVAs used by the Unicorn dump pipeline.
IMAGE_BASE = 0x140000000
TEXT_RVA = 0x0001000
RDATA_RVA = 0x14A2000
DATA_RVA = 0x1B30000

KEYWORDS_STRICT = (
    "vgui",
    "surface",
    "draw",
    "font",
    "text",
    "hud",
    "label",
    "overlay",
    "crosshair",
    "menu",
    "scoreboard",
    "subtitle",
)

KEYWORDS_SOFT = (
    "settext",
    "print",
    "printf",
    "setcolor",
    "position",
    "panel",
    "ui_",
    "r_ui",
    "rui",
)


def va_from_rva(rva: int) -> int:
    return IMAGE_BASE + rva


def rva_from_va(va: int) -> int:
    return va - IMAGE_BASE


def is_printable_ascii(blob: bytes) -> bool:
    return all(0x20 <= b <= 0x7E or b in (0x09, 0x0A, 0x0D) for b in blob)


def extract_rdata_strings(rdata: bytes, min_len: int = 5) -> list[dict]:
    out = []
    i = 0
    n = len(rdata)
    while i < n:
        if rdata[i] == 0:
            i += 1
            continue
        j = i
        while j < n and rdata[j] != 0:
            j += 1
        if j - i >= min_len:
            frag = rdata[i:j]
            if is_printable_ascii(frag):
                s = frag.decode("ascii", errors="ignore")
                out.append(
                    {
                        "str": s,
                        "rva": RDATA_RVA + i,
                        "va": va_from_rva(RDATA_RVA + i),
                        "len": len(s),
                    }
                )
        i = j + 1
    return out


def rank_string(name: str) -> int:
    s = name.lower()
    score = 0
    for kw in KEYWORDS_STRICT:
        if kw in s:
            score += 12
    for kw in KEYWORDS_SOFT:
        if kw in s:
            score += 4
    if "::" in s:
        score += 2
    if len(s) <= 80:
        score += 1
    return score


def build_lea_target_index(text: bytes) -> dict[int, list[int]]:
    idx: dict[int, list[int]] = defaultdict(list)
    i = 0
    n = len(text)
    while i < n - 7:
        b0, b1, b2 = text[i], text[i + 1], text[i + 2]
        if 0x48 <= b0 <= 0x4F and b1 == 0x8D:
            mod = (b2 >> 6) & 3
            rm = b2 & 7
            if mod == 0 and rm == 5:
                disp = struct.unpack_from("<i", text, i + 3)[0]
                xref_rva = TEXT_RVA + i
                tgt_rva = xref_rva + 7 + disp
                idx[tgt_rva].append(xref_rva)
                i += 7
                continue
        i += 1
    return idx


def disasm_window(md: capstone.Cs, text: bytes, anchor_rva: int,
                  back: int = 96, fwd: int = 128) -> tuple[list, int]:
    for backoff in (back, back - 8, back - 16, back - 32, 0):
        if backoff < 0:
            backoff = 0
        start_rva = max(TEXT_RVA, anchor_rva - backoff)
        off = start_rva - TEXT_RVA
        sz = min(backoff + fwd + 16, len(text) - off)
        if sz <= 0:
            continue
        insns = list(md.disasm(text[off:off + sz], va_from_rva(start_rva)))
        av = va_from_rva(anchor_rva)
        for i, ins in enumerate(insns):
            if ins.address == av:
                return insns, i
    return [], -1


def rip_mem_target(ins) -> int | None:
    if len(ins.operands) < 2:
        return None
    op = ins.operands[1]
    if op.type != X86_OP_MEM:
        return None
    if op.mem.base != X86_REG_RIP:
        return None
    return ins.address + ins.size + op.mem.disp


def reg_name(ins, op) -> str:
    if op.type != capstone.x86.X86_OP_REG:
        return ""
    return ins.reg_name(op.reg).lower()


def classify_callsite_role(insns: list, call_i: int) -> str:
    lo = max(0, call_i - 8)
    hi = min(len(insns), call_i + 1)
    textlike = False
    colorlike = False
    poslike = False
    for j in range(lo, hi):
        ins = insns[j]
        m = ins.mnemonic.lower()
        ops = ins.op_str.lower()
        if m == "lea" and ("r8" in ops or "rdx" in ops):
            textlike = True
        if m in ("mov", "xor", "or") and ("r9d" in ops or "ecx" in ops):
            if "0x" in ops:
                colorlike = True
        if m in ("mov", "lea") and ("edx" in ops or "ecx" in ops):
            if re.search(r"0x[0-9a-f]+", ops):
                poslike = True
    if colorlike:
        return "set_color_candidate"
    if poslike:
        return "set_pos_candidate"
    if textlike:
        return "draw_text_candidate"
    return "unknown"


def analyze_xref(md: capstone.Cs, text: bytes, xref_rva: int, name_rva: int) -> dict:
    insns, anchor_i = disasm_window(md, text, xref_rva)
    if anchor_i < 0:
        return {"status": "disasm_unaligned", "xref_rva": xref_rva}

    global_rvas: list[int] = []
    global_regs: dict[str, int] = {}
    calls: list[dict] = []

    lo = max(0, anchor_i - 10)
    hi = min(len(insns), anchor_i + 22)
    for i in range(lo, hi):
        ins = insns[i]
        mnem = ins.mnemonic.lower()
        if mnem in ("mov", "lea") and len(ins.operands) == 2:
            dst = ins.operands[0]
            tgt = rip_mem_target(ins)
            if dst.type == capstone.x86.X86_OP_REG and tgt is not None:
                rr = rva_from_va(tgt)
                if DATA_RVA <= rr < DATA_RVA + 0x2000000:
                    dn = reg_name(ins, dst)
                    global_rvas.append(rr)
                    global_regs[dn] = rr
        if mnem == "call" and ins.operands:
            op0 = ins.operands[0]
            role = classify_callsite_role(insns, i)
            if op0.type == capstone.x86.X86_OP_MEM:
                base = ins.reg_name(op0.mem.base).lower() if op0.mem.base else ""
                disp = op0.mem.disp
                slot = disp // 8 if disp >= 0 and (disp % 8 == 0) else None
                calls.append(
                    {
                        "kind": "indirect_mem",
                        "base_reg": base,
                        "disp": disp,
                        "slot_index": slot,
                        "role": role,
                        "rip_rva": rva_from_va(ins.address),
                    }
                )
            elif op0.type == capstone.x86.X86_OP_IMM:
                calls.append(
                    {
                        "kind": "direct",
                        "target_rva": rva_from_va(op0.imm),
                        "role": role,
                        "rip_rva": rva_from_va(ins.address),
                    }
                )

    chosen_global = None
    if global_rvas:
        chosen_global = Counter(global_rvas).most_common(1)[0][0]

    slot_hits = []
    for c in calls:
        if c["kind"] != "indirect_mem":
            continue
        if c["slot_index"] is None:
            continue
        bonus = 1 if c["base_reg"] in global_regs else 0
        slot_hits.append(
            {
                "slot_index": c["slot_index"],
                "disp": c["disp"],
                "role": c["role"],
                "rip_rva": c["rip_rva"],
                "base_reg": c["base_reg"],
                "global_reg_match": bonus,
            }
        )

    return {
        "status": "ok",
        "xref_rva": xref_rva,
        "name_rva": name_rva,
        "chosen_global_rva": chosen_global,
        "global_rvas": sorted(set(global_rvas)),
        "slot_hits": slot_hits,
        "calls": calls,
    }


def choose_top_slots(slot_rows: list[dict]) -> dict:
    by_slot: dict[int, list[dict]] = defaultdict(list)
    for r in slot_rows:
        by_slot[r["slot_index"]].append(r)
    scored = []
    for slot, rows in by_slot.items():
        role_counter = Counter(x["role"] for x in rows)
        score = len(rows) * 3
        score += role_counter.get("draw_text_candidate", 0) * 4
        score += role_counter.get("set_color_candidate", 0) * 2
        score += role_counter.get("set_pos_candidate", 0) * 2
        score += sum(x.get("global_reg_match", 0) for x in rows)
        scored.append((score, slot, role_counter, rows))
    scored.sort(reverse=True)

    top = {
        "top_slots": [],
        "slot_map": {
            "draw_text": None,
            "set_text_pos": None,
            "set_text_color": None,
            "set_font": None,
        },
    }
    for score, slot, role_counter, rows in scored[:10]:
        top["top_slots"].append(
            {
                "slot_index": slot,
                "score": score,
                "roles": dict(role_counter),
                "n_hits": len(rows),
                "sample_rip_rva": f"0x{rows[0]['rip_rva']:06X}",
            }
        )

    def pick_slot(preferred_role: str, used: set[int]) -> int | None:
        best = None
        for score, slot, role_counter, rows in scored:
            if slot in used:
                continue
            role_hits = role_counter.get(preferred_role, 0)
            if role_hits == 0:
                continue
            cand = (role_hits, score, slot)
            if best is None or cand > best:
                best = cand
        return None if best is None else best[2]

    used = set()
    for role, key in (
        ("draw_text_candidate", "draw_text"),
        ("set_pos_candidate", "set_text_pos"),
        ("set_color_candidate", "set_text_color"),
    ):
        s = pick_slot(role, used)
        if s is not None:
            top["slot_map"][key] = s
            used.add(s)
    for score, slot, role_counter, rows in scored:
        if slot in used:
            continue
        if top["slot_map"]["draw_text"] is not None:
            if abs(slot - top["slot_map"]["draw_text"]) <= 8:
                top["slot_map"]["set_font"] = slot
                used.add(slot)
                break
    if top["slot_map"]["set_text_pos"] is None:
        for score, slot, role_counter, rows in scored:
            if slot in used:
                continue
            top["slot_map"]["set_text_pos"] = slot
            used.add(slot)
            break
    if top["slot_map"]["set_text_color"] is None:
        for score, slot, role_counter, rows in scored:
            if slot in used:
                continue
            top["slot_map"]["set_text_color"] = slot
            used.add(slot)
            break
    if top["slot_map"]["set_text_color"] is None and top["slot_map"]["set_font"] is not None:
        top["slot_map"]["set_text_color"] = top["slot_map"]["set_font"]
    return top


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.abspath(os.path.join(here, ".."))
    ap = argparse.ArgumentParser()
    ap.add_argument("--dumps",
                    default="/srv/nfs/shared/Shared/Tools/UNICORN_DUMPER/EAC/EAC/r5apex")
    ap.add_argument("--out", default=os.path.join(repo, "output", "canon"))
    ap.add_argument("--min-string-len", type=int, default=5)
    ap.add_argument("--max-candidates", type=int, default=40)
    ap.add_argument("--max-xrefs-per-string", type=int, default=16)
    args = ap.parse_args()

    dumps = os.path.abspath(args.dumps)
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    text = open(os.path.join(dumps, "r5apex_live_text.bin"), "rb").read()
    rdata = open(os.path.join(dumps, "r5apex_live_rdata.bin"), "rb").read()
    data = open(os.path.join(dumps, "r5apex_live_data.bin"), "rb").read()
    print(f"[+] dumps: text={len(text)/1e6:.1f}MB rdata={len(rdata)/1e6:.1f}MB data={len(data)/1e6:.1f}MB")

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    strings = extract_rdata_strings(rdata, min_len=args.min_string_len)
    ranked_strings = []
    for s in strings:
        rank = rank_string(s["str"])
        if rank <= 0:
            continue
        s2 = dict(s)
        s2["keyword_score"] = rank
        ranked_strings.append(s2)
    ranked_strings.sort(key=lambda x: (-x["keyword_score"], x["str"]))
    ranked_strings = ranked_strings[: args.max_candidates]
    print(f"[+] keyword-matched strings: {len(ranked_strings)}")

    lea_idx = build_lea_target_index(text)
    print(f"[+] lea index: {sum(len(v) for v in lea_idx.values())} sites / {len(lea_idx)} targets")

    candidates = []
    for rs in ranked_strings:
        name = rs["str"]
        name_rva = rs["rva"]
        xrefs = lea_idx.get(name_rva, [])[: args.max_xrefs_per_string]
        if not xrefs:
            continue
        xref_analyses = []
        globals_seen = []
        slots_seen = []
        for xr in xrefs:
            a = analyze_xref(md, text, xr, name_rva)
            xref_analyses.append(a)
            if a.get("chosen_global_rva") is not None:
                globals_seen.append(a["chosen_global_rva"])
            slots_seen.extend(a.get("slot_hits", []))

        if not globals_seen and not slots_seen:
            continue
        global_vote = Counter(globals_seen).most_common(1)
        chosen_global = global_vote[0][0] if global_vote else None
        slot_meta = choose_top_slots(slots_seen)
        unique_slots = sorted({s["slot_index"] for s in slots_seen})
        candidate_score = rs["keyword_score"] * 3
        candidate_score += len(xrefs) * 2
        candidate_score += len(unique_slots) * 4
        if chosen_global is not None:
            candidate_score += 28
        if slot_meta["slot_map"]["draw_text"] is not None:
            candidate_score += 10
        if slot_meta["slot_map"]["set_text_pos"] is not None:
            candidate_score += 4
        if slot_meta["slot_map"]["set_text_color"] is not None:
            candidate_score += 4

        candidates.append(
            {
                "name": name,
                "name_rva": name_rva,
                "keyword_score": rs["keyword_score"],
                "n_xrefs": len(xrefs),
                "chosen_global_rva": chosen_global,
                "global_votes": dict(Counter(globals_seen)),
                "unique_slots": unique_slots,
                "slot_map": slot_meta["slot_map"],
                "top_slots": slot_meta["top_slots"],
                "candidate_score": candidate_score,
                "xrefs": [f"0x{x:06X}" for x in xrefs],
                "xref_analysis": xref_analyses,
            }
        )

    candidates.sort(key=lambda x: (-x["candidate_score"], x["name"]))

    iface_map: dict[int, dict] = {}
    for c in candidates:
        g = c.get("chosen_global_rva")
        if g is None:
            continue
        ent = iface_map.setdefault(
            g,
            {
                "global_rva": g,
                "names": [],
                "scores": [],
                "slot_hits": [],
            },
        )
        ent["names"].append(c["name"])
        ent["scores"].append(c["candidate_score"])
        for s in c["top_slots"]:
            ent["slot_hits"].append(s)

    vtable_map = []
    for g, v in iface_map.items():
        slot_counter = Counter()
        role_counter: dict[int, Counter] = defaultdict(Counter)
        for sh in v["slot_hits"]:
            slot = sh["slot_index"]
            slot_counter[slot] += sh["n_hits"]
            for rk, rv in sh["roles"].items():
                role_counter[slot][rk] += rv
        ranked_slots = []
        for slot, cnt in slot_counter.most_common():
            ranked_slots.append(
                {
                    "slot_index": slot,
                    "hits": cnt,
                    "roles": dict(role_counter[slot]),
                }
            )
        vtable_map.append(
            {
                "global_rva": g,
                "global_va": f"0x{va_from_rva(g):016X}",
                "names": sorted(set(v["names"])),
                "composite_score": sum(v["scores"]),
                "ranked_slots": ranked_slots,
            }
        )
    vtable_map.sort(key=lambda x: -x["composite_score"])

    dryrun = {
        "verdict": "fail",
        "reason": "no_candidate",
        "selected_name": None,
        "selected_global_rva": None,
        "slot_map": None,
        "required_slots": ["draw_text", "set_text_pos", "set_text_color"],
        "rollback_policy": {
            "enabled_default": 0,
            "fail_closed": 1,
            "max_faults": 4,
        },
    }
    top = None
    for c in candidates:
        if c["chosen_global_rva"] is not None:
            top = c
            break
    if top is None and candidates:
        top = candidates[0]
    if top is not None:
        slot_map = top["slot_map"]
        ok = all(slot_map.get(k) is not None for k in dryrun["required_slots"])
        dryrun["selected_name"] = top["name"]
        dryrun["selected_global_rva"] = top["chosen_global_rva"]
        dryrun["slot_map"] = slot_map
        if top["chosen_global_rva"] is None:
            dryrun["verdict"] = "fail"
            dryrun["reason"] = "missing_global_pointer"
        elif not ok:
            dryrun["verdict"] = "fail"
            dryrun["reason"] = "missing_required_slots"
        else:
            dryrun["verdict"] = "pass_static"
            dryrun["reason"] = "global_and_required_slots_present"

    today = datetime.date.today().isoformat()
    cand_json = os.path.join(out_dir, f"vgui_candidates_{today}.json")
    cand_md = os.path.join(out_dir, f"vgui_candidates_{today}.md")
    vmap_json = os.path.join(out_dir, f"vgui_vtable_map_{today}.json")
    probe_md = os.path.join(out_dir, f"vgui_probe_trace_{today}.md")
    dry_md = os.path.join(out_dir, f"vgui_dryrun_verdict_{today}.md")

    with open(cand_json, "w", encoding="utf-8") as f:
        json.dump(
            {
                "date": today,
                "dumps": dumps,
                "n_keyword_strings": len(ranked_strings),
                "n_candidates": len(candidates),
                "candidates": candidates,
            },
            f,
            indent=2,
        )
    with open(vmap_json, "w", encoding="utf-8") as f:
        json.dump(
            {
                "date": today,
                "dumps": dumps,
                "interfaces": vtable_map,
            },
            f,
            indent=2,
        )

    md_lines = [
        "# VGUI Candidate Discovery",
        "",
        f"- Date: `{today}`",
        f"- Dumps: `{dumps}`",
        f"- Keyword strings scanned: `{len(ranked_strings)}`",
        f"- Actionable candidates: `{len(candidates)}`",
        "",
        "## Hard Stop Conditions (Phase 0)",
        "",
        "- Stop immediately on repeated render-gate prologue drift markers (`DRR`/`DRX`).",
        "- Stop on unhandled NPF storms (`V0A`/`V0B`/`V0C`) or sustained VMEXIT slow-path markers (`VAD` with draw branch correlation).",
        "- Stop on orphan guest-call return markers (`XC3`) once dry-run mode is armed.",
        "- Fail closed if interface/vtable pointers become unstable across sampling windows.",
        "",
        "## Ranked Candidates",
        "",
        "| Rank | Name | Score | Global RVA | Slots (top) |",
        "|---|---|---:|---:|---|",
    ]
    for i, c in enumerate(candidates[:20], start=1):
        gr = "n/a" if c["chosen_global_rva"] is None else f"0x{c['chosen_global_rva']:08X}"
        tops = ", ".join(str(x["slot_index"]) for x in c["top_slots"][:4]) or "n/a"
        md_lines.append(f"| {i} | `{c['name']}` | {c['candidate_score']} | `{gr}` | `{tops}` |")
    md_lines += [
        "",
        "## Top Candidate Slot Map",
        "",
    ]
    if candidates:
        t = candidates[0]
        md_lines.append(f"- `name`: `{t['name']}`")
        md_lines.append(
            "- `global_rva`: "
            + ("`n/a`" if t["chosen_global_rva"] is None else f"`0x{t['chosen_global_rva']:08X}`")
        )
        sm = t["slot_map"]
        md_lines.append(f"- `draw_text`: `{sm.get('draw_text')}`")
        md_lines.append(f"- `set_text_pos`: `{sm.get('set_text_pos')}`")
        md_lines.append(f"- `set_text_color`: `{sm.get('set_text_color')}`")
        md_lines.append(f"- `set_font`: `{sm.get('set_font')}`")
    else:
        md_lines.append("- no candidates")
    with open(cand_md, "w", encoding="utf-8") as f:
        f.write("\n".join(md_lines) + "\n")

    probe_lines = [
        "# VGUI Probe Trace Spec",
        "",
        f"- Date: `{today}`",
        "",
        "Read-only probe evidence fields expected from HV runtime sampling:",
        "- interface global VA read success/failure",
        "- interface pointer stability (frame-to-frame)",
        "- vtable pointer module-range sanity",
        "- slot target pointer stability for draw/pos/color candidates",
        "- fault counter and fail-closed latch state",
        "",
        "Recommended marker namespace:",
        "- `VG0` params applied",
        "- `VG1` sample `(iface, vtable)`",
        "- `VG2` gate passed",
        "- `VG3` fail-closed latch / rollback",
        "- `VG4` dry-run precheck pass",
        "- `VG5` backend heartbeat (skeleton only)",
        "- `VGQ` probe fault code",
    ]
    with open(probe_md, "w", encoding="utf-8") as f:
        f.write("\n".join(probe_lines) + "\n")

    dry_lines = [
        "# VGUI Dry-Run Static Verdict",
        "",
        f"- Date: `{today}`",
        f"- Verdict: `{dryrun['verdict']}`",
        f"- Reason: `{dryrun['reason']}`",
        "",
        f"- Selected candidate: `{dryrun['selected_name']}`",
        "- Selected global RVA: "
        + (
            "`n/a`"
            if dryrun["selected_global_rva"] is None
            else f"`0x{dryrun['selected_global_rva']:08X}`"
        ),
        "",
        "Required slots:",
    ]
    for k in dryrun["required_slots"]:
        v = None if dryrun["slot_map"] is None else dryrun["slot_map"].get(k)
        dry_lines.append(f"- `{k}` -> `{v}`")
    dry_lines += [
        "",
        "Rollback policy:",
        f"- `enabled_default`: `{dryrun['rollback_policy']['enabled_default']}`",
        f"- `fail_closed`: `{dryrun['rollback_policy']['fail_closed']}`",
        f"- `max_faults`: `{dryrun['rollback_policy']['max_faults']}`",
    ]
    with open(dry_md, "w", encoding="utf-8") as f:
        f.write("\n".join(dry_lines) + "\n")

    print(f"[+] {cand_json}")
    print(f"[+] {cand_md}")
    print(f"[+] {vmap_json}")
    print(f"[+] {probe_md}")
    print(f"[+] {dry_md}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

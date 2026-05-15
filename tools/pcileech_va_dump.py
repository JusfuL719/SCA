#!/usr/bin/env python3
"""Pcileech-driven VA-contiguous dump.

Walks Win11 x64 4-level page tables via raw PA reads (pcileech on PC3),
coalesces PA runs, bulk-dumps each run, stitches into a VA-contiguous bin.

Hard rules respected:
  * direct pcileech.exe dump -device fpga -memmap (no probe, no kmd, no python Vmm)
  * dump chunks <= 64 MB
  * UNC output, deletes stale before re-dump
"""
import os
import struct
import subprocess
import sys

CR3        = 0x000000023b723000
IMG_BASE   = 0x00007ff7537d0000
WIN_LEN    = 0x04000000                                  # 64 MB VA window
WIN_END    = IMG_BASE + WIN_LEN
OUT_NAME   = "r5apex_live_data_full.bin"

PC3_DUMP_CMD = (
    r"C:\Tools\AGENT\pcileech.exe dump -device fpga "
    r"-memmap C:\Tools\AGENT\physmap\physmemmap.txt "
)
UNC_PREFIX = r"\\10.0.0.2\MyShared\PC3_DMA_results"
LOCAL_DIR  = "/srv/nfs/shared/Shared/PC3_DMA_results"
MAX_CHUNK  = 64 * 1024 * 1024                            # hard rule

PT_BITS = 0x000FFFFFFFFFF000


def ssh_pc3(cmd, timeout=120):
    return subprocess.run(
        ["ssh", "pc3", f'cmd /c "{cmd} 2>&1"'],
        capture_output=True, text=True, timeout=timeout,
    )


def pcileech_dump_pa(pa, length, out_name):
    """Dump [pa, pa+length) to UNC out_name. Delete stale first. Verify size."""
    local_path = os.path.join(LOCAL_DIR, out_name)
    if os.path.exists(local_path):
        os.remove(local_path)
    cmd = (
        PC3_DUMP_CMD
        + f"-min 0x{pa:x} -max 0x{pa+length:x} "
        + f"-out {UNC_PREFIX}\\{out_name}"
    )
    r = ssh_pc3(cmd, timeout=max(60, int(length / 1024 / 1024) + 30))
    if r.returncode != 0:
        sys.stderr.write(f"pcileech FAIL pa=0x{pa:x} len=0x{length:x}:\n{r.stdout}\n{r.stderr}\n")
        return None
    if not os.path.exists(local_path):
        sys.stderr.write(f"output missing: {local_path}\nstdout: {r.stdout}\n")
        return None
    sz = os.path.getsize(local_path)
    if sz != length:
        sys.stderr.write(f"size mismatch: {local_path} got {sz} want {length}\n")
    return local_path


def read_page(pa):
    """Fetch a single 4 KB page; return bytes (or None on failure)."""
    name = f"pt_walk_{pa:012x}.bin"
    path = pcileech_dump_pa(pa, 0x1000, name)
    if not path:
        return None
    with open(path, "rb") as f:
        return f.read()


def pte_present(e):
    return bool(e & 1)


def pte_ps(e):
    return bool(e & (1 << 7))


def pte_pa(e):
    return e & PT_BITS


def coalesce(va_pa_list):
    """va_pa_list: sorted by VA; each entry (va, pa, length).
       Yields (pa_start, pa_len, va_start, va_len) for contiguous-VA-and-PA runs."""
    if not va_pa_list:
        return
    cur_va, cur_pa, cur_len = va_pa_list[0]
    for va, pa, length in va_pa_list[1:]:
        if va == cur_va + cur_len and pa == cur_pa + cur_len:
            cur_len += length
        else:
            yield (cur_pa, cur_len, cur_va, cur_len)
            cur_va, cur_pa, cur_len = va, pa, length
    yield (cur_pa, cur_len, cur_va, cur_len)


def main():
    print(f"[*] CR3=0x{CR3:x} IMG_BASE=0x{IMG_BASE:x} LEN=0x{WIN_LEN:x}")

    # 1. PML4
    pml4 = read_page(CR3)
    if not pml4:
        sys.exit("PML4 read failed")
    pml4_idx = (IMG_BASE >> 39) & 0x1FF
    e = struct.unpack_from("<Q", pml4, pml4_idx * 8)[0]
    print(f"[+] PML4[{pml4_idx}] = 0x{e:016x}")
    if not pte_present(e):
        sys.exit("PML4 entry not present")
    pdpt_pa = pte_pa(e)

    # 2. PDPT
    pdpt = read_page(pdpt_pa)
    if not pdpt:
        sys.exit("PDPT read failed")
    pdpt_idx = (IMG_BASE >> 30) & 0x1FF
    e = struct.unpack_from("<Q", pdpt, pdpt_idx * 8)[0]
    print(f"[+] PDPT[{pdpt_idx}] = 0x{e:016x}")
    if not pte_present(e):
        sys.exit("PDPT entry not present")
    if pte_ps(e):
        sys.exit("1 GB page at PDPT — unhandled (range would overflow 64 MB anyway)")
    pd_pa = pte_pa(e)

    # 3. PD — read full page, iterate over the 33-ish entries we need
    pd = read_page(pd_pa)
    if not pd:
        sys.exit("PD read failed")

    pd_idx_lo = (IMG_BASE >> 21) & 0x1FF
    pd_idx_hi = ((WIN_END - 1) >> 21) & 0x1FF
    print(f"[*] PD idx range {pd_idx_lo}..{pd_idx_hi}")

    va_pa = []                                              # (va, pa, length)
    missing_va = []                                         # zero-fill list

    va_cursor = IMG_BASE
    for pd_i in range(pd_idx_lo, pd_idx_hi + 1):
        pd_e = struct.unpack_from("<Q", pd, pd_i * 8)[0]
        pd_va_base = (IMG_BASE & ~((1 << 39) - 1)) | (((IMG_BASE >> 30) & 0x1FF) << 30) | (pd_i << 21)
        # 2 MB region covered by this PD entry
        region_lo = max(pd_va_base, IMG_BASE)
        region_hi = min(pd_va_base + (1 << 21), WIN_END)
        region_len = region_hi - region_lo
        if not pte_present(pd_e):
            missing_va.append((region_lo, region_len))
            continue
        if pte_ps(pd_e):
            # 2 MB large page — PA = aligned PA + offset into 2 MB
            pa_2mb = pd_e & 0x000FFFFFFFE00000
            pa = pa_2mb + (region_lo - pd_va_base)
            va_pa.append((region_lo, pa, region_len))
            continue
        # 4 KB page table
        pt_pa = pte_pa(pd_e)
        pt = read_page(pt_pa)
        if not pt:
            sys.stderr.write(f"PT read failed at PA 0x{pt_pa:x}\n")
            missing_va.append((region_lo, region_len))
            continue
        # walk PT entries that fall in our range
        pt_idx_lo = ((region_lo >> 12) & 0x1FF)
        pt_idx_hi = (((region_hi - 1) >> 12) & 0x1FF)
        for pt_i in range(pt_idx_lo, pt_idx_hi + 1):
            pt_e = struct.unpack_from("<Q", pt, pt_i * 8)[0]
            va = pd_va_base + (pt_i << 12)
            if not pte_present(pt_e):
                missing_va.append((va, 0x1000))
                continue
            pa = pte_pa(pt_e)
            va_pa.append((va, pa, 0x1000))

    print(f"[+] mapped pages={len(va_pa)} missing_runs={len(missing_va)}")

    # 4. Coalesce
    runs = list(coalesce(va_pa))
    print(f"[+] coalesced into {len(runs)} PA runs")
    total = sum(r[1] for r in runs)
    print(f"[+] total mapped bytes = 0x{total:x} ({total / 1024 / 1024:.1f} MB)")

    # 5. Bulk-dump runs (max MAX_CHUNK per call)
    out_path = os.path.join(LOCAL_DIR, OUT_NAME)
    if os.path.exists(out_path):
        os.remove(out_path)
    # Pre-create as a 64 MB file of zeros, then overwrite mapped regions
    with open(out_path, "wb") as f:
        f.truncate(WIN_LEN)

    for idx, (pa, length, va, vlen) in enumerate(runs):
        off_in_out = va - IMG_BASE
        # Split run if > MAX_CHUNK
        sub_off = 0
        while sub_off < length:
            sub_len = min(MAX_CHUNK, length - sub_off)
            chunk_pa = pa + sub_off
            chunk_name = f"r5va_run{idx:03d}_p{sub_off // MAX_CHUNK}.bin"
            chunk_path = pcileech_dump_pa(chunk_pa, sub_len, chunk_name)
            if not chunk_path:
                sys.stderr.write(f"chunk {idx}/{sub_off} dump failed\n")
                sub_off += sub_len
                continue
            with open(chunk_path, "rb") as cf, open(out_path, "r+b") as of:
                of.seek(off_in_out + sub_off)
                of.write(cf.read())
            os.remove(chunk_path)                            # tidy
            sub_off += sub_len

    # 6. Tidy PT walk files
    for fn in os.listdir(LOCAL_DIR):
        if fn.startswith("pt_walk_"):
            os.remove(os.path.join(LOCAL_DIR, fn))

    sz = os.path.getsize(out_path)
    print(f"[+] {out_path} size=0x{sz:x} ({sz / 1024 / 1024:.1f} MB)")
    print(f"[+] missing {len(missing_va)} range(s), total "
          f"{sum(l for _, l in missing_va) / 1024 / 1024:.2f} MB zero-filled")


if __name__ == "__main__":
    main()

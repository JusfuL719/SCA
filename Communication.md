# HypeLoader Communication Protocol

**Owns:** wire protocol — auth (HANDSHAKE/MixKey), channel selection (VMMCALL bootstrap vs NPT-fault covert batch), command IDs and arguments, mailbox structure, status codes, back-pressure, lifecycle log markers (VC0/VC1). **Defers to:** [CLAUDE.md](CLAUDE.md) for HV-side internals (CR3 scanner phases, VMEXIT handlers, kernel offsets), [LOG_DECODER.txt](LOG_DECODER.txt) for full HvLog code dictionary.

Two channels: **VMMCALL** (bootstrap/init) and **NPT-Fault Covert Channel** (sole runtime command path, batch ≤80 commands per trigger). All work from ring-3 Windows usermode. No kernel driver required.

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│ Windows Guest (16C/32T — all cores visible)          │
│   └── Overlay process (ring 3, pinned to CPU 0)     │
│         ├── VMMCALL init (3× at startup)             │
│         └── NPT-fault trigger (batch commands)       │
└─────────────────────────────────────────────────────┘
              │ NPT write-fault on trigger page
              ▼
┌─────────────────────────────────────────────────────┐
│ Hypervisor (Type-0 SVM, beneath all cores)           │
│   └── VMEXIT handler processes mailbox batch         │
│   └── Up to 80 commands per NPT-fault trigger        │
│   └── Single-step past faulting write → #DB restore  │
│   └── Results written inline to mailbox command slots │
└─────────────────────────────────────────────────────┘
```

1 VMEXIT per batch trigger. Guest is paused during processing — page table walks are safe.

---

## 1. Authentication

Every boot generates a random 64-bit key `gBootAuthKey` from RDSEED/RDRAND via SplitMix64. Client obtains it via HANDSHAKE (0x60) — no DMA, no out-of-band transfer.

```c
// HypeSvm.h
#define HYPE_BUILD_SECRET  0x7A3F9B2E5D1C8064ULL

static uint64_t mix_key(uint64_t x) {
    x ^= x >> 30;  x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;  x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;  return x;
}

// At boot:
uint64_t gBootAuthKey       = mix_key(gBootKey + 1);         // per-boot, from RDSEED
uint64_t gHandshakeExpected = mix_key(GetBuildSecret());      // compile-time constant
```

```c
// Client (config.h XOR-encodes BUILD_SECRET; memory.h has same MixKey)
uint64_t auth_key = hype_vmmcall(MixKey(GetBuildSecret()), PMC_CMD_HANDSHAKE, 0, 0);
```

| Channel | Check | Failure |
|---------|-------|---------|
| HANDSHAKE | `RBX == MixKey(HYPE_BUILD_SECRET)` | `#UD` injected |
| VMMCALL (all others) | `RBX == gBootAuthKey` | `#UD` injected |
| NPT-fault mailbox | `Mailbox->AuthKey == gBootAuthKey` | single-step, no commands processed |

BUILD_SECRET must match between HV and client. Change on every build.

---

## 2. Channel 1: VMMCALL (Bootstrap)

One-time init commands. VMMCALL (`0F 01 D9`) causes mandatory VMEXIT on SVM; generates `#UD` on bare metal.

### Register Convention

| Register | Direction | Purpose |
|----------|-----------|---------|
| RBX | In | `gBootAuthKey` (auth gate) |
| RCX | In | CmdId |
| RDX | In | Arg1 |
| R8 | In | Arg2 |
| RAX | Out | Result (64-bit) |

HANDSHAKE advances RIP via NRip. All others advance RIP +3. Wrong auth → `#UD`, RIP not advanced.

### Commands

| CmdId | Name | RBX | RDX (Arg1) | R8 (Arg2) | Returns (RAX) |
|-------|------|-----|------------|-----------|---------------|
| `0x60` | HANDSHAKE | `MixKey(BUILD_SECRET)` | — | — | `gBootAuthKey` |
| `0x0A` | SET_EPROCESS | `gBootAuthKey` | EPROCESS VA (0 = auto-discover) | — | `HYPE_MEM_OK` or error |
| `0x50` | NPT_CHANNEL_INIT | `gBootAuthKey` | Trigger page VA | Mailbox page VA | `0` on success |

**SET_EPROCESS auto-discover (Arg1=0):** Walks KPCR → KPRCB → IdleThread → KTHREAD → EPROCESS, finds PID 4 (System).

### VMMCALL Stub

No VMMCALL opcode in binary. Stub is XOR-decoded at runtime into `PAGE_READWRITE`, flipped to `PAGE_EXECUTE_READ`, freed after init.

```c
// XOR-encoded stub (key 0x55)
static const uint8_t enc[] = {
    0x06, 0x1D, 0xDC, 0x9E, 0xDC, 0x84, 0x19, 0xDC,
    0x97, 0x18, 0xDC, 0x9D, 0x5A, 0x54, 0x8C, 0x0E, 0x96
};
// memory.h: InitVmmcallStub() / FreeVmmcallStub()
```

---

## 3. Channel 2: NPT-Fault Covert Channel (Sole Runtime Path)

1. `NPT_CHANNEL_INIT(trigger_va, mailbox_va)` → HV translates VAs → GPAs, marks trigger page read-only in NPT.
2. Client fills `COVERT_MAILBOX` with commands.
3. Client writes to trigger page → NPF fires.
4. HV validates CR3 (client process only) and AuthKey.
5. Processes all commands in batch (≤80), writes results inline.
6. Makes trigger page temporarily writable, sets RFLAGS.TF.
7. Guest re-executes faulting write → succeeds.
8. **#DB** fires → HV restores trigger page read-only.
9. Client reads results.

**Total cost: 1 logical VMEXIT per batch** (NPF + #DB are 2 VMEXITs but processed as one operation).

### Mailbox Structure (COVERT_MAILBOX — 3856 bytes)

```c
struct COVERT_CMD {
    uint32_t CmdId;
    uint32_t Status;    // written by HV
    uint64_t Arg1;
    uint64_t Arg2;
    uint64_t Arg3;
    uint64_t Result;    // written by HV
};

struct COVERT_MAILBOX {
    uint64_t AuthKey;       // must match gBootAuthKey
    uint32_t NumCommands;   // 1..80
    uint32_t Sequence;
    COVERT_CMD Cmd[80];     // 48 bytes each
};
```

### Supported Commands

| CmdId | Name | Description |
|-------|------|-------------|
| `0x00` | PING | Returns `0x48595045 ^ (gBootAuthKey >> 32)` |
| `0x02` | SET_CR3 | Sets per-VCPU TargetCr3 |
| `0x30` | VIRT_READ4 | 4-byte virtual read (uses TargetCr3) |
| `0x31` | VIRT_READ8 | 8-byte virtual read |
| `0x32` | VIRT_WRITE4 | 4-byte virtual write |
| `0x33` | VIRT_WRITE8 | 8-byte virtual write |
| `0x34` | PROC_CR3 | Find process CR3 by PID (EPROCESS walk) |
| `0x35` | VIRT_WRITE1 | 1-byte virtual write (low byte of Arg2) |
| `0x18` | TLB_PROBE | Research-only NPT iTLB-asymmetry probe (Zen 3). See § TLB_PROBE. |
| `0x36` | CR3_INTERCEPT | Arm chunked CR3 capture (`Cr3PassiveSample`) |
| `0x37` | GET_INTERCEPT_PEB | Poll chunked CR3 scanner; returns captured PEB |

Reserved/unused: `0x01, 0x03–0x06, 0x09, 0x0B–0x10, 0x11–0x13, 0x14–0x16, 0x38` → `BAD_CMD`.

### TLB_PROBE (0x18) — Research-only NPT iTLB-asymmetry probe

Measures how long a Zen 3 L1 iTLB combined entry survives after the underlying NPT PTE is mutated without TLB invalidation. Output: per-bin survival fraction + swap-cycle latency, written by apphost to `C:\PEX\overlay\tlb_probe_<unix_ts>.txt`. Full design in [docs/2026-05-tlb-probe-design.md](docs/2026-05-tlb-probe-design.md).

Sub-op selector in `Arg1`:

| Arg1 | Sub-op | Args | Result |
|------|--------|------|--------|
| `1` | ARM | Arg2 = ProbeVa (guest VA, page-aligned) | ProbeGpa. Allocates HpaA/HpaB, fills sentinel, captures original PTE. |
| `2` | SEED | Arg2 = which (0=HpaA, 1=HpaB) | 0. PTE → HpaWhich + TlbControl=1 broadcast. |
| `3` | SWAP | Arg2 = which | 0. PTE → HpaWhich. No TLB invalidation. |
| `4` | RESTORE | — | 0. PTE → original + FlushAllTlb. |
| `5` | DISARM | — | Total swap count. Restores PTE, clears state. HpaA/HpaB leak into bump pool (no free path). |

Status codes: `OK`(0), `ERR`(1), `BAD_CMD`(2), `BAD_ADDR`(3), `BAD_AUTH`(4), `NO_SPARE`(5).

Per-trial sequence:
```
SEED(0)                          // PTE → HpaA + flush
fetch ProbeVa                    // install (ProbeVa → HpaA) in L1 iTLB
RDTSC; SWAP(1); RDTSC            // PTE → HpaB, no flush — measure swap cycles
for i in 0..N: call(scratch + i*0x1000)       // L1 iTLB pressure
for j in 0..M: v = *(volatile uint8_t*)ProbeVa
fetched = ((uint8_t(*)())ProbeVa)()           // 'A'=iTLB held, 'B'=evicted
loaded  = *((uint64_t*)(ProbeVa + 0x100))     // 0x4141…=HpaA, 0x4242…=HpaB
RESTORE
```

Page layout (4 KB): `[0..2]` = `B0 sentinel C3`; `[0x100..0x107]` = 8-byte sentinel; all other bytes = `0xC3`. Sentinel = `0x41` (HpaA) / `0x42` (HpaB).

Lifecycle markers: H10 (ARM ok/GPA), H11 (arm-fail), H12 (decoy-alloc fail), H13 (rate-limited swap), H14 (RESTORE ok/OrigPte), H15 (DISARM ok/total-swaps), H16 (state violation), H17 (PTE canary mismatch).

### CR3_INTERCEPT (0x36)

Arms `Cr3PassiveSample`. Full mechanics in [CLAUDE.md § CR3 Recovery](CLAUDE.md).

- `Arg1`: Target PID (0 = disarm)
- `Arg2`: PEB VA (0 = blind: Phase 1 EPROCESS scan first)
- Returns: `COVERT_STATUS_OK`

Poll `PMC_CMD_GET_INTERCEPT_PEB` (0x37) every 50 ms (≤30 s). Non-zero = captured PEB; pull `TargetCr3` via `PROC_CR3` (0x34).

### Back-Pressure

Wait for batch complete (trigger write returns after single-step) before submitting next. Mailbox is shared — new commands before previous batch completes overwrite results.

### Diag Markers

- `VC0 <gpa>` — covert channel armed. First successful `NPT_CHANNEL_INIT` of the boot.
- `VC1 <gpa>` — covert channel torn down (auth fail, CR3 invalidation, or client re-init).

---

## 4. PCI Config Space I/O Interception

IOPM intercepts PCI config ports 0xCF8-0xCFF. Why: prevent guest enumeration of DMA FPGA devices.

```c
// HypeVmcb.c — byte 0x19F covers ports 0xCF8-0xCFF
Iopm[0x19F] = 0xFF;
```

`HandleIoio` (HypeVmexit.c):
- **CF8 writes**: shadowed into `Vcpu->LastCf8Value`
- **CFC-CFF reads**: if `g_HiddenPciBdf != 0` and CF8-addressed BDF matches, return 0xFF
- **All other I/O**: passed through transparently
- `g_HiddenPciBdf` format: `(Bus << 8) | (Dev << 3) | Func`; 0 = disabled

---

## 5. Init Sequence

```
0. VMMCALL HANDSHAKE                     — get per-boot auth key
1. VMMCALL SET_EPROCESS(0)               — auto-discover System EPROCESS
2. Allocate + VirtualLock trigger + mailbox pages
3. VMMCALL NPT_CHANNEL_INIT(trigger_va, mailbox_va)
4. FreeVmmcallStub()
5. NPT batch: PROC_CR3(target_pid)
6. NPT batch: VIRT_READ* for PEB walk
7. NPT batch reads for per-frame data    — sole runtime path
```

Total init: 3 VMEXITs. All ongoing I/O: 1 VMEXIT per batch.

---

## 6. Status Codes

| Value | Name | Meaning |
|-------|------|---------|
| `0x00` | OK | Success |
| `0x01` | ERR | Generic error |
| `0x02` | BAD_CMD | Unknown command ID |
| `0x03` | BAD_ADDR | Protected address / translation failed |
| `0x04` | BAD_AUTH | Auth key mismatch |
| `0x05` | NO_SPARE | Resource pool exhausted |

HYPE_MEM_* (VMMCALL returns): `0x00` OK, `0x01` ERR_UNMAPPED, `0x02` ERR_RANGE, `0x05` ERR_NOT_FOUND.

---

## 7. Important Notes

**Thread affinity:** All VCPU state is per-CPU. Pin overlay thread to CPU 0:
```c
SetThreadAffinityMask(GetCurrentThread(), 1);
```

**VirtualLock:** Both trigger and mailbox pages must stay pinned. Paged-out GPAs stored by NPT_CHANNEL_INIT point to wrong physical memory.

**Stale GPA re-validation:** Each NPT-fault trigger re-translates the mailbox VA to verify GPA hasn't changed (working set trim, COW, VirtualUnlock). If changed, channel torn down. Cost: 4 page-walk reads (~20-60ns) per trigger.

**ReadGuestVirtual page split:** Reads crossing a 4KB page boundary are split — each half may map to a different physical page.

**Performance budget:**

| Operation | VMEXITs | Approx. Time |
|-----------|---------|-------------|
| Full init (3× VMMCALL) | 3 | ~10µs |
| NPT-fault batch (≤80 cmds) | 1 | ~1µs + cmd processing |
| PCI config I/O (per port) | 1 | ~0.5µs |
| CPUID passthrough | 0 | native |
| **Per-frame total** | **~1-3** | **~5-10µs** |

**Security:** Trigger/mailbox pages are in overlay process userspace. NPT-fault channel validates CR3 — only the registered process triggers command processing. `NptIsProtectedAddress()` on all reads/writes prevents accessing HV memory regions.

---

## 8. Win11 Kernel Offsets

See [CLAUDE.md § Windows Kernel Offsets](CLAUDE.md). Used by `SET_EPROCESS` auto-discover and `PROC_CR3` EPROCESS walk.

---

## 9. Accepted Tradeoffs

| Issue | Rationale |
|-------|-----------|
| TSC drift (monotonic-negative) | No better reference; negative bias is fail-safe |
| Cross-VCPU TlbControl races | TLB staleness bounded by hardware; flush on every VMRUN too expensive |
| BSP-only deferred image protection | BSP launches last; small window is UEFI-only (no AC running) |

**Sorted Range Capacity:** `NptFinalizeProtections()` allocates `Count + 64` sorted range entries. Computes `MaxProtectedEnd` for binary-search fast-path.

**Spare Page Pool:** `NPT_SPARE_PT_PAGES` = 16. Supports 16 runtime 2MB→4KB splits.

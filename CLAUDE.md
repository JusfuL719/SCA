# SCA — HV architecture

**Owns:** HV architecture (VCPU/VMCB/NPT/intercepts/Phase 3 #SX/MSR shadowing), HV-side CR3 capture (`Cr3PassiveSample`), CPUID evasion, VCPU_DATA layout. **Defers to:** [Communication.md](Communication.md), [LOG_DECODER.txt](LOG_DECODER.txt), [../CLAUDE.md](../CLAUDE.md).

Type-0 AMD SVM hypervisor delivered as a UEFI DXE_RUNTIME_DRIVER. Virtualizes BSP+APs at DXE entry, boots Windows through ExitBootServices as guest with GPA == HPA. Target: Ryzen 9 5950X. PC1 has SMT off → 16 logical CPUs (code is correct for 32-thread configs). Version: `DRIVER_VERSION_MAJOR=1`, `DRIVER_VERSION_MINOR=0` (HypeUefi.h).

## Build

EDK2, GCC + NASM + Python 3. Package `SCAPkg.dsc`, INF `PlatformInit.inf`, output `PlatformInit.efi`.

```bash
cd /srv/nfs/shared/Shared/Tools/EDK2
PYTHON_COMMAND=python3 . ./edksetup.sh BaseTools
export PATH="$PWD/BaseTools/BinWrappers/PosixLike:$PATH"
export PACKAGES_PATH="$PWD:/srv/nfs/shared/Shared/SCA"
build -p SCAPkg.dsc -a X64 -t GCC -b DEBUG
```

In-tree build — `SCA/` IS the package (reachable via `PACKAGES_PATH`), no rsync into `Tools/EDK2/`. Wrapped by `tools/build_sca_hv.sh` (RELEASE).
Output: `Tools/EDK2/Build/SCAPkg/DEBUG_GCC/X64/PlatformInit.efi`.

GCC: `-mno-red-zone -fno-stack-protector -mabi=ms -mno-sse -mno-sse2 -msoft-float -fno-lto`. NASM: `-f elf64`. No EBS hook — virtualizes at entry, stays resident through EBS.

## Boot Sequence (tandasat-model)

1. UEFI Shell loads `PlatformInit.efi`.
2. `HypeLoaderEntry` → `HypeInit`: per-boot auth keys, scrub TPM log, install SecureBoot spoof, alloc VCPU structures, build NPT identity map, init host IDT+TSS, enable SVM all CPUs (EFER.SVME + VM_HSAVE_PA + `VM_CR.R_INIT=1`), install LAPIC NPT shadow, virtualize APs in parallel via `StartupAllAPs`, then BSP.
3. Driver returns `EFI_SUCCESS`; firmware continues to EBS with all CPUs already guests.
4. Windows INIT-SIPI-SIPI redirects: `R_INIT=1` converts INIT → `#SX`. BSP's ICR write (LAPIC 0x300 / WRMSR 0x830) faults into HV, state machine flips target `ActivityState` → `WaitForSipi`/`SipiIssued`. Target's `#SX` handler spins on state, then resumes real mode at `SipiVector << 12`.
5. After `gRemainingSipiCount = (nActive-1)*2` hits zero, LAPIC NPT shadow disarmed (PTE restored, TLB broadcast).

## File Roles

| File | Role |
|------|------|
| HypeEntry.c | UEFI entry, auth keys, TPM scrub, SecureBoot spoof |
| HypeCore.c | VCPU alloc, XSAVE, MP Services enum, SvmEnableOnCpu, HypeInit orchestration |
| HypeVmcb.c | VMCB layout, intercept config, MSRPM/IOPM, canaries |
| HypeVmexit.c | VMEXIT dispatch: VMMCALL, MSR, NPT batch, #SX, LAPIC ICR/MMIO, #DB, I/O, SHUTDOWN |
| HypeNpt.c | NPT identity map (2MB), protection regions, runtime split, LAPIC shadow |
| HypeVmrun.nasm | VMRUN loop, GPR save/restore, XSAVE/XRSTOR, debug ring, per-CPU virtualize entry, devirt path |
| HypeIsr.nasm | Host ISR stubs (0/2/6/8/13/14), 32-entry exception table |
| HypeIdt.c | Host IDT, TSS, IST1 double-fault stack |
| HypeDebug.c | 16KB log buffer (HypeDrain.efi reads it post-reboot), atomic append |
| HypeSvm.h | VCPU_DATA (~1104 bytes), MSR defines, command IDs, covert channel structs |
| HypeUefi.h | UEFI↔Windows type mappings, GCC/MSVC intrinsic shims, alloc helpers |
| HypeContext.h | DRIVER_CONTEXT & UEFI_HV_CONTEXT |
| HypeMemory.h | ReadGuest{Physical,Virtual}, TranslateGuestVirtual inlines |
| HypeNpt.h | NPT constants (HOST_PT_MAX, NPT_SPARE_PT_PAGES), protection range structs |
| HypeIdt.h, HypeVmcb.h, HypeDebug.h | Host IDT/TSS, VMCB setup decls, log macros |

## Communication Channels

Wire protocol in [Communication.md](Communication.md).

**Channel 1 — VMMCALL Bootstrap** (init only, 1 cmd/VMEXIT): HANDSHAKE (0x60), SET_EPROCESS (0x0A), NPT_CHANNEL_INIT (0x50). Auth via `gBootAuthKey` in RBX; wrong key → `#UD`. Client frees stub immediately after NPT_CHANNEL_INIT.

**Channel 2 — NPT-Fault Covert Channel** (sole runtime path, ≤80 cmds/VMEXIT): client writes to read-only trigger page → NPF → HV processes COVERT_MAILBOX batch → single-step past write → #DB restores trigger. Lifecycle markers (one-shot, silent in steady state): `VC0` armed, `VC1` torn down.

## Key Data Structures

- **VCPU_DATA** (~1104 bytes, `HypeSvm.h`): VMCB/HostSaveArea ptrs, guest GPRs at +0x30, XSAVE state, NPT PML4, comm state, debug ring (last 8 exits).
- **VMCB** (4096 bytes): Control 0x000–0x3FF (intercepts, IOPM/MSRPM PAs, TscOffset, exit codes), Save 0x400–0xFFF. Canaries at 0x2E0 / 0x6F0.
- **COVERT_MAILBOX** (3856 bytes): AuthKey + NumCommands(1-80) + Sequence + COVERT_CMD[80] (48B each: CmdId, Status, Arg1-3, Result).

## Intercepts

- **Misc1:** MSR, SHUTDOWN. IOIO conditionally added when `g_HiddenPciBdf != 0`.
- **Misc2:** VMRUN, VMMCALL, VMLOAD, VMSAVE, STGI, CLGI, SKINIT.
- **Exceptions:** #SX (bit 30, catches INIT redirected by `R_INIT=1`); #DB (bit 1) armed transiently by covert-channel path only.
- **NpEnable:** 3 (NPT + GMET exec-trap).
- **MSR (MSRPM):** EFER 0xC0000080 r/w (read=SVME=0 shadow, write strips SVME), VM_CR 0xC0010114 r/w (faked SVMDIS+LOCK), VM_HSAVE_PA 0xC0010117 r/w, APIC_BASE 0x1B w (x2APIC promotion), x2APIC ICR 0x830 w (Phase-3 only — cleared by `DisableLapicIntercept` once `gRemainingSipiCount==0`).
- **APERF/MPERF (0xE7/0xE8) NOT intercepted.** Why: trips `KiVerifyProcessorFeatures` → 0x3E BSOD (per-CPU VMEXIT overhead → heterogeneous-cores tell).
- **NMI NOT intercepted.** Vector-2 host stub kept as bare `iretq` to absorb stray host-side NMIs between VMEXIT and VMRUN re-entry.
- **RDPMC NOT intercepted.** Why: faking constant zero #GPs at CPL>0 (CPL/CR4.PCE must be enforced natively).
- **INVD NOT intercepted.** Native behavior is correct; prior handler ran `wbinvd` regardless of CPL.
- **CPUID intercept DISABLED.** Re-enabling without writing the handler hits `default:` → `cli; hlt`.
- **INVLPGA removed** — Windows never executes it.
- **CR3 write intercept removed** — replaced by `Cr3PassiveSample`. Why: arming `InterceptCrWrite` freezes PC1.

## NPT

Identity map: GPA == HPA, 2MB pages, up to 1TB. HV memory marked PRESENT=0. Runtime 2MB→4KB splits use pre-allocated spare pool (16 pages). Sorted range array, O(log n) `NptIsProtectedAddress()`, fast-path via `MaxProtectedEnd`.

## Authentication

Per-boot keys from RDSEED/RDRAND via SplitMix64 (`MixKey`). `HYPE_BUILD_SECRET = 0x7A3F9B2E5D1C8064` in HypeSvm.h — must match client. HANDSHAKE derives `gBootAuthKey`; all subsequent ops require it. VMCB integrity canary (`gBootCanary`) checked every VMEXIT at +0x2E0 and +0x6F0.

## Fingerprint mitigation — CPUID + MSR + TSC

1. Leaf 1 ECX[31] (hypervisor present) — force-cleared in passthrough
2. Leaves 0x40000000-0x400000FF (HV interface) — return zeros
3. Leaf 0x8000000A (SVM features) — native passthrough matches locked-out state
4. Leaf 0 / 0x80000000 (max leaf counts) — native passthrough, no virtual leaves
5. EFER.SVME (MSR 0xC0000080 bit 12) — shadowed to 0 on reads
6. VM_CR (MSR 0xC0010114) — faked SVMDIS=1, LOCK=1
7. VM_HSAVE_PA (MSR 0xC0010117) — returns 0
8. CPUID — not intercepted, native passthrough (zero VMEXIT cost, no timing artifact)
9. TSC — native passthrough; `Vmcb->Control.TscOffset` stays 0. Why: per-VCPU offset divergence is a heterogeneous-cores tell, and thread-migration TSC rewind drove WDDM TDR. Trade-off: RDTSC pairs straddling an intercepted RDMSR/IO leak ~1500 cy.

Other: TPM log scrubbed, SecureBoot spoofed, HV pages PRESENT=0 in NPT.

## Windows Kernel Offsets (Win11 25H2 / build 26200+)

| Offset | Field | Struct |
|--------|-------|--------|
| 0x028 | DirectoryTableBase | KPROCESS |
| 0x158 | UserDirectoryTableBase (KVAS) | KPROCESS |
| 0x1D0 | UniqueProcessId | EPROCESS |
| 0x1D8 | ActiveProcessLinks | EPROCESS |
| 0x2B0 | SectionBaseAddress | EPROCESS |
| 0x2D0 | InheritedFromUniqueProcessId | EPROCESS |
| 0x2E0 | Peb | EPROCESS |
| 0x338 | ImageFileName | EPROCESS |
| 0x370 | ThreadListHead | EPROCESS |

0x280 is CloneRoot, NOT UserDirectoryTableBase. KPROCESS is at EPROCESS+0x0.

## Key Constants

| Define | Value | Location |
|--------|-------|----------|
| HV_ALLOC_MAX_ADDRESS | 0x3FFFFFFF (1GB) | HypeUefi.h |
| HOST_STACK_SIZE | 32KB | HypeSvm.h |
| MSRPM_SIZE / IOPM_SIZE | 8KB / 12KB | HypeSvm.h |
| NPT_MAP_LIMIT | 1TB | HypeNpt.c |
| NPT_SPARE_PT_PAGES | 16 | HypeNpt.h |
| COVERT_MAX_BATCH | 80 | HypeSvm.h |

## Debug

- 16KB log buffer stamped with `gBootLogMagic`. Drained post-reboot by `HypeDrain.efi` to the SMB share.
- Per-CPU debug ring: last 8 exit codes + RIPs at VCPU offsets 0x170–0x1F0.
- VMCB canary checked every VMEXIT.
- Strings stripped in release — `LOG_DECODER.txt` is the code→string dictionary.
- **Always read with `strings -n 3 hype_log.raw`** — default `min=4` drops 3-char markers (E47, E54, E63, E66, C63-68).

## CR3 Recovery — `Cr3PassiveSample` chunked scanner

Defeats AC's EPROCESS-DTB poisoning + ActiveProcessLinks unlink. AC overwrites `KPROCESS.DirectoryTableBase` (0x028) and `UserDirectoryTableBase` (0x158) with a partial-copy decoy PML4 that maps PEB/Ldr/ntdll but zeroes target .data/.bss, and unlinks the EPROCESS from PsActiveProcessLinks. AC stopped rotating CR3 ~June 2025 — once captured, valid for the session.

Command: `PMC_CMD_CR3_INTERCEPT` (0x36). Arg1 = PID (0=disarm). Arg2 = PEB VA (0=blind, runs Phase 1 EPROCESS scan first).

Flow: client arms → HV sets `gCr3SampleArmed = StartPhase` (4 if PEB supplied, 1 if blind) → client polls `PMC_CMD_GET_INTERCEPT_PEB` (0x37) at 50ms → each poll runs one chunk (`CR3_SCAN_CHUNK_BYTES = 4 MB`, sized to keep one chunk under a Win11 clock tick).

Phases:
1. `Cr3ScanEprocChunk` — find EPROCESS where `ImageFileName` (+0x338) is `r5apex.exe`/`r5apex_dx12.exe`; extract `SectionBaseAddress` (+0x2B0) and `Peb` (+0x2E0). Skipped when client supplies PEB.
2/4. `Cr3ScanPml4PebChunk` — for each candidate PML4 PA: `PML4[idx(PebVa)]` PRESENT → walk to PEB.Ldr (usermode) → walk to PEB.ImageBase → MZ at ImageBase → `e_lfanew` ∈ [0x40, 0xF00] → `"PE\0\0"` at ImageBase+e_lfanew. Same-page reads (no extra TranslateGuestVirtual).

PML4[0] strict-mask (`& 0xFFFF000000000FFF == 0x867`) removed — bits 57+59 are software-ignored / version-dependent on Windows and rejected the real CR3.

On capture: HV broadcasts `TargetCr3` + `Cr3InterceptCapturedPeb` to every VCPU and sets phase 3 (idle). Client picks up via `PMC_CMD_PROC_CR3`.

Log codes: V20 (Phase 1 ImageBase), V21 (Phase 1 not found), V23 (Phase 2/4 not found), V24 (Phase 1 PEB), V25 (Phase 1 PID), V26 (Phase 4 ImageBase), V72 (captured CR3), V73 (PEB).

## Phase 3 — INIT/SIPI via #SX + LAPIC NPT shadow

- `VM_CR.R_INIT=1` set in `SvmEnableOnCpu` on every CPU → INIT becomes trappable `#SX` (APM §15.30.1). `VM_CR.LOCK` is 0 on 5950X during DXE; readback verified (`V80` before, `V81` after).
- VMCB `InterceptExceptions` bit 30 = `#SX`. `HandleSx` rewrites VMCB save to APM §15.14.1 real-mode reset.
- Per-VCPU `ActivityState ∈ {Active, WaitForSipi, SipiIssued}` + `SipiVector` (VCPU+0x390+). BSP's ICR write flips target's state; AP's `#SX` spins on CAS until `SipiIssued`, applies SIPI vector, re-enters guest at `(vec<<12):0`.
- `gRemainingSipiCount = (nActive-1)*2`. At 0 → LAPIC shadow disarmed (`DisableLapicIntercept` → PTE restored + TlbControl=3 broadcast + `Msrpm[0x020C] &= ~0x02` + `VmcbClean &= ~VMCB_CLEAN_IOMSRPM`); `V89` marks final SIPI, pairs with `V93 intercept off`.

### LAPIC NPT shadow — write-protect only (NOT PRESENT=0)

PTE is `PRESENT=1, WRITE=0, NX=1`. Only writes NPF. Why not PRESENT=0: HAL timer calibration reads LAPIC TIMER_CURRENT_COUNT (0x390) and APIC-ID (0x20) in tight loops — every read would NPF. Only writes that matter for AP bringup go to ICR (0x300) — `HandleLapicIcrWrite`. Other writes pass through via `PassThroughLapicMmio`. `gLapicInterceptArmed = 1` means write-protected.

### MSR detail

- **EFER (0xC0000080) r+w.** Reads return SVME=0. Writes validate LMA/LME, strip SVME.
- **x2APIC ICR (0x830) w.** MSRPM byte 0x20C bit 1. Emulates INIT/SIPI (decodes mode/dest from IcrLow + IcrHigh=Value>>32), updates target ActivityState, delivers real IPI. Cleared by `DisableLapicIntercept` once `gRemainingSipiCount==0`.
- **APIC_BASE (0x1B) w.** Logs `V85` on x2APIC promotion. If LAPIC shadow still armed at promotion, inject `#GP`. Once counter=0 / shadow disarmed, promotion passes through.

### HandleSx hardening

- **Spurious-INIT gate (`SipiApplied` at VCPU+0x39B).** If set, log `V9A` and return without reset. Why: WHEA/debugger INIT can re-fire after AP is active; replaying real-mode reset would crash the guest.
- **EventInj cleared on entry.** Why: replaying long-mode EventInj into 4-byte real-mode IDT triple-faults.
- **Bounded SIPI spin.** ~64M `CpuPause` iters, `V8A` heartbeat every ~1M. Timeout: `V90 ApicId`, break with `SipiVector=0` → AP runs CS:0:RIP:0 → `VMEXIT_SHUTDOWN`.

### SHUTDOWN dispatch

- **AP SHUTDOWN:** `cli; hlt`. Other CPUs keep running.
- **BSP SHUTDOWN:** `ShouldExit=1` + break → devirt path through VMRUN loop.
- `V9B` logs `CpuNumber` of triple-fault.

### Heartbeat

- `VB0`: one-shot per CPU on first VMEXIT — proves VMRUN returns.
- `VB1`: rate indicator, BSP-only every 0x400 exits via `Vcpu->VmexitCount`.

## Conventions

- All allocs ≤1GB (`HV_ALLOC_MAX_ADDRESS`) — firmware identity-map limit.
- No boot-services calls after EBS — only pre-allocated memory.
- R15 = VCPU pointer through VMRUN loop (persistent across VMEXITs).
- Hot-path memory ops are `static inline` in HypeMemory.h.
- VMCB clean bits set after modifying VMCB fields.
- Per-CPU state is per-VCPU — client must pin thread affinity.
- VirtualLock trigger+mailbox pages — paging out makes GPAs stale.

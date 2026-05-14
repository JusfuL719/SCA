# SCA Feature Audit
**Date:** 2026-05-14  
**Scope:** Full stack — HV core, VMEXIT/intercepts, NPT/CR3/#SX/LAPIC, comms/auth/installer, HookDraw/Menu/AimTrigger/Apex canon.  
**Source base:** `/srv/nfs/shared/Shared/SCA` as of commit-equivalent state after decouple-from-Pex_In plan completion.

---

## Executive Summary (top-10 by operational risk)

| # | Finding | Risk | File | Line |
|---|---------|------|------|------|
| 1 | Payload tick runs full 128-entity scan on NPF VMEXIT path — no async offload | P0 | HypeHookDraw.c | ~300 |
| 2 | DoAim applies 0.35 exponential smoothing and writes guest view angles (ADS only) | P0 | HypeAimTrigger.c | 425–446 |
| 3 | `BulkPhysRead` (cmd 0x41) exists in installer but has no HV dispatch handler | P0 | installer/covert.h | 160 |
| 4 | `m_flVisible` offset is `0x1A64` in HypeApexOffsets.h; canon in CLAUDE.md implies `0x1A74` | P1 | HypeApexOffsets.h | 17 |
| 5 | `NPT_SPARE_PT_PAGES` is 64 in code, CLAUDE.md says 16 | P1 | HypeNpt.h | 78 |
| 6 | `gRemainingSipiCount` initialized to `NumCpus-1`, CLAUDE.md says `(nActive-1)*2` | P1 | HypeCore.c | 922 |
| 7 | CR3 poll interval is `Sleep(5)` (5 ms) vs documented 50 ms | P1 | installer/main.cpp | 293 |
| 8 | Early-boot VE1/VE2 trace logs first 32 VMEXIT codes+RIPs (BSP, logging window at startup) | P2 | HypeVmexit.c | 2148–2155 |
| 9 | HID=78 write is unconditional per tick — no skip when entity already glow-78 | P2 | HypeHookDraw.c | 7 |
| 10 | `PEXEPEXE` magic (`0x5045584550455845`) hard-coded in scratch_layout.h — scannable sentinel | P2 | installer/scratch_layout.h | — |

---

## 1. VMEXIT / Intercept Coverage Matrix

### 1.1 Intercepts Active

| Intercept | Mechanism | Operational claim | Verdict |
|-----------|-----------|---------------|---------|
| EFER r+w | MSRPM byte 0x0820 bits 0+1 | Read shadows SVME=0; write strips SVME | **confirmed** — read returns `OriginalEfer`, write clears EFER_SVME |
| VM_CR r+w | MSRPM byte 0x1045 bits 0+1 | Fake SVMDIS=1, LOCK=1, R_INIT cleared | **confirmed** — read: `(native & ~R_INIT) \| SVMDIS \| LOCK`; write: silently dropped |
| VM_HSAVE_PA r+w | MSRPM byte 0x1045 bits 6+7 | Returns 0 | **confirmed** — read returns 0, write dropped |
| APIC_BASE w | MSRPM byte 0x0006 bit 7 | x2APIC promotion gate + #GP if LAPIC shadow armed | **confirmed** — V85 logged, #GP injected if `gLapicInterceptArmed` (HypeVmexit.c:258–265) |
| x2APIC ICR (0x830) w | MSRPM byte 0x020C bit 1 | Phase-3 only; cleared by DisableLapicIntercept | **confirmed** — HypeVmcb.c:80; DisableLapicIntercept clears bit |
| TSC_DEADLINE (0x6E0) w | MSRPM byte 0x01B8 bit 1 | Cap at ~20ms to bound heartbeat ceiling | **confirmed** — HypeVmexit.c:270–295 |
| x2APIC EOI (0x80B) w | MSRPM byte 0x0202 bit 7 | Logged, passed through native | **confirmed** — HypeVmexit.c:294–310 |
| #SX (exc bit 30) | VMCB InterceptExceptions | INIT→#SX redirect via VM_CR.R_INIT=1 | **confirmed** — HypeVmcb.c:125 |
| #DB (exc bit 1) | Transient — armed per NPF trigger | Covert channel single-step restore | **confirmed** — armed/cleared in HandleNpf/HandleDb (HypeVmexit.c:2230–2264) |
| SHUTDOWN | InterceptMisc1 bit | BSP devirt; AP cli/hlt | **confirmed** — HypeVmexit.c:2314–2330 |
| MSR (all) | InterceptMisc1 bit | Per MSRPM above | **confirmed** |
| VMRUN | InterceptMisc2 bit | #UD | **confirmed** — HypeVmexit.c:2285 |
| VMMCALL | InterceptMisc2 bit | Bootstrap channel | **confirmed** |
| VMLOAD/VMSAVE/STGI/CLGI/SKINIT | InterceptMisc2 bits | #UD | **confirmed** — HypeVmexit.c:2295–2301 |
| NPF | NpEnable=3 | Identity map, covert trigger, LAPIC shadow, hook exec trap | **confirmed** |
| IOIO | InterceptMisc1 — conditional | Only when `g_HiddenPciBdf != 0` | **confirmed** — HypeVmcb.c:116–118 |

### 1.2 Intercepts Intentionally Absent

| Intercept | Reason (CLAUDE.md) | Code evidence |
|-----------|-------------------|---------------|
| CPUID | Zero VMEXIT cost / no timing artifact | No CPUID intercept bit set — confirmed |
| RDPMC | Faking at CPL>0 requires PCE enforcement | Not in MSRPM, no RDPMC handler |
| INVD | Native behavior correct | Not intercepted |
| MOV CR3 | Freezes PC1 (documented deadlock — CLAUDE.md) | InterceptCrWrite absent — confirmed |
| RDTSC / RDTSCP | Heterogeneous-cores tell if per-VCPU offset diverges | TscOffset=0 (HypeVmcb.c), no RDTSC intercept |
| NMI | Host stub is bare iretq | HypeIsr.nasm NMI stub (ISR vector 2) — not intercepted in VMCB |

### 1.3 VMEXIT Dispatch Default Path

```
default:
    HvLogHex("V42"...) × 4
    while(1) { cli; hlt; }   // HypeVmexit.c:2336–2342
```
Any unhandled VMEXIT code hard-hangs the CPU. Correct defensive choice; no silent resume.

### 1.4 Claim verification against CLAUDE.md

1. Leaf 1 ECX[31] hypervisor bit — CPUID not intercepted; native passthrough cannot clear this if VMX-bit set... **actually confirmed safe** on AMD SVM: CPUID leaf 1 ECX[31] is the hypervisor-present bit, not set by AMD SVM hardware. Native passthrough returns 0 here unless Windows sets it.
2. Leaves 0x40000000–0x400000FF — not intercepted, native returns zeros. **confirmed**.
3. Leaf 0x8000000A — native passthrough; SVMDIS+LOCK fake via VM_CR read. **confirmed**.
4. EFER.SVME read = 0. **confirmed** — `OriginalEfer` never has EFER_SVME set.
5. VM_CR fake SVMDIS=1, LOCK=1. **confirmed** — read returns `(native & ~BIT1) | BIT3 | BIT4`.
6. VM_HSAVE_PA returns 0. **confirmed**.
7. CPUID passthrough (no VMEXIT). **confirmed**.
8. TSC native passthrough, TscOffset=0. **confirmed** — HypeVmrun.nasm has no TSC compensation.

### 1.5 LBR Virt Enabled

`VmcbInitialize` enables LBR virtualization if `SvmCheckLbrvSupport()` returns true (HypeVmcb.c:147–149). No audit concern but not documented in CLAUDE.md.

---

## 2. NPT / CR3 Passive Sampler / #SX / LAPIC

### 2.1 NPT Identity Map

- **Build:** 2MB pages, PML4/PDPT/PD 3-level walk, up to `NPT_MAP_LIMIT=1TB` (actually min of `1<<40` and CPUID max). HypeNpt.c:90–247.
- **HV pages:** `NptProtectOwnPages` sets `PRESENT=0` on HV EFI allocation ranges. Confirmed via `NptIsProtectedAddress` O(log n) sorted-range lookup.
- **Protection ranges:** Sorted `SortedRanges[]` with `MaxProtectedEnd` fast-path — confirmed in HypeNpt.c.
- **Spare pool:** `NPT_SPARE_PT_PAGES = 64` (HypeNpt.h:78). **DRIFT: CLAUDE.md states 16.** Actual pool is 64; runtime splits will succeed further before exhaustion. Not a bug — a relaxed constraint.

### 2.2 LAPIC NPT Shadow

- PTE is `PRESENT=1, WRITE=0, NX=1` — only writes fault (NPF).
- ICR_LOW (offset 0x300) writes go to `HandleLapicIcrWrite`; all other writes and all reads go to `PassThroughLapicMmio`.
- **Read stall concern confirmed zero:** PTE is PRESENT=1, so reads never NPF. HAL timer calibration loops on TIMER_CURRENT_COUNT and APIC-ID without HV involvement.
- **ICR read path:** If `gLapicInterceptArmed` and a read is decoded on ICR_LOW offset, `HandleLapicIcrWrite` is called (write-only path). It calls `DecodeLapicMmioAccess` which checks `IsWrite`; if not write, falls to `#UD` injection (HypeVmexit.c:1234–1238). This means an ICR_LOW **read** while armed injects `#UD`. This is a latent defect — Windows does not read ICR_LOW during AP bring-up so it is harmless in practice, but any ICR read while armed misbehaves.

### 2.3 LAPIC Teardown

`DisableLapicIntercept` triggered at `gRemainingSipiCount == 0`:
- PTE restore: NPT_WRITE bit set back on LAPIC page — confirmed.
- TlbControl=3 broadcast — confirmed via `FlushAllTlb`.
- MSRPM bit 0x020C bit 1 cleared (x2APIC ICR 0x830 intercept off) — confirmed HypeVmcb.c:80.
- VmcbClean updated — confirmed via `~VMCB_CLEAN_IOMSRPM` clear.
- V89 + V93 log codes — confirmed.

### 2.4 gRemainingSipiCount Initialization

HypeCore.c:921–925:
```c
if (Hv->NumCpus > 1)
    gRemainingSipiCount = (INT32)(Hv->NumCpus - 1);
else
    gRemainingSipiCount = 0;
```
**DRIFT: CLAUDE.md says `(nActive-1)*2`.** Code uses `NumCpus - 1` (one decrement per STARTUP, not two). On a 16-CPU machine (PC1 with SMT off): counter = 15, expects 15 STARTUPs (one per AP). CLAUDE.md's `(nActive-1)*2` would be 30 (two per AP: INIT + STARTUP). The actual Windows AP bring-up sends one STARTUP per AP after INIT; the INIT path calls `StampInit` (no decrement) and only `StampStartup` decrements. The code is functionally correct for one-STARTUP-per-AP. **Update CLAUDE.md to reflect `NumCpus - 1`.**

### 2.5 CR3 Passive Sampler

- `CR3_SCAN_CHUNK_BYTES = 4MB` — **confirmed** (HypeVmexit.c:34, comment explains 4MB = ~356µs << 1ms tick).
- **Phase 1** (`Cr3ScanEprocChunk`): Scans physical pages for `r5apex_dx12.exe`/`r5apex.exe` at EPROCESS+0x338. Extracts ImageBase (+0x2B0), PEB (+0x2E0), PID (+0x1D0), DTB (+0x028). Confirmed.
- **Phase 2/4** (`Cr3ScanPml4Chunk`): For each candidate PML4 PA, walks PML4→PEB→Ldr→ImageBase, checks MZ+PE signatures. Confirmed.
- **PML4[0] strict mask removed:** `Cr3ScanPml4Chunk` does NOT apply `0x0A00000000000867` mask. Confirmed — the check was removed per CLAUDE.md guidance.
- **On capture:** Broadcasts `TargetCr3` + `Cr3InterceptCapturedPeb` + `Cr3InterceptCapturedImageBase` to all VCPUs — confirmed HypeVmexit.c:646–653. Sets `gCr3SampleArmed = 3` (idle phase).
- **Timeout:** If `Cr3ScanPml4Chunk` returns `EFI_NOT_FOUND` (exhausted all pages), sets armed=3 and logs V23. Client will see phase=3 but result=0 — no PEB returned.

### 2.6 #SX / INIT-SIPI State Machine

- VM_CR.R_INIT=1 confirmed in `SvmEnableOnCpu` (HypeCore.c), readback V80/V81 logged.
- `InterceptExceptions |= (1U << EXCEPTION_SX)` — confirmed HypeVmcb.c:125.
- Per-VCPU ActivityState: `GUEST_ACTIVE → GUEST_WFS → GUEST_SIPI_ISSUED` — transitions via `StampInit` / `StampStartup`.
- Spurious-INIT gate (`SipiApplied` at VCPU+0x39B): confirmed — HypeVmexit.c HandleSx checks `SipiApplied`, logs V9A, returns without reset.
- EventInj cleared on HandleSx entry — confirmed.
- Bounded SIPI spin — confirmed with V8A heartbeat and V90 timeout.

---

## 3. Comms / Auth / Installer Protocol Contract

### 3.1 VMMCALL Bootstrap Channel

| Cmd | ID | Auth check | Implemented |
|-----|----|-----------|-------------|
| HANDSHAKE | 0x60 | `GuestRbx == gHandshakeExpected` | yes — returns `gBootAuthKey` in RAX, advances RIP+3 |
| SET_EPROCESS | 0x0A | `GuestRbx == gBootAuthKey` | yes — KPCR→thread→EPROCESS walk or direct Arg1 |
| NPT_CHANNEL_INIT | 0x50 | `GuestRbx == gBootAuthKey` | yes — maps trigger/mailbox, arms write-protect PTE |
| default | — | — | `#UD` injected |

Bootstrap is single-cmd-per-VMEXIT by construction. VMMCALL stub freed after `NPT_CHANNEL_INIT` in installer (main.cpp comment + `vmmcall::Free()` call) — confirmed. HV still dispatches `VMEXIT_VMMCALL` forever; only the installer stub is freed. Bootstrap commands remain callable by any code holding the key.

### 3.2 Covert NPT Channel

- Write-protect trigger (NPT_WRITE clear), NPF→batch dispatch, TF set for #DB→PTE restore. Confirmed.
- `COVERT_MAILBOX` layout: 8B AuthKey + 4B NumCommands + 4B Sequence + 80×48B cmds = 3856B. `static_assert(sizeof(Cmd)==48)` in installer/covert.h:39. **confirmed**.
- `COVERT_MAX_BATCH=80` enforced with clamp (not hard reject) at HypeVmexit.c:1430.
- Sequence replay defense: `Sequence <= LastCovertSequence` → same single-step path, no dispatch. Confirmed HypeVmexit.c:1412.
- AuthKey checked per batch before dispatch. Confirmed HypeVmexit.c:1393.
- VirtualLock on trigger + mailbox pages (installer/covert.h:57–58). Confirmed.
- Lifecycle: VC0 on arm, VC1 on teardown, VC2 logs mailbox GPA on arm.

### 3.3 Command Parity Table

| PMC_CMD | ID | HV handled | Installer issues | Notes |
|---------|----|-----------|-----------------|-------|
| PING | 0x00 | yes | yes | Response = `0x48595045 ^ (gBootAuthKey>>32)` |
| SET_CR3 | 0x02 | yes | yes | Arg1=0 → use KernelCr3 |
| SET_EPROCESS | 0x0A | yes (VMMCALL) | yes | Bootstrap only |
| HOOK_INSTALL_DRAW | 0x1B | yes | yes | Arms NPT NX exec trap |
| HOOK_DRAW_PEEK | 0x1C | yes | yes | Returns mirrored entity cache |
| SET_GLOW_PARAMS | 0x1D | yes | yes | Updates gGlowParams |
| HV_LOG_READ | 0x1E | yes | yes | 8B per call |
| VIRT_CALL_INIT | 0x1F | yes | yes | One-shot trampoline setup |
| VIRT_CALL | 0x20 | yes | yes | Must be last in batch |
| SET_AIM_PARAMS | 0x21 | yes | yes | |
| SET_MENU_ENABLE | 0x22 | yes | yes | |
| VIRT_READ4 | 0x30 | yes | **no** | HV-only orphan — installer never issues 0x30 |
| VIRT_READ8 | 0x31 | yes | yes | |
| VIRT_WRITE4 | 0x32 | yes | yes | |
| VIRT_WRITE8 | 0x33 | yes | yes | |
| PROC_CR3 | 0x34 | yes | yes | Returns cached TargetCr3 |
| VIRT_WRITE1 | 0x35 | yes | yes | HIGHLIGHT_ID byte writes |
| CR3_INTERCEPT | 0x36 | yes | yes | Arm CR3 sampler |
| GET_INTERCEPT_PEB | 0x37 | yes | yes | Poll sampler phase |
| GET_LAPIC_DISARMED_NPF_COUNT | 0x38 | yes | yes | Diag only |
| NPT_CHANNEL_INIT | 0x50 | yes (VMMCALL) | yes | Bootstrap only |
| HANDSHAKE | 0x60 | yes (VMMCALL) | yes | Bootstrap only |
| **BulkPhysRead** | **0x41** | **NO** | **yes** | **P0 — will hit `default → BAD_CMD`** |

### 3.4 Auth / Secret Consistency

- `HYPE_BUILD_SECRET = 0x7A3F9B2E5D1C8064` — matches between HypeSvm.h:47 and installer/auth.h:10 (`kBuildSecretEncoded ^ kBuildKey`). **confirmed**.
- `volatile` XOR in `auth::BuildSecret()` prevents MSVC constant-fold. **confirmed** (installer/auth.h:17–18).
- `PEXEPEXE` magic (`0x5045584550455845`) in `scratch_layout.h` — survives in built binary as a fixed sentinel. A userland scan of the sca-svc.exe process VA space would find this literal at offset 0 of the scratch/HookDraw buffer header.

### 3.5 Notes — Comms

- **VMMCALL always-active:** After bootstrap, VMEXIT_VMMCALL still dispatches. Unknown RCX → `#UD`. Minimal surface — normal guest stack rarely issues VMMCALL.
- **Mailbox AuthKey in user RW pages:** The 8-byte `auth_key` is readable by any process with ReadProcessMemory on sca-svc. Hostile introspection could observe this.
- **Trigger page write:** The write that fires the NPF is a single byte write of value 1 to a VirtualLock'd page. Minimal observable profile.

---

## 4. HookDraw / Menu / AimTrigger / Apex Canon

### 4.1 Hook Installation

- **Mechanism:** NPT NX exec-trap via `DrawHookArmExecTrap` — sets NPT_NX on the 4KB page containing the render-gate VA. On NPF exec-fault, `DrawHookOnNpfHit` fires. HypeHookDraw.c:51–105.
- **Rearm heartbeat:** `DrawHookRearm` re-sets NPT_NX every ~228ms from VmexitHandler tick path. Confirmed in file header comment.
- **Installer prologue check:** sca-svc verifies `APEX_RENDER_GATE_FN_PROLOGUE_LE64` before issuing `PMC_CMD_HOOK_INSTALL_DRAW` — guards against hook install on wrong build.

### 4.2 Per-Frame Payload Work (P0 Hot-Path Concern)

`DrawHookOnNpfHit` runs entirely on the NPF VMEXIT path:
1. Clears NPT_NX (quick).
2. Calls `DrawHookPayloadTick` which iterates **all 128 entity slots** — 8B name probe + origin + health/shield/team/highlight reads per slot.
3. Calls `DoAim` + `DoTrigger` (additional reads + optional writes).
4. Bone matrix reads (studio_hdr parse) for aim targets.

No async queue, no deferred execution. A single payload tick on 128 populated slots involves hundreds of `TranslateGuestVirtual` + `ReadGuestPhysical` calls on the VMEXIT path. The existing slow-VMEXIT detector (threshold 200K cycles ≈ 60µs at 3.4 GHz) at HypeVmexit.c:2355 will trip regularly if gEntCache is fully populated with alive players. **This is the primary timing/stability risk.**

### 4.3 Glow Implementation

- Default `gGlowParams.Slot = 78`, Mask = 0x01, Enabled = 1. HypeHookDraw.c:40–43.
- `RenderGlow` writes `HIGHLIGHT_ID = 78` via `VIRT_WRITE1` (1 byte) for every qualifying player every tick. **No skip when HID is already 78** — file header comment explicitly states "unconditional below." Per CLAUDE.md, engine already sets HID=78 for enemies; the HV write is redundant but not harmful. It does increase write pressure on the entity struct every frame.
- `WriteEntityGlow` uses `WriteGuestVirtual(..., 1)` at `APEX_ENT_HIGHLIGHT_ID`. Confirmed byte-width write.
- `InitHighlightSlot` writes `APEX_FN_BITS_VIS = 0xC7407889` into bucket slot 78. CLAUDE.md documents observed engine default as `0x47407887`. The HV intentionally overrides to full-view VIS bits — this is not drift but confirmed behavior. `APEX_BUCKET_FNBITS_SLOT78_OBS = 0x47407887` is documented in HypeApexCanon.h for reference.

### 4.4 AimTrigger — DoAim mode (P0 risk)

`DoAim` is gated on `gAimTriggerParams.AimEnabled && gLocal.IsZooming`. When active:
1. Computes view→target angle delta.
2. Applies **exponential smoothing** at factor `sm = 0x3EB33333 = 0.35f` (HypeAimTrigger.c:425).
3. Writes `new_p, new_y` to `gLocal.Ent + APEX_ENT_VIEW_ANGLES` as two float32s — **moves the guest view**.

DoAim applies exponential smoothing (0.35) to view-angle writes when enabled. Operator policy defers extra input-shaping layers. **Operator awareness required:** DoAim is disabled at boot (`AimEnabled = 0`); it must be explicitly armed via `--aim-aim-en 1`. View-angle writes from HV context may disagree with peripheral input deltas if validated by ring-3 code.

### 4.5 DoTrigger Mode (Low Risk)

- ADS + on-target → writes `IN_ATTACK kbutton+0x8 = 5` (press), then `= 4` (release) next tick. No view angle writes. Narrow write surface (button state only).
- Gate: `!gLocal.AttackPressed` — does not fight operator. Confirmed HypeAimTrigger.c:480.
- State machine drains (PRESSING→COOLDOWN) even when menu is open — prevents stranded IN_ATTACK=5. Confirmed.

### 4.6 Velocity Derivation

`ScanOneEntity` computes `Velocity = (Origin - PrevOrigin) / dt` from gEntCache origin delta. **Does not use `APEX_ENT_VELOCITY_RAW` (offset 0x170).** Confirmed — matches documented offset policy.

### 4.7 Punch Peak-Hold

`ReadLocalPlayer` reads `m_vecPunchWeapon_Angle` at `OFFSET_AIMPUNCH = 0x2528`. Peak-hold with `RECOIL_PEAK_DECAY_TICKS` decay. DoAim and DoTrigger use `gLocal.PunchPeak`. Confirmed.

### 4.8 Menu

- Rows: GlowEnabled, FilterMode, GlowSlot (0–89), GlowMask, VisType, GlowFix, WriteVisType, WriteGlowFix, AimEnabled, AimFovQ4_4, TrigEnabled, TrigFovQ4_4, TrigThreshQ4_4, TrigDebounce.
- **No skeleton/body canvas.** Confirmed.
- **No HV rendering surface.** Menu writes config only; no D3D draw calls.
- **Input:** IN_USE triple-tap (open); DUCK/JUMP/SPEED/FORWARD for navigation. kbutton reads at `APEX_OFF_IN_*+0x8`. No XInput/gamepad.

### 4.9 Apex Offsets vs Canon

| Field | HypeApexOffsets.h | CLAUDE.md canon | Status |
|-------|-------------------|-----------------|--------|
| m_vecPunchWeapon_Angle | 0x2528 | 0x2528 | match |
| HIGHLIGHT_ID | not defined (in HypeApexCanon.h only) | 0x298 | not in offsets file |
| model name | 0x479 | entity+0x479 | match |
| m_flVisible | 0x1A64 | comment says `0x1A54` in offsets file itself; CLAUDE.md implies `0x1A74` | **DRIFT** — three values in play |
| OFFSET_BULLET_SPEED | 0xDA8 | HypeApexCanon.h has 0x28C8/0x28D0 (weapon-local offsets) | different coordinate systems; not a conflict |
| Weapon=9, Legendary=16, Mythic=43, Epic=49, Rare=56, Ammo=60, Common=68 | ClassifyLootByItemId match | match | confirmed |
| BUCKET_COUNT=89 | GlowSlot menu 0–89 | 89 | match |

### 4.10 String / Signature Exposure

- Automated string scan over `Hype*.c/h` for sensational literals — **zero hits** in identifiers/comments.
- Menu item strings (`"GlowEnabled"`, `"FilterMode"`, etc.) exist as char array literals in HypeMenu.c. These are in HV EFI binary, not in user memory. Release build strips these (CLAUDE.md: "Strings stripped in release").
- `"r5apex_dx12.exe"` / `"r5apex.exe"` are stored as byte arrays (not string literals) in HypeVmexit.c:384–387 to avoid the 4-char minimum for `strings` tool.

---

## 5. Doc-vs-Code Drift Register

| Item | CLAUDE.md claims | Actual code | Severity |
|------|-----------------|-------------|----------|
| NPT_SPARE_PT_PAGES | 16 | 64 (HypeNpt.h:78) | P1 — update docs |
| gRemainingSipiCount init | `(nActive-1)*2` | `NumCpus - 1` (HypeCore.c:922) | P1 — update docs |
| CR3 poll interval | 50 ms | `Sleep(5)` = 5ms (installer/main.cpp:293) | P1 — update docs or code |
| NPT_MAP_LIMIT | 1TB | `1<<40` = 1TB — matches | ok |
| HOST_PT_MAX | 64GB | `64ULL<<30` = 64GB — matches | ok |
| COVERT_MAX_BATCH | 80 | 80 — matches | ok |
| HYPE_BUILD_SECRET | 0x7A3F9B2E5D1C8064 | matches both sides | ok |
| DoAim input shaping | defer extra layering (policy) | 0.35 smooth step in DoAim | operator awareness |
| HID=78 write | "largely redundant" | unconditional write every tick | low risk |
| x2APIC ICR read while LAPIC shadow armed | not documented | #UD injected | latent defect |

---

## 6. Confirmed Clean Surfaces

The following surfaces were explicitly checked and found correct:

- **EFER.SVME shadow** — read returns 0, write strips before VMCB commit.
- **VM_CR fake** — SVMDIS=1, LOCK=1 returned on every read.
- **TSC offset** — zero; no per-VCPU drift.
- **CPUID passthrough** — no intercept, no VMEXIT overhead.
- **MOV CR3 intercept absent** — InterceptCrWrite never set.
- **No heavy work at VMEXIT head** — EPROCESS scan in command handler only; per CLAUDE.md compliance.
- **strobf-equivalent in installer** — `volatile` XOR in `auth::BuildSecret()` defeats MSVC constant-fold.
- **VMCB canary** — checked every 64 exits (VK0–VK9 on corruption).
- **VIRT_WRITE1 for HIGHLIGHT_ID** — confirmed byte-width write.
- **No skeleton/body canvas** — menu is config-only.
- **Velocity from origin delta** — not from m_vecAbsVelocity at 0x170.
- **Legacy rebrand residue** — zero `Pex_In|wininit-svc|HypePkg` hits in SCA tree.
- **PEXEPEXE namespace** — scratch_layout.h uses `sca_scratch` namespace.

---

## 7. PC1 Operator Verification Checklist

Run after each HV boot before Apex session. Read log with `strings -n 3 hype_log.raw`.

### 7.1 HV Bring-up Validation

```
Look for:
  N01 <pml4_pa>       — NPT PML4 allocated
  N02 <page_count>    — identity map page count
  NSP <spare_count>   — spare PT pages (expect 64)
  C59                 — NPT verify passed (zero errors)
  V89 <sipi_count>    — expect 15 (16-CPU, SMT off)
  V80 <vm_cr_before>  — VM_CR before R_INIT set
  V81 <vm_cr_after>   — bit 1 (R_INIT) should be set
  VB0 (×16)           — first VMEXIT per CPU (proves VMRUN returns)
```

Failure indicators: `C57` (VMCB PA still in NPT), `C58` (VcpuTable unprotected), `V84 0xFA11` (LAPIC shadow install failed).

### 7.2 LAPIC Shadow / Phase-3 INIT-SIPI

```
After Windows INIT-SIPI-SIPI sequence:
  V88 <icr>           — INIT IPI dispatched (×15 expected)
  V8C <icr>           — STARTUP IPI dispatched (×15 expected)
  V89 0               — final STARTUP, counter hit zero
  V93                 — LAPIC intercept disarmed

Failure indicators:
  V9A <apic_id>       — spurious INIT after SipiApplied (harmless but notable)
  V90 <apic_id>       — AP SIPI spin timeout (AP will SHUTDOWN → V9B)
  V9B <cpu_num>       — CPU triple-faulted (SHUTDOWN)
```

If `M0F` appears after bring-up is complete, the LAPIC PTE was not fully restored.

### 7.3 Covert Channel Init

```
  VC0 <trigger_gpa>   — channel armed
  VC2 <mailbox_gpa>   — mailbox GPA logged

Tear-down (sca-svc exit):
  VC1 <trigger_gpa>   — channel disarmed
```

### 7.4 CR3 Capture (PMC_CMD_CR3_INTERCEPT)

```
Phase 1 (blind EPROCESS scan):
  V25 <pid>           — found Apex PID
  V24 <peb_va>        — PEB extracted
  V20 <image_base>    — Apex ImageBase from SectionBaseAddress

Phase 4 (PML4 scan):
  V26 <image_base>    — ImageBase confirmed via PE walk
  V72 <cr3>           — CR3 captured
  V73 <peb_or_imgbase>— PEB/ImageBase broadcast to all VCPUs

Not-found codes:
  V21                 — Phase 1: Apex EPROCESS not found in scanned range
  V23 <img_base>      — Phase 2/4: no PML4 validates
```

Poll `GET_INTERCEPT_PEB` (0x37) at 5ms intervals until phase returns 3 (idle). `Arg2` = phase number in response.

### 7.5 Hook Install and First Payload

```
  DRH <npf_delta>     — draw hook NPF rate (non-zero = hook firing)
  DRF <gpa>           — hook GPA (confirm matches expected render gate VA)
  EA0 <ent>           — first view-angle write (DoAim only — indicates aim is enabled)
  ET0                 — first IN_ATTACK write (DoTrigger first fire)
```

If `DRH` is zero after hook install: hook GPA did not match any NPF → wrong render-gate VA or Apex updated.

### 7.6 Auth / Build Secret Mismatch

```
  HV side:  wrong HANDSHAKE → #UD in guest (installer crashes)
  Installer: "HANDSHAKE failed" printed to stdout
```
If handshake fails, verify `HYPE_BUILD_SECRET` in `HypeSvm.h` equals `kBuildSecretEncoded ^ kBuildKey` in `installer/auth.h`.

### 7.7 Slow VMEXIT Warning

```
  VAD <delta_tsc>     — VMEXIT took > 200K cycles (~60µs)
  VAE <cpu|exit_code> — which CPU, which VMEXIT
  VAL <covert_packed> — if NPF covert path was slow: NumCmds|Slot|CmdId
```

Frequent VAD on exit code NPF (0x400) indicates payload tick is too heavy. Reduce entity count or skip bone reads on distant targets.

---

## 8. Remediation Shortlist (Priority Order)

1. **[P0] Implement `BulkPhysRead` HV handler (cmd 0x41)** — currently returns `BAD_CMD`, silently broken feature. Add case in NPF covert dispatch switch.
2. **[P0] Evaluate moving payload tick off NPF hot path** — consider a mailbox-driven poll instead of exec-trap tick. Near-term mitigation: gate `TryGetHeadBonePos` behind a distance threshold to reduce per-tick read count.
3. **[P0] Document and gate DoAim** — view-angle writes carry the highest guest-integrity scrutiny. Ensure operators know it is disabled at boot and must be explicitly enabled. Consider making DoAim require a separate `--aim-write-angles` flag distinct from `--aim-aim-en`.
4. **[P1] Update CLAUDE.md: NPT_SPARE_PT_PAGES = 64** — not 16.
5. **[P1] Update CLAUDE.md: gRemainingSipiCount = NumCpus-1** — not `(nActive-1)*2`.
6. **[P1] Resolve CR3 poll interval** — `Sleep(5)` is 5ms, not 50ms. Either update docs to say 5ms, or change to `Sleep(50)` if the documented cadence was deliberate for load reasons.
7. **[P1] Fix ICR_LOW read #UD** — `HandleLapicIcrWrite` injects `#UD` on reads while LAPIC shadow is armed. Add read passthrough path in `HandleLapicIcrWrite` or redirect reads to `PassThroughLapicMmio`.
8. **[P1] Fix m_flVisible offset ambiguity** — three values in play (0x1A54 in comment, 0x1A64 in code, 0x1A74 in CLAUDE.md). Re-verify against current build and pin one value.
9. **[P2] Remove/obfuscate PEXEPEXE scratch magic** — fixed sentinel in sca-svc VA is fingerprinted. Derive it from session key or remove the static magic entirely.
10. **[P2] Add HID=78 skip when entity already has correct HID** — minor write pressure reduction; confirms idempotency.

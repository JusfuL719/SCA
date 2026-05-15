# Hyperjacking detection surface — 2026 state of the art

**Scope:** what catches a custom AMD SVM type-0 hypervisor (operator: SCA on Ryzen 9 5950X)
in 2026. Primary anti-cheat target: **Apex Legends EAC** (operator's current
surface). Forward-looking secondary: **EA Javelin** (EA Sports rollout — the
direction EAC is being replaced toward, with shared bucket taxonomy).

This is a refinement intel doc. SCA already ships R_INIT-based #SX, NPT
identity-map, MSR shadowing for `EFER`/`VM_CR`/`VM_HSAVE_PA`, native CPUID
passthrough, native TSC passthrough, `Cr3PassiveSample` (passive CR3 capture
defeating AC's EPROCESS-DTB poisoning), and intercept-minimization. The intel
below is filtered through that lens.

Date of write: 2026-05-14.

---

## 1. Executive summary — what's actually new in 2026

- **Anti-cheat is publicly admitting it's moving down to ring -1.** The
  University of Birmingham talk at **Black Hat USA 2025** (`Watching the
  Watchers`, Collins/Muench/Chothia, video posted 2026-02-26) explicitly
  predicts the next battleground is hypervisor-vs-hypervisor and quotes anti-
  cheat developers saying *"if [Windows] start requiring VBS to be on, we will
  leverage those features that protect Windows for us."* That changes the
  game's threat model from "AC can't see ring -1" to "AC will park itself at
  VTL-1 and audit ring -1 from above."
- **Javelin's anti-HV taxonomy is now public.** Spontaneous (UC, 28-29 Apr
  2026) leaked five distinct `AntiHV::*` buckets: `CPUIDBackdoor`,
  `CPUIDBrand`, `InvalidHyperV`, `Emulation`, `InvalidInstructionExecution`.
  This is the first time the bucket names are visible — it tells us *what
  Javelin/EAC look for*, not just *that they look*. EAC for Apex still
  ships the older surface (and is due for the EOS migration), so these are
  forward-looking targets that will hit the operator on the day Apex rolls to
  Javelin/EOS-equivalent HV detection.
- **No IPI-cross-core jitter check confirmed in any current build.** Spontaneous
  searched launcher / GSL.dll / KM dumps for `KeIpiGenericCall`,
  `HalSendSoftwareInterrupt`, `KiIpiSend*`, "RDTSC on CPU0 → IPI to CPU1 →
  RDTSC compare" patterns — none found in classified Griffin handlers (~93% of
  ~27k handlers classified across the launcher; GSL.dll alone has 9,937 with
  the same coverage gap and Themida-stolen code). Best public evidence to
  date that **per-VCPU TSC-offset divergence is not yet the high-priority risk
  it would be on a more mature anti-HV build.** Confirms the operator's
  decision to run `Vmcb->Control.TscOffset = 0`.
- **CPUID timing — confirmed primary path.** Javelin scanner has a **~20ms
  wall-clock guard around its memmove operation, RDTSC-paired at the
  epilogue around launcher RVA `0xE3Fxxx`** (Spontaneous). Trip the threshold
  → game crashes (not a ban — used for prioritization). EAAC reads
  `RDTSCP` (not just RDTSC). On AMD, this is a strong argument for **not**
  intercepting CPUID — `papst` and `Hattie8491` (UC #737154, Feb 2026): "on
  amd you should actually disable cpuid vmexits to avoid timing attacks."
  SCA already does this — keep it that way.
- **EPT/NPT integrity probes — zero captured evidence either way.**
  Spontaneous: no `MmGetPhysicalAddress`, no manual PTE walks, no
  `INVPCID/INVEPT`-equivalent probing, no self-ref-PTE tricks observed in any
  KM dump. The closest thing is `ZwQueryVirtualMemory + AllocationProtect` —
  VAD-level userland, not page-table walking. **NPT identity-map artifact
  detection is not a public threat in 2026.** It's *plausible* (Themida-
  stolen code is invisible), but the empirical signal is "they aren't doing
  this yet."
- **The hard problem in 2026 is not HV detection — it's image integrity.**
  ILOVECHEATAS (UC #750523, 30 Apr 2026): "Hook on unrelated address =
  clean, hook on protected module = ban." The AC isn't catching the
  hypervisor; it's reading the protected module via kernel-mode primitives and
  noticing the executable view doesn't match what the loader produced. EPT
  split-view (`Read=Original / Execute=Fake`) loses on a long enough timeline
  because *the AC only has to catch one mismatch ever*. SCA's Phase 1 design
  (NPT identity-map, no executable hooks in r5apex.exe — only data reads via
  `Cr3PassiveSample` and external write paths) sidesteps this entire class of
  detection. **Don't add `.text` hooks in r5apex.exe** — that's the road to
  CVEAC-2020-style integrity bans.
- **EAC integrity check (CVEAC-2020 class) skips `.rdata` and writable
  sections.** Section-loop with `memcmp` of EAC.sys section copy vs live;
  `.rdata` and writable sections explicitly skipped (the function literally
  tests `*(ULONG*)psection->Name != 'adr.'`). Operator's HV doesn't patch
  EAC.sys or any kernel driver — it lives in firmware below NT — so this
  vector doesn't apply directly. Useful as confirmation that **EAC's
  integrity-scanning style is "pool-copy + memcmp", not "rolling-hash from a
  trusted enclave"** (yet).
- **AMD SinkClose (CVE-2023-31315) is patched on the 5950X.** August 2024
  firmware; if the operator is on a current AGESA, this is closed. Worth
  noting because SinkClose was the only public *practical* SVM-adjacent escape
  to ring -2 (SMM) — a stealth haven below SCA. Closed.
- **No EAC/Apex `.rdata` / `.text` page-content integrity check on the game
  module is publicly documented.** Source-engine self-integrity is dormant
  in current Apex builds (no public 2025/2026 UC post on it). Themida is on
  the game binary itself, not on EAC.
- **Public HVs caught one by one:** HyperDbg → instant HW-ban from EAAC
  (Spontaneous tested it personally, no cheats running, just HyperDbg loaded).
  HyperPlatform / hvpp / DdiMon / SentinelHV → still in the game-hacking
  ecosystem but well-fingerprinted. Tandasat's Barevisor / HelloAmdHvPkg /
  MiniVisorPkg are research-grade, not tested under live AC pressure. **None
  of the public HVs are "live-stealth" in 2026.**

---

## 2. EAC / EAAC anti-HV detection state, per bucket

EAC for Apex Legends is the operator's primary surface. The detail below
mixes **EAC** (current Apex) and **EAAC / Javelin** (where Apex/EAC is
heading via the EOS migration that started in 2024 and is the published
trajectory). Each bucket cites the source.

### 2.1 CPUID — five named detection categories

Source: Spontaneous, UC thread #750201 *"EA Javelin Anti-Cheat: Hypervisor
Detection Vectors?"*, post #10 (2026-04-29). All five are AntiHV namespaces
inside the Javelin blocklist:

| Bucket | What it catches | SCA status |
|---|---|---|
| `AntiHV::CPUIDBackdoor` | Named HV signatures at leaf `0x40000000` (vendor-string match against blocklist: `KVMKVMKVM\0\0\0`, `XenVMMXenVMM`, `VMwareVMware`, `prl hyperv `, `Microsoft Hv`, etc.) | **Defended.** SCA caches a bare-metal invalid-leaf response sampled before `EnableSvm` and replays it for any leaf in `0x40000000..0x4FFFFFFF`. The HV-reserved range looks identical to bare-metal "leaf-out-of-range" behavior with no per-leaf variation. |
| `AntiHV::CPUIDBrand` | Reserved-bit / inconsistent-value cross-leaf checks (e.g. AVX2 reported but XSAVE disabled, SMEP without `CR4.SMEP-able`, hypervisor-bit cleared but `MaxLeaf` value wrong, etc.) | **Defended by accident.** SCA's CPUID intercept is **disabled entirely** — guest CPUID runs natively, so all reserved bits and cross-leaf consistency are exactly what bare metal would produce. The only things SCA currently shadows are the EFER/VM_CR/VM_HSAVE_PA MSRs. |
| `AntiHV::InvalidHyperV` | Malformed / partial Hyper-V signature responses (e.g. leaf `0x40000000` claims Hyper-V but leaf `0x40000001` returns garbage) | **Defended by intercept-disabling.** Same as CPUIDBrand — SCA returns nothing in `0x4000000X` because CPUID is native passthrough. The bare-metal "leaf out of range" behavior is consistent across the whole range. |
| `AntiHV::Emulation` | Timing-anomaly bucket (overlap with #1) | **Partially defended.** SCA does not intercept CPUID, so the CPUID timing tell (`papst`'s "10× cycles" path on BattlEye, see §2.3) doesn't fire. RDMSR on `EFER` / `VM_CR` does cause a VMEXIT (those *are* intercepted), so an `RDTSC`-pair around `RDMSR 0xC0000080` would leak ~1500 cycles. **Open mitigation:** see §6.2. |
| `AntiHV::InvalidInstructionExecution` | Behavior inconsistency on `XSETBV` / `XGETBV` / `RDPMC` / `VMCALL`-from-ring-3 (Spontaneous's best-guess based on naming) | **Partially defended.** Operator's HV does not currently validate `XSETBV` reserved-XCR0-bit writes against a cached `ValidXcr0Mask`. SCA does not intercept `XSETBV` at all — guest writes go native, which is correct for legitimate `XCR0` values. Same for `RDPMC` (not intercepted). `VMCALL`-from-ring-3 is naturally a `#UD` on AMD because SCA only intercepts `VMMCALL`, not `VMCALL` (Intel). **Likely already clean.** |

**Citation:** [`https://www.unknowncheats.me/forum/anti-cheat-bypass/750201-ea-javelin-anti-cheat-hypervisor-detection-vectors.html`](https://www.unknowncheats.me/forum/anti-cheat-bypass/750201-ea-javelin-anti-cheat-hypervisor-detection-vectors.html), post #10, dated 29 Apr 2026.

### 2.2 Timing — RDTSC / RDTSCP paired-delta

Source: Spontaneous, same thread, post #10 (2026-04-29) and post #11
(2026-05-10).

- **Confirmed:** local RDTSC paired-delta only. ~20ms wall-clock guard
  around scanner's `memmove` operation, RDTSC-paired at epilogue around
  launcher RVA `0xE3Fxxx`. Trip the threshold → game crashes (not ban —
  prioritization).
- **EAAC reads `RDTSCP`** (not just RDTSC). On Intel that maps to VMCS
  secondary control `CPU_BASED2_ENABLE_RDTSCP`. On AMD/SVM the equivalent
  is `Vmcb->Control.InterceptInstruction1` bit `INTERCEPT_RDTSC` (bit 14)
  and `InterceptInstruction2` bit `INTERCEPT_RDTSCP` (bit 27). **SCA
  intercepts neither** — RDTSC and RDTSCP run native. Correct.
- **Not seen anywhere:** no `KeIpiGenericCall`, `HalSendSoftwareInterrupt`,
  `KiIpiSend*`, no "sample RDTSC on CPU0 → IPI to CPU1 → sample again →
  compare deltas" pattern in either static `.text` or classified Griffin
  handlers. Caveat: GSL.dll alone has 9,937 Griffin handlers, only ~93%
  classified, Themida-stolen code is invisible to static analysis. **An
  IPI-jitter check could be hiding there — empirically it has not tripped.**
- **Falsification recipe** (Spontaneous, paraphrased): an HV that fakes
  RDTSC on the trapping core but lets other cores see real TSC would expose
  it. SCA already lets all cores see real TSC (TscOffset=0), so it would
  read **identical TSC** across cores at any sampled instant — which is
  what bare metal does on a single TSC domain. Heterogeneous-core systems
  (P-cores vs E-cores on Intel 12th-gen+) drift; on Ryzen 5950X all 16
  cores are in one TSC domain, so this isn't a tell.

**BattlEye** (separate vendor, useful comparison) has documented CPUID-
sandwich timing per `secret.club` (2020): real hardware CPUID ~200 cycles,
HV-trapped CPUID ~10× longer. SCA disables CPUID interception entirely so
this fires native-fast.

**Citation:** [`https://secret.club/2020/01/12/battleye-hypervisor-detection.html`](https://secret.club/2020/01/12/battleye-hypervisor-detection.html), still relevant in 2026.

### 2.3 MSR consistency probing (`EFER.SVME`, `VM_CR.SVMDIS/LOCK`, `VM_HSAVE_PA`)

No 2026 UC post explicitly enumerates MSR-consistency probing for SVM
specifically (most cheat literature is Intel/VMX-centric). Status by
inference and SCA's own design choices:

- `EFER` (`0xC0000080`) — SCA shadows the SVME bit on read (always returns
  0). Native `RDMSR EFER` on bare metal returns SVME=0 too, **unless the
  user has booted into a Hyper-V-active guest** where Microsoft Hyper-V
  itself sets SVME. AC asking about SVME on bare metal expects 0; SCA
  returns 0; correct.
- `VM_CR` (`0xC0010114`) — SCA shadows to `SVMDIS=1, LOCK=1`. This is
  identical to a CPU where the BIOS has locked SVM off. AC has no signal
  here unless it correlates `VM_CR.SVMDIS=1` with `CPUID 0x80000001.ECX[2]
  (SVM)=1` (a contradiction on a real machine where SVM was locked at
  boot). **Open mitigation:** SCA could additionally mask `CPUID
  0x80000001.ECX[2]` to 0 if it intercepted leaf `0x80000001` — currently
  it does not, so the contradiction is technically there but not yet
  publicly weaponized.
- `VM_HSAVE_PA` (`0xC0010117`) — SCA shadows to 0. Bare-metal value would
  also be 0 (no HV ever wrote it). Correct.
- **APERF/MPERF (`0xE7`/`0xE8`) — SCA does NOT intercept** (per the
  handbook). This is correct: per Google/KVM patches `[v3] KVM: x86:
  Provide a capability to disable APERF/MPERF read intercepts` (lkml,
  Mar 2025), virtualizing these MSRs precisely is "challenging and
  impossible to implement with complete fidelity in multi-tenant
  environments." Native passthrough is the only stable answer. SCA's
  rationale (per CLAUDE.md) — that intercepting these would trip
  `KiVerifyProcessorFeatures` → 0x3E BSOD — is consistent with current
  vendor practice. **Don't add the intercept.**

### 2.4 EPT/NPT integrity probes

**Source:** Spontaneous, UC #750201 post #10 (2026-04-29):

> "Honest answer: zero captured evidence either way. The closest thing
> we've documented is `ZwQueryVirtualMemory` + `AllocationProtect` checks,
> but that's VAD-level userland inspection (looking for manually-mapped
> cheats with weird protection masks), not page-table walking. No
> `MmGetPhysicalAddress`, no manual PTE walks, no `INVPCID`/`INVEPT`-
> equivalent probing, no self-ref-PTE tricks observed in any KM dump."

This is huge for SCA. The NPT identity-map (GPA == HPA, 2MB pages, 1TB)
that SCA ships is **architecturally invisible to all currently-deployed
EAC/EAAC NPT-aware probes** because no probe is currently looking. Things
to *not* assume:

- AC could trivially add `MmGetPhysicalAddress` correlation against a known
  module's VA→PA map. The fact that none does today doesn't mean none
  will in 2027. Keep the identity-map; do *not* introduce GPA != HPA
  remappings unless required (each remap creates a fingerprint).
- `0AVX` (UC #750523, 30 Apr 2026) hinted at "novel detection methods for
  EPT" he wouldn't disclose publicly. He's MotM and credible. Treat as
  *unknown unknown* — the current `Cr3PassiveSample` + zero-write-into-
  guest model minimizes this risk.

### 2.5 Hyper-V / VBS / HVCI present?

EAAC and BattlEye both special-case Hyper-V — if `CPUID 0x40000000` returns
`Microsoft Hv` and the leaf chain looks legitimate (consistent
`HVMicrosoftHv` signature, valid `0x40000003` features, valid hypercall
page MSR `0x40000000`), they **disable hypervisor checks** because every
Win11 box with VBS/HVCI has Hyper-V underneath.

**Mevasik (UC #747401, 10 Apr 2026):** *"anti-cheats disable hypervisor
checks if they see a known vendor."*

**Open question for SCA:** the current strategy is to clear leaf 1 ECX
bit 31 and hide the HV completely (look like bare metal). That works on
the operator's HVCI-disabled PC1. On a Win11 25H2 box with VBS *enabled*,
the AC would expect `CPUID.1.ECX[31]=1` *and* `CPUID 0x40000000` to return
`Microsoft Hv` — and SCA returning bare-metal "no HV present" while the
guest *is* running under Hyper-V (because VBS is on) is itself suspicious
(cited as Priority-1 risk in the AI analysis quoted by DaRolex in UC
#750523). **The operator's PC1 has VBS off so this is not currently a
risk** — but if Apex/EAC begins requiring VBS (per the Black Hat 2025
quote), SCA will need a "Hyper-V passthrough" mode.

### 2.6 What EAC for Apex actually does *today*

Per the Black Hat USA 2025 talk and the Sentinel-framework comparison
(updated Mar 2026):

- **BattlEye** — actively detects HVs via CPUID-sandwich timing.
- **Vanguard** (Riot/Valorant) — actively detects HVs, uses CR3 swap to
  hide game memory from external probes; not Apex-relevant.
- **EAC for Apex** — focused on memory scanning, module enumeration,
  handle monitoring rather than HV-specific detection. The integrity
  check is a `memcmp` of EAC.sys sections (CVEAC-2020 class). This is
  the *current* surface the operator faces.

**The real risk window** is the EOS / Javelin migration. EAC has been
pushed under the EOS umbrella since 2022; Apex switched to EOS-EAC in
2024. The five-bucket `AntiHV::*` taxonomy from Javelin is the most
plausible direction for what Apex/EAC will look like in 12-24 months.

---

## 3. AMD SVM-specific tells

Most published anti-HV literature (BattlEye, Vanguard, Javelin reverse
engineering) is Intel/VMX-centric. SVM-specific signals that a 2026 AC
*could* check (and that SCA needs to handle correctly):

### 3.1 `CPUID 0x80000001.ECX[2]` (SVM) vs `VM_CR.SVMDIS`

If SVM is reported in CPUID but `VM_CR.SVMDIS=1` and `LOCK=1`, the only
explanation on bare metal is "BIOS locked SVM off." That's a normal,
common state on OEM laptops — but on a desktop Ryzen 5950X with SVM
visible to the user in BIOS and locked, it's an inconsistency. SCA today
does not mask `CPUID 0x80000001.ECX[2]` because it does not intercept
CPUID. **This is acceptable** — it matches the behavior of "user
explicitly enabled SVM in BIOS but BIOS later set the lock-disable for
some other reason," which exists in the wild. Don't fix what isn't
broken.

### 3.2 `R_INIT` redirection of INIT to `#SX`

SCA enables `VM_CR.R_INIT=1` to convert AP INIT IPIs into `#SX` traps for
the AP-bringup state machine. This is a *behavior* change visible from
the guest only via SIPI timing — and only during boot, when no AC is
loaded. **Not currently a detection vector.** Watch the Phase-3 log
codes (`V80`/`V81` before/after, `V89` final SIPI, `V93` intercept off)
to confirm it disarms cleanly post-boot.

### 3.3 GIF state

Global Interrupt Flag (GIF) is set on `STGI`, cleared on `CLGI`. Guest
can't see GIF directly, but it's observable indirectly via interrupt
delivery latency. SCA executes `STGI` immediately before VMRUN and lets
the guest observe interrupts normally; the only "invisible window" is
the few-instruction host span between VMEXIT save and `CLGI` (and
mirror on entry). **Not publicly documented as a check.**

### 3.4 VMCB canary / VM_HSAVE_PA inspection

SCA carries its own canaries in `VMCB+0x2E0` and `VMCB+0x6F0` for
integrity self-checks; these are host-side, not guest-visible. The
guest can read `VM_HSAVE_PA` via `RDMSR 0xC0010117` — SCA shadows this
to 0, which matches bare-metal-with-SVM-disabled behavior. Correct.

### 3.5 SKINIT intercept

SCA intercepts SKINIT (`InterceptInstruction2` bit 6). Guest issuing
SKINIT would VMEXIT to SCA — but Windows never executes SKINIT. **Not
a tell** unless an AC starts probing for it (none publicly does).

### 3.6 NPT vs EPT artifacts

NPT and EPT have different entry layouts (different bit positions for
NX, etc.) but the *guest* cannot directly read its own NPT entries.
The only way an AC could detect "NPT is in play" would be:

1. Compute physical address of a known guest VA via `MmGetPhysicalAddress`
   and check that it is in physical memory the AC trusts (legitimate
   RAM, not MMIO). SCA's identity-map means GPA == HPA, so this passes.
2. Time `INVLPG` vs `INVPCID` — but SCA doesn't intercept either, so
   they run native and no timing tell.

**Bottom line for Tier 1:** SVM-specific detection in 2026 is not
materially more dangerous than VMX-specific detection. SCA's current
architecture handles all publicly-documented SVM vectors.

### 3.7 `AperfMperf` / heterogeneous-cores tells

Per the Black Hat 2025 talk and KVM `[v3]` patch (Jan Mattson, Mar 2025):
APERF/MPERF cannot be virtualized with full fidelity in a multi-tenant
hypervisor. **For SCA on a single-tenant Ryzen 5950X** this is a non-
issue:

- All 16 cores are one TSC domain (no P/E-core split like Intel 12th-gen+).
- SCA's `Vmcb->Control.TscOffset = 0` means RDTSC across cores returns
  identical-domain values that match what bare metal would return.
- APERF/MPERF NOT intercepted means guest reads native counters per CPU,
  which is exactly what bare metal does.

**Confirmation:** SCA's "deliberately keeps `TscOffset = 0` to avoid this"
is still the correct call in 2026. Per-VCPU offset divergence on a
homogeneous Ryzen would create a heterogeneous-cores tell that doesn't
exist on the underlying hardware. Bigger reason it's still right:
thread-migration TSC rewind drove WDDM TDR in earlier SCA builds (per
CLAUDE.md). Native passthrough on a single TSC domain is the only stable
answer.

---

## 4. `.rdata` / `.text` page-content integrity scanning

### 4.1 EAC kernel-driver self-integrity

The CVEAC-2020 documented integrity check (Daax / secret.club, April
2020):

```cpp
// Reconstructed from EAC.sys reverse engineering
bool check_driver_integrity() {
    // Loop through all sections from EasyAntiCheat.sys
    for (WORD i = 0; i < num_sections; ++i) {
        const auto characteristics = psection_headers[i].Characteristics;
        // Ignore paged sections
        if (psection_headers[i].SizeOfRawData != 0 &&
            READABLE_NOT_PAGED_SECTION(characteristics)) {
            // Skip .rdata and writable sections
            if (!WRITABLE_SECTION(characteristics) &&
                (*reinterpret_cast<ULONG*>(psection_headers[i].Name)
                 != 'adr.')) {
                // memcmp(psection, psection_copy, raw_size)
            }
        }
    }
}
```

Key facts that survive into 2026 (no published 2025/2026 UC post claims
the algorithm changed):

- It is a **section-level memcmp**, not a SHA / page-hash — pool-copy of
  the EAC.sys image at load time, then periodic memcmp against live.
- It **explicitly skips `.rdata`** (the literal string check
  `psection_headers[i].Name != 'adr.'` — `adr.` is `.rda` reversed in
  little-endian) and writable sections.
- It targets EAC.sys itself, **not the game module** (`r5apex.exe` /
  `r5apex_dx12.exe`).

**Implication for SCA:** SCA does not patch EAC.sys (it doesn't patch
*any* kernel driver — the HV lives in firmware below NT). This entire
CVEAC class does not apply to SCA. The mitigation is "don't be a kernel
driver."

### 4.2 Apex / Source-engine self-integrity

Searches for "Apex Legends EAC .text .rdata page hash" / "Source engine
self-integrity 2025 2026" returned no concrete public evidence of
Apex-side `.text` hashing in current builds. The game binary itself is
Themida-packed (per BEDaisy.sys analysis by s4dbrd, Jan 2025 — same
protection family Respawn uses), which gives Themida-internal integrity
hash code; but Themida hashes the *packed* binary, not the unpacked
runtime view, so post-OEP a hypervisor that reads but doesn't write to
`r5apex.exe.text` does not get caught by Themida self-integrity.

**ILOVECHEATAS's empirical signal (UC #750523, 30 Apr 2026)** — that
EPT hooks on the protected module trigger bans while EPT hooks on
unrelated addresses don't — is **read-side detection**, not write-side
hashing. The AC reads suspect module pages via kernel primitives and
notices the executable view doesn't match the loader-produced bytes.
SCA, which does not hook executable pages anywhere in r5apex.exe (only
reads via `Cr3PassiveSample` and external write paths into known
mutable fields), avoids this entire class.

**Practical takeaway:** the integrity-scanning threat to SCA is not on
its own image (which lives in firmware where no AC reaches) and not on
the game image (which SCA doesn't modify). The remaining surface is the
*runtime overlay* (`HypeHookDraw` / `HypeRender`) that draws on the GPU
side — but that's a screen-overlay channel, not a page modification
channel, and is invisible to memcmp-class scans.

---

## 5. Public rogue-HV ecosystem in 2026

Status of the major public type-1/type-0 hypervisors as of 2026-05-14:

| Project | Arch | Last update | Live status under EAC/EAAC | Notes |
|---|---|---|---|---|
| **HyperDbg** | Intel VT-x | Active 2026 | **Hardware-banned** (Spontaneous, UC #750201, 28 Apr 2026 — "I have received a HW ban for using HyperDbg project (no cheats)") | Stealth not its goal; debug-focused. Designed to *resist* RDTSC timing, but the loader/driver footprint is well-fingerprinted. |
| **HyperPlatform** | Intel VT-x | Last update Nov 2023 | Fingerprinted | Tandasat. Inspired hvpp / DdiMon. Frequently caught by signature scans on its driver image. |
| **hvpp** | Intel VT-x | Inactive | Fingerprinted | Wbenny. Same fate as HyperPlatform — well-known signatures. |
| **DdiMon** | Intel VT-x | Active | Fingerprinted | Tandasat research tool. Not stealth-oriented. |
| **MoRE** | Intel VT-x | Inactive | Fingerprinted | Old proof-of-concept. |
| **SentinelHV** | Intel VT-x | Spontaneous's project, in active dev 2026 | Currently un-banned per author claims | Source not public. Reference design described in UC posts. Not directly applicable to AMD/SCA but the design choices (cached invalid-leaf CPUID, no IPI-jitter defense yet, RDTSCP intercept enabled) are visible to operator. |
| **AetherVisor** | AMD SVM | Last update 2022-ish | Fingerprinted | Original release got MotM, since fingerprinted. Used as base for many cheat HVs. |
| **AMD-SVM-Hypervisor-Base** | AMD SVM | UC #707970, late 2024 | Fingerprinted by signature | Educational only; was MotM, then signatures shipped. |
| **HelloAmdHvPkg** | AMD SVM (UEFI) | Last update Aug 2024 | Untested vs live AC | Tandasat. Research-grade UEFI hypervisor, type-1 (boots OS as guest). Closest published architectural cousin to SCA. Has not been pressure-tested vs Apex/EAC. |
| **Barevisor** | AMD SVM + Intel VT-x | Last update Sep 2025 | Untested vs live AC | Tandasat, written in Rust. UEFI + Windows driver build targets. Research-grade. |
| **MiniVisorPkg** | Intel VT-x (UEFI) | Last update Aug 2024 | Untested vs live AC | Tandasat. UEFI driver, not type-0. |
| **TitanHide** | Not a HV — handle/process hider | — | Detected | Off-topic but often confused. |
| **hyper-reV** | Hyper-V nested cheat | Active 2026 | Survives under VBS-on Hyper-V because it forwards CPUID to genuine Hyper-V and only consumes its own magic VMCALL key | Different threat model from SCA. Per the AI-quoted analysis in UC #750523, the *"discoverable hypercall portal"* (magic-key) is the biggest detectability risk; once removed/hidden, leaf-spoofing is unnecessary. |
| **Ophion** | Intel VT-x EPT-hook framework | Active 2026 | Catches bans when EPT hook lands on protected module | UC #750523 — same MotM author 0AVX who hinted at "novel EPT detection methods" in Javelin. |

**General takeaway:** **no public HV is "live-stealth"** vs current
EAC/Vanguard/Javelin. The only HV that survives is one the AC has never
seen — i.e. private, never-released, and run on a small fleet so it
never accumulates enough samples for a signature push. SCA is in this
category by virtue of being closed-source.

---

## 6. SCA-specific recommendations

Filtered for the operator's actual deployment (PC1, Ryzen 9 5950X, SMT
off, Win11 25H2 build 26200+, Apex Legends EAC, no VBS/HVCI on PC1,
single-tenant). Numbered by priority.

### 6.1 Keep doing — confirmed correct in 2026

1. **CPUID intercept disabled, native passthrough.** Per `papst` and
   `Hattie8491` (UC #737154 Feb 2026), per Spontaneous (UC #750201 Apr-
   May 2026), and per the AntiHV::Emulation timing-bucket analysis. Do
   not re-enable CPUID interception — every leaf you handle is a CPUID-
   sandwich timing tell. Operator's HypeVmexit.c already excludes
   CPUID. Lock that in.
2. **`Vmcb->Control.TscOffset = 0` and TSC native passthrough.**
   Confirmed in 2026: per-VCPU offset divergence is a heterogeneous-
   cores tell, exactly the falsification recipe Spontaneous proposes
   for catching SentinelHV-style HVs.
3. **APERF/MPERF (`0xE7`/`0xE8`) NOT intercepted.** Vendor consensus
   (KVM `[v3]` Mar 2025) says these can't be virtualized with fidelity.
   `KiVerifyProcessorFeatures → 0x3E` BSOD risk if intercepted,
   confirmed by SCA's own prior testing per CLAUDE.md.
4. **NPT identity-map (GPA == HPA, 2MB pages).** Spontaneous (Apr 2026)
   confirms no current EAAC/Javelin probes for NPT artifacts. Identity-
   map keeps the structure invariant so any future probe (e.g.
   `MmGetPhysicalAddress` correlation) sees expected values.
5. **Cached invalid-leaf CPUID replay for `0x40000000..0x4FFFFFFF`.**
   Defends against `AntiHV::CPUIDBackdoor` + `CPUIDBrand` +
   `InvalidHyperV`. SCA already does this implicitly by not intercepting
   (passthrough returns native invalid-leaf behavior). Verify in
   `HypeVmexit.c` that the CPUID intercept stays off in release builds.
6. **`Cr3PassiveSample` instead of `MOV CR3` interception.** Defeats
   AC's EPROCESS-DTB poisoning + ActiveProcessLinks unlink without
   tripping the freeze-PC1 path. Keep the chunked-scan model
   (`CR3_SCAN_CHUNK_BYTES = 4 MB`, sized under one Win11 clock tick) —
   any larger chunk would heartbeat-stall the BSP.
7. **SHUTDOWN dispatch — AP `cli; hlt`, BSP devirt.** This is the right
   model per APM and matches KVM's behavior post-CVE-2025-37957. Don't
   change to "force vCPU SMM exit on SHUTDOWN" (that CVE patch is for
   guest-induced TF in SMM; SCA doesn't expose SMM to guest at all).

### 6.2 Open mitigations worth adding

These are gaps in the current SCA build that map to documented 2026
detection risks. Listed in operator-priority order.

1. **`XSETBV` reserved-XCR0-bit gate (defends `AntiHV::Invalid-
   InstructionExecution`).** SCA does not currently intercept
   `XSETBV`. AC could write a reserved bit to XCR0, which on bare metal
   raises `#GP`; if SCA passes through naively the host XSAVE state
   could be polluted before the `#GP`. Bare-metal behavior IS to take
   `#GP`, so as long as SCA doesn't intercept (it doesn't), the guest
   gets the right behavior. **Verify by test:** boot SCA, run a VM-
   detect tool that does `XSETBV` with bits[63:62] set, confirm `#GP`
   is delivered to guest with no VMEXIT. If clean, drop this todo.
   Otherwise add an `INTERCEPT_XSETBV` handler that validates against a
   cached `ValidXcr0Mask` and either reflects `#GP` or passes the write.
2. **Suppress the RDMSR `EFER`/`VM_CR`/`VM_HSAVE_PA` timing tell.**
   These three MSRs are the ones SCA *does* intercept (because shadowing
   is required). An RDTSC-pair around `RDMSR 0xC0000080` will leak ~1500
   cycles. Mitigations:
   - **Option A (cheap):** since the intercept handler is short, the
     leak is small. Measure the actual delta on the operator's box and
     compare to bare-metal MSR-read delta. If <2-3× bare-metal,
     leave alone (within noise of SMI handling, page-table walks, etc.).
   - **Option B (work):** add `Vmcb->Control.TscOffset` adjustment in the
     `HandleMsr` handler to compensate for the VMEXIT round-trip. Risk:
     the offset compensation creates a per-VCPU TSC drift over time
     that Spontaneous's "fakes RDTSC on trapping core but not others"
     test would expose. **Don't do Option B.** Stick with Option A
     (verify first).
3. **VBS-passthrough mode (forward-looking, not for PC1 today).** If
   the operator ever moves SCA to a Win11 box with VBS enabled (or if
   Apex/EAC begins requiring VBS per the Black Hat 2025 forecast), SCA
   needs a mode where:
   - `CPUID 0x40000000` returns `Microsoft Hv` signature (forward to
     Hyper-V's actual response, like hyper-reV does).
   - `CPUID 0x40000003` returns Hyper-V feature flags consistent with
     the build.
   - `CPUID.1.ECX[31] = 1` (HV-present).
   This is not nested-virt — SCA stays underneath Hyper-V. It's
   "appear to be Hyper-V to the guest" mode. Architecturally similar to
   what hyper-reV does but type-0 instead of type-2.
4. **Add `Cr3PassiveSample` chunk-time monitoring.** Current chunk size
   `4 MB` is sized under one Win11 clock tick (15.625ms). On the
   operator's 5950X with SMT off and DDR4-3600, a 4MB linear scan is
   ~50µs — well under tick. But if SCA ever runs on slower memory or
   with SMT on, a chunk could approach the tick and cause clock drift
   the AC could measure. Add a perf-counter assertion that flags
   chunks > 5ms (`V` log code, similar to existing `V72`/`V73` flow).
5. **NPT spare-pool exhaustion guard.** `NPT_SPARE_PT_PAGES = 16` is
   tight. If runtime split exhausts the pool and `NptSplit*` returns
   error, the protection-region invariant breaks silently. Add a hard
   `cli; hlt` on exhaustion (better to wedge than to leak). Currently
   silent failure path is the worse option.

### 6.3 Things you can drop / stop worrying about

1. **Don't add IPI-cross-core jitter mitigation.** Spontaneous's
   Apr 2026 evidence: no IPI-jitter pattern seen in any current Javelin
   or EAAC build. Empty seat — don't waste cycles defending it.
2. **Don't add full Hyper-V leaf emulation just to hide HV-present.**
   On a VBS-disabled box (PC1's current state), the CPUID `0x40000000`
   leaf returns native invalid-leaf and there's no Hyper-V to spoof
   anyway. The risk is asymmetric in the other direction: faking
   `Microsoft Hv` while *not* providing functional hypercalls would be
   trivially caught by `HvCallGetSystemStatus` returning garbage.
3. **Don't implement VMCALL/VMCALL-from-ring-3 detection mitigation.**
   On AMD, `VMCALL` (Intel encoding `0F 01 C1`) is `#UD`. Native
   behavior. Don't intercept it.
4. **Don't write CPUID-vendor randomization or "look like VMware"
   schemes.** Spontaneous (UC #750201, 28 Apr 2026): "Mainstream vendor
   strings aren't safer than yours — the by-name list isn't where the
   action is, the heuristic AntiVM bucket is. A randomized or empty
   vendor string beats pretending to be VMware." SCA's current
   "return native invalid-leaf for the entire 0x4000_0000..0x4FFF_FFFF
   range" is exactly that: empty-string equivalent.
5. **Don't worry about SinkClose (CVE-2023-31315).** Patched on Ryzen
   5950X via August 2024 firmware. Confirm AGESA version on the board
   is post-AMD-SB-7008 (`agesa --version` from a UEFI shell, or check
   Asus/MSI BIOS release notes for the operator's specific board).

### 6.4 Defenses to harden if Apex/EAC migrates to Javelin

Forward-looking (12-24 months):

1. **`AntiHV::CPUIDBrand` cross-leaf consistency.** SCA's native CPUID
   passthrough means cross-leaf consistency is naturally bare-metal-
   correct. Risk: if SCA ever needs to spoof a leaf (e.g. mask out a
   feature for a workaround), every other leaf must remain consistent.
   Build a "no CPUID intercept ever" invariant into the test suite.
2. **`AntiHV::Emulation` (timing) on RDMSR EFER.** As noted in §6.2.2,
   measure first.
3. **`Cr3PassiveSample` Phase 4 PML4 walk timing.** Phase 4 chunked
   PML4 scan runs on every poll (50ms cadence). If Javelin/EAAC times
   `NtQueryInformationProcess` calls during cheat-startup window, the
   scan-induced RDMSR/INVLPG VMEXITs could leak. Currently SCA does
   not VMEXIT during the chunked scan (it's host-side, no MSR
   intercepts trigger), so this is theoretically clean. Verify.
4. **Image-integrity for `r5apex.exe.text`** — **don't add executable
   hooks here, ever.** ILOVECHEATAS's empirical "hook on protected
   module = ban" signal is the death-knell for any EPT-hook design.
   SCA's `Cr3PassiveSample` + read-only access pattern is the right
   answer.

---

## 7. Sources

All URLs fetched between 2026-05-13 and 2026-05-14.

### Anti-cheat-bypass forum (UC) — primary 2026 evidence

- [`https://www.unknowncheats.me/forum/anti-cheat-bypass/750201-ea-javelin-anti-cheat-hypervisor-detection-vectors.html`](https://www.unknowncheats.me/forum/anti-cheat-bypass/750201-ea-javelin-anti-cheat-hypervisor-detection-vectors.html) — *EA Javelin Anti-Cheat: Hypervisor Detection Vectors?* — Spontaneous, ILOVECHEATAS, Nehiki et al. Posts dated 28 Apr – 10 May 2026. **Primary source for `AntiHV::*` bucket taxonomy, RDTSC ~20ms wall-clock guard at launcher RVA `0xE3Fxxx`, no IPI-jitter pattern, no EPT integrity probes observed.**
- [`https://www.unknowncheats.me/forum/anti-cheat-bypass/750523-discussion-evading-eac-detections-ophion-based-ept-hooks-exec-fake-read-ori.html`](https://www.unknowncheats.me/forum/anti-cheat-bypass/750523-discussion-evading-eac-detections-ophion-based-ept-hooks-exec-fake-read-ori.html) — *Evading EAC/BE Detections on Ophion-based EPT Hooks* — ILOVECHEATAS, Spontaneous, 0AVX. Posts dated 30 Apr – 5 May 2026. **Primary source for "hook on protected module = ban" empirical signal, AC reads PFNs cheaply, image-backed-vs-private correlation. Also contains the AI-generated hyper-reV / Javelin analysis quoted in §6.2.3.**
- [`https://www.unknowncheats.me/forum/anti-cheat-bypass/737154-hv-detection-vectors-kernel-anticheats.html`](https://www.unknowncheats.me/forum/anti-cheat-bypass/737154-hv-detection-vectors-kernel-anticheats.html) — *HV detection vectors on kernel anticheats* — papst, Hattie8491, Mevasik. Posts dated Feb 2026. **Source for "on AMD disable CPUID vmexits", "you can always get caught by timings" sentiment.**
- [`https://www.unknowncheats.me/forum/anti-cheat-bypass/747401-hypervisor-future-detection.html`](https://www.unknowncheats.me/forum/anti-cheat-bypass/747401-hypervisor-future-detection.html) — *Hypervisor future detection* — derwildechoppa, Mevasik, ApexCV. Posts dated April 2026. **Source for "tlb profiling" comment and "anti-cheats disable hypervisor checks if they see a known vendor" claim.**
- [`https://www.unknowncheats.me/forum/anti-cheat-bypass/609412-bypass-easyanticheat-integrity-checks.html`](https://www.unknowncheats.me/forum/anti-cheat-bypass/609412-bypass-easyanticheat-integrity-checks.html) — *Bypass EasyAntiCheat integrity checks* — older thread (CVEAC-2020 era, still cited). **Source for EAC.sys section-loop integrity check pseudocode and `.rdata` skip behavior.**
- [`https://www.unknowncheats.me/forum/apex-legends/636386-soultions-eos-anti-cheat.html`](https://www.unknowncheats.me/forum/apex-legends/636386-soultions-eos-anti-cheat.html) — *Soultions to New EOS Anti-Cheat* — TwilightWolf et al. **Source for EOS/EAC integration in Apex (driver suppresses callbacks once loaded).**
- [`https://www.unknowncheats.me/forum/anti-cheat-bypass/575197-aethervisor-memory-hacking-library-powered-amd-svm-2.html`](https://www.unknowncheats.me/forum/anti-cheat-bypass/575197-aethervisor-memory-hacking-library-powered-amd-svm-2.html) — AetherVisor release thread.

### Conference talks & external research

- [`https://www.youtube.com/watch?v=lAW2mAl96KI`](https://www.youtube.com/watch?v=lAW2mAl96KI) — Black Hat USA 2025: *Watching the Watchers: Exploring and Testing Defenses of Anti-Cheat Systems* — Sam Collins, Marius Muench, Tom Chothia (University of Birmingham), video published 26 Feb 2026. **Source for "next battleground is hypervisor", AC dev quote on leveraging VBS, Vanguard CR3-swap memory hiding, BYOVD detection trends.** Slides at [`https://blackhat.com/us-25/briefings/schedule/?#watching-the-watchers-exploring-and-testing-defenses-of-anti-cheat-systems-46777`](https://blackhat.com/us-25/briefings/schedule/?#watching-the-watchers-exploring-and-testing-defenses-of-anti-cheat-systems-46777).
- [`https://secret.club/2020/01/12/battleye-hypervisor-detection.html`](https://secret.club/2020/01/12/battleye-hypervisor-detection.html) — *BattlEye hypervisor detection* — Daax. Still the canonical reference for CPUID-sandwich timing detection (~200 cycles bare-metal vs ~10× under HV).
- [`https://secret.club/2020/04/08/eac_integrity_check_bypass.html`](https://secret.club/2020/04/08/eac_integrity_check_bypass.html) — CVEAC-2020 — original disclosure of EAC kernel-driver integrity check.
- [`https://s4dbrd.github.io/posts/reversing-bedaisy/`](https://s4dbrd.github.io/posts/reversing-bedaisy/) — Adrian's analysis of BattlEye `BEDaisy.sys`, Jan 2025. Source for "VMProtect-with-custom-watermarks-stripped" packing identification.

### CVE / vendor advisories

- [`https://nvd.nist.gov/vuln/detail/CVE-2023-31315`](https://nvd.nist.gov/vuln/detail/CVE-2023-31315) — SinkClose AMD SMM ring-2 escalation (patched on 5950X via Aug 2024 AGESA).
- [`https://nvd.nist.gov/vuln/detail/CVE-2025-37957`](https://nvd.nist.gov/vuln/detail/CVE-2025-37957) — KVM SVM nested triple-fault in SMM. Linux KVM only — does not affect bare-metal SCA, but the patch logic ("force vCPU SMM exit on SHUTDOWN intercept") is informative.
- [`https://nvd.nist.gov/vuln/detail/CVE-2025-38455`](https://nvd.nist.gov/vuln/detail/CVE-2025-38455) — KVM SVM SEV-ES intra-host migration race. Same scope as above.
- [`https://www.sentinelone.com/vulnerability-database/cve-2025-29948/`](https://www.sentinelone.com/vulnerability-database/cve-2025-29948/) — AMD SEV firmware RMP bypass.
- [`https://www.sentinelone.com/vulnerability-database/cve-2025-29943/`](https://www.sentinelone.com/vulnerability-database/cve-2025-29943/) — AMD CPU SEV-SNP write-what-where via stack pointer corruption.
- [`https://www.amd.com/en/resources/product-security/bulletin/amd-sb-3014.html`](https://www.amd.com/en/resources/product-security/bulletin/amd-sb-3014.html) — AMD Server Vulnerabilities, August 2025.

### Background reading (Hyper-V / VBS / Secure Kernel)

- [`https://r0keb.github.io/posts/Hyper-V-Research/`](https://r0keb.github.io/posts/Hyper-V-Research/) — Comprehensive Hyper-V internals walk-through. Source for VTL admin (`hvix64.sys`), VTL 1 (`securekernel.exe`), `SkiSetFeatureBits` CPUID dependencies, hypercall page setup.
- [`https://connormcgarr.github.io/secure-calls-and-skbridge/`](https://connormcgarr.github.io/secure-calls-and-skbridge/) — Secure Calls bridge between NT and Secure Kernel; `HvCallVtlCall` (hypercall code 0x11). Useful for understanding VTL1's potential to detect a foreign HV beneath Hyper-V via VM-exit side-channels.
- [`https://amitmoshel1.github.io/posts/virtualization-based-security-with-hyper-v-exploring-hyper-v-mechanisms-and-virtualization-based-security/`](https://amitmoshel1.github.io/posts/virtualization-based-security-with-hyper-v-exploring-hyper-v-mechanisms-and-virtualization-based-security/) — VBS architecture overview.

### Public hypervisor projects referenced

- [`https://github.com/HyperDbg/hyperdbg`](https://github.com/HyperDbg/hyperdbg) — HyperDbg. **HW-banned by EAAC per Spontaneous (2026).**
- [`https://github.com/tandasat/HyperPlatform`](https://github.com/tandasat/HyperPlatform) — HyperPlatform.
- [`https://github.com/tandasat/HelloAmdHvPkg`](https://github.com/tandasat/HelloAmdHvPkg) — Closest published architectural cousin to SCA (UEFI-based AMD-V).
- [`https://github.com/tandasat/barevisor`](https://github.com/tandasat/barevisor/) — Rust UEFI hypervisor, AMD + Intel, Sep 2025.
- [`https://github.com/tandasat/MiniVisorPkg`](https://github.com/tandasat/MiniVisorPkg) — UEFI VT-x.
- [`https://github.com/kernelwernel/VMAware`](https://github.com/kernelwernel/VMAware) — VM detection library — useful for testing SCA against published anti-VM heuristics.

### `[unverified]` claims in this report

- 0AVX's claim of "novel detection methods for EPT" in Javelin — referenced but not detailed publicly. Treated as unknown unknown.
- The exact RVA cluster 0AVX referenced for HV-detect group in Javelin binary — not disclosed. Spontaneous (post #11, UC #750201, 10 May 2026) noted he hadn't traced it: "Most of the HV-detect families I'd expect to live together (CPUID `0x4000000X` ladder, MSR consistency block at `0x480-0x491`, SIDT/SGDT/SLDT triplet, RDTSC-sandwich timer) are ones I haven't traced as load-bearing in current builds — but 'haven't traced' is not 'isn't there.'"
- Sentinel framework's claim that "EAC's hypervisor detection capabilities are not listed" — `[unverified]` against current 2026 EAC build, sourced via secondary citations to bypasscore/sentinel github tracker.

#include "HypeRender.h"
#include "HypeApexCanon.h"
#include "HypeMemory.h"
#include "HypeDebug.h"
#include "HypeUefi.h"

volatile RENDER_ENTRY gRenderQueue[RENDER_QUEUE_DEPTH];
// Default backend is STRING_HIJACK. Canon APEX_FPS_FMT_* is marked DEAD
// (sca_render_path_pin.md rev 3) — install-confirm against canon defaults
// is now a no-op-on-screen until rev-3 sentinel-test promotes a winner.
// Operator drives sentinel iteration via PMC_CMD_RENDER_SET_SINK + the
// installer's --render-sink RVA LEN flag.
volatile UINT8        gRenderBackend = RENDER_BACKEND_STRING_HIJACK;

// Runtime sink overrides — set via PMC_CMD_RENDER_SET_SINK. Both 0 means
// "use canon APEX_FPS_FMT_RVA / APEX_FPS_FMT_LEN" (backwards compat for
// the legacy install-confirm wiring).
static volatile UINT64 gRenderSinkRvaOverride = 0;
static volatile UINT32 gRenderSinkLenOverride = 0;
// rev-4: when non-zero, FpsBackupOnce dereferences
// (ImageBase + EffectiveSinkRva + IndirectOff) as a 64-bit pointer cell
// and uses the resulting VA as the live sink (e.g. ConVar.m_pszString
// heap buffer). Zero = direct write at (ImageBase + RVA), preserving
// the legacy .rdata-format-string behavior bit-for-bit.
static volatile UINT32 gRenderSinkIndirectOff = 0;

static inline UINT64 EffectiveSinkRva(VOID) {
    UINT64 ov = __atomic_load_n(&gRenderSinkRvaOverride, __ATOMIC_ACQUIRE);
    return ov ? ov : APEX_FPS_FMT_RVA;
}
static inline UINT32 EffectiveSinkLen(VOID) {
    UINT32 ov = __atomic_load_n(&gRenderSinkLenOverride, __ATOMIC_ACQUIRE);
    if (ov == 0) return (UINT32)APEX_FPS_FMT_LEN;
    return (ov > RENDER_SINK_MAX_LEN) ? RENDER_SINK_MAX_LEN : ov;
}
static inline UINT32 EffectiveSinkIndirectOff(VOID) {
    return __atomic_load_n(&gRenderSinkIndirectOff, __ATOMIC_ACQUIRE);
}

// ---------- string-hijack backend state ----------
//
// One-shot prologue verify on APEX_FPS_PRINTF_RVA — only meaningful when
// override is unset (the canon prologue is the FPS-helper prologue). With
// runtime override active there's no pinned prologue to check, so verify
// short-circuits PASS.
//
// Backup captures the original sink bytes AND the VA they were read from.
// Restore writes back to the captured VA, not EffectiveSinkRva at restore
// time — this is correct across SET_SINK transitions: SetSink synchronously
// restores using the captured VA before invalidating the backup, then the
// next push captures fresh bytes at the new VA.
//
// Single-sink invariant: the sink is one resource. Slot 0 owns it on push;
// subsequent active slots are silently ignored until the owner expires.
// install-confirm uses slot 0 by construction (queue is empty at install
// time), so the simple owner model is correct here.
#define FPS_VERIFY_UNCHECKED  0
#define FPS_VERIFY_PASS       1
#define FPS_VERIFY_DRIFT      2

static volatile UINT8  gFpsVerifyState   = FPS_VERIFY_UNCHECKED;
static volatile UINT8  gFpsBackupValid   = 0;
static volatile UINT32 gFpsActiveSlot    = 0xFFFFFFFFu;
static volatile UINT32 gFpsNeedsRestore  = 0;
static volatile UINT64 gFpsBackupSinkVa  = 0;
static volatile UINT32 gFpsBackupSinkLen = 0;
static UINT8           gFpsFmtBackup[RENDER_SINK_MAX_LEN + 1];

// SHOW_FPS convar value lives at image + APEX_OFF_SHOW_FPS + 0x6C (per
// HypeApexOffsets.h). FPS HUD only paints when this dword is non-zero.
// Only invoked when the entry has RENDER_FLAG_FORCE_FPS_CONVAR set; default
// install-confirm leaves the convar untouched.
#define APEX_FPS_SHOW_VALUE_OFF  0x6CULL
static volatile UINT8  gFpsShowBackupValid = 0;
static UINT32          gFpsShowBackup       = 0;

static BOOLEAN
FpsVerifyPrologueOnce(UINT64 Cr3, UINT64 ImageBase)
{
    // Skip prologue check when runtime override is active — there's no
    // canon prologue for arbitrary sinks. Same for indirect-sink mode:
    // we're writing at a heap VA recovered via deref, the canon FPS
    // helper prologue is unrelated.
    if (__atomic_load_n(&gRenderSinkRvaOverride, __ATOMIC_ACQUIRE) != 0
        || __atomic_load_n(&gRenderSinkIndirectOff, __ATOMIC_ACQUIRE) != 0) {
        return TRUE;
    }

    UINT8 St = __atomic_load_n(&gFpsVerifyState, __ATOMIC_ACQUIRE);
    if (St == FPS_VERIFY_PASS)  return TRUE;
    if (St == FPS_VERIFY_DRIFT) return FALSE;

    UINT64 Got = 0;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, ImageBase + APEX_FPS_PRINTF_RVA,
                                   &Got, sizeof(Got)))) {
        // Transient — leave UNCHECKED so we retry next tick.
        return FALSE;
    }
    if (Got == APEX_FPS_PRINTF_PROLOGUE_LE64) {
        __atomic_store_n(&gFpsVerifyState, FPS_VERIFY_PASS, __ATOMIC_RELEASE);
        HvLog("RDP\n");
        return TRUE;
    }
    __atomic_store_n(&gFpsVerifyState, FPS_VERIFY_DRIFT, __ATOMIC_RELEASE);
    HvLogHex("RDD", Got);
    return FALSE;
}

static BOOLEAN
FpsBackupOnce(UINT64 Cr3, UINT64 ImageBase)
{
    if (__atomic_load_n(&gFpsBackupValid, __ATOMIC_ACQUIRE)) return TRUE;

    UINT64 SinkVa  = ImageBase + EffectiveSinkRva();
    UINT32 SinkLen = EffectiveSinkLen();
    UINT32 BackupBytes = SinkLen + 1;  // include NUL slot
    if (BackupBytes > sizeof(gFpsFmtBackup)) BackupBytes = sizeof(gFpsFmtBackup);

    // Indirect-sink (rev-4): SinkVa points at a pointer cell, deref it to
    // recover the live heap buffer VA. Zero IndirectOff preserves the
    // legacy direct-RVA path.
    UINT32 IndirectOff = EffectiveSinkIndirectOff();
    if (IndirectOff != 0) {
        UINT64 PtrCellVa = SinkVa + (UINT64)IndirectOff;
        UINT64 HeapVa = 0;
        if (EFI_ERROR(ReadGuestVirtual(Cr3, PtrCellVa,
                                       &HeapVa, sizeof(HeapVa)))) {
            HvLogHex("RDJ", PtrCellVa);   // deref read failed (transient)
            return FALSE;
        }
        // Heap pointers on Win11 x64 user-mode are typically far above
        // image base. Reject obvious garbage: NULL, or a value that
        // lands inside image (would mean the slot wasn't a heap ptr).
        if (HeapVa == 0 || HeapVa < 0x10000ULL) {
            HvLogHex("RDK", HeapVa);      // bogus heap VA — sink misconfigured
            return FALSE;
        }
        SinkVa = HeapVa;
        HvLogHex("RDI", HeapVa);          // one-shot: resolved heap VA
    }

    if (EFI_ERROR(ReadGuestVirtual(Cr3, SinkVa, gFpsFmtBackup, BackupBytes))) {
        return FALSE;
    }
    __atomic_store_n(&gFpsBackupSinkVa,  SinkVa,      __ATOMIC_RELEASE);
    __atomic_store_n(&gFpsBackupSinkLen, BackupBytes, __ATOMIC_RELEASE);
    __atomic_store_n(&gFpsBackupValid,   1,           __ATOMIC_RELEASE);
    HvLog("RDU\n");
    return TRUE;
}

static VOID
FpsWriteFmt(UINT64 Cr3, UINT64 ImageBase, const char *Text)
{
    (VOID)ImageBase;  // resolved sink VA comes from FpsBackupOnce's capture

    UINT32 SinkLen = EffectiveSinkLen();
    UINT32 WriteBytes = SinkLen + 1;
    if (WriteBytes > sizeof(gFpsFmtBackup)) WriteBytes = sizeof(gFpsFmtBackup);

    UINT8 Buf[RENDER_SINK_MAX_LEN + 1];
    SetMem(Buf, sizeof(Buf), 0);

    UINT32 Len = 0;
    while (Len < SinkLen && Text[Len] != '\0') {
        Buf[Len] = (UINT8)Text[Len];
        Len++;
    }
    // Implicit trailing NUL from SetMem above.

    // BackendStringHijack guarantees FpsBackupOnce ran first; the captured
    // VA is the truth for both direct and indirect sinks. Restore uses the
    // same VA, so a write/restore pair always targets the same address.
    UINT64 SinkVa = __atomic_load_n(&gFpsBackupSinkVa, __ATOMIC_ACQUIRE);
    if (SinkVa == 0) return;
    WriteGuestVirtual(Cr3, SinkVa, Buf, WriteBytes);
}

static VOID
FpsRestoreFmt(UINT64 Cr3, UINT64 ImageBase)
{
    (VOID)ImageBase;  // restore uses the captured-at-backup VA, not current
    if (!__atomic_load_n(&gFpsBackupValid, __ATOMIC_ACQUIRE)) return;

    UINT64 BackupVa  = __atomic_load_n(&gFpsBackupSinkVa,  __ATOMIC_ACQUIRE);
    UINT32 BackupLen = __atomic_load_n(&gFpsBackupSinkLen, __ATOMIC_ACQUIRE);
    if (BackupVa == 0 || BackupLen == 0) return;

    WriteGuestVirtual(Cr3, BackupVa, gFpsFmtBackup, BackupLen);
    HvLog("RDV\n");

    // Restore SHOW_FPS convar to its pre-hijack value (only if FORCE flag
    // set this slot — backup-valid is the latch).
    if (__atomic_load_n(&gFpsShowBackupValid, __ATOMIC_ACQUIRE)) {
        UINT64 ShowVa = ImageBase + APEX_OFF_SHOW_FPS + APEX_FPS_SHOW_VALUE_OFF;
        WriteGuestVirtual(Cr3, ShowVa, &gFpsShowBackup, sizeof(gFpsShowBackup));
        __atomic_store_n(&gFpsShowBackupValid, 0, __ATOMIC_RELEASE);
        HvLogHex("RDW", (UINT64)gFpsShowBackup);
    }
}

// Force the FPS HUD on if it isn't already. Backs up the prior convar value
// so FpsRestoreFmt can put it back. Only invoked when the active slot has
// RENDER_FLAG_FORCE_FPS_CONVAR set (legacy FPS-sink path).
static VOID
FpsForceShowOnce(UINT64 Cr3, UINT64 ImageBase)
{
    if (__atomic_load_n(&gFpsShowBackupValid, __ATOMIC_ACQUIRE)) return;
    UINT64 ShowVa = ImageBase + APEX_OFF_SHOW_FPS + APEX_FPS_SHOW_VALUE_OFF;
    UINT32 Cur = 0;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, ShowVa, &Cur, sizeof(Cur)))) return;
    gFpsShowBackup = Cur;
    __atomic_store_n(&gFpsShowBackupValid, 1, __ATOMIC_RELEASE);
    if (Cur == 0) {
        UINT32 On = 1;
        WriteGuestVirtual(Cr3, ShowVa, &On, sizeof(On));
        HvLog("RDF\n");
    }
}

UINT32
HypeRenderPush(const char *Text, UINT32 DurationTicks,
               UINT16 XQ8, UINT16 YQ8, UINT8 ColorIdx, UINT8 Flags)
{
    if (!Text) return COVERT_STATUS_ERR;
    if (DurationTicks == 0) DurationTicks = 1;

    for (UINT32 i = 0; i < RENDER_QUEUE_DEPTH; i++) {
        if (__atomic_load_n(&gRenderQueue[i].TicksRemaining,
                            __ATOMIC_ACQUIRE) != 0) continue;

        UINT32 j = 0;
        for (; j < RENDER_TEXT_MAX && Text[j] != '\0'; j++) {
            gRenderQueue[i].Text[j] = Text[j];
        }
        gRenderQueue[i].Text[j] = '\0';

        gRenderQueue[i].XQ8      = XQ8;
        gRenderQueue[i].YQ8      = YQ8;
        gRenderQueue[i].ColorIdx = ColorIdx;
        gRenderQueue[i].Flags    = Flags;

        // Store TicksRemaining last — this is the live-slot sentinel.
        __atomic_store_n(&gRenderQueue[i].TicksRemaining,
                         DurationTicks, __ATOMIC_RELEASE);

        HvLogHex("RDQ", ((UINT64)i << 16) | (UINT64)(DurationTicks & 0xFFFFu));
        return COVERT_STATUS_OK;
    }

    return COVERT_STATUS_ERR;
}

VOID
HypeRenderClear(VOID)
{
    for (UINT32 i = 0; i < RENDER_QUEUE_DEPTH; i++) {
        __atomic_store_n(&gRenderQueue[i].TicksRemaining, 0, __ATOMIC_RELEASE);
    }
    // If the string-hijack backend currently owns the sink, defer the
    // restore to the next HypeRenderTick (it has Cr3+ImageBase). Just flag
    // the need + drop ownership so the dispatch loop won't re-arm it.
    if (__atomic_exchange_n(&gFpsActiveSlot, 0xFFFFFFFFu, __ATOMIC_ACQ_REL)
        != 0xFFFFFFFFu) {
        __atomic_store_n(&gFpsNeedsRestore, 1, __ATOMIC_RELEASE);
    }
}

UINT32
HypeRenderSetSink(UINT64 Cr3, UINT64 ImageBase, UINT64 NewRva, UINT32 NewLen,
                  UINT32 IndirectOff)
{
    if (Cr3 == 0 || ImageBase == 0) return COVERT_STATUS_BAD_ADDR;

    // 1. Restore current sink synchronously using captured VA + backup
    //    (so the new sink isn't corrupted by the old backup on next RDV).
    if (__atomic_exchange_n(&gFpsActiveSlot, 0xFFFFFFFFu, __ATOMIC_ACQ_REL)
        != 0xFFFFFFFFu) {
        FpsRestoreFmt(Cr3, ImageBase);
    }
    // 2. Drop any pending deferred restore — we already restored above.
    __atomic_store_n(&gFpsNeedsRestore, 0, __ATOMIC_RELEASE);

    // 3. Clear queue — entries pushed against the old sink are stale.
    for (UINT32 i = 0; i < RENDER_QUEUE_DEPTH; i++) {
        __atomic_store_n(&gRenderQueue[i].TicksRemaining, 0, __ATOMIC_RELEASE);
    }

    // 4. Invalidate backup state — captured for the old sink, no longer applies.
    __atomic_store_n(&gFpsBackupValid,   0, __ATOMIC_RELEASE);
    __atomic_store_n(&gFpsBackupSinkVa,  0, __ATOMIC_RELEASE);
    __atomic_store_n(&gFpsBackupSinkLen, 0, __ATOMIC_RELEASE);
    // Reset prologue verify so canon check re-runs if override goes back to 0.
    __atomic_store_n(&gFpsVerifyState, FPS_VERIFY_UNCHECKED, __ATOMIC_RELEASE);

    // 5. Install new override (NewRva=0 reverts to canon, same for NewLen).
    if (NewLen > RENDER_SINK_MAX_LEN) NewLen = RENDER_SINK_MAX_LEN;
    __atomic_store_n(&gRenderSinkLenOverride, NewLen, __ATOMIC_RELEASE);
    __atomic_store_n(&gRenderSinkRvaOverride, NewRva, __ATOMIC_RELEASE);
    __atomic_store_n(&gRenderSinkIndirectOff, IndirectOff, __ATOMIC_RELEASE);

    HvLogHex("RDS", NewRva);
    HvLogHex("RDM", (UINT64)NewLen);
    HvLogHex("RDN", (UINT64)IndirectOff);
    return COVERT_STATUS_OK;
}

// Phase 1 baseline — log-only. Rate-gated to keep RDR off the 4 MB ring
// during steady-state validation.
static VOID
BackendNone(UINT32 SlotIdx, UINT32 TicksRemaining, UINT64 Fired)
{
    if ((Fired & 0x7Fu) == 0) {
        HvLogHex("RDR", ((UINT64)SlotIdx << 32) | (UINT64)TicksRemaining);
    }
}

// Phase 2 — overwrite the effective sink VA once when slot becomes the owner.
// Engine paints the buffer every frame from its own callsite (no per-frame
// VMEXIT, no foreign call). On expiry/clear/SetSink the original bytes are
// restored from the captured backup.
static VOID
BackendStringHijack(UINT32 SlotIdx, UINT64 Cr3, UINT64 ImageBase)
{
    if (Cr3 == 0 || ImageBase == 0) return;
    if (!FpsVerifyPrologueOnce(Cr3, ImageBase)) return;
    if (!FpsBackupOnce(Cr3, ImageBase)) return;

    UINT32 Owner = __atomic_load_n(&gFpsActiveSlot, __ATOMIC_ACQUIRE);
    if (Owner == SlotIdx) return;        // already painting this entry
    if (Owner != 0xFFFFFFFFu) return;    // another slot owns the sink

    UINT8 EntryFlags = gRenderQueue[SlotIdx].Flags;
    if (EntryFlags & RENDER_FLAG_FORCE_FPS_CONVAR) {
        FpsForceShowOnce(Cr3, ImageBase);
    }

    FpsWriteFmt(Cr3, ImageBase, (const char *)gRenderQueue[SlotIdx].Text);
    __atomic_store_n(&gFpsActiveSlot, SlotIdx, __ATOMIC_RELEASE);

    UINT32 Len = 0;
    while (Len < RENDER_TEXT_MAX && gRenderQueue[SlotIdx].Text[Len] != '\0') Len++;
    HvLogHex("RDH", ((UINT64)SlotIdx << 32) | (UINT64)Len);
}

VOID
HypeRenderTick(PVCPU_DATA Vcpu, UINT64 Cr3, UINT64 ImageBase, UINT64 Fired)
{
    (VOID)Vcpu;

    UINT8 Backend = __atomic_load_n(&gRenderBackend, __ATOMIC_ACQUIRE);

    // Drain a deferred restore (HypeRenderClear or async ownership drop).
    // Cheap: only fires when gFpsNeedsRestore was set by a producer.
    if (Backend == RENDER_BACKEND_STRING_HIJACK
        && __atomic_exchange_n(&gFpsNeedsRestore, 0, __ATOMIC_ACQ_REL)) {
        if (Cr3 != 0 && ImageBase != 0) FpsRestoreFmt(Cr3, ImageBase);
    }

    for (UINT32 i = 0; i < RENDER_QUEUE_DEPTH; i++) {
        UINT32 Ticks = __atomic_load_n(&gRenderQueue[i].TicksRemaining,
                                       __ATOMIC_ACQUIRE);
        if (Ticks == 0) continue;

        switch (Backend) {
        case RENDER_BACKEND_STRING_HIJACK:
            BackendStringHijack(i, Cr3, ImageBase);
            break;
        case RENDER_BACKEND_NONE:
        default:
            BackendNone(i, Ticks, Fired);
            break;
        // RENDER_BACKEND_VGUI deferred to Phase 3.
        }

        UINT32 Next = Ticks - 1;
        __atomic_store_n(&gRenderQueue[i].TicksRemaining, Next, __ATOMIC_RELEASE);
        if (Next == 0) {
            HvLogHex("RDX", (UINT64)i);

            // Sink owner expired — restore original bytes.
            if (Backend == RENDER_BACKEND_STRING_HIJACK) {
                UINT32 Owner = __atomic_load_n(&gFpsActiveSlot, __ATOMIC_ACQUIRE);
                if (Owner == i) {
                    FpsRestoreFmt(Cr3, ImageBase);
                    __atomic_store_n(&gFpsActiveSlot, 0xFFFFFFFFu,
                                     __ATOMIC_RELEASE);
                }
            }
        }
    }
}

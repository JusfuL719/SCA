// HypeHookDraw.c — per-frame draw-hook + glow/aim payload (cycle 9m).
// Phase 2 rearm: NPF clears NX + logs DR0 (any-RIP, cycle 9h+); no TF/#DB.
// VmexitHandler heartbeat re-sets NX every ~228 ms (DrawHookRearm).

// HID_SNAPSHOT_MODE probe retired 2026-05-12 cycle 11g — HSE=120 events
// in drain 11g2 confirmed player bucket 78 is live. HIGHLIGHT_ID write
// is unconditional below.

#include "HypeHookDraw.h"
#include "HypeAimTrigger.h"
#include "HypeMenu.h"
#include "HypeContext.h"
#include "HypeDebug.h"
#include "HypeMemory.h"
#include "HypeNpt.h"
#include "HypeApexCanon.h"

// Read/Write GuestVirtual helpers now live in HypeMemory.h so HypeMenu and
// HypeAimTrigger can share the same inlines without ODR clashes.

extern DRIVER_CONTEXT g_DriverContext;

static DRAW_HOOK_DESC gDrawHook;
static UINT32         gDrawHookInstalled = 0;
static volatile UINT64 gDrawHookNpfTotal = 0;
static volatile UINT64 gPayloadTickCount  = 0;
static volatile UINT64 gPayloadTicksFired = 0;
static volatile UINT64 gDrawHookGpa     = 0;
static UINT32         gWrapperProbed    = 0;
#define WRAPPER_RING_SIZE 64
static UINT64          gWrapperRing[WRAPPER_RING_SIZE] = {0};
static UINT64          gWrapperRingInst[WRAPPER_RING_SIZE] = {0};
static volatile UINT32 gWrapperRingIdx   = 0;
static volatile UINT32 gWrapperRingDone  = 0;
static UINT8 gDrawHookHvBuf[HOOK_DRAW_BUF_BYTES] __attribute__((aligned(0x1000)));

// Runtime glow knobs — settable via PMC_CMD_SET_GLOW_PARAMS, also mutated
// directly by HypeMenu. Defaults match cycle 9k visual + canon entity-write
// values. Typedef + extern are in HypeHookDraw.h so HypeMenu can reference.
volatile GLOW_PARAMS_RT gGlowParams = {
    .Slot = 78, .Mask = 0x01, .FilterMode = 0, .Enabled = 1,
    .VisType = 1, .GlowFix = 2, .WriteVisType = 1, .WriteGlowFix = 1
};

VOID *HookDrawScratchHvBuf(VOID) {
    return (gDrawHook.CopiedBytes == HOOK_DRAW_BUF_BYTES)
                ? (VOID *)gDrawHookHvBuf
                : NULL;
}

static EFI_STATUS DrawHookArmExecTrap(PVCPU_DATA Vcpu, UINT64 HookVa) {
    if (Vcpu->TargetCr3 == 0) {
        HvLogHex("DRA", HookVa);
        return EFI_NOT_READY;
    }
    UINT64 PrevGpa = __atomic_load_n(&gDrawHookGpa, __ATOMIC_ACQUIRE);
    if (PrevGpa != 0) {
        PNPT_CONTEXT NptPrev = &g_DriverContext.NptContext;
        UINT64 *PrevPte = NptGetPte(NptPrev, PrevGpa, FALSE);
        if (PrevPte && !(*PrevPte & NPT_LARGE_PAGE)) {
            __atomic_fetch_and(PrevPte, ~(UINT64)NPT_NX, __ATOMIC_ACQ_REL);
        }
        __atomic_store_n(&gDrawHookGpa, 0, __ATOMIC_RELEASE);
    }
    UINT64 HookGpa = 0;
    EFI_STATUS St = TranslateGuestVirtual(Vcpu->TargetCr3, HookVa, &HookGpa);
    if (EFI_ERROR(St)) {
        HvLogHex("DRB", HookVa);
        return St;
    }
    UINT64 HookGpaAligned = HookGpa & ~(PAGE_SIZE_4KB - 1);
    PNPT_CONTEXT Npt = &g_DriverContext.NptContext;
    UINT64 *Pte = NptGetPte(Npt, HookGpaAligned, FALSE);
    if (!Pte) {
        HvLogHex("DRC", HookGpaAligned);
        return EFI_NOT_FOUND;
    }
    if (*Pte & NPT_LARGE_PAGE) {
        NTSTATUS SplitSt = NptSplitLargePageRuntime(Npt, HookGpaAligned);
        if (!NT_SUCCESS(SplitSt)) {
            HvLogHex("DRD", (UINT64)SplitSt);
            return EFI_DEVICE_ERROR;
        }
        Pte = NptGetPte(Npt, HookGpaAligned, FALSE);
        if (!Pte || (*Pte & NPT_LARGE_PAGE)) {
            HvLogHex("DRE", HookGpaAligned);
            return EFI_DEVICE_ERROR;
        }
    }
    __atomic_fetch_or(Pte, NPT_NX, __ATOMIC_ACQ_REL);
    __atomic_store_n(&gDrawHookGpa, HookGpaAligned, __ATOMIC_RELEASE);

    UINT32 N = g_DriverContext.HvState.NumCpus;
    for (UINT32 i = 0; i < N; i++) {
        PVCPU_DATA V = &g_DriverContext.HvState.VcpuTable[i];
        if (V->Vmcb) V->Vmcb->Control.TlbControl = 1;
    }
    HvLogHex("DR9", HookGpaAligned);
    return EFI_SUCCESS;
}

BOOLEAN DrawHookGpaMatches(UINT64 FaultGpa) {
    UINT64 Snap = __atomic_load_n(&gDrawHookGpa, __ATOMIC_ACQUIRE);
    if (Snap == 0) return FALSE;
    return (FaultGpa & ~(PAGE_SIZE_4KB - 1)) == Snap;
}

// =====================================================================
// SSE-via-inline-asm float helpers. Compiler builds with -mno-sse so it
// never emits SSE itself. AMD64 long-mode mandates SSE2 hardware, so
// explicit-clobber inline asm is safe. UINT32 carries IEEE-754 bits.
// XMM0/1 are volatile under MS x64 ABI; safe to clobber per call.
//
// Per-fn target("sse2") pragma overrides the file's -mno-sse so GCC
// allows XMM register clobbers — without it, GCC errors on the asm.
// =====================================================================

#pragma GCC push_options
#pragma GCC target("sse2")

static inline UINT32 f_add(UINT32 a, UINT32 b) {
    UINT32 r;
    __asm__ volatile ("movd %1,%%xmm0; movd %2,%%xmm1; addss %%xmm1,%%xmm0; movd %%xmm0,%0"
                      : "=r"(r) : "r"(a), "r"(b) : "xmm0","xmm1");
    return r;
}
static inline UINT32 f_sub(UINT32 a, UINT32 b) {
    UINT32 r;
    __asm__ volatile ("movd %1,%%xmm0; movd %2,%%xmm1; subss %%xmm1,%%xmm0; movd %%xmm0,%0"
                      : "=r"(r) : "r"(a), "r"(b) : "xmm0","xmm1");
    return r;
}
static inline UINT32 f_mul(UINT32 a, UINT32 b) {
    UINT32 r;
    __asm__ volatile ("movd %1,%%xmm0; movd %2,%%xmm1; mulss %%xmm1,%%xmm0; movd %%xmm0,%0"
                      : "=r"(r) : "r"(a), "r"(b) : "xmm0","xmm1");
    return r;
}
static inline UINT32 f_div(UINT32 a, UINT32 b) {
    UINT32 r;
    __asm__ volatile ("movd %1,%%xmm0; movd %2,%%xmm1; divss %%xmm1,%%xmm0; movd %%xmm0,%0"
                      : "=r"(r) : "r"(a), "r"(b) : "xmm0","xmm1");
    return r;
}
static inline UINT32 f_sqrt(UINT32 a) {
    UINT32 r;
    __asm__ volatile ("movd %1,%%xmm0; sqrtss %%xmm0,%%xmm0; movd %%xmm0,%0"
                      : "=r"(r) : "r"(a) : "xmm0");
    return r;
}
static inline UINT32 f_abs(UINT32 a) { return a & 0x7FFFFFFFUL; }
static inline UINT32 f_neg(UINT32 a) { return a ^ 0x80000000UL; }
static inline INT32  f_cmp_lt(UINT32 a, UINT32 b) {
    INT32 r;
    __asm__ volatile ("xor %0,%0; movd %1,%%xmm0; movd %2,%%xmm1; ucomiss %%xmm1,%%xmm0; setb %b0"
                      : "=q"(r) : "r"(a), "r"(b) : "xmm0","xmm1","cc");
    return r;
}
static inline UINT32 f_from_i32(INT32 v) {
    UINT32 r;
    __asm__ volatile ("cvtsi2ss %1,%%xmm0; movd %%xmm0,%0"
                      : "=r"(r) : "r"(v) : "xmm0");
    return r;
}
static inline INT16 f_to_q15(UINT32 a) {
    UINT32 scaled = f_mul(a, 0x47000000UL);  // a * 32768.0
    INT32 i;
    __asm__ volatile ("movd %1,%%xmm0; cvttss2si %%xmm0,%0"
                      : "=r"(i) : "r"(scaled) : "xmm0");
    if (i >  32767) i =  32767;
    if (i < -32768) i = -32768;
    return (INT16)i;
}

#pragma GCC pop_options

#define F32_ZERO     0x00000000UL
#define F32_ONE      0x3F800000UL
#define F32_HALF     0x3F000000UL
#define F32_PI       0x40490FDBUL
#define F32_PI_2     0x3FC90FDBUL
#define F32_RAD2DEG  0x42652EE0UL
#define F32_180      0x43340000UL
#define F32_360      0x43B40000UL
#define F32_NEG_180  0xC3340000UL
#define F32_89       0x42B20000UL
#define F32_NEG_89   0xC2B20000UL

// atan(z) for |z| <= 1 — Hastings minimax (deg-7 odd, ~0.5 deg worst).
static UINT32 f_atan_unit(UINT32 z) {
    UINT32 z2 = f_mul(z, z);
    UINT32 c0 = 0x3F7FFE83UL;  //  0.99997726
    UINT32 c1 = 0xBEAA48F8UL;  // -0.33262347
    UINT32 c2 = 0x3E462DAEUL;  //  0.19354346
    UINT32 c3 = 0xBDEE7240UL;  // -0.11643287
    UINT32 t  = c3;
    t = f_add(c2, f_mul(t, z2));
    t = f_add(c1, f_mul(t, z2));
    t = f_add(c0, f_mul(t, z2));
    return f_mul(z, t);
}

static UINT32 f_atan2(UINT32 y, UINT32 x) {
    UINT32 ay = f_abs(y);
    UINT32 ax = f_abs(x);
    if (ay == 0 && ax == 0) return F32_ZERO;
    UINT32 r;
    if (f_cmp_lt(ay, ax)) {
        r = f_atan_unit(f_div(y, x));
        if (x & 0x80000000UL) {
            r = (y & 0x80000000UL) ? f_sub(r, F32_PI) : f_add(r, F32_PI);
        }
    } else {
        UINT32 q = f_atan_unit(f_div(x, y));
        UINT32 pi2 = (y & 0x80000000UL) ? f_neg(F32_PI_2) : F32_PI_2;
        r = f_sub(pi2, q);
    }
    return r;
}

static inline UINT32 f_atan2_deg(UINT32 y, UINT32 x) {
    return f_mul(f_atan2(y, x), F32_RAD2DEG);
}

// =====================================================================
// Cycle 9m payload state — snapshot ring + local + aim.
// All buffers HV-private; PEEK on scratch buffer mirrors at fixed offsets.
// =====================================================================

#define APEX_NAME_PLAYER              0x0000726579616C70ULL
#define APEX_NAME_PLAYER_MASK         0x00FFFFFFFFFFFFFFULL
#define APEX_NAME_PROP_4              0x70726F70UL  // "prop"

// Non-static — shared with HypeAimTrigger.c via extern declarations there.
ENT_SNAPSHOT   gEntCache[HOOK_DRAW_CACHE_SIZE];
LOCAL_SNAPSHOT gLocal;
static volatile UINT32 gScanCursor = 0;

// One-shot capture flags so we can fire markers without per-frame spam.
// gEverEngagedAim / gEverWroteAngle live in HypeAimTrigger.c now.
static UINT8 gEverSawLocal   = 0;
static UINT8 gEverSawWeapon  = 0;

#ifdef HID_SNAPSHOT_MODE
#define HID_LOG_MAX 128
static UINT8           gHidLastVal[HOOK_DRAW_CACHE_SIZE]   = {0};
static UINT8           gHidLastSt [HOOK_DRAW_CACHE_SIZE]   = {0};
static UINT8           gHidLastInit[HOOK_DRAW_CACHE_SIZE]  = {0};
static volatile UINT32 gHidLogged                          = 0;
#endif

// ---------- classification ----------
//
// Player check: classname mask of "player\0" first 7 bytes.
// Loot: classname starts "prop" → SURVIVAL_ITEM_ID bucket (Bobby's slots).
static UINT8 ClassifyLootByItemId(UINT32 ItemId) {
    switch (ItemId) {
        case 9:  return APEX_LOOT_TIER_WEAPON;
        case 16: return APEX_LOOT_TIER_LEGENDARY;
        case 43: return APEX_LOOT_TIER_MYTHIC;
        case 49: return APEX_LOOT_TIER_EPIC;
        case 56: return APEX_LOOT_TIER_RARE;
        case 60: return APEX_LOOT_TIER_AMMO;
        case 68: return APEX_LOOT_TIER_COMMON;
        default: return APEX_LOOT_TIER_NONE;
    }
}

static UINT8 SlotForTier(UINT8 Tier) {
    switch (Tier) {
        case APEX_LOOT_TIER_MYTHIC:    return APEX_SLOT_LOOT_MYTHIC;
        case APEX_LOOT_TIER_LEGENDARY: return APEX_SLOT_LOOT_LEGENDARY;
        case APEX_LOOT_TIER_EPIC:      return APEX_SLOT_LOOT_EPIC;
        case APEX_LOOT_TIER_RARE:      return APEX_SLOT_LOOT_RARE;
        case APEX_LOOT_TIER_AMMO:      return APEX_SLOT_LOOT_AMMO;
        default:                        return 0;
    }
}

static VOID InitHighlightSlot(UINT64 Cr3, UINT64 Settings,
                               UINT8 Slot, UINT32 R, UINT32 G, UINT32 B) {
    if (Slot == 0) return;
    UINT64 Base = Settings + APEX_HIGHLIGHT_TYPE_SIZE * (UINT64)Slot;
    UINT32 FnBits = APEX_FN_BITS_VIS;
    // FunctionBits at bucket+0x2C — verified layout (2026-05-13).
    // SetHighlightId (live RVA 0x00818500, was 0x817600 pre +0xF00 drift)
    // copies bucket+0x2C → entity+0x268 on success path. +0x00 is a
    // type tag, NOT FunctionBits. cab44ef "+0x2C kills glow" test was
    // confounded by cycle 14 [B]'s 16k NPF/sec EOI-trap storm.
    WriteGuestVirtual(Cr3, Base + 0x2C, &FnBits, 4);
    WriteGuestVirtual(Cr3, Base + 0x04, &R, 4);
    WriteGuestVirtual(Cr3, Base + 0x08, &G, 4);
    WriteGuestVirtual(Cr3, Base + 0x0C, &B, 4);
}

static VOID WriteEntityGlow(UINT64 Cr3, UINT64 Ent, UINT8 Slot) {
    if (Ent == 0 || Slot == 0) return;
    UINT32 GlowFix  = (UINT32)gGlowParams.GlowFix;
    UINT32 VisType  = (UINT32)gGlowParams.VisType;
    UINT32 GlowDist = 0x47C35000UL;
    UINT8  Mask     = gGlowParams.Mask;
    if (gGlowParams.WriteGlowFix) {
        WriteGuestVirtual(Cr3, Ent + APEX_ENT_GLOW_FIX,          &GlowFix,  4);
    }
    if (gGlowParams.WriteVisType) {
        WriteGuestVirtual(Cr3, Ent + APEX_ENT_GLOW_VISIBLE_TYPE, &VisType,  4);
    }
    WriteGuestVirtual(Cr3, Ent + APEX_ENT_GLOW_DISTANCE,     &GlowDist, 4);
    WriteGuestVirtual(Cr3, Ent + APEX_ENT_HIGHLIGHT_ID,      &Slot,     1);
    WriteGuestVirtual(Cr3, Ent + APEX_ENT_HIGHLIGHT_STACK,   &Slot,     1);
    WriteGuestVirtual(Cr3, Ent + APEX_ENT_HIGHLIGHT_MASK,    &Mask,     1);
}

// ---------- snapshot mirror (PEEK consumer) ----------
static VOID MirrorSnapshots(UINT64 Tick) {
    if (gDrawHook.CopiedBytes != HOOK_DRAW_BUF_BYTES) return;
    CopyMem(gDrawHookHvBuf + DRAWBUF_OFF_LOCAL,     &gLocal, sizeof(LOCAL_SNAPSHOT));
    CopyMem(gDrawHookHvBuf + DRAWBUF_OFF_AIM,       &gAim,   sizeof(AIM_STATE));
    CopyMem(gDrawHookHvBuf + DRAWBUF_OFF_HEARTBEAT, &Tick,   sizeof(UINT64));
    UINT32 Mirrored = 0;
    for (UINT32 i = 0; i < HOOK_DRAW_CACHE_SIZE
                         && Mirrored < HOOK_DRAW_MIRROR_SLOTS; i++) {
        if (gEntCache[i].Ent == 0) continue;
        CopyMem(gDrawHookHvBuf + DRAWBUF_OFF_ENT_SNAPSHOTS
                                + Mirrored * sizeof(ENT_SNAPSHOT),
                &gEntCache[i], sizeof(ENT_SNAPSHOT));
        Mirrored++;
    }
    if (Mirrored < HOOK_DRAW_MIRROR_SLOTS) {
        UINT64 zero = 0;
        CopyMem(gDrawHookHvBuf + DRAWBUF_OFF_ENT_SNAPSHOTS
                                + Mirrored * sizeof(ENT_SNAPSHOT),
                &zero, sizeof(zero));
    }
}

// ---------- local player + recoil peak-hold + view matrix ----------
static BOOLEAN ReadLocalPlayer(UINT64 Cr3, UINT64 ImageBase, UINT64 Tick) {
    UINT64 LocalPtr = 0;
    EFI_STATUS RdSt = ReadGuestVirtual(Cr3, ImageBase + APEX_OFF_LOCAL_PLAYER,
                                       &LocalPtr, 8);
    // Diagnostic: emit raw slot value first 16 ticks + every 0x400 after.
    // Pins whether the slot is null at runtime vs an offset/CR3 issue.
    if (Tick < 16 || (Tick & 0x3FF) == 1) {
        if (EFI_ERROR(RdSt)) {
            HvLogHex("LPR", (UINT64)RdSt);  // read fail status
        } else {
            HvLogHex("LPV", LocalPtr);                          // raw slot value
            HvLogHex("LPB", ImageBase + APEX_OFF_LOCAL_PLAYER); // VA we read from
        }
    }
    if (EFI_ERROR(RdSt)) return FALSE;
    if (LocalPtr < 0x10000 || LocalPtr >= 0x800000000000ULL) return FALSE;
    gLocal.Ent = LocalPtr;

    ReadGuestVirtual(Cr3, LocalPtr + APEX_ENT_ORIGIN,             gLocal.Origin,     12);
    ReadGuestVirtual(Cr3, LocalPtr + APEX_ENT_CAMERA_ORIGIN,      gLocal.Camera,     12);
    ReadGuestVirtual(Cr3, LocalPtr + APEX_ENT_VIEW_ANGLES,        gLocal.ViewAngles, 12);
    ReadGuestVirtual(Cr3, LocalPtr + APEX_ENT_PUNCH_WEAPON_ANGLE, gLocal.PunchRaw,   12);
    ReadGuestVirtual(Cr3, LocalPtr + APEX_ENT_TEAM,               &gLocal.Team,      4);
    ReadGuestVirtual(Cr3, LocalPtr + APEX_ENT_HEALTH,             &gLocal.Health,    4);
    ReadGuestVirtual(Cr3, LocalPtr + APEX_ENT_MAX_HEALTH,         &gLocal.MaxHealth, 4);
    ReadGuestVirtual(Cr3, LocalPtr + APEX_ENT_SHIELD,             &gLocal.Shield,    4);
    ReadGuestVirtual(Cr3, LocalPtr + APEX_ENT_MAX_SHIELD,         &gLocal.MaxShield, 4);

    UINT8 Zoom = 0;
    UINT32 BleedW = 0;
    ReadGuestVirtual(Cr3, LocalPtr + APEX_ENT_ZOOMING,            &Zoom,   1);
    ReadGuestVirtual(Cr3, LocalPtr + APEX_ENT_BLEEDOUT_STATE,     &BleedW, 4);
    gLocal.IsZooming = Zoom;
    gLocal.IsDowned  = (BleedW != 0) ? 1 : 0;

    UINT32 AttackState = 0;
    ReadGuestVirtual(Cr3, ImageBase + APEX_OFF_IN_ATTACK_KBUTTON + 0x8,
                     &AttackState, 4);
    gLocal.AttackPressed = (AttackState == 5) ? 1 : 0;

    if (!gEverSawLocal) {
        gEverSawLocal = 1;
        HvLogHex("EL0", LocalPtr);
    }

    // Recoil peak-hold per axis. View+punch is the bullet-origin angle, so
    // we subtract TRACKED PEAK (not raw sample) from aim delta — naive
    // sampling misses the spike between two ticks. Decay drops the peak
    // after RECOIL_PEAK_DECAY_TICKS so it follows recovery.
    for (UINT32 i = 0; i < 3; i++) {
        UINT32 sample = ((UINT32 *)gLocal.PunchRaw)[i];
        UINT32 peak   = ((UINT32 *)gLocal.PunchPeak)[i];
        UINT64 ts     = gLocal.PunchPeakTick[i];
        UINT8 expired = (Tick - ts) > RECOIL_PEAK_DECAY_TICKS;
        if (expired) {
            ((UINT32 *)gLocal.PunchPeak)[i] = sample;
            gLocal.PunchPeakTick[i] = Tick;
        } else if (f_cmp_lt(f_abs(peak), f_abs(sample))) {
            ((UINT32 *)gLocal.PunchPeak)[i] = sample;
            gLocal.PunchPeakTick[i] = Tick;
        }
    }

    // View matrix capture — 64 floats from VIEW_RENDER deref + matrix offset.
    // Used for downstream W2S; HV doesn't render directly.
    UINT64 ViewPtr = 0;
    if (!EFI_ERROR(ReadGuestVirtual(Cr3, ImageBase + APEX_OFF_VIEW_RENDER,
                                     &ViewPtr, 8))
        && ViewPtr >= 0x10000) {
        ReadGuestVirtual(Cr3, ViewPtr + APEX_OFF_VIEW_MATRIX_DEREF,
                         gLocal.ViewMatrix, sizeof(gLocal.ViewMatrix));
        if (gDrawHook.CopiedBytes == HOOK_DRAW_BUF_BYTES) {
            ReadGuestVirtual(Cr3, ViewPtr + APEX_OFF_VIEW_MATRIX_DEREF,
                             gDrawHookHvBuf + DRAWBUF_OFF_VIEW_MATRIX_RAW, 256);
        }
        // Phase-4 diag — one-shot dump of the 16 ViewMatrix floats. Layout-
        // sanity for W2S debug (saturated NDC means matrix is wrong/transposed).
        static volatile UINT32 gViewMatrixDumped = 0;
        if (__atomic_exchange_n(&gViewMatrixDumped, 1, __ATOMIC_ACQ_REL) == 0) {
            HvLogHex("VPT", ViewPtr);
            UINT32 *vm = (UINT32 *)gLocal.ViewMatrix;
            for (UINT32 i = 0; i < 16; i++) {
                UINT64 Pack = ((UINT64)i << 32) | (UINT64)vm[i];
                HvLogHex("VMM", Pack);
            }
        }
    }

    return TRUE;
}

// ---------- active weapon ----------
static VOID ReadActiveWeapon(UINT64 Cr3, UINT64 ImageBase) {
    if (gLocal.Ent == 0) return;
    UINT32 Handle = 0;
    UINT64 WepEnt = 0;
    for (UINT32 s = 0; s < 2; s++) {
        if (EFI_ERROR(ReadGuestVirtual(Cr3,
                gLocal.Ent + APEX_ENT_WEAPON_HANDLE + s * 4,
                &Handle, 4))) continue;
        if (Handle == 0xFFFFFFFFUL || Handle == 0) continue;
        UINT32 Slot = Handle & 0xFFFFUL;
        if (Slot >= 0x4000) continue;
        UINT64 SlotVa = ImageBase + APEX_OFF_ENTITY_LIST
                                  + (UINT64)Slot * APEX_ENT_STRIDE;
        if (EFI_ERROR(ReadGuestVirtual(Cr3, SlotVa, &WepEnt, 8))) continue;
        if (WepEnt < 0x10000 || WepEnt >= 0x800000000000ULL) {
            WepEnt = 0;
            continue;
        }
        break;
    }
    if (WepEnt == 0) {
        gLocal.WeaponName[0] = 0;
        *(UINT32 *)&gLocal.ProjSpeed        = F32_ZERO;
        *(UINT32 *)&gLocal.ProjGravityScale = F32_ZERO;
        *(UINT32 *)&gLocal.InheritOwner     = F32_ZERO;
        return;
    }
    char NameBuf[32] = {0};
    ReadGuestVirtual(Cr3, WepEnt + APEX_ENT_WEAPON_NAME_STR, NameBuf, 32);
    if (NameBuf[0] != 'm') { gLocal.WeaponName[0] = 0; return; }
    CopyMem(gLocal.WeaponName, NameBuf, 32);

    UINT32 Speed = 0, GScale = 0, Inherit = 0;
    ReadGuestVirtual(Cr3, WepEnt + APEX_WEP_BULLET_SPEED,         &Speed,   4);
    ReadGuestVirtual(Cr3, WepEnt + APEX_WEP_BULLET_GRAVITY_SCALE, &GScale,  4);
    ReadGuestVirtual(Cr3, WepEnt + APEX_WEP_BULLET_INHERIT_OWNER, &Inherit, 4);
    *(UINT32 *)&gLocal.ProjSpeed        = Speed;
    *(UINT32 *)&gLocal.ProjGravityScale = GScale;
    *(UINT32 *)&gLocal.InheritOwner     = Inherit;

    if (!gEverSawWeapon) {
        gEverSawWeapon = 1;
        UINT64 Pack = 0;
        CopyMem(&Pack, NameBuf, 8);
        HvLogHex("EW0", Pack);
    }
}

// ---------- entity scan (one slot per tick) ----------
static VOID ScanOneEntity(UINT64 Cr3, UINT64 ImageBase, UINT32 Cursor, UINT64 Tick) {
    UINT64 SlotVa = ImageBase + APEX_OFF_ENTITY_LIST
                              + (UINT64)Cursor * APEX_ENT_STRIDE;
    UINT64 Ent = 0;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, SlotVa, &Ent, 8))
        || Ent < 0x10000 || Ent >= 0x800000000000ULL) {
        gEntCache[Cursor].Ent = 0;
        return;
    }
    UINT64 NameLo = 0;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, Ent + APEX_ENT_NAME, &NameLo, 8))) {
        gEntCache[Cursor].Ent = 0;
        return;
    }
    UINT8 IsPlayer = ((NameLo & APEX_NAME_PLAYER_MASK) == APEX_NAME_PLAYER) ? 1 : 0;
    UINT8 IsProp   = ((UINT32)NameLo == APEX_NAME_PROP_4) ? 1 : 0;
    if (!IsPlayer && !IsProp) {
        gEntCache[Cursor].Ent = 0;
        return;
    }

    ENT_SNAPSHOT *S = &gEntCache[Cursor];
    UINT8 IsRefresh = (S->Ent == Ent) ? 1 : 0;
    if (IsRefresh) {
        for (UINT32 i = 0; i < 3; i++) {
            ((UINT32 *)S->PrevOrigin)[i] = ((UINT32 *)S->Origin)[i];
        }
        S->PrevSeenTick = S->LastSeenTick;
    } else {
        for (UINT32 i = 0; i < 3; i++) {
            ((UINT32 *)S->PrevOrigin)[i] = F32_ZERO;
            ((UINT32 *)S->Origin)[i]     = F32_ZERO;
            ((UINT32 *)S->Velocity)[i]   = F32_ZERO;
        }
        S->PrevSeenTick = Tick;
        S->Ent = Ent;
    }

    ReadGuestVirtual(Cr3, Ent + APEX_ENT_ORIGIN, S->Origin, 12);

    S->HighlightId = 0xFF;
    ReadGuestVirtual(Cr3, Ent + APEX_ENT_HIGHLIGHT_ID, &S->HighlightId, 1);

    if (IsPlayer) {
        S->IsPlayer = 1;
        ReadGuestVirtual(Cr3, Ent + APEX_ENT_HEALTH,         &S->Health,    4);
        ReadGuestVirtual(Cr3, Ent + APEX_ENT_MAX_HEALTH,     &S->MaxHealth, 4);
        ReadGuestVirtual(Cr3, Ent + APEX_ENT_SHIELD,         &S->Shield,    4);
        ReadGuestVirtual(Cr3, Ent + APEX_ENT_MAX_SHIELD,     &S->MaxShield, 4);
        ReadGuestVirtual(Cr3, Ent + APEX_ENT_TEAM,           &S->Team,      4);
        ReadGuestVirtual(Cr3, Ent + APEX_ENT_ARMOR_TYPE,     &S->ArmorType, 4);
        UINT32 Life = 0, Bleed = 0, Decoy = 0;
        ReadGuestVirtual(Cr3, Ent + APEX_ENT_LIFE_STATE,     &Life,  4);
        ReadGuestVirtual(Cr3, Ent + APEX_ENT_BLEEDOUT_STATE, &Bleed, 4);
        ReadGuestVirtual(Cr3, Ent + APEX_ENT_DECOY_FLAGS,    &Decoy, 4);
        S->IsAlive  = (Life == 0 && S->Health > 0) ? 1 : 0;
        S->IsDowned = (Bleed != 0) ? 1 : 0;
        S->IsDecoy  = (Decoy != 0) ? 1 : 0;
        S->LootTier = APEX_LOOT_TIER_NONE;
    } else {
        S->IsPlayer  = 0;
        S->IsAlive   = 1;
        S->IsDowned  = 0;
        S->IsDecoy   = 0;
        UINT32 ItemId = 0;
        ReadGuestVirtual(Cr3, Ent + APEX_ENT_SURVIVAL_ITEM_ID, &ItemId, 4);
        S->LootTier = ClassifyLootByItemId(ItemId);
    }

    S->LastSeenTick = Tick;

    UINT64 Dt = (S->LastSeenTick > S->PrevSeenTick)
                ? (S->LastSeenTick - S->PrevSeenTick) : 1;
    if (IsRefresh && Dt > 0) {
        UINT32 DtBits = f_from_i32((INT32)Dt);
        for (UINT32 i = 0; i < 3; i++) {
            UINT32 dPos = f_sub(((UINT32 *)S->Origin)[i],
                                 ((UINT32 *)S->PrevOrigin)[i]);
            ((UINT32 *)S->Velocity)[i] = f_div(dPos, DtBits);
        }
    }

    if (gLocal.Ent != 0) {
        UINT32 dx = f_sub(((UINT32 *)S->Origin)[0], ((UINT32 *)gLocal.Camera)[0]);
        UINT32 dy = f_sub(((UINT32 *)S->Origin)[1], ((UINT32 *)gLocal.Camera)[1]);
        UINT32 dz = f_sub(((UINT32 *)S->Origin)[2], ((UINT32 *)gLocal.Camera)[2]);
        UINT32 d2 = f_add(f_add(f_mul(dx, dx), f_mul(dy, dy)), f_mul(dz, dz));
        *(UINT32 *)&S->DistSqToLocal = d2;
    }

#ifdef HID_SNAPSHOT_MODE
    // HID snapshot — only players, log on (Hid OR state) change. Bounded.
    if (IsPlayer && Cursor < HOOK_DRAW_CACHE_SIZE) {
        UINT8 Hid = 0xFF;
        if (!EFI_ERROR(ReadGuestVirtual(Cr3, Ent + APEX_ENT_HIGHLIGHT_ID,
                                         &Hid, 1))) {
            UINT8 SameTeam = (gLocal.Ent != 0 && S->Team == gLocal.Team) ? 1 : 0;
            UINT8 StateBits = (UINT8)(
                  (S->IsAlive   & 1)
                | ((S->IsDowned & 1) << 1)
                | ((S->IsDecoy  & 1) << 2)
                | ((SameTeam    & 1) << 3)
                | (((UINT8)(S->ArmorType & 0x0F)) << 4));
            UINT8 Init = gHidLastInit[Cursor];
            if (!Init
                || Hid != gHidLastVal[Cursor]
                || StateBits != gHidLastSt[Cursor]) {
                gHidLastVal [Cursor] = Hid;
                gHidLastSt  [Cursor] = StateBits;
                gHidLastInit[Cursor] = 1;
                UINT32 N = __atomic_fetch_add(&gHidLogged, 1, __ATOMIC_RELAXED);
                if (N < HID_LOG_MAX) {
                    UINT8 Hp = (S->Health > 255) ? 255
                              : (S->Health < 0)  ? 0
                              : (UINT8)S->Health;
                    UINT8 Sh = (S->Shield > 255) ? 255
                              : (S->Shield < 0)  ? 0
                              : (UINT8)S->Shield;
                    UINT64 Pack =
                          ((UINT64)Hid       & 0xFFULL)
                        | (((UINT64)StateBits & 0xFFULL) << 8)
                        | (((UINT64)Sh        & 0xFFULL) << 16)
                        | (((UINT64)Hp        & 0xFFULL) << 24)
                        | (((UINT64)(UINT32)Ent)        << 32);
                    HvLogHex("HSE", Pack);
                }
            }
        }
    }
#endif
}

// ---------- glow rendering (tier-coded highlight) ----------
static VOID RenderGlow(UINT64 Cr3, UINT64 ImageBase) {
    UINT64 HighlightSettings = 0;
    if (EFI_ERROR(ReadGuestVirtual(Cr3,
                                    ImageBase + APEX_OFF_HIGHLIGHT_SETTINGS,
                                    &HighlightSettings, 8))
        || HighlightSettings < 0x10000) return;

    InitHighlightSlot(Cr3, HighlightSettings, APEX_SLOT_PLAYER,
                       APEX_F32_ONE,     APEX_F32_ZERO,    APEX_F32_ZERO);
    InitHighlightSlot(Cr3, HighlightSettings, APEX_SLOT_LOOT_MYTHIC,
                       APEX_F32_ONE,     APEX_F32_HALF,    APEX_F32_ZERO);
    InitHighlightSlot(Cr3, HighlightSettings, APEX_SLOT_LOOT_LEGENDARY,
                       APEX_F32_HALF,    APEX_F32_ZERO,    APEX_F32_ONE);
    InitHighlightSlot(Cr3, HighlightSettings, APEX_SLOT_LOOT_EPIC,
                       APEX_F32_QUARTER, APEX_F32_ZERO,    APEX_F32_HALF);
    InitHighlightSlot(Cr3, HighlightSettings, APEX_SLOT_LOOT_RARE,
                       APEX_F32_ZERO,    APEX_F32_HALF,    APEX_F32_ONE);
    InitHighlightSlot(Cr3, HighlightSettings, APEX_SLOT_LOOT_AMMO,
                       APEX_F32_HALF,    APEX_F32_HALF,    APEX_F32_HALF);

    UINT8 GlowSlot   = gGlowParams.Slot;
    UINT8 GlowFilter = gGlowParams.FilterMode;
    UINT8 GlowOn     = gGlowParams.Enabled;
    BOOLEAN PlayerGlowOff = (!GlowOn) || (GlowFilter == 3);

    for (UINT32 i = 0; i < HOOK_DRAW_CACHE_SIZE; i++) {
        ENT_SNAPSHOT *S = &gEntCache[i];
        if (S->Ent == 0) continue;
        UINT8 Slot = 0;
        if (S->IsPlayer) {
            if (PlayerGlowOff) continue;
            if (!S->IsAlive)   continue;
            if (S->IsDecoy)    continue;
            BOOLEAN SameTeam = (gLocal.Ent != 0 && S->Team == gLocal.Team);
            // 0=enemy_only 1=all 2=teammates_only
            if (GlowFilter == 0 && SameTeam) continue;
            if (GlowFilter == 2 && !SameTeam) continue;
            Slot = GlowSlot;
        } else {
            Slot = SlotForTier(S->LootTier);
            if (Slot == 0) continue;
        }
        WriteEntityGlow(Cr3, S->Ent, Slot);
    }
}

// ---------- Phase-4 precursor: W2S projection probe ----------
// Projects each alive-enemy ENT_SNAPSHOT through gLocal.ViewMatrix to NDC
// space (row-major, world*view*proj combined per Source-engine convention).
// Logs W2P = (ndc.y_q15 << 48) | (ndc.x_q15 << 32) | ent_lo32 per entity,
// bounded by W2P_LOG_MAX so the ring isn't drowned.  No game-state writes,
// no NPT changes — purely observational. Validates the math half of Phase 4
// before the trampoline-side emit half (Phase 3b/3c) lands.
#define W2P_LOG_MAX 128
static volatile UINT32 gW2pLogged = 0;

static BOOLEAN ProjectW2S(const UINT32 *vm, const UINT32 *world,
                          UINT32 *out_ndcx, UINT32 *out_ndcy) {
    UINT32 cx = f_add(f_add(f_mul(vm[ 0], world[0]), f_mul(vm[ 1], world[1])),
                      f_add(f_mul(vm[ 2], world[2]), vm[ 3]));
    UINT32 cy = f_add(f_add(f_mul(vm[ 4], world[0]), f_mul(vm[ 5], world[1])),
                      f_add(f_mul(vm[ 6], world[2]), vm[ 7]));
    UINT32 cw = f_add(f_add(f_mul(vm[12], world[0]), f_mul(vm[13], world[1])),
                      f_add(f_mul(vm[14], world[2]), vm[15]));
    if ((cw & 0x80000000UL) || f_cmp_lt(cw, 0x3C23D70AUL /* 0.01f */)) {
        return FALSE;
    }
    *out_ndcx = f_div(cx, cw);
    *out_ndcy = f_div(cy, cw);
    return TRUE;
}

static VOID ProbeGlowW2S(VOID) {
    if (__atomic_load_n(&gW2pLogged, __ATOMIC_RELAXED) >= W2P_LOG_MAX) return;
    if (gLocal.Ent == 0) return;
    for (UINT32 i = 0; i < HOOK_DRAW_CACHE_SIZE; i++) {
        ENT_SNAPSHOT *S = &gEntCache[i];
        if (S->Ent == 0 || !S->IsPlayer) continue;
        if (!S->IsAlive || S->IsDowned || S->IsDecoy) continue;
        if (S->Team == gLocal.Team) continue;
        UINT32 ndcx = 0, ndcy = 0;
        if (!ProjectW2S((const UINT32 *)gLocal.ViewMatrix,
                        (const UINT32 *)S->Origin,
                        &ndcx, &ndcy)) continue;
        UINT32 N = __atomic_fetch_add(&gW2pLogged, 1, __ATOMIC_RELAXED);
        if (N >= W2P_LOG_MAX) break;
        INT16 qx = f_to_q15(ndcx);
        INT16 qy = f_to_q15(ndcy);
        UINT64 Pack =
              ((UINT64)(UINT32)S->Ent)
            | (((UINT64)(UINT16)qx) << 32)
            | (((UINT64)(UINT16)qy) << 48);
        HvLogHex("W2P", Pack);
    }
}

// ---------- per-tick orchestration ----------
static VOID DrawHookPayloadTick(PVCPU_DATA Vcpu) {
    UINT64 Fired = __atomic_add_fetch(&gPayloadTicksFired, 1, __ATOMIC_RELAXED);
    if ((Fired & 0x7) == 1) HvLogHex("DPL", Fired);

    UINT64 Cr3       = Vcpu->TargetCr3;
    UINT64 ImageBase = Vcpu->Cr3InterceptCapturedImageBase;
    if (Cr3 == 0 || ImageBase == 0) {
        if ((Fired & 0x7) == 1) HvLogHex("DPC", (Cr3 ? 0 : 1) | (ImageBase ? 0 : 2));
        return;
    }

    BOOLEAN HasLocal = ReadLocalPlayer(Cr3, ImageBase, Fired);
    if (HasLocal && (Fired & 0x7) == 0) ReadActiveWeapon(Cr3, ImageBase);

    // PEX/Overlay/players.h walker — read all 128 entity slots EVERY tick
    // (no cursor sliding). Cache is fully fresh after one NPF instead of 16.
    for (UINT32 Cursor = 0; Cursor < HOOK_DRAW_CACHE_SIZE; Cursor++) {
        ScanOneEntity(Cr3, ImageBase, Cursor, Fired);
    }

    // RenderGlow doesn't strictly need LocalPlayer — gLocal.Team filter
    // becomes a no-op when gLocal.Ent=0 (all alive players glow). Keep this
    // path live so glow works while LOCAL_PLAYER offset is stale post-patch.
    RenderGlow(Cr3, ImageBase);
    if (HasLocal) HypeAimTriggerTick(Cr3, ImageBase, Fired);
    if (HasLocal) MenuTick(Cr3, ImageBase, Fired);
    if (HasLocal) ProbeGlowW2S();

    MirrorSnapshots(Fired);

    // Diff-probe (cycle 9j+) — diagnostic; gated to one entity per 8 ticks.
    if ((Fired & 0x7) == 1) {
        for (UINT32 i = 0; i < HOOK_DRAW_CACHE_SIZE; i++) {
            ENT_SNAPSHOT *S = &gEntCache[i];
            if (S->Ent == 0 || !S->IsPlayer) continue;
            UINT32 Buf[16] = {0};
            if (EFI_ERROR(ReadGuestVirtual(Cr3, S->Ent + 0x250,
                                            Buf, sizeof(Buf)))) continue;
            for (UINT32 k = 0; k < 16; k++) {
                UINT64 Tag = ((UINT64)i << 56) | ((UINT64)k << 48) | (UINT64)Buf[k];
                HvLogHex("DV0", Tag);
            }
            break;
        }
    }
}

// Cycle 11e Phase-3a — per-NPF wrapper-A ring sampler (plan §H milestone 3a).
//
// Each NPF on the hooked page: direct-read *(wrapper_a) via the per-frame
// ctx-ring (same path as the install-time probe in HookDrawHandleInstall).
// Stores the vtable VA in a 64-entry ring; on the 64th sample, flushes all
// entries as an "EA2" burst then stops permanently. Pass criterion: ≥60 of
// 64 samples land in d3d12.dll's image range — proves wrapper_a is stable
// in-image across frames before committing to Phase-3b's NPT-split detour.
static VOID DrawHookSampleWrapperRing(PVCPU_DATA Vcpu) {
    if (__atomic_load_n(&gWrapperRingDone, __ATOMIC_ACQUIRE)) return;

    UINT64 Cr3 = Vcpu->TargetCr3;
    UINT64 ImageBase = Vcpu->Cr3InterceptCapturedImageBase;
    if (Cr3 == 0 || ImageBase == 0) return;

    UINT64 RingBase = 0;
    UINT32 FrameIdx = 0, RingSize = 0;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, ImageBase + 0x1F6AE78ULL, &RingBase, 8)) ||
        EFI_ERROR(ReadGuestVirtual(Cr3, ImageBase + 0x1F6AE4CULL, &FrameIdx, 4)) ||
        EFI_ERROR(ReadGuestVirtual(Cr3, ImageBase + 0x1F6AE30ULL, &RingSize, 4))) return;
    if (RingBase < 0x10000ULL || RingBase >= 0x0000800000000000ULL) return;
    if (RingSize == 0 || FrameIdx >= RingSize) return;

    UINT64 Ctx = 0;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, RingBase + (UINT64)FrameIdx * 8ULL, &Ctx, 8))) return;
    if (Ctx < 0x10000ULL || Ctx >= 0x0000800000000000ULL) return;

    UINT64 WrapperA = 0;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, Ctx + 0x5EF0ULL, &WrapperA, 8))) return;
    if (WrapperA < 0x10000ULL || WrapperA >= 0x0000800000000000ULL) return;

    UINT64 Vt = 0;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, WrapperA, &Vt, 8)) || Vt == 0) return;

    UINT32 Idx = __atomic_fetch_add(&gWrapperRingIdx, 1, __ATOMIC_RELAXED);
    if (Idx >= WRAPPER_RING_SIZE) return;
    gWrapperRing[Idx]     = Vt;
    gWrapperRingInst[Idx] = WrapperA;

    if (Idx + 1 == WRAPPER_RING_SIZE) {
        for (UINT32 i = 0; i < WRAPPER_RING_SIZE; i++) {
            HvLogHex("EA2", gWrapperRing[i]);
        }
        for (UINT32 i = 0; i < WRAPPER_RING_SIZE; i++) {
            HvLogHex("EAW", gWrapperRingInst[i]);
        }
        __atomic_store_n(&gWrapperRingDone, 1, __ATOMIC_RELEASE);
    }
}

VOID DrawHookOnNpfHit(PVCPU_DATA Vcpu) {
    PVMCB Vmcb = Vcpu->Vmcb;
    UINT64 Gpa = __atomic_load_n(&gDrawHookGpa, __ATOMIC_ACQUIRE);
    if (Gpa == 0) return;

    PNPT_CONTEXT Npt = &g_DriverContext.NptContext;
    UINT64 *Pte = NptGetPte(Npt, Gpa, FALSE);
    if (Pte && !(*Pte & NPT_LARGE_PAGE)) {
        __atomic_fetch_and(Pte, ~(UINT64)NPT_NX, __ATOMIC_ACQ_REL);
    }

    __atomic_fetch_add(&gDrawHookNpfTotal, 1, __ATOMIC_RELAXED);
    HvLog("DR0");
    Vcpu->FrameCount++;

    DrawHookSampleWrapperRing(Vcpu);

    UINT64 Tick = __atomic_add_fetch(&gPayloadTickCount, 1, __ATOMIC_RELAXED);
    if ((Tick & HOOK_DRAW_PAYLOAD_MASK) == 0) {
        DrawHookPayloadTick(Vcpu);
    }

    Vmcb->Control.TlbControl = 1;
}

UINT64 DrawHookSampleNpfTotal(VOID) {
    return __atomic_exchange_n(&gDrawHookNpfTotal, 0, __ATOMIC_ACQ_REL);
}

UINT64 DrawHookCurrentGpa(VOID) {
    return __atomic_load_n(&gDrawHookGpa, __ATOMIC_ACQUIRE);
}

VOID DrawHookOnDbRestore(PVCPU_DATA Vcpu) { (VOID)Vcpu; }

UINT64 DrawHookRearm(VOID) {
    UINT64 Gpa = __atomic_load_n(&gDrawHookGpa, __ATOMIC_ACQUIRE);
    if (Gpa == 0) return 0;
    PNPT_CONTEXT Npt = &g_DriverContext.NptContext;
    UINT64 *Pte = NptGetPte(Npt, Gpa, FALSE);
    if (!Pte || (*Pte & NPT_LARGE_PAGE) || (*Pte & NPT_NX)) return 0;
    __atomic_fetch_or(Pte, NPT_NX, __ATOMIC_ACQ_REL);
    UINT32 N = g_DriverContext.HvState.NumCpus;
    for (UINT32 i = 0; i < N; i++) {
        PVCPU_DATA V = &g_DriverContext.HvState.VcpuTable[i];
        if (V->Vmcb) V->Vmcb->Control.TlbControl = 1;
    }
    return Gpa;
}

UINT32 HookDrawHandlePeek(PVCPU_DATA Vcpu, COVERT_CMD *Cmd) {
    (VOID)Vcpu;
    UINT64 Off = Cmd->Arg1;
    if (gDrawHook.CopiedBytes != HOOK_DRAW_BUF_BYTES) {
        Cmd->Result = 0;
        return COVERT_STATUS_BAD_ADDR;
    }
    if (Off + sizeof(UINT64) > HOOK_DRAW_BUF_BYTES) {
        Cmd->Result = 0;
        return COVERT_STATUS_BAD_ADDR;
    }
    UINT64 V = 0;
    CopyMem(&V, gDrawHookHvBuf + Off, sizeof(UINT64));
    Cmd->Result = V;
    return COVERT_STATUS_OK;
}

// PMC_CMD_SET_GLOW_PARAMS — pack two qwords of knobs into gGlowParams.
// Arg1 layout:
//   [63:56] WriteGlowFix  [55:48] WriteVisType
//   [47:40] GlowFix       [39:32] VisType
//   [31:24] Enabled       [23:16] FilterMode
//   [15:8]  Mask          [7:0]   Slot
UINT32 HookDrawHandleSetGlowParams(PVCPU_DATA Vcpu, COVERT_CMD *Cmd) {
    (VOID)Vcpu;
    UINT64 A = Cmd->Arg1;
    gGlowParams.Slot         = (UINT8)( A        & 0xFF);
    gGlowParams.Mask         = (UINT8)((A >>  8) & 0xFF);
    gGlowParams.FilterMode   = (UINT8)((A >> 16) & 0xFF);
    gGlowParams.Enabled      = (UINT8)((A >> 24) & 0xFF);
    gGlowParams.VisType      = (UINT8)((A >> 32) & 0xFF);
    gGlowParams.GlowFix      = (UINT8)((A >> 40) & 0xFF);
    gGlowParams.WriteVisType = (UINT8)((A >> 48) & 0xFF);
    gGlowParams.WriteGlowFix = (UINT8)((A >> 56) & 0xFF);
    HvLogHex("GP0", A);
    Cmd->Result = A;
    return COVERT_STATUS_OK;
}

UINT32 HookDrawHandleInstall(PVCPU_DATA Vcpu, COVERT_CMD *Cmd) {
    if (gDrawHookInstalled >= HOOK_DRAW_MAX_INSTALLS) {
        HvLogHex("DR2", (UINT64)gDrawHookInstalled);
        Cmd->Result = 0;
        return COVERT_STATUS_BAD_CMD;
    }

    UINT64 HookVa     = Cmd->Arg1;
    UINT64 ScratchGva = Cmd->Arg2;

    HvLogHex("DR1", HookVa & 0xFFFFFFFFULL);
    HvLogHex("DR3", ScratchGva);

    if (HookVa == 0 || ScratchGva == 0 || (ScratchGva & 0xFFFULL) != 0) {
        HvLogHex("DR5", ScratchGva);
        Cmd->Result = 0;
        return COVERT_STATUS_BAD_ADDR;
    }

    UINT32 PageCount   = 0;
    UINT32 CopiedBytes = 0;
    UINT64 InstallerCr3 = Vcpu->Vmcb->Save.Cr3;
    for (UINT32 i = 0; i < HOOK_DRAW_BUF_PAGES; i++) {
        UINT64 Va = ScratchGva + (UINT64)i * 0x1000ULL;
        UINT64 Pa = 0;
        EFI_STATUS St = TranslateGuestVirtual(InstallerCr3, Va, &Pa);
        if (EFI_ERROR(St)) {
            HvLogHex("DR6", Va);
            Cmd->Result = 0;
            return COVERT_STATUS_BAD_ADDR;
        }
        gDrawHook.GpaList[i] = Pa;
        PageCount++;
        St = ReadGuestPhysical(Pa, gDrawHookHvBuf + i * 0x1000ULL, 0x1000);
        if (EFI_ERROR(St)) {
            HvLogHex("DR8", Pa);
            Cmd->Result = 0;
            return COVERT_STATUS_BAD_ADDR;
        }
        CopiedBytes += 0x1000;
    }
    HvLogHex("DR4", (gDrawHook.GpaList[0] & 0xFFFFFFFFFFFFF000ULL)
                    | (UINT64)PageCount);

    UINT64 sentinel = 0;
    CopyMem(&sentinel, gDrawHookHvBuf, sizeof(sentinel));
    if (sentinel != 0x5045584550455845ULL) HvLogHex("DR7", sentinel);

    gDrawHook.PageCount   = PageCount;
    gDrawHook.CopiedBytes = CopiedBytes;

    // Hard-fail anchor check: read the first 8 bytes at the canon
    // render-gate fn RVA BEFORE arming the NPT exec trap. If the
    // prologue doesn't match the dumper-pinned signature, the canon
    // has drifted (e.g. +0xF00 like 2026-05-13) and we'd be NX-trapping
    // garbage — abort cleanly instead of silently corrupting timing.
    // ImageBase==0 means CR3 intercept hasn't captured yet; in that
    // case skip the check (legacy non-fatal behavior).
    UINT64 ImageBase = Vcpu->Cr3InterceptCapturedImageBase;
    if (ImageBase != 0) {
        UINT64 Prologue = 0;
        if (EFI_ERROR(ReadGuestVirtual(Vcpu->TargetCr3,
                ImageBase + APEX_OFF_RENDER_GATE_FN,
                &Prologue, sizeof(Prologue)))) {
            HvLogHex("DRR", ImageBase + APEX_OFF_RENDER_GATE_FN);
            Cmd->Result = 0;
            return COVERT_STATUS_BAD_ADDR;
        }
        HvLogHex("DRG", Prologue);
        if (Prologue != APEX_RENDER_GATE_FN_PROLOGUE_LE64) {
            HvLogHex("DRX", APEX_RENDER_GATE_FN_PROLOGUE_LE64);
            Cmd->Result = 0;
            return COVERT_STATUS_BAD_ADDR;
        }
    }

    EFI_STATUS ArmSt = DrawHookArmExecTrap(Vcpu, HookVa);
    if (EFI_ERROR(ArmSt)) {
        Cmd->Result = (UINT64)ArmSt;
        return COVERT_STATUS_BAD_ADDR;
    }

    // Cycle 11e — wrapper-A direct-read via per-frame ctx-ring (per §G
    // follow-ups). Ring base is a pointer stored at the static slot, not
    // an inline array. Three flat statics:
    //   ring  = *(u64)(ImageBase + 0x1F6AE78)   ; heap-allocated ctx*[] base
    //   idx   = *(u32)(ImageBase + 0x1F6AE4C)   ; per-frame writer @ 0x54E8B9
    //   size  = *(u32)(ImageBase + 0x1F6AE30)   ; mod divisor (init only)
    // Guards: ring != 0, idx < size. Then ctx = *(ring + idx*8), then
    // wrapper_a = *(ctx + 0x5EF0), then vtable = *wrapper_a.
    //
    // EI0 = anchor RIP, EI1 = frame_idx, EI2 = ring base, EI3 = ring size,
    // EI4 = ctx VA. Missing EI3 = ring/size read failed; missing EI4 =
    // ctx slot deref failed; missing EA2 = ctx+0x5EF0 read failed.
    if (ImageBase != 0 && Vcpu->TargetCr3 != 0) {
        UINT64 RingBase = 0;
        UINT32 FrameIdx = 0;
        UINT32 RingSize = 0;
        if (!EFI_ERROR(ReadGuestVirtual(Vcpu->TargetCr3,
                ImageBase + 0x1F6AE78ULL, &RingBase, 8))
            && !EFI_ERROR(ReadGuestVirtual(Vcpu->TargetCr3,
                ImageBase + 0x1F6AE4CULL, &FrameIdx, 4))
            && !EFI_ERROR(ReadGuestVirtual(Vcpu->TargetCr3,
                ImageBase + 0x1F6AE30ULL, &RingSize, 4))) {
            HvLogHex("EI0", ImageBase + 0x54E9C7ULL);
            HvLogHex("EI1", (UINT64)FrameIdx);
            HvLogHex("EI2", RingBase);
            HvLogHex("EI3", (UINT64)RingSize);
            if (RingBase >= 0x10000ULL
                && RingBase < 0x0000800000000000ULL
                && RingSize > 0
                && FrameIdx < RingSize) {
                UINT64 Ctx = 0;
                if (!EFI_ERROR(ReadGuestVirtual(Vcpu->TargetCr3,
                        RingBase + (UINT64)FrameIdx * 8ULL, &Ctx, 8))
                    && Ctx >= 0x10000ULL
                    && Ctx < 0x0000800000000000ULL) {
                    HvLogHex("EI4", Ctx);
                    UINT64 WrapperA = 0;
                    if (!EFI_ERROR(ReadGuestVirtual(Vcpu->TargetCr3,
                            Ctx + 0x5EF0ULL, &WrapperA, 8))
                        && WrapperA >= 0x10000ULL
                        && WrapperA < 0x0000800000000000ULL) {
                        UINT64 Qw[8] = {0};
                        EFI_STATUS RdSt = EFI_SUCCESS;
                        for (UINT32 i = 0; i < 8 && !EFI_ERROR(RdSt); i++) {
                            RdSt = ReadGuestVirtual(Vcpu->TargetCr3,
                                    WrapperA + i * 8ULL, &Qw[i], 8);
                        }
                        if (!EFI_ERROR(RdSt) && Qw[0] != 0) {
                            HvLogHex("EA1", WrapperA);
                            HvLogHex("EA2", Qw[0]);
                            HvLogHex("EA3", Qw[1]);
                            HvLogHex("EA4", Qw[2]);
                            HvLogHex("EA5", Qw[3]);
                            HvLogHex("EA6", Qw[4]);
                            HvLogHex("EA7", Qw[5]);
                            HvLogHex("EA8", Qw[6]);
                            HvLogHex("EA9", Qw[7]);
                            UINT64 Vt = Qw[0];
                            const UINT32 SlotOffs[8] = {
                                0x00, 0x08, 0x10, 0x40, 0x48, 0xA8, 0xD0, 0xE8
                            };
                            UINT64 Slot[8] = {0};
                            for (UINT32 i = 0; i < 8; i++) {
                                if (EFI_ERROR(ReadGuestVirtual(Vcpu->TargetCr3,
                                        Vt + SlotOffs[i], &Slot[i], 8))) break;
                            }
                            HvLogHex("ES0", Slot[0]);
                            HvLogHex("ES1", Slot[1]);
                            HvLogHex("ES2", Slot[2]);
                            HvLogHex("ES3", Slot[3]);
                            HvLogHex("ES4", Slot[4]);
                            HvLogHex("ES5", Slot[5]);
                            HvLogHex("ES6", Slot[6]);
                            HvLogHex("ES7", Slot[7]);
                            __atomic_store_n(&gWrapperProbed, 1,
                                             __ATOMIC_RELEASE);
                        }
                    }
                }
            }
        }
    }

    Cmd->Result = 0;
    gDrawHookInstalled++;
    return COVERT_STATUS_OK;
}

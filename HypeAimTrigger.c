// HypeAimTrigger.c — aim + auto-fire, shared target-pick path.
// Extracted from HypeHookDraw.c (cycle 9m DoAim) + new DoTrigger.
// Both features disabled at boot; PMC_CMD_SET_AIM_PARAMS (0x21) enables.

#include "HypeAimTrigger.h"
#include "HypeHookDraw.h"
#include "HypeMenu.h"
#include "HypeContext.h"
#include "HypeDebug.h"
#include "HypeMemory.h"
#include "HypeApexCanon.h"

// gEntCache and gLocal are defined (non-static) in HypeHookDraw.c.
extern ENT_SNAPSHOT   gEntCache[];
extern LOCAL_SNAPSHOT gLocal;

// =====================================================================
// SSE float helpers — duplicated from HypeHookDraw.c.
// Static inlines can't link across TUs; each TU gets its own copy.
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
// Convert UINT8 integer → float bits (used for q4.4 → degrees).
static inline UINT32 f_from_u8(UINT8 v) {
    UINT32 r; INT32 vi = (INT32)v;
    __asm__ volatile ("cvtsi2ss %1,%%xmm0; movd %%xmm0,%0"
                      : "=r"(r) : "r"(vi) : "xmm0");
    return r;
}

#pragma GCC pop_options

#define F32_ZERO     0x00000000UL
#define F32_ONE      0x3F800000UL
#define F32_PI       0x40490FDBUL
#define F32_PI_2     0x3FC90FDBUL
#define F32_RAD2DEG  0x42652EE0UL
#define F32_180      0x43340000UL
#define F32_360      0x43B40000UL
#define F32_NEG_180  0xC3340000UL
#define F32_89       0x42B20000UL
#define F32_NEG_89   0xC2B20000UL
// q4.4 fixed-point scale: multiply by 0x3D800000 (0.0625f) to get degrees.
#define F32_Q4_4_SCALE 0x3D800000UL

#pragma GCC push_options
#pragma GCC target("sse2")

static UINT32 f_atan_unit(UINT32 z) {
    UINT32 z2 = f_mul(z, z);
    UINT32 c0 = 0x3F7FFE83UL;
    UINT32 c1 = 0xBEAA48F8UL;
    UINT32 c2 = 0x3E462DAEUL;
    UINT32 c3 = 0xBDEE7240UL;
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
        if (x & 0x80000000UL)
            r = (y & 0x80000000UL) ? f_sub(r, F32_PI) : f_add(r, F32_PI);
    } else {
        UINT32 q   = f_atan_unit(f_div(x, y));
        UINT32 pi2 = (y & 0x80000000UL) ? f_neg(F32_PI_2) : F32_PI_2;
        r = f_sub(pi2, q);
    }
    return r;
}
static inline UINT32 f_atan2_deg(UINT32 y, UINT32 x) {
    return f_mul(f_atan2(y, x), F32_RAD2DEG);
}

#pragma GCC pop_options

// ReadGuestVirtual / WriteGuestVirtual moved to HypeMemory.h (shared with HypeMenu).

// =====================================================================
// Module state
// =====================================================================

AIM_STATE                   gAim;       // non-static — mirrored by MirrorSnapshots
static TRIGGER_STATE        gTrigger;
volatile AIM_TRIGGER_PARAMS gAimTriggerParams = {
    .TriggerEnabled       = 0,
    .AimEnabled           = 0,
    .TriggerFovQ4_4       = 0x0B,   // ~0.69°
    .TriggerThreshQ4_4    = 0x05,   // ~0.31°
    .TriggerDebounceTicks = 12,
    .AimFovQ4_4           = 0x40,   // 4.0°
};

static UINT8  gEverWroteAngle  = 0;
static UINT8  gEverEngagedAim  = 0;
static UINT8  gTrigEverWrote   = 0;    // ET0 — first kbutton write
static UINT8  gTrigEverEngaged = 0;    // ET1 — first armed (on-target) cycle

// studioHdr -> head-bone index cache.
#define STUDIO_CACHE_SLOTS 16
static UINT64 gStudioHdrCacheKey[STUDIO_CACHE_SLOTS];
static UINT16 gStudioHdrCacheVal[STUDIO_CACHE_SLOTS];
static UINT8  gStudioHdrCacheOk[STUDIO_CACHE_SLOTS];

static BOOLEAN IsHeadNameToken(const CHAR8 *name, UINT32 max_len) {
    // Match common Source/Apex bone name variants:
    // "head", "j_head", "bip_head", "Bip01 Head", "HEAD"
    for (UINT32 i = 0; i < max_len && name[i] != '\0'; ++i) {
        CHAR8 c0 = name[i];
        if (c0 >= 'A' && c0 <= 'Z') c0 = (CHAR8)(c0 - 'A' + 'a');
        if (c0 != 'h') continue;
        if (i + 3 >= max_len) continue;
        CHAR8 c1 = name[i + 1], c2 = name[i + 2], c3 = name[i + 3];
        if (c1 >= 'A' && c1 <= 'Z') c1 = (CHAR8)(c1 - 'A' + 'a');
        if (c2 >= 'A' && c2 <= 'Z') c2 = (CHAR8)(c2 - 'A' + 'a');
        if (c3 >= 'A' && c3 <= 'Z') c3 = (CHAR8)(c3 - 'A' + 'a');
        if (c1 == 'e' && c2 == 'a' && c3 == 'd') return TRUE;
    }
    return FALSE;
}

// Parse CStudioHdr and try to resolve a model-specific head bone index.
// Returns FALSE when parsing fails; caller should fallback to APEX_BONE_HEAD_IDX.
static BOOLEAN ResolveHeadBoneIndex(UINT64 Cr3, UINT64 studio_hdr, UINT32 *out_idx) {
    if (Cr3 == 0 || studio_hdr < 0x10000 || out_idx == NULL) return FALSE;

    UINT32 slot = (UINT32)((studio_hdr >> 4) & (STUDIO_CACHE_SLOTS - 1));
    if (gStudioHdrCacheOk[slot] && gStudioHdrCacheKey[slot] == studio_hdr) {
        *out_idx = (UINT32)gStudioHdrCacheVal[slot];
        return TRUE;
    }

    // Source1 studiohdr_t canonical offsets:
    // 0x9C = numbones, 0xA0 = boneindex
    INT32 numbones = 0;
    INT32 boneindex = 0;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, studio_hdr + 0x9C, &numbones, sizeof(numbones))))
        return FALSE;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, studio_hdr + 0xA0, &boneindex, sizeof(boneindex))))
        return FALSE;
    if (numbones <= 0 || numbones > 256) return FALSE;
    if (boneindex <= 0 || boneindex > 0x40000) return FALSE;

    UINT64 bones_base = studio_hdr + (UINT64)(UINT32)boneindex;
    static const UINT32 kBoneStrideCandidates[] = { 0xD8, 0xE0, 0xF0, 0xB0 };
    for (UINT32 si = 0; si < (sizeof(kBoneStrideCandidates) / sizeof(kBoneStrideCandidates[0])); ++si) {
        UINT32 stride = kBoneStrideCandidates[si];
        for (INT32 i = 0; i < numbones; ++i) {
            UINT64 bone = bones_base + (UINT64)(UINT32)i * (UINT64)stride;
            INT32 name_off = 0;
            if (EFI_ERROR(ReadGuestVirtual(Cr3, bone + 0x00, &name_off, sizeof(name_off))))
                continue;
            if (name_off <= 0 || name_off > 0x200) continue;

            CHAR8 name[64];
            SetMem(name, sizeof(name), 0);
            if (EFI_ERROR(ReadGuestVirtual(Cr3, bone + (UINT64)(UINT32)name_off,
                                           name, sizeof(name) - 1)))
                continue;
            if (IsHeadNameToken(name, sizeof(name))) {
                *out_idx = (UINT32)i;
                gStudioHdrCacheKey[slot] = studio_hdr;
                gStudioHdrCacheVal[slot] = (UINT16)i;
                gStudioHdrCacheOk[slot]  = 1;
                return TRUE;
            }
        }
    }
    return FALSE;
}

// Read head-bone world position from C_BaseAnimating.
// Fallback is caller-side origin+Z offset when this probe fails.
static BOOLEAN TryGetHeadBonePos(UINT64 Cr3, UINT64 Ent,
                                 UINT32 *out_x, UINT32 *out_y, UINT32 *out_z) {
    if (Cr3 == 0 || Ent < 0x10000 || out_x == NULL || out_y == NULL || out_z == NULL)
        return FALSE;

    UINT64 studio_hdr = 0;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, Ent + APEX_ENT_STUDIO_HDR,
                                   &studio_hdr, sizeof(studio_hdr))))
        return FALSE;
    if (studio_hdr < 0x10000 || studio_hdr >= 0x800000000000ULL)
        return FALSE;

    UINT64 bones = 0;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, Ent + APEX_ENT_BONES, &bones, sizeof(bones))))
        return FALSE;
    if (bones < 0x10000 || bones >= 0x800000000000ULL)
        return FALSE;

    UINT32 head_idx = (UINT32)APEX_BONE_HEAD_IDX;
    (VOID)ResolveHeadBoneIndex(Cr3, studio_hdr, &head_idx);

    UINT64 head = bones + ((UINT64)head_idx * (UINT64)APEX_BONE_STRIDE);
    UINT32 x = 0, y = 0, z = 0;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, head + 0x0C, &x, sizeof(x)))) return FALSE;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, head + 0x1C, &y, sizeof(y)))) return FALSE;
    if (EFI_ERROR(ReadGuestVirtual(Cr3, head + 0x2C, &z, sizeof(z)))) return FALSE;
    if (x == 0 && y == 0 && z == 0) return FALSE;

    *out_x = x;
    *out_y = y;
    *out_z = z;
    return TRUE;
}

// =====================================================================
// PickBestTarget — extracted from DoAim (lines 769-826 of old HypeHookDraw.c).
// Shared by DoAim and DoTrigger; caller passes the appropriate FOV cone.
// =====================================================================
static UINT64 PickBestTarget(UINT64 Cr3, UINT32 fov_max_bits,
                              UINT32 cam_x, UINT32 cam_y, UINT32 cam_z,
                              UINT32 comp_p, UINT32 comp_y_bits,
                              UINT32 max_d2, UINT32 min_d2,
                              UINT32 *out_tp, UINT32 *out_ty,
                              UINT32 *out_dist, UINT32 *out_fov) {
    UINT32 fov_max2   = f_mul(fov_max_bits, fov_max_bits);
    UINT32 best_score = 0x7F800000UL;  // +inf
    UINT64 best_ent   = 0;
    UINT32 best_tp    = F32_ZERO;
    UINT32 best_ty    = F32_ZERO;
    UINT32 best_dist  = F32_ZERO;
    UINT32 best_fov   = F32_ZERO;

    for (UINT32 i = 0; i < HOOK_DRAW_CACHE_SIZE; i++) {
        ENT_SNAPSHOT *S = &gEntCache[i];
        if (S->Ent == 0 || !S->IsPlayer || !S->IsAlive) continue;
        if (S->IsDecoy || S->IsDowned)                  continue;
        if (S->Team == gLocal.Team)                     continue;

        UINT32 d2 = *(UINT32 *)&S->DistSqToLocal;
        if (f_cmp_lt(max_d2, d2)) continue;
        if (f_cmp_lt(d2, min_d2)) continue;

        UINT32 dist = f_sqrt(d2);
        if (dist == 0) continue;

        UINT32 ps_bits = *(UINT32 *)&gLocal.ProjSpeed;
        UINT32 t = (ps_bits != 0 && (ps_bits & 0x80000000UL) == 0)
                   ? f_div(dist, ps_bits)
                   : F32_ZERO;
        UINT32 base_x = ((UINT32 *)S->Origin)[0];
        UINT32 base_y = ((UINT32 *)S->Origin)[1];
        UINT32 base_z = ((UINT32 *)S->Origin)[2];
        BOOLEAN head_ok = TryGetHeadBonePos(Cr3, S->Ent, &base_x, &base_y, &base_z);

        UINT32 lead_x = f_add(base_x,
                              f_mul(((UINT32 *)S->Velocity)[0], t));
        UINT32 lead_y = f_add(base_y,
                              f_mul(((UINT32 *)S->Velocity)[1], t));
        UINT32 lead_z = f_add(base_z,
                              f_mul(((UINT32 *)S->Velocity)[2], t));
        UINT32 grav = *(UINT32 *)&gLocal.ProjGravityScale;
        if (grav & 0x80000000UL) grav = F32_ZERO;
        if (f_cmp_lt(F32_ONE, grav)) grav = F32_ONE;
        UINT32 drop = f_mul(f_mul(f_mul(0x43BB8000UL, grav), t), t);
        lead_z = f_add(lead_z, drop);
        if (!head_ok) {
            lead_z = f_add(lead_z, APEX_AIM_Z_OFFSET_BITS);
        }

        UINT32 dx  = f_sub(lead_x, cam_x);
        UINT32 dy  = f_sub(lead_y, cam_y);
        UINT32 dz  = f_sub(lead_z, cam_z);
        UINT32 hyp = f_sqrt(f_add(f_mul(dx, dx), f_mul(dy, dy)));

        UINT32 tgt_p = f_neg(f_atan2_deg(dz, hyp));
        UINT32 tgt_y = f_atan2_deg(dy, dx);

        UINT32 d_p = f_sub(tgt_p, comp_p);
        UINT32 d_y = f_sub(tgt_y, comp_y_bits);
        if (f_cmp_lt(F32_180, d_y))     d_y = f_sub(d_y, F32_360);
        if (f_cmp_lt(d_y, F32_NEG_180)) d_y = f_add(d_y, F32_360);

        UINT32 fov2 = f_add(f_mul(d_p, d_p), f_mul(d_y, d_y));
        if (f_cmp_lt(fov_max2, fov2)) continue;

        UINT32 fov = f_sqrt(fov2);
        if (f_cmp_lt(fov, best_score)) {
            best_score = fov;
            best_ent   = S->Ent;
            best_tp    = tgt_p;
            best_ty    = tgt_y;
            best_dist  = dist;
            best_fov   = fov;
        }
    }

    *out_tp   = best_tp;
    *out_ty   = best_ty;
    *out_dist = best_dist;
    *out_fov  = best_fov;
    return best_ent;
}

// =====================================================================
// DoAim — moved from HypeHookDraw.c:733-894. Inner target loop replaced
// by PickBestTarget; FOV and sticky are unchanged. Gate: AimEnabled.
// =====================================================================
static VOID DoAim(UINT64 Cr3, UINT64 Tick) {
    if (gMenu.Open)                    { gAim.TargetEnt = 0; return; }
    if (!gAimTriggerParams.AimEnabled) { gAim.TargetEnt = 0; return; }
    if (gLocal.Ent == 0)               { gAim.TargetEnt = 0; return; }
    if (gLocal.IsDowned)               { gAim.TargetEnt = 0; return; }
    if (gLocal.IsZooming == 0) {
        gAim.TargetEnt = 0;
        *(UINT32 *)&gAim.LastDeltaPitch = F32_ZERO;
        *(UINT32 *)&gAim.LastDeltaYaw   = F32_ZERO;
        return;
    }

    UINT32 cam_x = ((UINT32 *)gLocal.Camera)[0];
    UINT32 cam_y = ((UINT32 *)gLocal.Camera)[1];
    UINT32 cam_z = ((UINT32 *)gLocal.Camera)[2];
    if (cam_x == 0 && cam_y == 0 && cam_z == 0) { gAim.TargetEnt = 0; return; }

    UINT32 view_p  = ((UINT32 *)gLocal.ViewAngles)[0];
    UINT32 view_y  = ((UINT32 *)gLocal.ViewAngles)[1];
    UINT32 punch_p = ((UINT32 *)gLocal.PunchPeak)[0];
    UINT32 punch_y = ((UINT32 *)gLocal.PunchPeak)[1];
    UINT32 comp_p  = f_add(view_p, punch_p);
    UINT32 comp_y  = f_add(view_y, punch_y);

    // q4.4 → float degrees: integer / 16 = integer * 0.0625.
    UINT32 fov_max   = f_mul(f_from_u8(gAimTriggerParams.AimFovQ4_4), F32_Q4_4_SCALE);
    UINT32 max_units = f_mul(0x43160000UL, APEX_UNITS_PER_METER_BITS);
    UINT32 min_units = f_mul(0x40400000UL, APEX_UNITS_PER_METER_BITS);
    UINT32 max_d2    = f_mul(max_units, max_units);
    UINT32 min_d2    = f_mul(min_units, min_units);

    UINT32 best_tp, best_ty, best_dist, best_fov;
    UINT64 best_ent = PickBestTarget(Cr3, fov_max, cam_x, cam_y, cam_z,
                                     comp_p, comp_y, max_d2, min_d2,
                                     &best_tp, &best_ty, &best_dist, &best_fov);

    // Sticky fallback: re-pick last target if still cached/valid.
    if (best_ent == 0 && gAim.TargetEnt != 0 && Tick < gAim.StickyUntilTick) {
        for (UINT32 i = 0; i < HOOK_DRAW_CACHE_SIZE; i++) {
            ENT_SNAPSHOT *S = &gEntCache[i];
            if (S->Ent != gAim.TargetEnt) continue;
            if (!S->IsPlayer || !S->IsAlive || S->IsDecoy || S->IsDowned) break;
            UINT32 d2 = *(UINT32 *)&S->DistSqToLocal;
            if (f_cmp_lt(max_d2, d2)) break;
            UINT32 tgt_x = ((UINT32 *)S->Origin)[0];
            UINT32 tgt_y = ((UINT32 *)S->Origin)[1];
            UINT32 tgt_z = ((UINT32 *)S->Origin)[2];
            if (!TryGetHeadBonePos(Cr3, S->Ent, &tgt_x, &tgt_y, &tgt_z)) {
                tgt_z = f_add(tgt_z, APEX_AIM_Z_OFFSET_BITS);
            }
            UINT32 dx  = f_sub(tgt_x, cam_x);
            UINT32 dy  = f_sub(tgt_y, cam_y);
            UINT32 dz  = f_sub(tgt_z, cam_z);
            UINT32 hyp = f_sqrt(f_add(f_mul(dx, dx), f_mul(dy, dy)));
            best_ent  = S->Ent;
            best_tp   = f_neg(f_atan2_deg(dz, hyp));
            best_ty   = f_atan2_deg(dy, dx);
            best_dist = f_sqrt(d2);
            best_fov  = F32_ZERO;
            break;
        }
    }

    gAim.TargetEnt           = best_ent;
    *(UINT32 *)&gAim.LastFov  = best_fov;
    *(UINT32 *)&gAim.LastDist = best_dist;
    if (best_ent == 0) {
        *(UINT32 *)&gAim.LastDeltaPitch = F32_ZERO;
        *(UINT32 *)&gAim.LastDeltaYaw   = F32_ZERO;
        return;
    }
    gAim.StickyUntilTick = Tick + AIM_STICKY_TICKS;

    UINT32 d_p = f_sub(best_tp, comp_p);
    UINT32 d_y = f_sub(best_ty, comp_y);
    if (f_cmp_lt(F32_180, d_y))     d_y = f_sub(d_y, F32_360);
    if (f_cmp_lt(d_y, F32_NEG_180)) d_y = f_add(d_y, F32_360);

    UINT32 sm     = 0x3EB33333UL;  // 0.35
    UINT32 prev_p = *(UINT32 *)&gAim.LastDeltaPitch;
    UINT32 prev_y = *(UINT32 *)&gAim.LastDeltaYaw;
    UINT32 sd_p   = f_add(prev_p, f_mul(sm, f_sub(d_p, prev_p)));
    UINT32 sd_y   = f_add(prev_y, f_mul(sm, f_sub(d_y, prev_y)));
    *(UINT32 *)&gAim.LastDeltaPitch = sd_p;
    *(UINT32 *)&gAim.LastDeltaYaw   = sd_y;

    UINT32 new_p = f_add(view_p, sd_p);
    UINT32 new_y = f_add(view_y, sd_y);
    if (f_cmp_lt(F32_89, new_p))      new_p = F32_89;
    if (f_cmp_lt(new_p, F32_NEG_89))  new_p = F32_NEG_89;
    if (f_cmp_lt(F32_180, new_y))     new_y = f_sub(new_y, F32_360);
    if (f_cmp_lt(new_y, F32_NEG_180)) new_y = f_add(new_y, F32_360);

    UINT32 packed[2] = { new_p, new_y };
    EFI_STATUS Wr = WriteGuestVirtual(Cr3, gLocal.Ent + APEX_ENT_VIEW_ANGLES,
                                       packed, 8);
    if (!EFI_ERROR(Wr)) {
        gAim.WriteCount++;
        if (!gEverWroteAngle) { gEverWroteAngle = 1; HvLogHex("EA0", best_ent); }
    }
    if (!gEverEngagedAim) {
        gEverEngagedAim  = 1;
        gAim.EngagedTick = Tick;
        HvLogHex("EA1", best_ent & 0x00FFFFFFFFFFFFFFULL);
    }
    gAim.EngageCount++;
}

// =====================================================================
// DoTrigger — ADS-held crosshair-on-target kbutton fire.
// State: IDLE → PRESSING (wrote 5) → COOLDOWN (write 4) → IDLE.
// Gate: TriggerEnabled, AttackPressed==0, IsZooming==1, !IsDowned.
// =====================================================================
static VOID DoTrigger(UINT64 Cr3, UINT64 ImageBase, UINT64 Tick) {
    // Advance state machine before the fire check.
    if (gTrigger.Phase == TRIG_PRESSING) {
        UINT32 val = 4;
        WriteGuestVirtual(Cr3, ImageBase + APEX_OFF_IN_ATTACK_KBUTTON + 0x8,
                          &val, 4);
        gTrigger.Phase        = TRIG_COOLDOWN;
        gTrigger.NextReadyTick = Tick + gAimTriggerParams.TriggerDebounceTicks;
        return;
    }
    if (gTrigger.Phase == TRIG_COOLDOWN) {
        if (Tick >= gTrigger.NextReadyTick) gTrigger.Phase = TRIG_IDLE;
        return;
    }

    // IDLE gate checks. Menu open suppresses NEW fires but the PRESSING/
    // COOLDOWN drain above always runs — otherwise opening the menu mid-press
    // would strand IN_ATTACK at 5 (weapon held visibly).
    if (gMenu.Open)                        return;
    if (!gAimTriggerParams.TriggerEnabled) return;
    if (gLocal.AttackPressed)              return;  // don't fight operator
    if (!gLocal.IsZooming)                 return;
    if (gLocal.IsDowned)                   return;

    UINT32 cam_x = ((UINT32 *)gLocal.Camera)[0];
    UINT32 cam_y = ((UINT32 *)gLocal.Camera)[1];
    UINT32 cam_z = ((UINT32 *)gLocal.Camera)[2];
    if (cam_x == 0 && cam_y == 0 && cam_z == 0) return;

    UINT32 view_p  = ((UINT32 *)gLocal.ViewAngles)[0];
    UINT32 view_y  = ((UINT32 *)gLocal.ViewAngles)[1];
    UINT32 punch_p = ((UINT32 *)gLocal.PunchPeak)[0];
    UINT32 punch_y = ((UINT32 *)gLocal.PunchPeak)[1];
    UINT32 comp_p  = f_add(view_p, punch_p);
    UINT32 comp_y  = f_add(view_y, punch_y);

    UINT32 fov_max   = f_mul(f_from_u8(gAimTriggerParams.TriggerFovQ4_4), F32_Q4_4_SCALE);
    UINT32 max_units = f_mul(0x43160000UL, APEX_UNITS_PER_METER_BITS);
    UINT32 min_units = f_mul(0x40400000UL, APEX_UNITS_PER_METER_BITS);
    UINT32 max_d2    = f_mul(max_units, max_units);
    UINT32 min_d2    = f_mul(min_units, min_units);

    UINT32 best_tp, best_ty, best_dist, best_fov;
    UINT64 best_ent = PickBestTarget(Cr3, fov_max, cam_x, cam_y, cam_z,
                                     comp_p, comp_y, max_d2, min_d2,
                                     &best_tp, &best_ty, &best_dist, &best_fov);
    if (best_ent == 0) return;

    if (!gTrigEverEngaged) { gTrigEverEngaged = 1; HvLogHex("ET1", best_ent); }

    // On-target: |d_pitch| < thresh && |d_yaw| < thresh.
    UINT32 thresh = f_mul(f_from_u8(gAimTriggerParams.TriggerThreshQ4_4), F32_Q4_4_SCALE);

    UINT32 d_p = f_sub(best_tp, comp_p);
    UINT32 d_y = f_sub(best_ty, comp_y);
    if (f_cmp_lt(F32_180, d_y))     d_y = f_sub(d_y, F32_360);
    if (f_cmp_lt(d_y, F32_NEG_180)) d_y = f_add(d_y, F32_360);

    if (f_cmp_lt(thresh, f_abs(d_p))) return;
    if (f_cmp_lt(thresh, f_abs(d_y))) return;

    // Fire: press IN_ATTACK kbutton.
    UINT32 val = 5;
    if (EFI_ERROR(WriteGuestVirtual(Cr3,
                                    ImageBase + APEX_OFF_IN_ATTACK_KBUTTON + 0x8,
                                    &val, 4)))
        return;

    gTrigger.Phase = TRIG_PRESSING;
    gTrigger.FireCount++;
    if (!gTrigEverWrote) { gTrigEverWrote = 1; HvLogHex("ET0", best_ent); }
    if ((gTrigger.FireCount & 0x3F) == 0) HvLogHex("ET2", gTrigger.FireCount);
}

// =====================================================================
// Public API
// =====================================================================

VOID HypeAimTriggerTick(UINT64 Cr3, UINT64 ImageBase, UINT64 Tick) {
    // Menu-open suppression lives INSIDE DoAim (full skip) and DoTrigger
    // (skip only the IDLE re-arm, not the PRESSING/COOLDOWN drain) so an
    // in-flight trigger press always releases — otherwise IN_ATTACK would
    // stay at 5 across the menu session and the weapon would visibly hold.
    DoAim(Cr3, Tick);
    DoTrigger(Cr3, ImageBase, Tick);
}

UINT32 HookAimHandleSetParams(PVCPU_DATA Vcpu, COVERT_CMD *Cmd) {
    (VOID)Vcpu;
    UINT64 A = Cmd->Arg1;
    gAimTriggerParams.TriggerEnabled       = (UINT8)( A        & 0xFF);
    gAimTriggerParams.AimEnabled           = (UINT8)((A >>  8) & 0xFF);
    gAimTriggerParams.TriggerFovQ4_4       = (UINT8)((A >> 16) & 0xFF);
    gAimTriggerParams.TriggerThreshQ4_4    = (UINT8)((A >> 24) & 0xFF);
    gAimTriggerParams.TriggerDebounceTicks = (UINT8)((A >> 32) & 0xFF);
    gAimTriggerParams.AimFovQ4_4           = (UINT8)((A >> 40) & 0xFF);
    HvLogHex("AT0", A);
    Cmd->Result = A;
    return COVERT_STATUS_OK;
}

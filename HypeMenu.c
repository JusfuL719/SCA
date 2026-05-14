// HypeMenu.c — HV-resident config controller.
//
// No rendering surface; the menu mutates HV-resident globals (gGlowParams,
// gAimTriggerParams) in response to kbutton edges. Apex's renderer reflects
// the changes next frame via the existing RenderGlow path.

#include "HypeMenu.h"
#include "HypeHookDraw.h"      // GLOW_PARAMS_RT + gGlowParams
#include "HypeAimTrigger.h"    // AIM_TRIGGER_PARAMS + gAimTriggerParams
#include "HypeApexCanon.h"
#include "HypeMemory.h"
#include "HypeDebug.h"

volatile MENU_STATE gMenu = { 0 };

// Each kbutton +0x8 read returns a 4-byte int; HypeAimTrigger writes 5
// (pressed) and 4 (released). Keep semantics identical.
#define KB_STATE_PRESSED  5

// Menu rows. Pointer-into-globals + kind/range. Adding a row = appending an
// entry. Slot/Mask use full u8 range; FilterMode bounded 0..3.
static const MENU_ITEM kMenuItems[] = {
    { "GlowEnabled",    &gGlowParams.Enabled,                       MENU_ITEM_BOOL,   0, 1, 1 },
    { "FilterMode",     &gGlowParams.FilterMode,                    MENU_ITEM_SLIDER, 0, 3, 1 },
    { "GlowSlot",       &gGlowParams.Slot,                          MENU_ITEM_SLIDER, 0, 89, 1 },
    { "GlowMask",       &gGlowParams.Mask,                          MENU_ITEM_SLIDER, 0, 0xFF, 1 },
    { "VisType",        &gGlowParams.VisType,                       MENU_ITEM_SLIDER, 0, 0xFF, 1 },
    { "GlowFix",        &gGlowParams.GlowFix,                       MENU_ITEM_SLIDER, 0, 0xFF, 1 },
    { "WriteVisType",   &gGlowParams.WriteVisType,                  MENU_ITEM_BOOL,   0, 1, 1 },
    { "WriteGlowFix",   &gGlowParams.WriteGlowFix,                  MENU_ITEM_BOOL,   0, 1, 1 },
    { "AimEnabled",     (volatile UINT8 *)&gAimTriggerParams.AimEnabled,       MENU_ITEM_BOOL,   0, 1, 1 },
    { "AimFovQ4_4",     (volatile UINT8 *)&gAimTriggerParams.AimFovQ4_4,       MENU_ITEM_SLIDER, 0, 0xFF, 1 },
    { "TrigEnabled",    (volatile UINT8 *)&gAimTriggerParams.TriggerEnabled,   MENU_ITEM_BOOL,   0, 1, 1 },
    { "TrigFovQ4_4",    (volatile UINT8 *)&gAimTriggerParams.TriggerFovQ4_4,   MENU_ITEM_SLIDER, 0, 0xFF, 1 },
    { "TrigThreshQ4_4", (volatile UINT8 *)&gAimTriggerParams.TriggerThreshQ4_4,MENU_ITEM_SLIDER, 0, 0xFF, 1 },
    { "TrigDebounce",   (volatile UINT8 *)&gAimTriggerParams.TriggerDebounceTicks, MENU_ITEM_SLIDER, 1, 60, 1 },
};
#define MENU_ITEM_COUNT  (sizeof(kMenuItems) / sizeof(kMenuItems[0]))

static const UINT64 kKeyRva[MENU_KEY_COUNT] = {
    [MENU_KEY_USE]     = APEX_OFF_IN_USE,
    [MENU_KEY_DUCK]    = APEX_OFF_IN_DUCK,
    [MENU_KEY_JUMP]    = APEX_OFF_IN_JUMP,
    [MENU_KEY_SPEED]   = APEX_OFF_IN_SPEED,
    [MENU_KEY_FORWARD] = APEX_OFF_IN_FORWARD,
};

// IN_USE triple-tap: shift each new edge tick into a 3-slot ring; if the
// oldest of the three falls inside MENU_TRIPLE_TAP_TICKS of the newest,
// fire MenuToggleOpen.
static VOID OnUseEdge(UINT64 Tick) {
    UINT32 idx = gMenu.UseEdgeIdx % 3;
    gMenu.UseEdgeRing[idx] = Tick;
    gMenu.UseEdgeIdx++;
    if (gMenu.UseEdgeIdx < 3) return;
    UINT64 oldest = gMenu.UseEdgeRing[(gMenu.UseEdgeIdx) % 3];
    if (Tick - oldest <= MENU_TRIPLE_TAP_TICKS) {
        gMenu.Open = gMenu.Open ? 0 : 1;
        // Reset cursor and ring so the next triple-tap requires 3 fresh edges.
        gMenu.CursorRow = 0;
        gMenu.UseEdgeRing[0] = gMenu.UseEdgeRing[1] = gMenu.UseEdgeRing[2] = 0;
        gMenu.UseEdgeIdx = 0;
        HvLogHex("MN5", (UINT64)gMenu.Open);
    }
}

static VOID NavRow(INT32 dir) {
    INT32 row = (INT32)gMenu.CursorRow + dir;
    if (row < 0) row = (INT32)MENU_ITEM_COUNT - 1;
    if (row >= (INT32)MENU_ITEM_COUNT) row = 0;
    gMenu.CursorRow = (UINT8)row;
}

static VOID EditValue(INT32 dir) {
    if (gMenu.CursorRow >= MENU_ITEM_COUNT) return;
    const MENU_ITEM *it = &kMenuItems[gMenu.CursorRow];
    UINT8 v = *it->Field;
    if (it->Kind == MENU_ITEM_BOOL) {
        v = v ? 0 : 1;
    } else {
        INT32 nv = (INT32)v + dir * (INT32)it->Step;
        if (nv < (INT32)it->Min) nv = (INT32)it->Max;
        if (nv > (INT32)it->Max) nv = (INT32)it->Min;
        v = (UINT8)nv;
    }
    *it->Field = v;
    HvLogHex("MN6", ((UINT64)gMenu.CursorRow << 8) | (UINT64)v);
}

static VOID MenuHandleEdge(UINT32 KeyId, UINT64 Tick) {
    HvLogHex("MN4", ((UINT64)KeyId << 8) | 0x1ULL);
    if (KeyId == MENU_KEY_USE) {
        OnUseEdge(Tick);
        return;
    }
    if (!gMenu.Open) return;        // nav keys are no-op when menu closed
    switch (KeyId) {
        case MENU_KEY_DUCK:    NavRow(+1); break;
        case MENU_KEY_JUMP:    NavRow(-1); break;
        case MENU_KEY_SPEED:   EditValue(-1); break;
        case MENU_KEY_FORWARD: EditValue(+1); break;
        default: break;
    }
}

VOID MenuTick(UINT64 Cr3, UINT64 ImageBase, UINT64 Tick) {
    if (!gMenu.Enabled) return;
    if (Cr3 == 0 || ImageBase == 0) return;

    // First tick after MenuHandleSetEnable: just snapshot current kbutton
    // state into LastKeyState so a key already held at enable time doesn't
    // fire a synthetic 0→pressed edge on the next tick.
    if (gMenu.SeedNextTick) {
        for (UINT32 i = 0; i < MENU_KEY_COUNT; ++i) {
            UINT32 cur = 0;
            if (!EFI_ERROR(ReadGuestVirtual(Cr3, ImageBase + kKeyRva[i] + 0x8,
                                            &cur, sizeof(cur)))) {
                gMenu.LastKeyState[i] = cur;
            }
        }
        gMenu.SeedNextTick = 0;
        return;
    }

    for (UINT32 i = 0; i < MENU_KEY_COUNT; ++i) {
        UINT32 cur = 0;
        if (EFI_ERROR(ReadGuestVirtual(Cr3, ImageBase + kKeyRva[i] + 0x8,
                                       &cur, sizeof(cur)))) continue;
        UINT32 prev = gMenu.LastKeyState[i];
        gMenu.LastKeyState[i] = cur;
        BOOLEAN pressed_now  = (cur  == KB_STATE_PRESSED);
        BOOLEAN pressed_prev = (prev == KB_STATE_PRESSED);
        if (pressed_now && !pressed_prev &&
            (Tick - gMenu.LastKeyEdgeTick[i]) > MENU_DEBOUNCE_TICKS) {
            gMenu.LastKeyEdgeTick[i] = Tick;
            MenuHandleEdge(i, Tick);
        }
    }
}

UINT32 MenuHandleSetEnable(PVCPU_DATA Vcpu, COVERT_CMD *Cmd) {
    (VOID)Vcpu;
    UINT8 enable = (UINT8)(Cmd->Arg1 & 0xFF);
    gMenu.Enabled = enable;
    if (enable) {
        // Seed the kbutton state on the next tick so a held key at enable
        // time (e.g. operator running with IN_USE pressed) doesn't fire a
        // synthetic 0→pressed edge.
        gMenu.SeedNextTick = 1;
    } else {
        gMenu.Open = 0;
        gMenu.CursorRow = 0;
        gMenu.UseEdgeIdx = 0;
    }
    HvLogHex("MN0", (UINT64)enable);
    Cmd->Result = (UINT64)enable;
    return COVERT_STATUS_OK;
}

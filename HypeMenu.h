#ifndef HYPE_MENU_H
#define HYPE_MENU_H

#include "HypeUefi.h"
#include "HypeSvm.h"

// HV-resident config controller. No rendering: input is the existing IN_*
// kbuttons (read at +0x8 same as HypeAimTrigger), output is direct mutation
// of gGlowParams / gAimTriggerParams. Apex's own renderer reflects the
// changes (glow visibility, etc.) on the next frame.

// Triple-tap on IN_USE within MENU_TRIPLE_TAP_TICKS toggles Open. While Open:
//   DUCK    -> cursor row down
//   JUMP    -> cursor row up
//   SPEED   -> decrement current item value
//   FORWARD -> increment current item value
// HypeAimTrigger early-outs on gMenu.Open so navigation keys don't fire.

#define MENU_KEY_USE      0
#define MENU_KEY_DUCK     1
#define MENU_KEY_JUMP     2
#define MENU_KEY_SPEED    3
#define MENU_KEY_FORWARD  4
#define MENU_KEY_COUNT    5

#define MENU_DEBOUNCE_TICKS    8        // min edges-per-key spacing
// Triple-tap window tightened from 500 ms to ~300 ms — looting an enemy
// kill routinely hits 3 E-edges in <500 ms; 300 ms still allows a deliberate
// triple-tap but rejects most rapid-loot patterns.
#define MENU_TRIPLE_TAP_TICKS  30

#define MENU_ITEM_BOOL     0
#define MENU_ITEM_SLIDER   1

typedef struct _MENU_ITEM {
    const char        *Label;
    volatile UINT8    *Field;
    UINT8              Kind;
    UINT8              Min;
    UINT8              Max;
    UINT8              Step;
} MENU_ITEM;

typedef struct _MENU_STATE {
    UINT8   Enabled;                            // 0 = module armed off
    UINT8   Open;                               // 1 = menu open, navigating
    UINT8   CursorRow;
    UINT8   SeedNextTick;                       // 1 = first tick after enable; seed LastKeyState w/o firing edges
    UINT32  LastKeyState [MENU_KEY_COUNT];      // last read of kbutton +0x8
    UINT64  LastKeyEdgeTick[MENU_KEY_COUNT];    // last accepted edge tick
    UINT64  UseEdgeRing  [3];                   // triple-tap ring for IN_USE
    UINT32  UseEdgeIdx;
    UINT64  LastValueChangeTick;                // for log-marker rate-limit
} MENU_STATE;

extern volatile MENU_STATE gMenu;

// Called every payload tick from DrawHookPayloadTick. Reads kbutton state,
// debounces edges, dispatches via MenuHandleEdge.
VOID MenuTick(UINT64 Cr3, UINT64 ImageBase, UINT64 Tick);

// PMC_CMD_SET_MENU_ENABLE handler. Arg1 = 1 (arm) / 0 (disarm).
UINT32 MenuHandleSetEnable(PVCPU_DATA Vcpu, COVERT_CMD *Cmd);

#endif // HYPE_MENU_H

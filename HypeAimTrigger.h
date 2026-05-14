#ifndef HYPE_AIM_TRIGGER_H
#define HYPE_AIM_TRIGGER_H

#include "HypeUefi.h"
#include "HypeSvm.h"

// Aim tunables (moved from HypeHookDraw.h).
#define AIM_FOV_DEG               4.0f
#define AIM_SMOOTH                0.35f
#define AIM_MAX_DIST_M            150.0f
#define AIM_MIN_DIST_M            3.0f
#define AIM_STICKY_TICKS          10
#define RECOIL_PEAK_DECAY_TICKS   15

// Trigger tunables.
#define TRIGGER_FOV_DEG           0.7f
#define TRIGGER_THRESH_DEG        0.3f
#define TRIGGER_DEBOUNCE_TICKS    12

typedef struct _AIM_STATE {
    UINT64  TargetEnt;
    UINT64  StickyUntilTick;
    float   LastDeltaPitch;
    float   LastDeltaYaw;
    UINT64  EngagedTick;
    UINT32  EngageCount;
    UINT32  WriteCount;
    float   LastFov;
    float   LastDist;
} AIM_STATE;

typedef enum { TRIG_IDLE = 0, TRIG_PRESSING = 1, TRIG_COOLDOWN = 2 } TRIGGER_PHASE;

typedef struct _TRIGGER_STATE {
    TRIGGER_PHASE Phase;
    UINT64        NextReadyTick;
    UINT64        FireCount;
} TRIGGER_STATE;

// Runtime params — set via PMC_CMD_SET_AIM_PARAMS (0x21).
// All fields disabled (0) at boot; operator enables via --aim-params each session.
typedef struct _AIM_TRIGGER_PARAMS {
    UINT8  TriggerEnabled;       // 0=off (default), 1=on
    UINT8  AimEnabled;           // 0=off (default), 1=on
    UINT8  TriggerFovQ4_4;       // q4.4 deg (16x deg), default 0x0B (~0.69°)
    UINT8  TriggerThreshQ4_4;    // q4.4 deg, default 0x05 (~0.31°)
    UINT8  TriggerDebounceTicks; // ticks between fires, default 12 (~120 ms)
    UINT8  AimFovQ4_4;           // q4.4 deg, default 0x40 (4.0°)
} AIM_TRIGGER_PARAMS;

// Exported so HypeHookDraw.c can mirror gAim into the draw buffer.
extern AIM_STATE                    gAim;
extern volatile AIM_TRIGGER_PARAMS  gAimTriggerParams;

// PMC_CMD_SET_AIM_PARAMS (0x21) handler.
// Arg1 packing: [7:0]=TriggerEnabled [15:8]=AimEnabled [23:16]=TriggerFovQ4_4
//   [31:24]=TriggerThreshQ4_4 [39:32]=TriggerDebounceTicks [47:40]=AimFovQ4_4
//   [63:48]=reserved.
UINT32 HookAimHandleSetParams(PVCPU_DATA Vcpu, COVERT_CMD *Cmd);

// Called every payload tick (HasLocal==TRUE). Runs DoAim + DoTrigger.
VOID   HypeAimTriggerTick(UINT64 Cr3, UINT64 ImageBase, UINT64 Tick);

#endif // HYPE_AIM_TRIGGER_H

#ifndef HYPE_HOOK_DRAW_H
#define HYPE_HOOK_DRAW_H

#include "HypeUefi.h"
#include "HypeSvm.h"
#include "HypeAimTrigger.h"

// PMC_CMD_HOOK_INSTALL_DRAW (0x1B): Arg1=HookVa, Arg2=ScratchBufferGva (4K-aligned, 64K).
#define HOOK_DRAW_MAX_INSTALLS  16
#define HOOK_DRAW_BUF_PAGES     16
#define HOOK_DRAW_BUF_BYTES     (HOOK_DRAW_BUF_PAGES * 0x1000)

// Payload tick gating — shift=0 fires every NPF (~250µs).
#define HOOK_DRAW_PAYLOAD_SHIFT 0
#define HOOK_DRAW_PAYLOAD_MASK  ((1ULL << HOOK_DRAW_PAYLOAD_SHIFT) - 1)

typedef struct _DRAW_HOOK_DESC {
    UINT32   PageCount;                       // 0..HOOK_DRAW_BUF_PAGES
    UINT32   CopiedBytes;                     // bytes copied into HvBuf
    UINT64   GpaList[HOOK_DRAW_BUF_PAGES];    // installer-side GPAs (diagnostic)
} DRAW_HOOK_DESC;

VOID *HookDrawScratchHvBuf(VOID);
UINT32 HookDrawHandleInstall(PVCPU_DATA Vcpu, COVERT_CMD *Cmd);

// PMC_CMD_HOOK_DRAW_PEEK (0x1C): Arg1=offset, Result=8B at offset.
UINT32 HookDrawHandlePeek(PVCPU_DATA Vcpu, COVERT_CMD *Cmd);

// PMC_CMD_SET_GLOW_PARAMS (0x1D) — Arg1: [31:24]=Slot [23:16]=Mask [15:8]=Filter [7:0]=Enabled.
UINT32 HookDrawHandleSetGlowParams(PVCPU_DATA Vcpu, COVERT_CMD *Cmd);

// Runtime glow knobs. HypeMenu mutates these directly when the operator edits
// a menu row; RenderGlow consumes them next tick.
typedef struct _GLOW_PARAMS_RT {
    UINT8  Slot;        // HID + STACK[0] value (78=APEX_SLOT_PLAYER)
    UINT8  Mask;        // +0x2A1 gate (0x01 = HS[0]==HID enable)
    UINT8  FilterMode;  // 0=enemy_only 1=all 2=teammates 3=off
    UINT8  Enabled;     // 0 = no entity writes (gate off entirely)
    UINT8  VisType;     // +0x26C value (engine resets to 5 — race target)
    UINT8  GlowFix;     // +0x278 value (engine writes -1.0 — race target)
    UINT8  WriteVisType;// 0 = skip the +0x26C write entirely (let engine drive)
    UINT8  WriteGlowFix;// 0 = skip the +0x278 write entirely
    UINT8  SquadGlow;   // 1 = override engine's HID=28 squad-suppress (route to GlowSlot)
} GLOW_PARAMS_RT;
extern volatile GLOW_PARAMS_RT gGlowParams;

// NPT exec-trap dispatch — coexists with covert single-step via DrawHookActive flag.
BOOLEAN DrawHookGpaMatches(UINT64 FaultGpa);
VOID    DrawHookOnNpfHit  (PVCPU_DATA Vcpu);
VOID    DrawHookOnDbRestore(PVCPU_DATA Vcpu);
UINT64  DrawHookRearm     (VOID);
UINT64  DrawHookSampleNpfTotal(VOID);
UINT64  DrawHookCurrentGpa(VOID);

// Entity cache size (shared with HypeAimTrigger.c via this header).
#define HOOK_DRAW_CACHE_SIZE          128
#define HOOK_DRAW_CACHE_MASK          (HOOK_DRAW_CACHE_SIZE - 1)
#define HOOK_DRAW_MIRROR_SLOTS        64

// Snapshot mirror in gDrawHookHvBuf, read via PMC_CMD_HOOK_DRAW_PEEK.

typedef struct _ENT_SNAPSHOT {
    UINT64  Ent;             // entity ptr (0 = empty/stale slot)
    float   Origin[3];
    float   PrevOrigin[3];   // origin from last sample of this slot
    float   Velocity[3];     // (Origin - PrevOrigin) / dt — m_vecAbsVelocity often wrong at runtime
    INT32   Health;
    INT32   Shield;
    INT32   MaxHealth;
    INT32   MaxShield;
    INT32   Team;
    INT32   ArmorType;       // 0..5
    UINT8   IsPlayer;
    UINT8   IsAlive;
    UINT8   IsDowned;
    UINT8   IsDecoy;
    UINT8   LootTier;        // APEX_LOOT_TIER_*
    UINT8   HighlightId;     // raw byte at Ent+0x298; player buckets 73-79 + drift
    UINT8   _Pad0[2];
    UINT64  LastSeenTick;    // gPayloadTicksFired at last refresh
    UINT64  PrevSeenTick;    // tick at PrevOrigin sample (for dt)
    float   DistSqToLocal;
} ENT_SNAPSHOT;

typedef struct _LOCAL_SNAPSHOT {
    UINT64  Ent;
    float   Origin[3];
    float   Camera[3];
    float   ViewAngles[3];
    float   PunchRaw[3];
    float   PunchPeak[3];           // peak-held over RECOIL_PEAK_DECAY_TICKS
    UINT64  PunchPeakTick[3];       // tick of last peak set, per-axis
    float   ProjSpeed;              // bullet launch speed (units/sec)
    float   ProjGravityScale;       // 0..1
    float   InheritOwner;           // 0..1
    INT32   Team;
    INT32   Health;
    INT32   Shield;
    INT32   MaxHealth;
    INT32   MaxShield;
    UINT8   IsZooming;
    UINT8   IsDowned;
    UINT8   AttackPressed;          // IN_ATTACK_KBUTTON +0x8 == 5
    UINT8   _Pad0;
    char    WeaponName[32];
    float   ViewMatrix[16];         // captured for W2S downstream
} LOCAL_SNAPSHOT;

#define DRAWBUF_OFF_SENTINEL          0x0000
#define DRAWBUF_OFF_LOCAL             0x0100
#define DRAWBUF_OFF_AIM               0x0300
#define DRAWBUF_OFF_VIEW_MATRIX_RAW   0x0400
#define DRAWBUF_OFF_ENT_SNAPSHOTS     0x0800
#define DRAWBUF_OFF_HEARTBEAT         0x3F00

#endif

#ifndef HYPE_APEX_CANON_H
#define HYPE_APEX_CANON_H

// Apex r5apex offsets — UNICORN_DUMPER/apex_dumper output 2026-05-13.
// Tier-1 globals re-resolved via sig_mov/sig_lea/emulate strategies; +0x22C0
// data-section slide from prior 2026-05-11 build (matches HIGHLIGHT_SETTINGS
// drift Bobby pinned manually). Anchor sigs in apex_dumper/apex_sigs.py.


// --- globals ---
#define APEX_OFF_ENTITY_LIST             0x06268BE8ULL  // drift +0x22C0 from 0x06266928 — emulate(r8) at CHandleTable::Lookup prologue
#define APEX_OFF_VIEW_RENDER             0x03D9A008ULL  // drift +0x2290 from 0x03D97D78 — sig_mov anchor (cmp [rax+0x5c],r14d; je; mov rax,[rip+VR])
#define APEX_OFF_NAME_LIST               0x08C65160ULL  // drift +0x22C0 from 0x08C62EA0 — sig_lea anchor (movsxd; lea rcx,[rax+rax*2]; lea rax,[rip+NL])
#define APEX_OFF_OBSERVER_LIST           0x0626AC08ULL  // drift +0x22C0 from 0x06268948 — sig_mov anchor (lea eax,[rbx-1]; cmp eax,0x7f; ja; mov rcx,[rip+OL])
#define APEX_OFF_LOCAL_PLAYER            0x0268FA08ULL  // confirmed via in-match .data dump 2026-05-12 15:43 (slot held 0x000002F3CA137FB0; Steam ±0xC00 candidates were null/wrong-shape)
#define APEX_OFF_HIGHLIGHT_SETTINGS      0x069B28C0ULL  // re-pinned 2026-05-13 via apex_dumper SetHighlightId trace: +0x22C0 drift from 0x069B0600. Pointer cell holding bucket-array base; verified by `add rdx, qword ptr [rip+0x619A350]` at live RVA 0x00818569 inside SetHighlightId success path. BUCKET_COUNT lives at +0x69B28D8 (+0x18 past this).
#define APEX_OFF_VIEW_MATRIX_DEREF       0x0011A350ULL  // reference: struct displacement inside CClientView
// kbutton globals (6 below) — EA build registers IN_* indirectly; no static anchor recoverable. Negative result: PSTools/UNICORN_DUMPER/output/canon/kbuttons_emul_2026-05-14.json. Revisit only if HV/overlay actually reads these cells.
#define APEX_OFF_IN_ATTACK_KBUTTON       0x03D9A658ULL  // reference: kbutton via Cbuf_AddText — needs +attack sig
#define APEX_OFF_MP_GAMEMODE             0x026CA650ULL  // CORRECTED 2026-05-13 via pastebin s9sqRHxZ EA App canon (was 0x0259DC90, wrong build epoch)
#define APEX_OFF_CROSSPLAY_ENABLED       0x01EADA60ULL  // reference: rdata convar — needs name sig
#define APEX_OFF_SHOW_FPS                0x01F05300ULL  // CORRECTED 2026-05-13 via pastebin s9sqRHxZ EA App canon (was 0x01DD7290, wrong build epoch)
#define APEX_OFF_IN_JUMP                 0x03D9AF10ULL  // CORRECTED 2026-05-13 via pastebin s9sqRHxZ EA App canon (was 0x03F39270, wrong build — actual cluster mate of other 5 kbuttons in 0x3D9_xxx range)
#define APEX_OFF_IN_DUCK                 0x03D9B008ULL  // reference: +duck kbutton — needs sig
#define APEX_OFF_IN_FORWARD              0x03D9B048ULL  // reference: +forward kbutton — needs sig
#define APEX_OFF_IN_USE                  0x03D9AF80ULL  // reference: +use kbutton — needs sig
#define APEX_OFF_IN_SPEED                0x03D9A5E0ULL  // reference: +speed kbutton — needs sig
#define APEX_OFF_RENDER_GATE_FN          0x008174F0ULL  // re-pinned 2026-05-13: +0xF00 drift from 0x008165F0; live prologue `sub rsp,8; movzx r10d, [rcx+0x298]` confirms entry. Stale anchor decoded as data → NPT exec trap was arming on the wrong page (root cause of "nothing glows" before this fix).
// First 8 bytes of APEX_OFF_RENDER_GATE_FN as a u64 LE — used by
// HypeHookDraw.c to hard-fail PMC_CMD_HOOK_INSTALL_DRAW if the canon
// RVA no longer lands on the expected prologue. Bytes (little-endian):
// 48 83 EC 08 44 0F B6 91  ->  sub rsp,8 ; movzx r10d, byte ptr [rcx+0x298]
// On drift, the install handler logs "DRX" with both expected and
// observed prologue then aborts the trap arm — no more silent
// arm-on-garbage failure mode.
#define APEX_RENDER_GATE_FN_PROLOGUE_LE64 0x91B60F4408EC8348ULL

// --- entity ---
#define APEX_ENT_STRIDE                  0x00000020ULL  // reference: entity slot stride
#define APEX_ENT_ORIGIN                  0x0000017CULL
#define APEX_ENT_VELOCITY_RAW            0x00000170ULL  // runtime: unreliable — derive from origin delta
#define APEX_ENT_SHIELD                  0x000001A0ULL
#define APEX_ENT_MAX_SHIELD              0x000001A4ULL
// HighlightContext + DT_HighlightSettings fields (Orion + renderer-disasm).
#define APEX_HC_RESULT_STATE             0x0000026CULL  // HighlightContext LOS result dword
#define APEX_HC_LAST_LOS_TIME            0x00000278ULL  // float, -1.0f sentinel (RVA 0x815F3A)
#define APEX_ENT_HIGHLIGHT_TEAM_INDEX    0x000001E8ULL  // Orion: m_highlightTeamIndex uint8_t[16] (netvar source for +0x298)
#define APEX_ENT_HIGHLIGHT_TEAM_BITS     0x000001F8ULL  // Orion: m_highlightTeamBits uint32_t[16]
#define APEX_ENT_HIGHLIGHT_FADE_SCALE    0x00000294ULL  // float, ctor=1.0f (RVA 0xEB0C3F)
#define APEX_ENT_HIGHLIGHT_ID            0x00000298ULL  // uint8_t client-resolved bucket idx (not a DT_ netvar)
#define APEX_ENT_HIGHLIGHT_GENERIC_CTX   0x00000299ULL  // Orion: m_highlightGenericContexts uint8_t[8]
#define APEX_ENT_HIGHLIGHT_FOCUSED       0x000002A1ULL  // Orion: m_highlightFocused uint8_t
#define APEX_ENT_HIGHLIGHT_FADE_DURATION 0x000002A4ULL  // Orion: m_highlightFadeDuration float
#define APEX_ENT_HIGHLIGHT_FADE_PARITY   0x000002ACULL  // Orion: m_highlightFadeParity uint32_t
// Back-compat aliases — keep HookDraw cycle-9k glow path compiling
#define APEX_ENT_GLOW_VISIBLE_TYPE       APEX_HC_RESULT_STATE
#define APEX_ENT_GLOW_FIX                APEX_HC_LAST_LOS_TIME
#define APEX_ENT_GLOW_DISTANCE           APEX_ENT_HIGHLIGHT_FADE_SCALE
#define APEX_ENT_HIGHLIGHT_STACK         APEX_ENT_HIGHLIGHT_GENERIC_CTX
#define APEX_ENT_HIGHLIGHT_MASK          APEX_ENT_HIGHLIGHT_FOCUSED
#define APEX_ENT_HEALTH                  0x00000324ULL
#define APEX_ENT_TEAM                    0x00000334ULL
#define APEX_ENT_SQUAD                   0x00000340ULL
#define APEX_ENT_MAX_HEALTH              0x00000468ULL
#define APEX_ENT_NAME                    0x00000479ULL
#define APEX_ENT_LIFE_STATE              0x00000690ULL
// C_BaseAnimating/CStudioHdr path used for bone-aware aim.
#define APEX_ENT_STUDIO_HDR              0x00001000ULL  // m_pStudioHdr
#define APEX_ENT_BONES                   0x00000E00ULL  // bone matrix pointer (legacy 0xDB8+0x48)
#define APEX_BONE_STRIDE                 0x00000030ULL  // matrix3x4_t stride (48 bytes per bone)
#define APEX_BONE_HEAD_IDX               8UL            // default Source-style head index; fallback path handles mismatch
#define APEX_ENT_SURVIVAL_ITEM_ID        0x000015E4ULL
#define APEX_ENT_WEAPON_NAME_STR         0x000018B8ULL
#define APEX_ENT_DECOY_FLAGS             0x00001924ULL
#define APEX_ENT_WEAPON_HANDLE           0x000019D4ULL
#define APEX_ENT_LAST_VISIBLE_TIME       0x00001A74ULL
#define APEX_ENT_LAST_AIMED_AT_TIME      0x00001A7CULL
#define APEX_ENT_ZOOMING                 0x00001CC1ULL
#define APEX_ENT_CAMERA_ORIGIN           0x00001FD4ULL
#define APEX_ENT_PUNCH_WEAPON_ANGLE      0x00002528ULL
#define APEX_ENT_VIEW_ANGLES             0x00002610ULL
#define APEX_ENT_BLEEDOUT_STATE          0x000027E0ULL
#define APEX_ENT_ARMOR_TYPE              0x0000487CULL  // unverified: disp32 refs=0 < 3

// --- weapon ---
#define APEX_WEP_BULLET_INHERIT_OWNER    0x000028BCULL  // unverified: disp32 refs=0 < 3
#define APEX_WEP_BULLET_SPEED            0x000028C8ULL  // unverified: disp32 refs=0 < 3
#define APEX_WEP_BULLET_GRAVITY_SCALE    0x000028D0ULL

// --- constants ---
// Bucket +0x2C LE dword (FunctionBits, copied to entity+0x268 by SetHighlightId):
// [0]=insideMode [1]=outlineMode [2]=borderSize [3]=flags.
// Flags bits: 7=AfterPostProcess, 6=EntityVisible, 0..5=State.
// 0x47407887 = engine default (occluded only); 0xC7407889 = full-view override.
// Bit 15 ("armed for render") is OR'd into entity+0x268 by Highlight_EnableRender
// (live RVA 0x00818240, was 0x817340 pre +0xF00 drift) per-entity — it is NOT
// part of the bucket value. SetHighlightId live RVA 0x00818500 (was 0x817600).
// Verified bucket layout: +0x04..+0x0F R/G/B primary, +0x10..+0x1B R/G/B
// secondary, +0x1C/+0x20 copied to entity+0x260/+0x264, +0x2C = FunctionBits.
// +0x00 is a type tag — NOT FunctionBits (PEX/Overlay/glow.h writes there in
// error; safe but ineffective).
#define APEX_BUCKET_FNBITS_SLOT78_OBS      0x47407887UL  // engine default (occluded-only)
#define APEX_BUCKET_FNBITS_SLOT78_FULLVIEW 0xC7407889UL  // override: AfterPostProcess + reahly insideMode
#define APEX_FN_BITS_VIS                   APEX_BUCKET_FNBITS_SLOT78_FULLVIEW
#define APEX_SLOT_PLAYER                 78UL
#define APEX_SLOT_LOOT_MYTHIC            79UL
#define APEX_SLOT_LOOT_LEGENDARY         77UL
#define APEX_SLOT_LOOT_EPIC              76UL
#define APEX_SLOT_LOOT_RARE              75UL
#define APEX_SLOT_LOOT_AMMO              74UL
#define APEX_HIGHLIGHT_TYPE_SIZE         0x00000034UL  // slot table stride
#define APEX_F32_ONE                     0x3F800000UL  // 1.0f bits
#define APEX_F32_HALF                    0x3F000000UL  // 0.5f bits
#define APEX_F32_QUARTER                 0x3E800000UL  // 0.25f bits
#define APEX_F32_ZERO                    0x00000000UL  // 0.0f bits
#define APEX_UNITS_PER_METER_BITS        0x424A0000UL  // 52.5f bits
#define APEX_AIM_Z_OFFSET_BITS           0x42480000UL  // 50.0f — feet → chest aim

// --- loot tier (HV-internal classification — not from Apex) ---
#define APEX_LOOT_TIER_NONE              0
#define APEX_LOOT_TIER_WEAPON            1
#define APEX_LOOT_TIER_LEGENDARY         2
#define APEX_LOOT_TIER_MYTHIC            3
#define APEX_LOOT_TIER_EPIC              4
#define APEX_LOOT_TIER_RARE              5
#define APEX_LOOT_TIER_AMMO              6
#define APEX_LOOT_TIER_COMMON            7

#endif // HYPE_APEX_CANON_H

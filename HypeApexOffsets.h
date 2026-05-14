#pragma once

/// Main — values re-resolved 2026-05-13 via apex_dumper sig_mov/sig_lea/emulate
DWORD dwLocalPlayer = 0x268fa08; // localplayer (still ref-only; anchor sig pending)
DWORD dwEntityList = 0x6268be8; // entitylist (was 0x6266928, drift +0x22C0)
DWORD OFFSET_VIEWRENDER = 0x3d9a008; // viewrender (was 0x3d97d78, drift +0x2290)
DWORD OFFSET_MATRIX = 0x11a350; // matrix (struct disp inside CClientView — not a global)
DWORD OFFSET_NAME_LIST = 0x8c65160; // namelist (was 0x8c62ea0, drift +0x22C0)

// Player
DWORD m_iTeamNum = 0x334; // m_iTeamNum
DWORD m_iName = 0x479; // m_iname
DWORD m_iHealth = 0x324; // m_health
DWORD m_iMaxHealth = 0x468; // m_health
DWORD OFFSET_SHIELD = 0x1a0;// m_shield
DWORD OFFSET_MAX_SHIELD = 0x1a4; // m_shield
DWORD m_flVisible = 0x1a64; // CPlayer!lastVisibleTime 0x1a54
DWORD OFFSET_BLEED_OUT_STATE = 0x27e0; // m_bleedoutState
DWORD OFFSET_LIFE_STATE = 0x690; // m_lifeState
DWORD OFFSET_ORIGIN = 0x17c; // origin
DWORD OFFSET_CAMERAPOS = 0x1fd4; //CPlayer!camera_origin 0x1fc4
DWORD OFFSET_ABS_VELOCITY = 0x170; //velo
DWORD OFFSET_BONES = 0xdb8 + 0x48;// bone 0xda8 + 0x48
DWORD OFFSET_AIMPUNCH = 0x2528; /// m_vecPunchWeapon_Angle
DWORD OFFSET_VIEWANGLES = 0x2624 - 0x14; // m_ammoPoolCapacity
DWORD OFFSET_BREATH_ANGLES = OFFSET_VIEWANGLES - 0x10;
DWORD OFFSET_FLAGS = 0xc8; // m_fFlags
DWORD armor_type = 0x487c; // m_armorType 0x485c
DWORD OFFSET_m_xp = 0x384c; // 0x380c m_xp

// Weapon & Projectiles
DWORD OFFSET_WEAPON = 0x19d4; // m_latestPrimaryWeapons 0x19C4
DWORD OFFSET_WEAPON_NAME = 0x1888; //[RecvTable.DT_WeaponX]->m_weaponNameIndex //0x1818
DWORD OFFSET_BULLET_SPEED = 0xda8; // //weapon_base + projectile_launch_speed
DWORD OFFSET_BULLET_SCALE = OFFSET_BULLET_SPEED + 0x08; // weapon_base + projectile_gravity_scale
DWORD OFFSET_ZOOM_FOV = 0x1724; //
DWORD OFFSET_AMMO = 0x1610; // m_ammoInClip
DWORD OFF_OFFHAND_WEAPON = 0x19e4; // m_latestNonOffhandWeapons 0x19a4
DWORD View_model = 0x2e28; // m_hViewModels 0x2e68

// Grapple
DWORD OFFSET_GRAPPLE = 0x2d48; // m_grapple
DWORD OFFSET_GRAPPLE_ACTIVE = 0x2dd0;
DWORD OFFSET_GRAPPLE_ATTACHED = 0x48;

// Actions / Input
DWORD OFFSET_IN_ATTACK = 0x3d9a658;
DWORD IN_JUMP = 0x3d9af10; // corrected 2026-05-13 via pastebin s9sqRHxZ EA App canon (was 0x3f39270 wrong build)
DWORD OFFSET_in_speed = 0x3d9a5e0;
DWORD OFFSET_IN_DUCK = 0x3d9b008;
DWORD OFFSET_in_forward = 0x3d9b048;
DWORD OFFSET_in_use = 0x3d9af80;
DWORD skin = 0xd68; // m_nSkin


// Traversal / Wallrun
DWORD OFFSET_TIME_BASE = 0x2178; //m_currentFramePlayer.timeBase
DWORD OFFSET_TRAVERSAL_START_TIME = 0x2bfc; //m_traversalStartTime
DWORD OFFSET_TRAVERSAL_PROGRESS = 0x2bf4; // m_traversalProgress
DWORD OFFSET_WALL_RUN_START_TIME = 0x3734; // m_wallRunStartTime
DWORD OFFSET_WALL_RUN_CLEAR_TIME = 0x3738; // m_wallRunClearTime

// Misc / Debug
DWORD OFF_STUDIOHDR = 0x1000;
DWORD crossPlay_Enabled = 0x1eada60; // pastebin s9sqRHxZ EA App canon confirms
DWORD showfps_enabled = 0x1f05300 + 0x6c; // base corrected 2026-05-13 via pastebin (was 0x1dd7290) — +0x6c is the convar struct field offset

#endif // HYPE_APEX_OFFSETS_H

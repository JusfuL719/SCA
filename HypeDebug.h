#pragma once

#include "HypeUefi.h"

// Define PEX_STEALTH_RELEASE to compile out all HV log emissions.
// Removes ~226 HvLog/HvLogHex callsites (saving binary size + .rdata strings
// + per-VMEXIT dispatcher cycles). Keeps the struct/global declarations so
// HypeDrain.c stays buildable, but emissions become no-ops and the 4 MB log
// buffer is never allocated.
//
//   #define PEX_STEALTH_RELEASE 1     <-- uncomment for ship build

#define HV_DEBUG_LOG_SIZE   (4U * 1024U * 1024U)

#pragma pack(push, 1)
typedef struct _HV_DEBUG_LOG {
    UINT64  Magic;
    UINT64  WritePos;
    CHAR8   Buffer[HV_DEBUG_LOG_SIZE - 16];
} HV_DEBUG_LOG;
#pragma pack(pop)

extern HV_DEBUG_LOG *g_DebugLog;

#ifdef PEX_STEALTH_RELEASE
  #define HvDebugInit()      ((void)0)
  #define HvLog(...)         ((void)0)
  #define HvLogHex(...)      ((void)0)
#else
  VOID HvDebugInit(VOID);
  VOID HvLog(const CHAR8 *Msg);
  VOID HvLogHex(const CHAR8 *Prefix, UINT64 Value);
#endif

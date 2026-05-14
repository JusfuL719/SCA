// 4 MB DMA-readable debug log. PCILeech search by gBootLogMagic.

#include "HypeDebug.h"

HV_DEBUG_LOG *g_DebugLog = NULL;

extern struct _DRIVER_CONTEXT g_DriverContext;

#ifndef PEX_STEALTH_RELEASE

VOID
HvDebugInit(
    VOID
    )
{
    EFI_STATUS          Status;
    EFI_PHYSICAL_ADDRESS PhysAddr;

    PhysAddr = HV_ALLOC_MAX_ADDRESS;

    Status = gBS->AllocatePages(
        AllocateMaxAddress,
        EfiRuntimeServicesData,
        1024,  // 4 MB
        &PhysAddr
    );

    if (EFI_ERROR(Status)) {
        return;
    }

    g_DebugLog = (HV_DEBUG_LOG *)(UINTN)PhysAddr;

    ZeroMem(g_DebugLog, HV_DEBUG_LOG_SIZE);

    g_DebugLog->Magic = gBootLogMagic;
    g_DebugLog->WritePos = 0;
}

// Wrap-around ring. Bounded CAS reservation — WritePos stays in [0, MaxLen).
// On wrap, oldest bytes are overwritten. Post-EBS safe.
VOID
HvLog(
    const CHAR8 *Msg
    )
{
    UINTN Len;
    UINTN Pos;
    UINTN NewPos;
    UINTN MaxLen;
    const CHAR8 *Src;

    if (g_DebugLog == NULL || Msg == NULL) {
        return;
    }

    Len = 0;
    Src = Msg;
    while (*Src++) {
        Len++;
    }

    MaxLen = HV_DEBUG_LOG_SIZE - 16;

    if (Len == 0 || Len > MaxLen) {
        return;
    }

    // Bounded CAS reservation. Two CPUs claiming adjacent regions straddling
    // the wrap point produce torn bytes at the seam — readers skip unknown-prefix.
    do {
        Pos = (UINTN)g_DebugLog->WritePos;
        NewPos = Pos + Len;
        if (NewPos >= MaxLen) NewPos -= MaxLen;
    } while (!__sync_bool_compare_and_swap(
        &g_DebugLog->WritePos, (UINT64)Pos, (UINT64)NewPos));

    Src = Msg;
    if (Pos + Len <= MaxLen) {
        for (UINTN i = 0; i < Len; i++) {
            g_DebugLog->Buffer[Pos + i] = Src[i];
        }
    } else {
        UINTN Tail = MaxLen - Pos;
        for (UINTN i = 0; i < Tail; i++) {
            g_DebugLog->Buffer[Pos + i] = Src[i];
        }
        for (UINTN i = 0; i < Len - Tail; i++) {
            g_DebugLog->Buffer[i] = Src[Tail + i];
        }
    }
}

// "Prefix: 0xHHHHHHHHHHHHHHHH\n" — fixed width for DMA scan.
VOID
HvLogHex(

    const CHAR8 *Prefix,
    UINT64       Value
    )
{
    CHAR8 HexBuf[64];
    UINTN Idx = 0;
    UINTN i;
    UINT8 Nibble;
    const CHAR8 HexChars[] = "0123456789ABCDEF";
    const CHAR8 *Src;

    if (g_DebugLog == NULL) {
        return;
    }

    if (Prefix != NULL) {
        Src = Prefix;
        while (*Src && Idx < 40) {
            HexBuf[Idx++] = *Src++;
        }
    }

    HexBuf[Idx++] = ':';
    HexBuf[Idx++] = ' ';
    HexBuf[Idx++] = '0';
    HexBuf[Idx++] = 'x';

    for (i = 0; i < 16; i++) {
        Nibble = (UINT8)((Value >> (60 - i * 4)) & 0xF);
        HexBuf[Idx++] = HexChars[Nibble];
    }

    HexBuf[Idx++] = '\n';
    HexBuf[Idx] = '\0';

    HvLog(HexBuf);
}

#endif  // !PEX_STEALTH_RELEASE

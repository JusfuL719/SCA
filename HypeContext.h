#pragma once

#include "HypeSvm.h"
#include "HypeNpt.h"
#include "HypeIdt.h"

typedef struct _DRIVER_CONTEXT {
    HYPERVISOR_STATE    HvState;
    NPT_CONTEXT         NptContext;
    HOST_IDT_CONTEXT    HostIdtContext;
} DRIVER_CONTEXT, *PDRIVER_CONTEXT;

typedef struct _UEFI_HV_CONTEXT {
    UINTN       NumProcessors;
    UINTN       NumEnabledProcessors;
    UINTN       BspNumber;
    BOOLEAN     Initialized;
} UEFI_HV_CONTEXT;

extern UEFI_HV_CONTEXT gUefiHvContext;



#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/DevicePathLib.h>
#include <Pi/PiDxeCis.h>
#include <Protocol/MpService.h>
#include <Protocol/LoadedImage.h>

#include "HypeUefi.h"
#include "HypeDebug.h"
#include "HypeContext.h"

extern DRIVER_CONTEXT g_DriverContext;

EFI_MP_SERVICES_PROTOCOL *gMpServices = NULL;

UINT64 gHypeLoaderImageBase = 0;
UINT64 gHypeLoaderImageSize = 0;

extern NTSTATUS HypeInit(VOID);
extern NTSTATUS LaunchAllAPs(VOID);
extern NTSTATUS HypeStartBsp(VOID);

UINT64 gBootCanary   = 0;
UINT64 gBootAuthKey  = 0;
UINT64 gBootLogMagic = 0;
UINT64 gHandshakeExpected = 0;

STATIC
BOOLEAN
BootTryRdseed(UINT64 *Out)
{
    UINT8 Ok = 0;
    for (UINT32 Retry = 0; Retry < 16; Retry++) {
        __asm__ volatile (
            "rdseed %0; setc %1"
            : "=r"(*Out), "=qm"(Ok)
            :
            : "cc"
        );
        if (Ok) return TRUE;
    }
    return FALSE;
}

STATIC
BOOLEAN
BootTryRdrand(UINT64 *Out)
{
    UINT8 Ok = 0;
    for (UINT32 Retry = 0; Retry < 16; Retry++) {
        __asm__ volatile (
            "rdrand %0; setc %1"
            : "=r"(*Out), "=qm"(Ok)
            :
            : "cc"
        );
        if (Ok) return TRUE;
    }
    return FALSE;
}

STATIC
UINT64
MixKey(UINT64 x)
{
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

#define BOOT_ENTROPY(v) do { \
    if (!BootTryRdseed(&(v))) { \
        if (!BootTryRdrand(&(v))) { \
            (v) = __rdtsc(); \
        } \
    } \
} while (0)

VOID
HvBootKeyInit(VOID)
{
    UINT64 Seed = 0;
    UINT64 Extra = 0;

    BOOT_ENTROPY(Seed);
    BOOT_ENTROPY(Extra);
    Seed ^= (Extra << 17) | (Extra >> 47);

    if (Seed == 0) Seed = __rdtsc() | 1;

    gBootCanary   = MixKey(Seed);
    gBootAuthKey  = MixKey(Seed + 1);
    gBootLogMagic = 0x4859504544424700ULL ^ MixKey(Seed + 2);
    gHandshakeExpected = MixKey(HYPE_BUILD_SECRET);
}

UEFI_HV_CONTEXT gUefiHvContext = {0};

STATIC
EFI_STATUS
InitializeMpServices(
    VOID
    )
{
    EFI_STATUS Status;

    Status = gBS->LocateProtocol(
        &gEfiMpServiceProtocolGuid,
        NULL,
        (VOID **)&gMpServices
    );

    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_WARN, "[HYPE] MP Services not available (single-core?): %r\n", Status));
        gUefiHvContext.NumProcessors = 1;
        gUefiHvContext.NumEnabledProcessors = 1;
        gUefiHvContext.BspNumber = 0;
        return EFI_SUCCESS;
    }

    Status = gMpServices->GetNumberOfProcessors(
        gMpServices,
        &gUefiHvContext.NumProcessors,
        &gUefiHvContext.NumEnabledProcessors
    );

    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_ERROR, "[HYPE] Failed to get processor count: %r\n", Status));
        return Status;
    }

    Status = gMpServices->WhoAmI(gMpServices, &gUefiHvContext.BspNumber);
    if (EFI_ERROR(Status)) {
        gUefiHvContext.BspNumber = 0;
    }

    DEBUG((DEBUG_INFO, "[HYPE] Processors: %d total, %d enabled, BSP=#%d\n",
           gUefiHvContext.NumProcessors,
           gUefiHvContext.NumEnabledProcessors,
           gUefiHvContext.BspNumber));

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
HypeLoaderEntry(
    IN EFI_HANDLE           ImageHandle,
    IN EFI_SYSTEM_TABLE     *SystemTable
    )
{
    EFI_STATUS Status;
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage = NULL;
    UINT64 Tsc0, Tsc1;

    DEBUG((DEBUG_INFO, "\n"));
    DEBUG((DEBUG_INFO, "===========================================\n"));
    DEBUG((DEBUG_INFO, "[HYPE] HypeLoader v%d.%d - DXE Runtime Driver\n",
           DRIVER_VERSION_MAJOR, DRIVER_VERSION_MINOR));
    DEBUG((DEBUG_INFO, "===========================================\n"));

    Print(L"[HYPE] v%d.%d entry\r\n", DRIVER_VERSION_MAJOR, DRIVER_VERSION_MINOR);

    HvBootKeyInit();
    Print(L"[HYPE] keys OK\r\n");

    HvDebugInit();
    HvLog("E22\n");
    Print(L"[HYPE] debug log @ PA 0x%lx\r\n", (UINT64)(UINTN)g_DebugLog);

    Tsc0 = __rdtsc();

    Status = gBS->HandleProtocol(
        ImageHandle,
        &gEfiLoadedImageProtocolGuid,
        (VOID **)&LoadedImage
    );
    if (!EFI_ERROR(Status) && LoadedImage) {
        gHypeLoaderImageBase = (UINT64)(UINTN)LoadedImage->ImageBase;
        gHypeLoaderImageSize = LoadedImage->ImageSize;
        Print(L"[HYPE] image @ 0x%lx size 0x%lx\r\n",
              gHypeLoaderImageBase, gHypeLoaderImageSize);
        DEBUG((DEBUG_INFO, "[HYPE] Image base: 0x%lx, size: 0x%lx\n",
               gHypeLoaderImageBase, gHypeLoaderImageSize));
        HvLogHex("E09", gHypeLoaderImageBase);
        HvLogHex("E10", gHypeLoaderImageSize);
    } else {
        Print(L"[HYPE] FAIL: LoadedImage query %r\r\n", Status);
        DEBUG((DEBUG_WARN, "[HYPE] Could not query LoadedImage protocol: %r\n", Status));
        HvLog("E23\n");
    }

    HvLog("E44\n");
    Print(L"[HYPE] MP init...\r\n");
    Status = InitializeMpServices();
    if (EFI_ERROR(Status)) {
        HvLog("E45\n");
        HvLogHex("E46", (UINT64)Status);
        Print(L"[HYPE] FAIL: MP init %r\r\n", Status);
        return Status;
    }
    HvLog("E47\n");
    HvLogHex("E48", (UINT64)gUefiHvContext.NumProcessors);
    Print(L"[HYPE] MP OK - %d CPUs\r\n", (UINT32)gUefiHvContext.NumProcessors);

    HvLog("E49\n");
    Print(L"[HYPE] HypeInit...\r\n");
    Status = HypeInit();
    if (EFI_ERROR(Status)) {
        HvLog("E50\n");
        HvLogHex("E51", (UINT64)Status);
        Print(L"[HYPE] FAIL: HypeInit %r\r\n", Status);
        return Status;
    }

    gUefiHvContext.Initialized = TRUE;
    HvLog("E54\n");
    HvLog("EBP\n");
    Print(L"[HYPE] init OK\r\n");

    {
        NTSTATUS BspStatus = HypeStartBsp();
        if (BspStatus == 0) {
            HvLog("E66\n");
        } else {
            HvLog("E67\n");
            HvLogHex("E68", (UINT64)BspStatus);
        }
    }

    {
        NTSTATUS ApStatus = LaunchAllAPs();
        if (ApStatus == 0) {
            HvLog("E63\n");
        } else {
            HvLog("E64\n");
            HvLogHex("E65", (UINT64)ApStatus);
        }
    }

    Tsc1 = __rdtsc();
    HvLogHex("E11", Tsc1 - Tsc0);

    return EFI_SUCCESS;
}

// HypeDrain — scan PA [0x400000,0x3FC00000] for 4MB log ring, dump to hypedbg-<TSC>.bin.
// Slot match: nonzero/non-UINT64_MAX Magic, in-range WritePos, ≥16 printable in first 128B.

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>

#define HV_DEBUG_LOG_SIZE       (4U * 1024U * 1024U)

#pragma pack(push, 1)
typedef struct {
    UINT64  Magic;
    UINT64  WritePos;
    CHAR8   Buffer[HV_DEBUG_LOG_SIZE - 16];
} HV_DEBUG_LOG;
#pragma pack(pop)

STATIC BOOLEAN
LooksLikeDebugLog (
    IN HV_DEBUG_LOG *Log
    )
{
    UINTN  MaxLen = HV_DEBUG_LOG_SIZE - 16;
    UINTN  AsciiCount = 0;
    UINTN  i;
    CHAR8  C;

    if (Log->Magic == 0 || Log->Magic == 0xFFFFFFFFFFFFFFFFULL) {
        return FALSE;
    }
    if (Log->WritePos >= (UINT64)MaxLen) {
        return FALSE;
    }
    for (i = 0; i < 128; i++) {
        C = Log->Buffer[i];
        if (C == 0 || (C >= 0x20 && C <= 0x7E)) {
            AsciiCount++;
        }
    }
    return (AsciiCount >= 16);
}

STATIC EFI_STATUS
DumpToFile (
    IN EFI_FILE_HANDLE  Root,
    IN CHAR16           *FileName,
    IN VOID             *Data,
    IN UINTN             Len
    )
{
    EFI_STATUS      Status;
    EFI_FILE_HANDLE File;

    Status = Root->Open(
        Root, &File, FileName,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
        0);
    if (EFI_ERROR(Status)) {
        return Status;
    }
    Status = File->Write(File, &Len, Data);
    File->Close(File);
    return Status;
}

EFI_STATUS
EFIAPI
HypeDrainEntry (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE  *SystemTable
    )
{
    EFI_STATUS                        Status;
    EFI_PHYSICAL_ADDRESS              Pa;
    HV_DEBUG_LOG                      *Log = NULL;
    UINT64                            Tsc;
    CHAR16                            FileName[64];
    EFI_LOADED_IMAGE_PROTOCOL         *LoadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL   *Fs;
    EFI_FILE_HANDLE                   Root;

    Print(L"HypeDrain: scanning 4MB-aligned PA slots [0x400000, 0x3FC00000]...\n");

    for (Pa = 0x00400000ULL; Pa <= 0x3FC00000ULL; Pa += HV_DEBUG_LOG_SIZE) {
        HV_DEBUG_LOG *Candidate = (HV_DEBUG_LOG *)(UINTN)Pa;
        if (LooksLikeDebugLog(Candidate)) {
            Log = Candidate;
            break;
        }
    }

    if (Log == NULL) {
        Print(L"HypeDrain: no log found — was HypeLoader booted this session?\n");
        return EFI_NOT_FOUND;
    }

    Print(L"HypeDrain: hit at PA 0x%lX  Magic=0x%016lX  WritePos=0x%lX\n",
          (UINTN)Log, Log->Magic, (UINTN)Log->WritePos);

    Tsc = AsmReadTsc();
    UnicodeSPrint(FileName, sizeof(FileName), L"hypedbg-%016lX.bin", Tsc);

    Status = gBS->HandleProtocol(
        ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
    if (EFI_ERROR(Status)) {
        Print(L"HypeDrain: LoadedImage failed: %r\n", Status);
        return Status;
    }

    Status = gBS->HandleProtocol(
        LoadedImage->DeviceHandle,
        &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR(Status)) {
        Print(L"HypeDrain: SimpleFileSystem on boot device failed: %r\n", Status);
        return Status;
    }

    Status = Fs->OpenVolume(Fs, &Root);
    if (EFI_ERROR(Status)) {
        Print(L"HypeDrain: OpenVolume failed: %r\n", Status);
        return Status;
    }

    Status = DumpToFile(Root, FileName, Log, HV_DEBUG_LOG_SIZE);
    Root->Close(Root);

    if (EFI_ERROR(Status)) {
        Print(L"HypeDrain: write %s failed: %r\n", FileName, Status);
        return Status;
    }

    Print(L"HypeDrain: wrote %u bytes -> %s\n", (UINT32)HV_DEBUG_LOG_SIZE, FileName);
    Print(L"Decode:  strings -n 3 %s | grep \"^V4\"\n", FileName);
    return EFI_SUCCESS;
}

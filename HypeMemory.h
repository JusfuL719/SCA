

#ifndef HYPE_MEMORY_H
#define HYPE_MEMORY_H

#include "HypeUefi.h"
#include "HypeSvm.h"
#include "HypeNpt.h"

#define GUEST_PTE_PRESENT       (1ULL << 0)
#define GUEST_PTE_LARGE_PAGE    (1ULL << 7)

#define GUEST_PTE_ADDR_4KB      0x000FFFFFFFFFF000ULL
#define GUEST_PTE_ADDR_2MB      0x000FFFFFFFE00000ULL
#define GUEST_PTE_ADDR_1GB      0x000FFFFFC0000000ULL

#define GUEST_PML4_INDEX(va)    (((va) >> 39) & 0x1FF)
#define GUEST_PDPT_INDEX(va)    (((va) >> 30) & 0x1FF)
#define GUEST_PD_INDEX(va)      (((va) >> 21) & 0x1FF)
#define GUEST_PT_INDEX(va)      (((va) >> 12) & 0x1FF)

#define GUEST_OFFSET_4KB(va)    ((va) & 0xFFF)
#define GUEST_OFFSET_2MB(va)    ((va) & 0x1FFFFF)
#define GUEST_OFFSET_1GB(va)    ((va) & 0x3FFFFFFF)

#define HYPE_MEM_OK             0x00000000
#define HYPE_MEM_ERR_UNMAPPED   0x00000001
#define HYPE_MEM_ERR_RANGE      0x00000002
#define HYPE_MEM_ERR_SIZE       0x00000003
#define HYPE_MEM_ERR_NOT_FOUND  0x00000005

#define DTB_PHYS_MASK           0x0000FFFFFFFFF000ULL

static inline UINT64 SanitizeDtb(UINT64 RawDtb) {
    return RawDtb & DTB_PHYS_MASK;
}

extern UINT64 gEffectiveLimit;

static inline __attribute__((always_inline))
EFI_STATUS
ReadGuestPhysical(
    UINT64  Gpa,
    VOID    *Buffer,
    UINT32  Size
    )
{
    if (Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    if (Size == 0 || Size > PAGE_SIZE_4KB) {
        return HYPE_MEM_ERR_SIZE;
    }

    {
        UINT64 Limit = gEffectiveLimit;
        if (Limit == 0) Limit = HOST_PT_MAX - 1;
        if (Size > Limit + 1 || Gpa > Limit + 1 - Size) {
            return HYPE_MEM_ERR_RANGE;
        }
    }

    RtlCopyMemory(Buffer, (VOID *)(UINTN)Gpa, Size);
    return EFI_SUCCESS;
}

static inline __attribute__((always_inline))
EFI_STATUS
WriteGuestPhysical(
    UINT64  Gpa,
    VOID    *Buffer,
    UINT32  Size
    )
{
    if (Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    if (Size == 0 || Size > PAGE_SIZE_4KB) {
        return HYPE_MEM_ERR_SIZE;
    }

    {
        UINT64 Limit = gEffectiveLimit;
        if (Limit == 0) Limit = HOST_PT_MAX - 1;
        if (Size > Limit + 1 || Gpa > Limit + 1 - Size) {
            return HYPE_MEM_ERR_RANGE;
        }
    }

    RtlCopyMemory((VOID *)(UINTN)Gpa, Buffer, Size);
    return EFI_SUCCESS;
}

static inline __attribute__((always_inline))
EFI_STATUS
TranslateGuestVirtual(
    UINT64  GuestCr3,
    UINT64  GuestVa,
    UINT64  *PhysAddr
    )
{
    EFI_STATUS  Status;
    UINT64      TableBase;
    UINT64      Entry;
    UINT64      EntryAddr;

    if (PhysAddr == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    *PhysAddr = 0;

    TableBase = GuestCr3 & GUEST_PTE_ADDR_4KB;

    EntryAddr = TableBase + (GUEST_PML4_INDEX(GuestVa) * sizeof(UINT64));
    Status = ReadGuestPhysical(EntryAddr, &Entry, sizeof(UINT64));
    if (EFI_ERROR(Status)) return HYPE_MEM_ERR_UNMAPPED;
    if (!(Entry & GUEST_PTE_PRESENT)) return HYPE_MEM_ERR_UNMAPPED;

    TableBase = Entry & GUEST_PTE_ADDR_4KB;
    EntryAddr = TableBase + (GUEST_PDPT_INDEX(GuestVa) * sizeof(UINT64));
    Status = ReadGuestPhysical(EntryAddr, &Entry, sizeof(UINT64));
    if (EFI_ERROR(Status)) return HYPE_MEM_ERR_UNMAPPED;
    if (!(Entry & GUEST_PTE_PRESENT)) return HYPE_MEM_ERR_UNMAPPED;

    if (Entry & GUEST_PTE_LARGE_PAGE) {
        *PhysAddr = (Entry & GUEST_PTE_ADDR_1GB) | GUEST_OFFSET_1GB(GuestVa);
        return EFI_SUCCESS;
    }

    TableBase = Entry & GUEST_PTE_ADDR_4KB;
    EntryAddr = TableBase + (GUEST_PD_INDEX(GuestVa) * sizeof(UINT64));
    Status = ReadGuestPhysical(EntryAddr, &Entry, sizeof(UINT64));
    if (EFI_ERROR(Status)) return HYPE_MEM_ERR_UNMAPPED;
    if (!(Entry & GUEST_PTE_PRESENT)) return HYPE_MEM_ERR_UNMAPPED;

    if (Entry & GUEST_PTE_LARGE_PAGE) {
        *PhysAddr = (Entry & GUEST_PTE_ADDR_2MB) | GUEST_OFFSET_2MB(GuestVa);
        return EFI_SUCCESS;
    }

    TableBase = Entry & GUEST_PTE_ADDR_4KB;
    EntryAddr = TableBase + (GUEST_PT_INDEX(GuestVa) * sizeof(UINT64));
    Status = ReadGuestPhysical(EntryAddr, &Entry, sizeof(UINT64));
    if (EFI_ERROR(Status)) return HYPE_MEM_ERR_UNMAPPED;
    if (!(Entry & GUEST_PTE_PRESENT)) return HYPE_MEM_ERR_UNMAPPED;

    *PhysAddr = (Entry & GUEST_PTE_ADDR_4KB) | GUEST_OFFSET_4KB(GuestVa);
    return EFI_SUCCESS;
}

static inline EFI_STATUS ReadGuestVirtual(UINT64 Cr3, UINT64 Va,
                                          VOID *Buf, UINT32 Size) {
    UINT64 Pa = 0;
    EFI_STATUS St = TranslateGuestVirtual(Cr3, Va, &Pa);
    if (EFI_ERROR(St)) return St;
    return ReadGuestPhysical(Pa, Buf, Size);
}

static inline EFI_STATUS WriteGuestVirtual(UINT64 Cr3, UINT64 Va,
                                           VOID *Buf, UINT32 Size) {
    UINT64 Pa = 0;
    EFI_STATUS St = TranslateGuestVirtual(Cr3, Va, &Pa);
    if (EFI_ERROR(St)) return St;
    return WriteGuestPhysical(Pa, Buf, Size);
}

#endif

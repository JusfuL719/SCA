

#ifndef HYPE_NPT_H
#define HYPE_NPT_H

#include "HypeUefi.h"

#define NPT_PRESENT         (1ULL << 0)
#define NPT_WRITE           (1ULL << 1)
#define NPT_USER            (1ULL << 2)
#define NPT_PWT             (1ULL << 3)
#define NPT_PCD             (1ULL << 4)
#define NPT_LARGE_PAGE      (1ULL << 7)
#define NPT_NX              (1ULL << 63)

#define NPT_ADDR_MASK       0x000FFFFFFFFFF000ULL
#define NPT_LARGE_2MB_MASK  0x000FFFFFFFE00000ULL

#define PAGE_SIZE_4KB       (4ULL * 1024)
#define PAGE_SIZE_2MB       (2ULL * 1024 * 1024)
#define PAGE_SIZE_1GB       (1ULL * 1024 * 1024 * 1024)

#define NPT_PML4_INDEX(addr)    (((addr) >> 39) & 0x1FF)
#define NPT_PDPT_INDEX(addr)    (((addr) >> 30) & 0x1FF)
#define NPT_PD_INDEX(addr)      (((addr) >> 21) & 0x1FF)
#define NPT_PT_INDEX(addr)      (((addr) >> 12) & 0x1FF)

#define NPT_DEFAULT_2MB     (NPT_PRESENT | NPT_WRITE | NPT_USER | NPT_LARGE_PAGE)

#define HOST_PT_MAX  (64ULL * 1024 * 1024 * 1024)

typedef struct _NPT_PAGE_ENTRY {
    LIST_ENTRY  ListEntry;
    VOID        *VirtualAddress;
    UINT64      PhysicalAddress;
} NPT_PAGE_ENTRY, *PNPT_PAGE_ENTRY;

typedef struct _NPT_PROTECTED_REGION {
    LIST_ENTRY  ListEntry;
    UINT64      PhysicalBase;
    UINT64      Size;
} NPT_PROTECTED_REGION, *PNPT_PROTECTED_REGION;

typedef struct _NPT_PROTECTED_RANGE {
    UINT64      Start;
    UINT64      End;
} NPT_PROTECTED_RANGE, *PNPT_PROTECTED_RANGE;

typedef struct _NPT_CONTEXT {
    UINT64          *Pml4;
    UINT64          Pml4Physical;

    LIST_ENTRY      PageList;
    UINT32          PageCount;

    LIST_ENTRY      ProtectedList;
    UINT32          ProtectedCount;

    PNPT_PROTECTED_RANGE    SortedRanges;
    UINT32                  SortedRangeCount;
    UINT32                  SortedRangeCapacity;
    BOOLEAN                 ProtectionsFinalized;

    UINT64          MaxPhysicalAddress;
    UINT32          PhysicalAddressBits;

    UINT64          EffectiveLimit;

    UINT64          *ZeroPage;
    UINT64          ZeroPagePhysical;

    UINT64          DecoyPoolBase;
    UINT64          DecoyPoolSize;
    volatile UINT64 DecoyPoolNext;

    KSPIN_LOCK      RemapLock;

    #define NPT_SPARE_PT_PAGES  64
    UINT64          *SparePageVa[NPT_SPARE_PT_PAGES];
    UINT64          SparePagePa[NPT_SPARE_PT_PAGES];
    volatile LONG   SparePageCount;

    UINT64          MaxProtectedEnd;

} NPT_CONTEXT, *PNPT_CONTEXT;

NTSTATUS NptBuildIdentityMap(PNPT_CONTEXT Context);
VOID NptDestroy(PNPT_CONTEXT Context);
NTSTATUS NptSplitLargePage(PNPT_CONTEXT Context, UINT64 GuestPhysical);
NTSTATUS NptSplitLargePageRuntime(PNPT_CONTEXT Context, UINT64 GuestPhysical);
UINT64 *NptGetPte(PNPT_CONTEXT Context, UINT64 GuestPhysical, BOOLEAN Allocate);

NTSTATUS NptAddProtectedRegion(PNPT_CONTEXT Context, UINT64 PhysicalBase, UINT64 Size);
NTSTATUS NptProtectOwnPages(PNPT_CONTEXT Context);
NTSTATUS NptApplyProtections(PNPT_CONTEXT Context);
NTSTATUS NptFinalizeProtections(PNPT_CONTEXT Context);
BOOLEAN NptIsProtectedAddress(PNPT_CONTEXT Context, UINT64 GuestPhysical);

NTSTATUS NptInitDecoyPool(PNPT_CONTEXT Context, UINT32 PageCount);
UINT64 NptAllocateDecoyPage(PNPT_CONTEXT Context);
NTSTATUS NptRemapToDecoy(PNPT_CONTEXT Context, UINT64 GuestPhysical);

#endif

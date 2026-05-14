

#ifndef HYPE_VMCB_H
#define HYPE_VMCB_H

#include "HypeUefi.h"

#pragma pack(push, 1)

typedef struct _VMCB_SEGMENT {
    UINT16      Selector;
    UINT16      Attrib;
    UINT32      Limit;
    UINT64      Base;
} VMCB_SEGMENT;

typedef struct _VMCB_CONTROL {
    UINT16      InterceptCrRead;
    UINT16      InterceptCrWrite;
    UINT16      InterceptDrRead;
    UINT16      InterceptDrWrite;
    UINT32      InterceptExceptions;
    UINT32      InterceptMisc1;
    UINT32      InterceptMisc2;
    UINT8       Reserved1[0x03C - 0x14];

    UINT16      PauseFilterThreshold;
    UINT16      PauseFilterCount;

    UINT64      IopmBasePA;
    UINT64      MsrpmBasePA;
    UINT64      TscOffset;
    UINT32      GuestAsid;
    UINT8       TlbControl;
    UINT8       Reserved2[3];

    UINT64      VIntr;

    UINT64      InterruptShadow;

    UINT64      ExitCode;
    UINT64      ExitInfo1;
    UINT64      ExitInfo2;
    UINT64      ExitIntInfo;

    UINT64      NpEnable;

    UINT64      AvicApicBar;

    UINT64      GhcbGpa;

    UINT64      EventInj;

    UINT64      NCr3;

    UINT64      LbrVirt;

    UINT32      VmcbClean;
    UINT32      Reserved3;

    UINT64      NRip;

    UINT8       NumBytes;
    UINT8       GuestInstructionBytes[15];

    UINT64      AvicBackingPage;

    UINT8       Reserved4[8];

    UINT64      AvicLogicalTable;
    UINT64      AvicPhysicalTable;

    UINT8       Reserved5[0x108 - 0x100];

    UINT64      VmcbSaveStatePointer;

    UINT8       Reserved6[0x400 - 0x110];

} VMCB_CONTROL;

typedef struct _VMCB_SAVE {
    VMCB_SEGMENT    ES;
    VMCB_SEGMENT    CS;
    VMCB_SEGMENT    SS;
    VMCB_SEGMENT    DS;
    VMCB_SEGMENT    FS;
    VMCB_SEGMENT    GS;
    VMCB_SEGMENT    GDTR;
    VMCB_SEGMENT    LDTR;
    VMCB_SEGMENT    IDTR;
    VMCB_SEGMENT    TR;

    UINT8           Reserved1[0x4CB - 0x4A0];

    UINT8           Cpl;

    UINT8           Reserved2[4];

    UINT64          Efer;

    UINT8           Reserved3[0x548 - 0x4D8];

    UINT64          Cr4;
    UINT64          Cr3;
    UINT64          Cr0;
    UINT64          Dr7;
    UINT64          Dr6;

    UINT64          Rflags;
    UINT64          Rip;

    UINT8           Reserved4[0x5D8 - 0x580];

    UINT64          Rsp;

    UINT8           Reserved5[0x5F8 - 0x5E0];

    UINT64          Rax;

    UINT64          Star;
    UINT64          Lstar;
    UINT64          Cstar;
    UINT64          Sfmask;
    UINT64          KernelGsBase;

    UINT64          SysenterCs;
    UINT64          SysenterEsp;
    UINT64          SysenterEip;
    UINT64          Cr2;

    UINT8           Reserved6[0x668 - 0x648];

    UINT64          GPat;
    UINT64          DbgCtl;
    UINT64          BrFrom;
    UINT64          BrTo;
    UINT64          LastExcepFrom;
    UINT64          LastExcepTo;

    UINT8           Reserved7[0x6E0 - 0x698];

    UINT64          SpecCtrl;

    UINT8           Reserved8[0x1000 - 0x6E8];

} VMCB_SAVE;

typedef struct _VMCB {
    VMCB_CONTROL    Control;
    VMCB_SAVE       Save;
} VMCB, *PVMCB;

#pragma pack(pop)

// Misc1 intercepts (in-use subset; AMD APM Vol 2 Table B-1).
#define INTERCEPT_IOIO          (1 << 27)
#define INTERCEPT_MSR           (1 << 28)
#define INTERCEPT_SHUTDOWN      (1 << 31)

// Misc2 intercepts (in-use subset).
#define INTERCEPT_VMRUN         (1 << 0)
#define INTERCEPT_VMMCALL       (1 << 1)
#define INTERCEPT_VMLOAD        (1 << 2)
#define INTERCEPT_VMSAVE        (1 << 3)
#define INTERCEPT_STGI          (1 << 4)
#define INTERCEPT_CLGI          (1 << 5)
#define INTERCEPT_SKINIT        (1 << 6)

// VMEXIT codes (in-use subset).
#define VMEXIT_EXCP_BASE        0x040
#define VMEXIT_IOIO             0x07B
#define VMEXIT_MSR              0x07C
#define VMEXIT_SHUTDOWN         0x07F
#define VMEXIT_VMRUN            0x080
#define VMEXIT_VMMCALL          0x081
#define VMEXIT_VMLOAD           0x082
#define VMEXIT_VMSAVE           0x083
#define VMEXIT_STGI             0x084
#define VMEXIT_CLGI             0x085
#define VMEXIT_SKINIT           0x086
#define VMEXIT_NPF              0x400
#define VMEXIT_INVALID          0xFFFFFFFFFFFFFFFFULL

// Exception vectors (in-use subset; intercepts are #DB and #SX,
// injects fault on #UD/#GP/#PF paths).
#define EXCEPTION_DB            1
#define EXCEPTION_UD            6
#define EXCEPTION_GP            13
#define EXCEPTION_PF            14
#define EXCEPTION_SX            30

// EventInj fields (in-use subset).
#define EVENT_INJ_VALID         (1ULL << 31)
#define EVENT_INJ_TYPE_EXCEPT   (3ULL << 8)
#define EVENT_INJ_EV            (1ULL << 11)

#define VMCB_CLEAN_IOMSRPM      (1 << 1)
#define VMCB_CLEAN_ASID         (1 << 2)
#define VMCB_CLEAN_TPR          (1 << 3)
#define VMCB_CLEAN_NP           (1 << 4)
#define VMCB_CLEAN_CRX          (1 << 5)
#define VMCB_CLEAN_DRX          (1 << 6)
#define VMCB_CLEAN_DT           (1 << 7)
#define VMCB_CLEAN_SEG          (1 << 8)
#define VMCB_CLEAN_CR2          (1 << 9)
#define VMCB_CLEAN_LBR          (1 << 10)
#define VMCB_CLEAN_AVIC         (1 << 11)

#endif

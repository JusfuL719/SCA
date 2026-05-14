; VMRUN loop. AMD64 APM Vol 2 Ch. 15. UEFI x64 ABI: RCX=arg1, RDX=arg2, R8=arg3.

    SECTION .text

; INVARIANT: VCPU_DATA offsets MUST match HypeSvm.h exactly.
VCPU_VMCB           EQU 000h
VCPU_VMCB_PA        EQU 008h
VCPU_HOST_SAVE      EQU 010h
VCPU_HOST_SAVE_PA   EQU 018h
VCPU_HOST_STACK     EQU 020h
VCPU_HOST_STACK_SZ  EQU 028h
VCPU_GUEST_RBX      EQU 030h
VCPU_GUEST_RCX      EQU 038h
VCPU_GUEST_RDX      EQU 040h
VCPU_GUEST_RSI      EQU 048h
VCPU_GUEST_RDI      EQU 050h
VCPU_GUEST_RBP      EQU 058h
VCPU_GUEST_R8       EQU 060h
VCPU_GUEST_R9       EQU 068h
VCPU_GUEST_R10      EQU 070h
VCPU_GUEST_R11      EQU 078h
VCPU_GUEST_R12      EQU 080h
VCPU_GUEST_R13      EQU 088h
VCPU_GUEST_R14      EQU 090h
VCPU_GUEST_R15      EQU 098h

VCPU_XSAVE_AREA     EQU 0A8h
VCPU_XSAVE_SIZE     EQU 0B8h
VCPU_XSAVE_MASK     EQU 0C0h

VCPU_SHOULD_EXIT    EQU 118h

; Per-CPU debug ring offsets (must match HypeSvm.h)
VCPU_RING_EXIT_CODE EQU 170h
VCPU_RING_RIP       EQU 1B0h
VCPU_RING_INDEX     EQU 1F0h
VCPU_VMEXIT_COUNT   EQU 1F8h
VCPU_STATE_MARKER   EQU 200h

; F-PERF-14: must match HYPE_DEBUG_RING in HypeSvm.h
%define HYPE_DEBUG_RING 1

VMCB_SAVE_RSP       EQU 5D8h
VMCB_SAVE_RIP       EQU 578h
VMCB_SAVE_RFLAGS    EQU 570h
VMCB_SAVE_RAX       EQU 5F8h

VMCB_CTRL_EXIT_CODE  EQU 070h

extern VmexitHandler
extern gUseXsaveopt

; void SvmLaunch(VCPU_DATA *Vcpu) — RCX = VCPU. Returns only on devirtualization.
global ASM_PFX(SvmLaunch)
ASM_PFX(SvmLaunch):
    ; Save non-volatile regs (UEFI x64 ABI).
    push    rbp
    mov     rbp, rsp
    push    rbx
    push    rdi
    push    rsi
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 28h            ; shadow space

    ; INVARIANT: R15 = VCPU pointer throughout VMRUN loop.
    mov     r15, rcx

    mov     r14, [r15 + VCPU_VMCB]

    ; Guest resumes at .guest_entry on first VMRUN.
    mov     [r14 + VMCB_SAVE_RSP], rsp
    lea     rax, [rel .guest_entry]
    mov     [r14 + VMCB_SAVE_RIP], rax

    ; Switch to host stack (top of region, 16-byte aligned).
    mov     rsp, [r15 + VCPU_HOST_STACK]
    add     rsp, [r15 + VCPU_HOST_STACK_SZ]
    and     rsp, 0FFFFFFFFFFFFFFF0h

    jmp     .vmrun_first_entry

.vmrun_loop:
    clgi

    ; XRSTOR64 guest extended state (if enabled)
    mov     rax, [r15 + VCPU_XSAVE_AREA]
    test    rax, rax
    jz      .skip_xrstor
    mov     rcx, rax
    mov     eax, dword [r15 + VCPU_XSAVE_MASK]
    mov     edx, dword [r15 + VCPU_XSAVE_MASK + 4]
    db      048h, 00Fh, 0AEh, 029h    ; xrstor64 [rcx]
.skip_xrstor:

    mov     rax, [r15 + VCPU_VMCB_PA]
    vmload

    ; Guest RDTSC/RDTSCP execute natively. No TSC comp in asm hot path.
    jmp     .vmrun_gprs_load

.vmrun_first_entry:
    mov     rax, [r15 + VCPU_VMCB_PA]
    clgi
    cli             ; IF=0 saved to host save area by VMRUN — ensures physical
                    ; interrupts never fire in host context on VMEXIT.
    vmload

.vmrun_gprs_load:
    ; Hardware handles RAX (VMRUN loads from VMCB, VMEXIT saves back). We
    ; manage RBX-R15 manually. [RSP] = VCPU pointer for VMEXIT recovery.
    push    r15

    mov     rbx, [r15 + VCPU_GUEST_RBX]
    mov     rcx, [r15 + VCPU_GUEST_RCX]
    mov     rdx, [r15 + VCPU_GUEST_RDX]
    mov     rsi, [r15 + VCPU_GUEST_RSI]
    mov     rdi, [r15 + VCPU_GUEST_RDI]
    mov     rbp, [r15 + VCPU_GUEST_RBP]
    mov     r8,  [r15 + VCPU_GUEST_R8]
    mov     r9,  [r15 + VCPU_GUEST_R9]
    mov     r10, [r15 + VCPU_GUEST_R10]
    mov     r11, [r15 + VCPU_GUEST_R11]
    mov     r12, [r15 + VCPU_GUEST_R12]
    mov     r13, [r15 + VCPU_GUEST_R13]
    mov     r14, [r15 + VCPU_GUEST_R14]

    mov     rax, [r15 + VCPU_VMCB_PA]

    ; Load guest R15 last — destroys VCPU pointer, but it's on stack.
    mov     r15, [r15 + VCPU_GUEST_R15]

    ; Physical interrupts delivered through guest IDT (no VMEXIT).
    ; Guest CLI/STI controls masking natively (V_INTR_MASKING=0).
    vmrun

    ; State on VMEXIT (APM §15.6): R15=guest R15, guest RAX→VMCB.Save.RAX,
    ; RAX=VMCB PA (hw-restored), [RSP]=VCPU pointer.
    ;
    ; Avoid xchg reg,[mem] — implicit LOCK, ~100+ cy bus lock penalty.
    mov     rax, r15
    mov     r15, [rsp]
    mov     [rsp], rax

    mov     [r15 + VCPU_GUEST_RDX], rdx
    mov     [r15 + VCPU_GUEST_RBX], rbx
    mov     [r15 + VCPU_GUEST_RCX], rcx
    mov     [r15 + VCPU_GUEST_RSI], rsi
    mov     [r15 + VCPU_GUEST_RDI], rdi
    mov     [r15 + VCPU_GUEST_RBP], rbp
    mov     [r15 + VCPU_GUEST_R8],  r8
    mov     [r15 + VCPU_GUEST_R9],  r9
    mov     [r15 + VCPU_GUEST_R10], r10
    mov     [r15 + VCPU_GUEST_R11], r11
    mov     [r15 + VCPU_GUEST_R12], r12
    mov     [r15 + VCPU_GUEST_R13], r13
    mov     [r15 + VCPU_GUEST_R14], r14

    pop     rax
    mov     [r15 + VCPU_GUEST_R15], rax

    mov     rax, [r15 + VCPU_VMCB_PA]
    vmsave

%if HYPE_DEBUG_RING
    ; Per-CPU debug ring write BEFORE C handler — even a C-handler crash leaves
    ; last exit visible via DMA. R14 free here (guest R14 saved); use as VMCB VA.
    mov     r14, [r15 + VCPU_VMCB]

    inc     qword [r15 + VCPU_VMEXIT_COUNT]

    mov     qword [r15 + VCPU_STATE_MARKER], 2  ; in handler

    mov     rbx, [r15 + VCPU_RING_INDEX]
    and     rbx, 7
    lea     rcx, [rbx + 1]
    mov     [r15 + VCPU_RING_INDEX], rcx

    mov     rax, [r14 + VMCB_CTRL_EXIT_CODE]
    mov     [r15 + VCPU_RING_EXIT_CODE + rbx*8], rax

    mov     rax, [r14 + VMCB_SAVE_RIP]
    mov     [r15 + VCPU_RING_RIP + rbx*8], rax
%endif

    ; XSAVEOPT skips unchanged state components (~50-100 cy saving).
    mov     rax, [r15 + VCPU_XSAVE_AREA]
    test    rax, rax
    jz      .skip_xsave
    mov     rcx, rax
    mov     eax, dword [r15 + VCPU_XSAVE_MASK]
    mov     edx, dword [r15 + VCPU_XSAVE_MASK + 4]
    cmp     byte [rel gUseXsaveopt], 0
    je      .do_xsave
    db      048h, 00Fh, 0AEh, 031h    ; xsaveopt64 [rcx]
    jmp     .skip_xsave
.do_xsave:
    db      048h, 00Fh, 0AEh, 021h    ; xsave64 [rcx]
.skip_xsave:

    mov     rcx, r15
    sub     rsp, 20h
    call    VmexitHandler
    add     rsp, 20h

%if HYPE_DEBUG_RING
    mov     qword [r15 + VCPU_STATE_MARKER], 1  ; returning to guest
%endif

    cmp     dword [r15 + VCPU_SHOULD_EXIT], 0
    je      .vmrun_loop

.devirtualize:
    ; VMLOAD guest hidden state (FS/GS/TR/LDTR/STAR/LSTAR; NOT GDTR/IDTR).
    mov     rax, [r15 + VCPU_VMCB_PA]
    vmload

    ; STGI before clearing SVME (APM §15.17) — GIF must be 1 when SVM is
    ; disabled, otherwise INIT is blocked on APs and firmware EBS hangs.
    stgi

    ; Disable SVM
    mov     ecx, 0C0000080h     ; MSR_EFER
    rdmsr
    btr     eax, 12             ; clear SVME
    wrmsr

    ; Clear VM_HSAVE_PA
    mov     ecx, 0C0010117h
    xor     eax, eax
    xor     edx, edx
    wrmsr

    ; XRSTOR guest state
    mov     rax, [r15 + VCPU_XSAVE_AREA]
    test    rax, rax
    jz      .skip_xrstor_devirt
    mov     rcx, rax
    mov     eax, dword [r15 + VCPU_XSAVE_MASK]
    mov     edx, dword [r15 + VCPU_XSAVE_MASK + 4]
    db      048h, 00Fh, 0AEh, 029h
.skip_xrstor_devirt:

    ; Devirtualize: transfer host → guest address space.
    ;
    ; After MOV CR3, host memory (VMCB, VCPU_DATA, host stack) is gone —
    ; Windows does NOT identity-map EfiRuntimeServicesData at VA=PA. All VMCB
    ; / VCPU reads must complete before MOV CR3.
    ;
    ; Problem: 19 values cross the CR3 boundary (14 guest GPRs + CR3 + RSP +
    ; RIP + RFLAGS + guest RAX from VMCB) but only 16 GPRs exist. GDTR/IDTR
    ; (4 values) applied before CR3 switch, consuming them.
    ;
    ; Solution: stash 4 conflict GPRs (guest RCX/RDX/RSI/RDI) in DR0-DR3 —
    ; debug registers are CPU-internal, survive CR3 switches. DR7=0 first to
    ; prevent spurious #DB from non-address values, then restored.
    ;
    ; Register allocation at MOV CR3:
    ;   RCX = guest CR3       (consumed by MOV CR3)
    ;   RDX = guest RSP       (consumed by MOV RSP)
    ;   RSI = guest RIP       (pushed to guest stack for RET)
    ;   RDI = guest RFLAGS    (pushed for POPFQ)
    ;   RAX = guest RAX       (pushed for POP RAX)
    ;   RBX,RBP,R8-R15 = 10 guest GPRs in final regs
    ;   DR0..DR3 = guest RCX/RDX/RSI/RDI (recovered after CR3 switch)
    ;
    ; Clobber: DR0-DR3 (guest HW breakpoint addresses lost — not in VMCB
    ; save area per APM §15.5.1, not saved by VMRUN/VMEXIT/VMLOAD/VMSAVE),
    ; DR6 (carries guest DR7 across CR3 boundary). Acceptable for research HV
    ; — no kernel debugger in production.
    ;
    ; NMI window: ~20 insns between LIDT (guest IDTR active under host CR3)
    ; and MOV CR3. NMI in window reads guest IDT VA through host PTs →
    ; unmapped → #DF → triple-fault. Probability ~30ns × NMI_rate ≈ 10^-8.

    mov     r14, [r15 + VCPU_VMCB]

    ; Step 1: stash DR7 in DR6 (DR7 not in host save area, APM Table 15-4),
    ; zero DR7, stash 4 conflict GPRs in DR0-DR3.
    mov     rax, dr7
    mov     dr6, rax
    xor     rax, rax
    mov     dr7, rax
    mov     rax, [r15 + VCPU_GUEST_RCX]
    mov     dr0, rax
    mov     rax, [r15 + VCPU_GUEST_RDX]
    mov     dr1, rax
    mov     rax, [r15 + VCPU_GUEST_RSI]
    mov     dr2, rax
    mov     rax, [r15 + VCPU_GUEST_RDI]
    mov     dr3, rax

    mov     rbx, [r15 + VCPU_GUEST_RBX]
    mov     rbp, [r15 + VCPU_GUEST_RBP]
    mov     r8,  [r15 + VCPU_GUEST_R8]
    mov     r9,  [r15 + VCPU_GUEST_R9]
    mov     r10, [r15 + VCPU_GUEST_R10]
    mov     r11, [r15 + VCPU_GUEST_R11]
    mov     r12, [r15 + VCPU_GUEST_R12]
    mov     r13, [r15 + VCPU_GUEST_R13]

    ; Restore CR4/CR0 from VMCB. OP_START cleared SMEP/SMAP in live CR4;
    ; guest CR4 in VMCB is the firmware original. CR4 before CR0 — firmware
    ; values match running mode so both transitions safe.
    mov     rax, [r14 + 548h]       ; guest CR4
    mov     cr4, rax
    mov     rax, [r14 + 558h]       ; guest CR0
    mov     cr0, rax

    ; Apply GDTR/IDTR from VMCB on host stack. lgdt/lidt: [limit:16][base:64]
    ; = 10 bytes padded to 16. NMI window opens at LIDT.
    sub     rsp, 10h
    mov     eax, [r14 + 464h]
    mov     [rsp], ax
    mov     rax, [r14 + 468h]
    mov     [rsp + 2], rax
    lgdt    [rsp]
    mov     eax, [r14 + 484h]
    mov     [rsp], ax
    mov     rax, [r14 + 488h]
    mov     [rsp + 2], rax
    lidt    [rsp]
    add     rsp, 10h

    mov     rcx, [r14 + 550h]              ; guest CR3
    mov     rdx, [r14 + VMCB_SAVE_RSP]
    mov     rsi, [r14 + VMCB_SAVE_RIP]
    mov     rdi, [r14 + VMCB_SAVE_RFLAGS]
    mov     rax, [r14 + VMCB_SAVE_RAX]

    ; Last 2 GPRs — destroys VMCB/VCPU pointers. No more host memory reads.
    mov     r14, [r15 + VCPU_GUEST_R14]
    mov     r15, [r15 + VCPU_GUEST_R15]

    ; Back-to-back MOV CR3 + MOV RSP — no memory access between them.
    ; After MOV CR3, host stack inaccessible. NMI window closes here.
    mov     cr3, rcx
    mov     rsp, rdx

    ; Build return frame on guest stack (consumes RSI/RDI/RAX), then recover.
    push    rsi                 ; guest RIP (RET target)
    push    rdi                 ; guest RFLAGS (POPFQ)
    push    rax                 ; guest RAX (POP RAX)

    mov     rcx, dr0
    mov     rdx, dr1
    mov     rsi, dr2
    mov     rdi, dr3

    mov     rax, dr6            ; restore guest DR7 (stashed in step 1)
    mov     dr7, rax

    pop     rax
    popfq
    ret                         ; → guest RIP

.guest_entry:
    add     rsp, 28h
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rsi
    pop     rdi
    pop     rbx
    pop     rbp
    ret

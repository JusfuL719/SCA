#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "vmmcall.h"

// Mailbox client — mirrors HypeSvm.h COVERT_MAILBOX/COVERT_CMD layout.

namespace covert {

// Mirror SCA/HypeSvm.h:259-264 verbatim.
inline constexpr uint32_t kStatusOk       = 0;
inline constexpr uint32_t kStatusErr      = 1;
inline constexpr uint32_t kStatusBadCmd   = 2;
inline constexpr uint32_t kStatusBadAddr  = 3;
inline constexpr uint32_t kStatusBadAuth  = 4;
inline constexpr uint32_t kStatusNoSpare  = 5;
inline constexpr uint32_t kMaxBatch       = 80;

#pragma pack(push, 1)
struct Cmd {
    uint32_t cmd_id;
    uint32_t status;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    uint64_t result;
    uint64_t reserved;
};

struct Mailbox {
    uint64_t auth_key;
    uint32_t num_commands;
    uint32_t sequence;
    Cmd      cmd[kMaxBatch];
};
#pragma pack(pop)

static_assert(sizeof(Cmd) == 48, "CovertCmd layout must match HypeSvm.h");

// Header = auth_key(8) + num_commands(4) + sequence(4). HV only reads
// header + Cmd[0..num_commands-1]; unused slots are not touched, so a
// one-time mailbox zero at Init() plus per-call cmd-slice zero is enough.
inline constexpr uint32_t kHeaderBytes = 16;

class Channel {
public:
    bool Init(uint64_t auth_key, uint64_t hv_cpu_mask) {
        auth_key_ = auth_key;

        trigger_ = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
        mailbox_pg_ = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_READWRITE);
        if (!trigger_ || !mailbox_pg_) return false;

        VirtualLock(trigger_, 4096);
        VirtualLock(mailbox_pg_, 4096);
        std::memset(trigger_, 0, 4096);
        std::memset(mailbox_pg_, 0, 4096);
        mailbox_ = static_cast<Mailbox*>(mailbox_pg_);

        SetThreadAffinityMask(GetCurrentThread(),
                              static_cast<DWORD_PTR>(hv_cpu_mask));
        SwitchToThread();

        uint64_t r = 0;
        __try {
            r = vmmcall::Call(
                auth_key_,
                /*PMC_CMD_NPT_CHANNEL_INIT=*/0x50,
                reinterpret_cast<uint64_t>(trigger_),
                reinterpret_cast<uint64_t>(mailbox_pg_));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        return r == 0;
    }

    // Fire one command synchronously. Returns the HV's status code.
    // memset is bounded to header + cmd[0] (64 B vs the full 3856 B mailbox).
    // HV only iterates Cmd[0..NumCommands), so unused slots stay untouched.
    uint32_t Fire(uint32_t cmd_id, uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t* out_result, uint64_t* out_arg2 = nullptr) {
        if (!mailbox_) return kStatusBadAddr;

        std::memset(mailbox_, 0, kHeaderBytes + sizeof(Cmd));
        mailbox_->auth_key     = auth_key_;
        mailbox_->num_commands = 1;
        mailbox_->sequence     = ++sequence_;
        mailbox_->cmd[0].cmd_id = cmd_id;
        mailbox_->cmd[0].arg1   = a1;
        mailbox_->cmd[0].arg2   = a2;
        mailbox_->cmd[0].arg3   = a3;

        *static_cast<volatile uint8_t*>(trigger_) = 1;  // fault trigger → HV NPF

        if (out_result) *out_result = mailbox_->cmd[0].result;
        // PROC_CR3 returns ImageBase via Arg2 (Vcpu->Cr3InterceptCapturedImageBase).
        // GET_INTERCEPT_PEB returns scanner phase. Other commands ignore Arg2.
        if (out_arg2)   *out_arg2   = mailbox_->cmd[0].arg2;
        return mailbox_->cmd[0].status;
    }

    // Pack a heterogeneous list of commands into a single mailbox round-trip.
    // HV's mailbox loop dispatches each Cmd[i] in array order with its own
    // CmdId/Args, writing back per-slot Status/Result (HypeVmexit.c:1436+).
    // Caller fills cmds[i].{cmd_id, arg1, arg2, arg3}; on return,
    // cmds[i].{status, result, arg2} are populated. n must be ≤ kMaxBatch.
    // Returns true on dispatch; per-cmd success is in cmds[i].status.
    bool FireBatch(Cmd* cmds, uint32_t n) {
        if (!mailbox_ || n == 0 || n > kMaxBatch) return false;
        const uint32_t bytes = kHeaderBytes + n * (uint32_t)sizeof(Cmd);
        std::memset(mailbox_, 0, bytes);
        mailbox_->auth_key     = auth_key_;
        mailbox_->num_commands = n;
        mailbox_->sequence     = ++sequence_;
        for (uint32_t i = 0; i < n; ++i) {
            mailbox_->cmd[i].cmd_id = cmds[i].cmd_id;
            mailbox_->cmd[i].arg1   = cmds[i].arg1;
            mailbox_->cmd[i].arg2   = cmds[i].arg2;
            mailbox_->cmd[i].arg3   = cmds[i].arg3;
        }
        *static_cast<volatile uint8_t*>(trigger_) = 1;
        for (uint32_t i = 0; i < n; ++i) {
            cmds[i].status = mailbox_->cmd[i].status;
            cmds[i].result = mailbox_->cmd[i].result;
            cmds[i].arg2   = mailbox_->cmd[i].arg2;
        }
        return true;
    }

    // Scatter VIRT_READ8 — N arbitrary guest VAs in up to kMaxBatch per NPF.
    // Returns # qwords successfully read; bails on first per-slot error.
    uint32_t BulkVirtRead8Scatter(const uint64_t* vas, uint64_t* out,
                                  uint32_t n) {
        return ScatterRead(/*cmd=*/0x31 /*PMC_CMD_VIRT_READ8*/,
                           vas, out, n);
    }

    // Scatter HOOK_DRAW_PEEK — N arbitrary mirror offsets in one batch.
    uint32_t BulkPeekScatter(const uint64_t* offs, uint64_t* out,
                             uint32_t n) {
        return ScatterRead(/*cmd=*/0x1C /*PMC_CMD_HOOK_DRAW_PEEK*/,
                           offs, out, n);
    }

    // Bulk VIRT_READ8 — N qwords starting at VA `base` (stride 8).
    uint32_t BulkVirtRead8(uint64_t base, uint64_t* out, uint32_t n_qwords) {
        return LinearRead(/*cmd=*/0x31, base, out, n_qwords);
    }

    // Bulk PMC_CMD_HV_LOG_READ — N qwords starting at HV-log offset `base_off`.
    // HV checks Off+8 <= sizeof(HV_DEBUG_LOG); caller must respect that.
    uint32_t BulkHvLogRead(uint64_t base_off, uint64_t* out, uint32_t n_qwords) {
        return LinearRead(/*cmd=*/0x1E, base_off, out, n_qwords);
    }

    // Bulk PHYS_READ — N qwords starting at PA `base` (stride 8).
    uint32_t BulkPhysRead(uint64_t base, uint64_t* out, uint32_t n_qwords) {
        return LinearRead(/*cmd=*/0x41, base, out, n_qwords);
    }

    ~Channel() {
        if (trigger_)    { VirtualUnlock(trigger_, 4096);    VirtualFree(trigger_, 0, MEM_RELEASE); }
        if (mailbox_pg_) { VirtualUnlock(mailbox_pg_, 4096); VirtualFree(mailbox_pg_, 0, MEM_RELEASE); }
    }

private:
    // Shared body for the three linear bulk readers (VIRT_READ8 / HV_LOG_READ /
    // PHYS_READ — same shape, only the cmd_id and base address space differ).
    uint32_t LinearRead(uint32_t cmd_id, uint64_t base,
                        uint64_t* out, uint32_t n_qwords) {
        if (!mailbox_) return 0;
        uint32_t done = 0;
        while (done < n_qwords) {
            uint32_t batch = n_qwords - done;
            if (batch > kMaxBatch) batch = kMaxBatch;
            const uint32_t bytes = kHeaderBytes + batch * (uint32_t)sizeof(Cmd);
            std::memset(mailbox_, 0, bytes);
            mailbox_->auth_key     = auth_key_;
            mailbox_->num_commands = batch;
            mailbox_->sequence     = ++sequence_;
            for (uint32_t i = 0; i < batch; ++i) {
                mailbox_->cmd[i].cmd_id = cmd_id;
                mailbox_->cmd[i].arg1   = base + (uint64_t)(done + i) * 8;
            }
            *static_cast<volatile uint8_t*>(trigger_) = 1;
            for (uint32_t i = 0; i < batch; ++i) {
                if (mailbox_->cmd[i].status != kStatusOk) return done + i;
                out[done + i] = mailbox_->cmd[i].result;
            }
            done += batch;
        }
        return done;
    }

    // Shared body for scatter readers (per-slot independent arg1).
    uint32_t ScatterRead(uint32_t cmd_id, const uint64_t* args,
                         uint64_t* out, uint32_t n) {
        if (!mailbox_) return 0;
        uint32_t done = 0;
        while (done < n) {
            uint32_t batch = n - done;
            if (batch > kMaxBatch) batch = kMaxBatch;
            const uint32_t bytes = kHeaderBytes + batch * (uint32_t)sizeof(Cmd);
            std::memset(mailbox_, 0, bytes);
            mailbox_->auth_key     = auth_key_;
            mailbox_->num_commands = batch;
            mailbox_->sequence     = ++sequence_;
            for (uint32_t i = 0; i < batch; ++i) {
                mailbox_->cmd[i].cmd_id = cmd_id;
                mailbox_->cmd[i].arg1   = args[done + i];
            }
            *static_cast<volatile uint8_t*>(trigger_) = 1;
            for (uint32_t i = 0; i < batch; ++i) {
                if (mailbox_->cmd[i].status != kStatusOk) return done + i;
                out[done + i] = mailbox_->cmd[i].result;
            }
            done += batch;
        }
        return done;
    }

    uint64_t  auth_key_   = 0;
    uint32_t  sequence_   = 0;
    void*     trigger_    = nullptr;
    void*     mailbox_pg_ = nullptr;
    Mailbox*  mailbox_    = nullptr;
};

}  // namespace covert

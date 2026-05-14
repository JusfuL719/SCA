#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

// VMMCALL stub — XOR-encoded opcodes decoded into RX page at runtime.

namespace vmmcall {

using Fn = uint64_t(__fastcall*)(uint64_t auth_key, uint32_t cmd_id,
                                  uint64_t arg1, uint64_t arg2);

namespace detail {
inline Fn   g_fn   = nullptr;
inline void* g_pg  = nullptr;
}  // namespace detail

inline bool Init() {
    if (detail::g_fn) return true;

    detail::g_pg = VirtualAlloc(nullptr, 4096,
                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!detail::g_pg) return false;
    std::memset(detail::g_pg, 0xCC, 4096);

    // 17 bytes XOR 0x55: mov rax,rcx; mov ecx,edx; mov rdx,r8; mov r8,r9; vmmcall; ret.
    static const uint8_t enc[] = {
        0x06, 0x1D, 0xDC, 0x9E, 0xDC, 0x84, 0x19, 0xDC,
        0x97, 0x18, 0xDC, 0x9D, 0x5A, 0x54, 0x8C, 0x0E, 0x96
    };
    auto* p = static_cast<uint8_t*>(detail::g_pg);
    for (size_t i = 0; i < sizeof(enc); ++i) p[i] = enc[i] ^ 0x55;

    DWORD old = 0;
    if (!VirtualProtect(detail::g_pg, 4096, PAGE_EXECUTE_READ, &old)) {
        SecureZeroMemory(detail::g_pg, 4096);
        VirtualFree(detail::g_pg, 0, MEM_RELEASE);
        detail::g_pg = nullptr;
        return false;
    }
    detail::g_fn = reinterpret_cast<Fn>(detail::g_pg);
    return true;
}

inline void Free() {
    if (detail::g_pg) {
        DWORD old = 0;
        VirtualProtect(detail::g_pg, 4096, PAGE_READWRITE, &old);
        SecureZeroMemory(detail::g_pg, 4096);
        VirtualFree(detail::g_pg, 0, MEM_RELEASE);
        detail::g_pg = nullptr;
        detail::g_fn = nullptr;
    }
}

inline uint64_t Call(uint64_t auth, uint32_t cmd, uint64_t a1, uint64_t a2) {
    return detail::g_fn ? detail::g_fn(auth, cmd, a1, a2) : 0;
}

}  // namespace vmmcall

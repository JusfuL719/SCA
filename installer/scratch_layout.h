// SCA scratch buffer v1 — 64 KB region for PMC_CMD_HOOK_INSTALL_DRAW.
// Magic at offset 0 doubles as HV cloak-copy sentinel ("PEXEPEXE" LE).

#pragma once
#include <cstdint>

namespace sca_scratch {

inline constexpr uint64_t kMagic    = 0x5045584550455845ULL;  // "PEXEPEXE"
inline constexpr uint32_t kVersion  = 1;

// Unit-cube corners [-0.5,+0.5], line-list edges (D3D12_PRIMITIVE_TOPOLOGY_LINELIST).
struct Vec3 { float x, y, z; };

struct alignas(16) Header {
    uint64_t magic;             // 0x5045584550455845
    uint32_t version;           // kVersion
    uint32_t flags;             // bit 0 = vertex+index ready
    Vec3     box_vertices[8];   // unit cube corners — 96 B
    uint16_t box_lines[24];     // 12 edges × 2 endpoints — 48 B
    uint8_t  _reserved[96];     // pad header out to 256 B
};
static_assert(sizeof(Header) == 256, "Header must be 256 B");

inline constexpr uint32_t kHeaderSize = sizeof(Header);
inline constexpr uint32_t kInstanceTableOffset = 0x100;   // after Header
inline constexpr uint32_t kInstanceTableBytes  = 0;       // unused this phase

// Box corners — same convention as ImGui debug box: -0.5 to +0.5 cube.
inline constexpr Vec3 kBoxVerts[8] = {
    {-0.5f, -0.5f, -0.5f},   // 0  bottom face, near-left
    { 0.5f, -0.5f, -0.5f},   // 1  bottom face, near-right
    { 0.5f,  0.5f, -0.5f},   // 2  bottom face, far-right
    {-0.5f,  0.5f, -0.5f},   // 3  bottom face, far-left
    {-0.5f, -0.5f,  0.5f},   // 4  top face,    near-left
    { 0.5f, -0.5f,  0.5f},   // 5  top face,    near-right
    { 0.5f,  0.5f,  0.5f},   // 6  top face,    far-right
    {-0.5f,  0.5f,  0.5f},   // 7  top face,    far-left
};

inline constexpr uint16_t kBoxLines[24] = {
    0,1, 1,2, 2,3, 3,0,    // bottom face   (4 edges)
    4,5, 5,6, 6,7, 7,4,    // top face      (4 edges)
    0,4, 1,5, 2,6, 3,7,    // verticals     (4 edges)
};

inline constexpr uint32_t kFlagVertexReady = 1u << 0;

// Stamp 256-byte header at buf — caller zeros anything beyond.
inline void StampHeader(void* buf) {
    auto* h = static_cast<Header*>(buf);
    h->magic   = kMagic;
    h->version = kVersion;
    h->flags   = kFlagVertexReady;
    for (int i = 0; i < 8;  ++i) h->box_vertices[i] = kBoxVerts[i];
    for (int i = 0; i < 24; ++i) h->box_lines[i]    = kBoxLines[i];
    for (auto& b : h->_reserved) b = 0;
}

}  // namespace sca_scratch

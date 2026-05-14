#pragma once
#include <cstdint>

// Must match SCA/HypeSvm.h HYPE_BUILD_SECRET. Compile-time XOR'd to keep
// the literal off the binary; mirrors the encoding apphost uses in config.h.
namespace auth {

inline constexpr uint64_t kBuildKey = 0x0123456789ABCDEFULL;
inline constexpr uint64_t kBuildSecretEncoded =
    0x7A3F9B2E5D1C8064ULL ^ kBuildKey;

inline uint64_t BuildSecret() {
    // volatile defeats MSVC constant-folding of the XOR — we want the
    // literal (kBuildSecretEncoded) to ship in .rdata, with the unmask
    // happening at runtime.
    volatile uint64_t enc = kBuildSecretEncoded;
    volatile uint64_t key = kBuildKey;
    return enc ^ key;
}

inline uint64_t MixKey(uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

}  // namespace auth

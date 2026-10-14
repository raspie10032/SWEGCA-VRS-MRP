#pragma once

#include <array>
#include <cstddef>

namespace swegca::architecture {

inline constexpr std::size_t digest256_width = 32;
using DigestBytes = std::array<std::byte, digest256_width>;
inline constexpr DigestBytes zero_digest_bytes{};

}  // namespace swegca::architecture

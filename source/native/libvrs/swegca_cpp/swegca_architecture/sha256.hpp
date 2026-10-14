#pragma once

#include "swegca_architecture/digest_bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace swegca::architecture {

// SHA-256 as specified by FIPS 180-4. The journal uses it for record, segment
// and manifest identity; a digest is provenance, never an authority token.
// Rule: traceable, provenance-bound persistent lineage,
// ARCHITECTURE_SPEC.md@5901a5a:226,230 (I03, I07).
// re-created (user@2026-09-23): native digest primitive; no Python hashlib.
class Sha256 final {
public:
    using Bytes = DigestBytes;

    Sha256() noexcept;

    // Updating or finishing a finished object is a programming error.
    void update(std::span<const std::byte> data);
    void update(std::string_view text);
    [[nodiscard]] Bytes finish();

    [[nodiscard]] static Bytes of(std::span<const std::byte> data);

private:
    void compress(const std::byte* block) noexcept;

    std::array<std::uint32_t, 8> state_;
    std::array<std::byte, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
    bool finished_ = false;
};

}  // namespace swegca::architecture

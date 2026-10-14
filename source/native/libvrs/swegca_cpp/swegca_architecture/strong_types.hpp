#pragma once

#include "swegca_architecture/digest_bytes.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace swegca::architecture {

class Digest256 final {
public:
    static constexpr std::size_t width = digest256_width;
    using Bytes = DigestBytes;

    // Rule: provenance and immutable artifact identity, SWEGCA I03 and I07.
    // SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:23-45
    explicit Digest256(Bytes bytes);

    // SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:23-25
    [[nodiscard]] static Digest256 from_hex(std::string_view value);

    // SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:15-25
    [[nodiscard]] std::string hex() const;

    // SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:23-25
    [[nodiscard]] const Bytes& bytes() const noexcept { return bytes_; }
    auto operator<=>(const Digest256&) const = default;

private:
    Bytes bytes_;
};

}  // namespace swegca::architecture

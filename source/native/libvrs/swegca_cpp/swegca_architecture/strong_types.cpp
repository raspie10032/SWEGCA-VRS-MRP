#include "swegca_architecture/strong_types.hpp"

#include <stdexcept>

namespace swegca::architecture {
namespace {

// SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:23-25
std::uint8_t hex_nibble(char value) {
    if (value >= '0' && value <= '9')
        return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
        return static_cast<std::uint8_t>(value - 'a' + 10);
    throw std::invalid_argument("digest_must_be_lowercase_hex");
}

}  // namespace

// SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:23-45
Digest256::Digest256(Bytes bytes) : bytes_(bytes) {}

// SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:23-25
Digest256 Digest256::from_hex(std::string_view value) {
    if (value.size() != width * 2)
        throw std::invalid_argument("digest_must_have_64_hex_characters");
    Bytes bytes{};
    for (std::size_t index = 0; index < width; ++index) {
        const auto high = hex_nibble(value[index * 2]);
        const auto low = hex_nibble(value[index * 2 + 1]);
        bytes[index] = std::byte{static_cast<std::uint8_t>((high << 4) | low)};
    }
    return Digest256(bytes);
}

// SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:15-25
std::string Digest256::hex() const {
    static constexpr char digits[] = "0123456789abcdef";
    std::string value(width * 2, '0');
    for (std::size_t index = 0; index < width; ++index) {
        const auto byte = std::to_integer<std::uint8_t>(bytes_[index]);
        value[index * 2] = digits[byte >> 4];
        value[index * 2 + 1] = digits[byte & 0x0f];
    }
    return value;
}

}  // namespace swegca::architecture

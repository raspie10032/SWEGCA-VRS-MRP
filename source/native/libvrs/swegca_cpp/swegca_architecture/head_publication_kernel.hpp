#pragma once

#include "swegca_architecture/evidence_observation_kernel.hpp"
#include "swegca_architecture/record_address.hpp"
#include <bit>

namespace swegca::architecture::kernel {

// Main's current-head and same-lineage condition (original architecture I03,
// I07, sections 4.5/4.8). This controls publication of an already core-verified
// connection version; it does not supply a new evidence/semantic verdict.
struct ConnectionHead {
    Digest identity{};
    RecordAddress record;
    std::uint64_t revision = 0, ordinal = 0, observations = 0;
    double strength = 0;
};
enum class HeadPublication : std::uint8_t {
    invalid = 0, stale = 1, unrelated_lineage = 2, unchanged = 3, publish = 4,
};
[[nodiscard]] constexpr bool catalog_root_ready(bool experience_available, std::uint64_t generation) noexcept {
    return experience_available && generation != std::numeric_limits<std::uint64_t>::max();
}
[[nodiscard]] inline bool head_address_valid(const RecordAddress& address) noexcept {
    return named_digest(address.block) && named_digest(address.digest) && address.bytes > 0 &&
        address.bytes <= std::numeric_limits<std::uint64_t>::max() - address.offset;
}
[[nodiscard]] inline bool connection_head_valid(const ConnectionHead& head) noexcept {
    return named_digest(head.identity) && head_address_valid(head.record) && finite_count(head.strength) &&
        head.observations > 0 && head.revision >= head.observations && head.ordinal >= head.revision;
}
[[nodiscard]] inline HeadPublication assess_head_publication(const ConnectionHead* current,
    const RecordAddress& expected, const ConnectionHead& candidate, bool lineage_verified) noexcept {
    if (!connection_head_valid(candidate) || (current && (!connection_head_valid(*current) || current->identity != candidate.identity)))
        return HeadPublication::invalid;
    if (expected != (current ? current->record : RecordAddress{})) return HeadPublication::stale;
    if (!current) return HeadPublication::publish;
    if (current->record == candidate.record) {
        if (current->revision != candidate.revision || current->ordinal != candidate.ordinal ||
            current->observations != candidate.observations ||
            std::bit_cast<std::uint64_t>(current->strength) != std::bit_cast<std::uint64_t>(candidate.strength))
            return HeadPublication::invalid;
        return HeadPublication::unchanged;
    }
    if (!lineage_verified || candidate.ordinal <= current->ordinal || candidate.revision < current->revision ||
        candidate.observations < current->observations) return HeadPublication::unrelated_lineage;
    return HeadPublication::publish;
}

}  // namespace swegca::architecture::kernel

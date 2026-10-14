#pragma once

#include "swegca_architecture/evidence_observation_kernel.hpp"

#include <cstddef>
#include <span>

namespace swegca::architecture::kernel {

// An observation of the explicitly requested predicate "equal byte content".
// This is not a language classifier or an accept/reject/abstain judgment.
// Only judge_evidence, after normal admission/shuffle, can issue that judgment.
// Incomplete or changing reads cannot supply support OR refutation.
[[nodiscard]] constexpr PredicateObservation observe_content_equality(
    bool complete, bool stable, bool equal) noexcept {
    return {complete && stable, equal};
}

// Expectation is an explicit predicate operand, never inferred from prose.
// Missing or unstable data stays insufficient for either polarity.
[[nodiscard]] constexpr PredicateObservation observe_content_relation(
    bool complete,bool stable,bool equal,bool expect_equal) noexcept {
    return observe_content_equality(complete,stable,equal==expect_equal);
}

// The measured operands enter the core, rather than a host-computed `equal`
// flag. This retains the existing explicitly declared equal/different
// predicate. It is NOT an automatic multimodal relationship definition.
// All bytes, including NUL, invalid UTF-8, noise and contradictory content,
// participate unchanged. Input buffers remain owned by the caller.
// Work is O(bytes inspected); the whole-buffer operation has no ns claim.
[[nodiscard]] constexpr PredicateObservation observe_raw_content_relation(
    std::span<const std::byte> left, std::span<const std::byte> right,
    bool complete, bool stable, bool expect_equal) noexcept {
    if (!complete || !stable)
        return observe_content_relation(complete, stable, false, expect_equal);
    bool equal = left.size() == right.size();
    if (equal)
        for (std::size_t i = 0; i < left.size(); ++i)
            if (left[i] != right[i]) { equal = false; break; }
    return observe_content_relation(true, true, equal, expect_equal);
}

} // namespace swegca::architecture::kernel

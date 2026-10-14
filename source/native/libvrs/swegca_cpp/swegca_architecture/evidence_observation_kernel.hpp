#pragma once

#include "swegca_architecture/evidence_kernel.hpp"

namespace swegca::architecture::kernel {

// Observation values are recorded experimental outcomes, not the core's
// accept/reject/abstain decision. Unknown observations remain insufficient.
// Rules: original mosaic_evidence_accumulator.py@5901a5a:98-197,360-428.
enum class EvidenceOutcome : std::uint8_t { insufficient = 0, support = 1, refute = 2 };
// Observation kernels report predicate validity and truth separately.
// Only this converter maps an observation into accumulated evidence.
struct PredicateObservation { bool valid = false; bool holds = false; };
[[nodiscard]] constexpr EvidenceOutcome to_outcome(bool valid, bool holds) noexcept {
    return !valid ? EvidenceOutcome::insufficient
        : holds ? EvidenceOutcome::support : EvidenceOutcome::refute;
}
[[nodiscard]] constexpr EvidenceOutcome to_outcome(PredicateObservation value) noexcept {
    return to_outcome(value.valid, value.holds);
}

enum class ObservationUse : std::uint8_t {
    invalid = 0, expired = 1, insufficient = 2, duplicate = 3, applied = 4,
};

struct EvidenceObservation {
    Digest hypothesis{};
    Digest address{};
    Digest source{};
    Digest context{};
    Digest producer{};
    std::uint64_t observed_at = 0;
    std::uint64_t expires_at = 0;
    double producer_confidence = 0;
    std::uint32_t axis = 0;
    EvidenceOutcome outcome = EvidenceOutcome::insufficient;
    bool has_expiry = false;
};

[[nodiscard]] inline bool named_digest(const Digest& value) noexcept {
    for (const auto byte : value)
        if (byte != std::byte{}) return true;
    return false;
}

// Value validation also runs before a new record has its content address.
[[nodiscard]] inline bool observation_values_valid(
    const EvidenceRules& rules, const Digest& hypothesis,
    const EvidenceObservation& value) noexcept {
    if (!named_digest(value.hypothesis) || value.hypothesis != hypothesis ||
        !named_digest(value.source) ||
        !named_digest(value.context) || !named_digest(value.producer) ||
        value.axis >= rules.axis_count() || !finite_unit(value.producer_confidence) ||
        (value.has_expiry && value.expires_at < value.observed_at) ||
        (value.outcome != EvidenceOutcome::support && value.outcome != EvidenceOutcome::refute &&
         value.outcome != EvidenceOutcome::insufficient)) return false;
    return true;
}

// Seen-address membership is storage bookkeeping supplied by Main. Admission
// order matches the original: validate, hypothesis/axis, expiry, insufficient,
// duplicate, apply. Producer confidence never replaces evidence verification.
[[nodiscard]] inline ObservationUse admit_observation(
    const EvidenceRules& rules, const Digest& hypothesis,
    const EvidenceObservation& value, std::uint64_t current_step, bool seen) noexcept {
    if (!observation_values_valid(rules, hypothesis, value) || !named_digest(value.address))
        return ObservationUse::invalid;
    if (value.has_expiry && current_step > value.expires_at) return ObservationUse::expired;
    if (value.outcome == EvidenceOutcome::insufficient) return ObservationUse::insufficient;
    if (seen) return ObservationUse::duplicate;
    return ObservationUse::applied;
}

struct EffectiveEvidence { double support = 0; double refute = 0; };

// One (axis, source, context) group contributes at most one effective sample.
// Independent source/producer/context cardinalities are counted by Main and
// checked by judge_evidence. No new stability threshold is introduced here.
[[nodiscard]] inline EffectiveEvidence normalize_evidence_group(
    std::uint32_t supports, std::uint32_t refutes) noexcept {
    const auto total = std::uint64_t{supports} + refutes;
    if (total == 0) return {};
    return {double(supports) / double(total), double(refutes) / double(total)};
}

}  // namespace swegca::architecture::kernel

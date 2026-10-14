#pragma once

#include "swegca_architecture/evidence_kernel.hpp"

namespace swegca::architecture::kernel {

// Eligibility affects consumption only: weak numeric state is retained and
// can receive new evidence, including evidence that restores eligibility.
[[nodiscard]] constexpr bool connection_evidence_eligible(double strength) noexcept {
    return strength >= 1.0 && strength <= std::numeric_limits<double>::max();
}

// User-selected three-state VRS rule. The factors come from the user's
// original mosaic_vrs_state_update.py@9aa5f555cf:15-16,108-120.
// Source: support -> x1.01, refute -> x0.995, unresolved -> unchanged.
// No alternate classifier, saturation threshold, or initial strength is added.
class ConnectionStrengthResult final {
public:
    constexpr ConnectionStrengthResult() noexcept = default;
    [[nodiscard]] constexpr bool valid() const noexcept { return valid_; }
    [[nodiscard]] constexpr double previous() const noexcept { return previous_; }
    [[nodiscard]] constexpr double current() const noexcept { return current_; }

    // Original mosaic_memory_promotion.py@9aa5f555cf:83,134-145.
    // Eligibility is not permission for a semantic write or external action.
    [[nodiscard]] constexpr bool evidence_eligible() const noexcept {
        return valid_ && connection_evidence_eligible(current_);
    }

private:
    friend ConnectionStrengthResult revise_connection_strength(
        double, const EvidenceJudgment&) noexcept;
    bool valid_ = false;
    double previous_ = 0;
    double current_ = 0;
};

// Pure, fixed-size projection of a core decision. Main applies valid results
// only to the same connection/version from which `previous` was obtained.
// An invalid judgment is not a successful preserve operation. In particular,
// arithmetic overflow never publishes infinity or fabricates another verdict.
[[nodiscard]] inline ConnectionStrengthResult revise_connection_strength(
    double previous, const EvidenceJudgment& judgment) noexcept {
    ConnectionStrengthResult result;
    if (!finite_count(previous) || judgment.reason() == EvidenceReason::invalid_input)
        return result;

    double current = previous;
    switch (judgment.status()) {
    case EvidenceStatus::accept:
        current = previous * 1.01;
        break;
    case EvidenceStatus::reject:
        current = previous * 0.995;
        break;
    case EvidenceStatus::abstain:
        break;
    }
    if (!finite_count(current)) return result;
    result.valid_ = true;
    result.previous_ = previous;
    result.current_ = current;
    return result;
}

}  // namespace swegca::architecture::kernel

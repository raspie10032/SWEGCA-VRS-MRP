#pragma once
#include "swegca_architecture/core_platform.hpp"
namespace swegca::architecture {

struct EvidencePolicy;
namespace kernel {
class EvidenceRules;
}  // namespace kernel
[[nodiscard]] kernel::EvidenceRules make_evidence_rules(const EvidencePolicy& policy);

namespace kernel {

// The shared digest byte type; its header is exception- and allocation-free (codex KJ6).
using Digest = platform::array<platform::byte, 32>;

inline constexpr platform::size_t max_axes = 8;

enum class EvidenceStatus : platform::uint8_t { abstain = 0, accept = 1, reject = 2 };

enum class EvidenceReason : platform::uint8_t {
    minimum_effective_samples = 1,
    source_diversity = 2,
    axis_source_diversity = 3,
    context_diversity = 4,
    regime_change_suspected = 5,
    causal_lower_bound = 6,
    upper_bound_below_threshold = 7,
    uncertain = 8,
    invalid_input = 9,
};

// Fixed-size evidence tally of one claim, maintained by the Main-owned shell.
struct EvidenceTally {
    platform::array<double, max_axes> axis_support{};
    platform::array<double, max_axes> axis_refute{};
    platform::array<platform::uint32_t, max_axes> axis_source_diversity{};
    platform::uint32_t source_diversity = 0;
    platform::uint32_t context_diversity = 0;
    platform::uint32_t recent_count = 0;
    double recent_sum = 0;
    platform::uint64_t revision = 0;
};

// A default value is invalid. Only judge_evidence can produce another result.
// Copying a result preserves its fields; normal callers cannot fabricate an
// acceptance or edit a copied result. This is an API invariant, not a security
// capability or proof that Main still owns the same current state.
class EvidenceJudgment final {
public:
    constexpr EvidenceJudgment() noexcept = default;
    [[nodiscard]] constexpr EvidenceStatus status() const noexcept { return status_; }
    [[nodiscard]] constexpr EvidenceReason reason() const noexcept { return reason_; }
    [[nodiscard]] constexpr double posterior_mean() const noexcept { return posterior_mean_; }
    [[nodiscard]] constexpr double causal_lower_bound() const noexcept { return causal_lower_bound_; }
    [[nodiscard]] constexpr double overall_upper_bound() const noexcept { return overall_upper_bound_; }
    [[nodiscard]] constexpr double effective_sample_size() const noexcept { return effective_sample_size_; }
    [[nodiscard]] constexpr double regime_change_score() const noexcept { return regime_change_score_; }
    [[nodiscard]] constexpr platform::uint32_t source_diversity() const noexcept { return source_diversity_; }
    [[nodiscard]] constexpr platform::uint32_t context_diversity() const noexcept { return context_diversity_; }
    [[nodiscard]] constexpr platform::uint64_t revision() const noexcept { return revision_; }

private:
    friend EvidenceJudgment judge_evidence(const EvidenceRules&, const EvidenceTally&) noexcept;
    EvidenceStatus status_ = EvidenceStatus::abstain;
    EvidenceReason reason_ = EvidenceReason::invalid_input;
    double posterior_mean_ = 0;
    double causal_lower_bound_ = 0;
    double overall_upper_bound_ = 0;
    double effective_sample_size_ = 0;
    double regime_change_score_ = 0;
    platform::uint32_t source_diversity_ = 0;
    platform::uint32_t context_diversity_ = 0;
    platform::uint64_t revision_ = 0;
};

struct EvidenceColumns;
struct EvidenceJudgmentColumns;

// Validated decision rules. Only `make_evidence_rules` constructs one; the
// fields are private so a valid value cannot be edited into an invalid one.
class EvidenceRules final {
public:
    [[nodiscard]] constexpr platform::uint32_t axis_count() const noexcept { return axis_count_; }
    [[nodiscard]] constexpr platform::uint32_t recent_window() const noexcept { return recent_window_; }

private:
    EvidenceRules() = default;
    friend EvidenceRules swegca::architecture::make_evidence_rules(const EvidencePolicy&);
    friend bool rules_valid(const EvidenceRules& rules) noexcept;
    friend EvidenceJudgment judge_evidence(const EvidenceRules& rules,
                                           const EvidenceTally& tally) noexcept;
    friend bool judge_evidence_batch(const EvidenceRules& rules, const EvidenceColumns& in,
                                     const EvidenceJudgmentColumns& out, platform::size_t first,
                                     platform::size_t last) noexcept;

    double z_ = 0;
    double z_squared_ = 0;
    double threshold_ = 0;  // chance rate + accept margin
    double prior_alpha_ = 0;
    double prior_beta_ = 0;
    double minimum_effective_samples_per_axis_ = 0;
    platform::uint32_t minimum_source_diversity_ = 0;
    platform::uint32_t minimum_axis_source_diversity_ = 0;
    platform::uint32_t minimum_context_diversity_ = 0;
    platform::uint32_t minimum_recent_samples_ = 0;
    platform::uint32_t recent_window_ = 0;
    double regime_change_threshold_ = 0;
    platform::uint32_t axis_count_ = 0;
};

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:69-95
[[nodiscard]] inline bool finite_unit(double value) noexcept {
    return platform::isfinite(value) && value >= 0 && value <= 1;
}

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:69-95
[[nodiscard]] inline bool rules_valid(const EvidenceRules& r) noexcept {
    return platform::isfinite(r.z_) && r.z_ > 0 && r.z_squared_ == r.z_ * r.z_ &&
           finite_unit(r.threshold_) && platform::isfinite(r.prior_alpha_) && r.prior_alpha_ > 0 &&
           platform::isfinite(r.prior_beta_) && r.prior_beta_ > 0 &&
           platform::isfinite(r.minimum_effective_samples_per_axis_) &&
           r.minimum_effective_samples_per_axis_ > 0 && r.minimum_source_diversity_ > 0 &&
           r.minimum_axis_source_diversity_ > 0 && r.minimum_context_diversity_ > 0 &&
           r.minimum_recent_samples_ > 0 && r.recent_window_ >= r.minimum_recent_samples_ &&
           finite_unit(r.regime_change_threshold_) && r.axis_count_ > 0 &&
           r.axis_count_ <= max_axes;
}

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:98-131
[[nodiscard]] inline bool finite_count(double value) noexcept {
    return platform::isfinite(value) && value >= 0;
}

struct Interval {
    double lower = 0;
    double upper = 1;
};

// Wilson score interval; no samples means the whole unit interval.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:261-283
[[nodiscard]] inline Interval wilson_interval(double supports, double refutes, double z,
                                              double z_squared) noexcept {
    const double samples = supports + refutes;
    if (!(samples > 0)) return {0.0, 1.0};
    const double rate = supports / samples;
    const double denominator = 1 + z_squared / samples;
    const double center = (rate + z_squared / (2 * samples)) / denominator;
    const double radius =
        z * platform::sqrt(rate * (1 - rate) / samples + z_squared / (4 * samples * samples)) /
        denominator;
    return {platform::fmax(0.0, center - radius), platform::fmin(1.0, center + radius)};
}

// Invalid rules or tally abstain with `invalid_input`. Otherwise: abstain on
// too few samples, diversity or a regime change; accept only when
// the weakest axis lower bound clears the threshold; reject only when the
// pooled upper bound stays at or below it; abstain otherwise. As the author
// does (:314-318, :335), a window with fewer than `minimum_recent_samples`
// outcomes scores 0 and is still compared, so threshold 0 always abstains
// (Codex 2026-09-23 17:28).
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:285-357
[[nodiscard]] inline EvidenceJudgment judge_evidence(const EvidenceRules& r,
                                                     const EvidenceTally& t) noexcept {
    EvidenceJudgment out;
    out.source_diversity_ = t.source_diversity;
    out.context_diversity_ = t.context_diversity;
    out.revision_ = t.revision;
    if (!rules_valid(r) || t.recent_count > r.recent_window_ ||
        !finite_count(t.recent_sum) || t.recent_sum > t.recent_count)
        return out;  // abstain, invalid_input
    // The author's authoritative empty accumulator has revision 0 and is
    // assessed as minimum_effective_samples, including an insufficient-only
    // input batch. Revision 0 with claimed evidence remains inconsistent.
    if (t.revision == 0) {
        if (t.source_diversity || t.context_diversity || t.recent_count || t.recent_sum != 0)
            return out;
        for (platform::size_t axis = 0; axis < r.axis_count_; ++axis)
            if (t.axis_support[axis] != 0 || t.axis_refute[axis] != 0 || t.axis_source_diversity[axis])
                return out;
    }
    for (platform::size_t axis = 0; axis < r.axis_count_; ++axis)
        if (!finite_count(t.axis_support[axis]) || !finite_count(t.axis_refute[axis]) ||
            t.axis_source_diversity[axis] > t.source_diversity)
            return out;

    double supports = 0;
    double refutes = 0;
    double causal_lower = 1;
    bool short_axis = false;
    bool narrow_axis = false;
    for (platform::size_t axis = 0; axis < r.axis_count_; ++axis) {
        const double support = t.axis_support[axis];
        const double refute = t.axis_refute[axis];
        supports += support;
        refutes += refute;
        causal_lower =
            platform::fmin(causal_lower, wilson_interval(support, refute, r.z_, r.z_squared_).lower);
        short_axis |= support + refute < r.minimum_effective_samples_per_axis_;
        narrow_axis |= t.axis_source_diversity[axis] < r.minimum_axis_source_diversity_;
    }
    const double samples = supports + refutes;
    // D8(h), I04: an invalid tally must abstain. The author's Python lets
    // finite components overflow the posterior to NaN and can then accept;
    // this explicit fail-closed guard deliberately rejects that arithmetic.
    if (!platform::isfinite(samples)) return out;
    const double posterior_numerator = supports + r.prior_alpha_;
    const double posterior_denominator = samples + r.prior_alpha_ + r.prior_beta_;
    if (!platform::isfinite(posterior_numerator) || !platform::isfinite(posterior_denominator))
        return out;
    const double posterior_mean = posterior_numerator / posterior_denominator;
    const double overall_upper = wilson_interval(supports, refutes, r.z_, r.z_squared_).upper;
    const bool measured = t.recent_count >= r.minimum_recent_samples_;
    const double regime_score =
        measured ? platform::fabs(t.recent_sum / t.recent_count - posterior_mean) : 0.0;
    if (!platform::isfinite(posterior_mean) || !platform::isfinite(causal_lower) ||
        !platform::isfinite(overall_upper) || !platform::isfinite(regime_score))
        return out;
    out.posterior_mean_ = posterior_mean;
    out.causal_lower_bound_ = causal_lower;
    out.overall_upper_bound_ = overall_upper;
    out.effective_sample_size_ = samples;
    out.regime_change_score_ = regime_score;

    using S = EvidenceStatus;
    using R = EvidenceReason;
    if (short_axis) {
        out.status_ = S::abstain, out.reason_ = R::minimum_effective_samples;
    } else if (t.source_diversity < r.minimum_source_diversity_) {
        out.status_ = S::abstain, out.reason_ = R::source_diversity;
    } else if (narrow_axis) {
        out.status_ = S::abstain, out.reason_ = R::axis_source_diversity;
    } else if (t.context_diversity < r.minimum_context_diversity_) {
        out.status_ = S::abstain, out.reason_ = R::context_diversity;
    } else if (out.regime_change_score_ >= r.regime_change_threshold_) {
        out.status_ = S::abstain, out.reason_ = R::regime_change_suspected;
    } else if (out.causal_lower_bound_ > r.threshold_) {
        out.status_ = S::accept, out.reason_ = R::causal_lower_bound;
    } else if (out.overall_upper_bound_ <= r.threshold_) {
        out.status_ = S::reject, out.reason_ = R::upper_bound_below_threshold;
    } else {
        out.status_ = S::abstain, out.reason_ = R::uncertain;
    }
    return out;
}


} // namespace kernel
} // namespace swegca::architecture

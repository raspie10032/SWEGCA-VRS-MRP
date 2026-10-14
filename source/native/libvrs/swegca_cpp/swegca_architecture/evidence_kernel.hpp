#pragma once

#include "swegca_architecture/digest_bytes.hpp"
#include "swegca_architecture/numeric_contract.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

// SWEGCA nano-core: the pure ternary judgment of evidence (accept, reject,
// abstain), the verifier the core is (user 2026-09-23). Gate and arbiter
// kernels belong to the host layer (gate_kernel.hpp, arbiter_kernel.hpp; codex 16:55).
//
// Kernel rules (user@2026-09-23 nano-core directive; design board nano-core
// boundary): no allocation, lock, exception, I/O, string, virtual call or
// global mutable state; fixed-width inputs and outputs; every kernel checks
// its own inputs and fails closed, so no authority result depends on a
// comment about what the shell validated (codex KJ1-KJ3).
//
// Arithmetic contract (codex KJ7). Bit-identical results are claimed only
// when all of these hold, and are not yet verified on any GPU/NPU:
//   - IEEE-754 binary32 (arbiter) and binary64 (evidence), round to nearest
//     even, subnormals preserved (no flush-to-zero / denormals-are-zero);
//   - no floating-point contraction or reassociation (-ffp-contract=off,
//     no -ffast-math);
//   - only +, -, *, /, sqrt, fabs, fmin, fmax, all exactly specified by
//     IEEE-754; no other libm call inside a kernel;
//   - reductions run in the sequential order written here.
// The Wilson z value comes from the shell once per rule set (a libm call),
// so every executor must receive the same rule bits rather than recompute z.
//
//
// Rules: ARCHITECTURE_SPEC.md@5901a5a §4.4 (decision).
#include "swegca_architecture/evidence_scalar.hpp"

namespace swegca::architecture::kernel {
// Structure-of-arrays view of `count` claims. Axis columns are axis-major:
// axis_support[axis * count + item].
struct EvidenceColumns {
    std::size_t count = 0;
    std::span<const double> axis_support;
    std::span<const double> axis_refute;
    std::span<const std::uint32_t> axis_source_diversity;
    std::span<const std::uint32_t> source_diversity;
    std::span<const std::uint32_t> context_diversity;
    std::span<const std::uint32_t> recent_count;
    std::span<const double> recent_sum;
    std::span<const std::uint64_t> revision;
};

struct EvidenceJudgmentColumns {
    std::span<EvidenceStatus> status;
    std::span<EvidenceReason> reason;
    std::span<double> posterior_mean;
    std::span<double> causal_lower_bound;
    std::span<double> overall_upper_bound;
    std::span<double> effective_sample_size;
    std::span<double> regime_change_score;
    // Optional caller-owned results for composing another core operation.
    // Preserve the SoA outputs, and do not reconstruct a trusted judgment
    // from their public numeric columns or run the judgment a second time.
    std::span<EvidenceJudgment> judgments{};
};

struct EvidenceByteRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
};

// Native column-boundary guard for the author's accumulator batch decision;
// byte-address overlap arithmetic has no direct Python counterpart.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:285-357
template <class T>
[[nodiscard]] inline bool evidence_byte_range(std::span<T> column,
                                              EvidenceByteRange& range) noexcept {
    if (column.empty()) {
        range = {};
        return true;
    }
    constexpr auto max_address = std::numeric_limits<std::uintptr_t>::max();
    if (column.size() > max_address / sizeof(T)) return false;
    const auto bytes = column.size() * sizeof(T);
    const auto begin = reinterpret_cast<std::uintptr_t>(column.data());
    if (begin > max_address - bytes) return false;
    range = {begin, begin + bytes};
    return true;
}

// Native column-boundary guard for the same batch decision.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:285-357
[[nodiscard]] inline bool evidence_ranges_overlap(EvidenceByteRange a,
                                                  EvidenceByteRange b) noexcept {
    return a.begin < a.end && b.begin < b.end && a.begin < b.end && b.begin < a.end;
}

// Judges items [first, last) of a batch; disjoint ranges may run on different
// workers. Returns false (and writes nothing) when a column does not fit or
// any output column overlaps an input or another output column.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:285-357
[[nodiscard]] inline bool judge_evidence_batch(const EvidenceRules& rules,
                                               const EvidenceColumns& in,
                                               const EvidenceJudgmentColumns& out,
                                               std::size_t first, std::size_t last) noexcept {
    const std::size_t n = in.count;
    const std::size_t axis_count = rules.axis_count_;
    if (axis_count == 0 || axis_count > max_axes || first > last || last > n) return false;
    if (n > SIZE_MAX / max_axes) return false;
    if (in.axis_support.size() != axis_count * n || in.axis_refute.size() != axis_count * n ||
        in.axis_source_diversity.size() != axis_count * n || in.source_diversity.size() != n ||
        in.context_diversity.size() != n || in.recent_count.size() != n ||
        in.recent_sum.size() != n || in.revision.size() != n)
        return false;
    if (out.status.size() != n || out.reason.size() != n || out.posterior_mean.size() != n ||
        out.causal_lower_bound.size() != n || out.overall_upper_bound.size() != n ||
        out.effective_sample_size.size() != n || out.regime_change_score.size() != n ||
        (!out.judgments.empty() && out.judgments.size() != n))
        return false;
    std::array<EvidenceByteRange, 8> inputs;
    if (!evidence_byte_range(in.axis_support, inputs[0]) ||
        !evidence_byte_range(in.axis_refute, inputs[1]) ||
        !evidence_byte_range(in.axis_source_diversity, inputs[2]) ||
        !evidence_byte_range(in.source_diversity, inputs[3]) ||
        !evidence_byte_range(in.context_diversity, inputs[4]) ||
        !evidence_byte_range(in.recent_count, inputs[5]) ||
        !evidence_byte_range(in.recent_sum, inputs[6]) ||
        !evidence_byte_range(in.revision, inputs[7]))
        return false;
    std::array<EvidenceByteRange, 8> outputs;
    if (!evidence_byte_range(out.status, outputs[0]) ||
        !evidence_byte_range(out.reason, outputs[1]) ||
        !evidence_byte_range(out.posterior_mean, outputs[2]) ||
        !evidence_byte_range(out.causal_lower_bound, outputs[3]) ||
        !evidence_byte_range(out.overall_upper_bound, outputs[4]) ||
        !evidence_byte_range(out.effective_sample_size, outputs[5]) ||
        !evidence_byte_range(out.regime_change_score, outputs[6]) ||
        !evidence_byte_range(out.judgments, outputs[7]))
        return false;
    for (std::size_t output = 0; output < outputs.size(); ++output) {
        for (const auto input : inputs)
            if (evidence_ranges_overlap(outputs[output], input)) return false;
        for (std::size_t earlier = 0; earlier < output; ++earlier)
            if (evidence_ranges_overlap(outputs[output], outputs[earlier])) return false;
    }
    for (std::size_t item = first; item < last; ++item) {
        EvidenceTally tally;
        for (std::size_t axis = 0; axis < axis_count; ++axis) {
            tally.axis_support[axis] = in.axis_support[axis * n + item];
            tally.axis_refute[axis] = in.axis_refute[axis * n + item];
            tally.axis_source_diversity[axis] = in.axis_source_diversity[axis * n + item];
        }
        tally.source_diversity = in.source_diversity[item];
        tally.context_diversity = in.context_diversity[item];
        tally.recent_count = in.recent_count[item];
        tally.recent_sum = in.recent_sum[item];
        tally.revision = in.revision[item];
        const auto judged = judge_evidence(rules, tally);
        out.status[item] = judged.status();
        out.reason[item] = judged.reason();
        out.posterior_mean[item] = judged.posterior_mean();
        out.causal_lower_bound[item] = judged.causal_lower_bound();
        out.overall_upper_bound[item] = judged.overall_upper_bound();
        out.effective_sample_size[item] = judged.effective_sample_size();
        out.regime_change_score[item] = judged.regime_change_score();
        if (!out.judgments.empty()) out.judgments[item] = judged;
    }
    return true;
}

}  // namespace kernel

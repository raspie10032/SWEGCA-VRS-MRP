#pragma once

#include "swegca_architecture/evidence_kernel.hpp"
#include "swegca_architecture/strong_types.hpp"

#include <cstdint>

// Shell side of the nano-core: validates Main's policy once and produces the
// prevalidated rule values the kernels read. Validation may throw; kernels
// never do. Rules: ARCHITECTURE_SPEC.md@5901a5a §4.4.
namespace swegca::architecture {

struct EvidencePolicy {
    double chance_rate = 0.2;
    double accept_margin = 0.25;
    double confidence_level = 0.9;
    double prior_alpha = 1.0;
    double prior_beta = 1.0;
    std::uint32_t minimum_effective_samples_per_axis = 4;
    std::uint32_t minimum_source_diversity = 2;
    std::uint32_t minimum_axis_source_diversity = 1;
    std::uint32_t minimum_context_diversity = 4;
    std::uint32_t recent_window = 6;  // used by the shell tally, not the kernel
    std::uint32_t minimum_recent_samples = 4;
    double regime_change_threshold = 0.3;
    std::uint32_t axis_count = 4;  // observational, counterfactual, intervention, cross-context
};

// Throws `std::invalid_argument("<policy>_invalid:<field>")`.
[[nodiscard]] kernel::EvidenceRules make_evidence_rules(const EvidencePolicy& policy);

// Digest of the canonical encoding of an evidence policy (tag, then every
// field in declaration order: doubles as IEEE-754 bits, all little-endian),
// so a decision names exactly which rule configuration produced it.
// Rule: design board @7c0b62f:195-198 (rule configuration digest).
[[nodiscard]] Digest256 evidence_policy_digest(const EvidencePolicy& policy);

// Standard normal quantile for probability in (0, 1), by bisection on erfc.
// Cold path only (rule construction).
[[nodiscard]] double standard_normal_quantile(double probability);

}  // namespace swegca::architecture

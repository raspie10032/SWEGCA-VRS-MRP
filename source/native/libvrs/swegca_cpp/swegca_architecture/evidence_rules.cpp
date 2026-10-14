#include "swegca_architecture/evidence_rules.hpp"

#include "swegca_architecture/sha256.hpp"

#include <array>
#include <bit>

#include <cmath>
#include <stdexcept>
#include <string>

namespace swegca::architecture {
namespace {

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:69-95
[[noreturn]] void invalid(const char* policy, const char* field) {
    throw std::invalid_argument(std::string(policy) + "_invalid:" + field);
}

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:42-55
bool unit(double value) { return std::isfinite(value) && value >= 0 && value <= 1; }

}  // namespace

// Wichura's AS241 exactly as CPython's statistics module computes it for
// NormalDist().inv_cdf, which is what the author calls: its constants and
// its order of operations (built with -ffp-contract=off), so the z the
// rules keep equals the author's to the bit. The author computes it on
// every interval; the value depends only on the confidence level, so the
// rules compute it once. Codex 2026-09-23 17:28.
// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:261-283
double standard_normal_quantile(double probability) {
    if (!(probability > 0 && probability < 1))
        throw std::invalid_argument("normal_quantile_probability_invalid");
    const double p = probability;
    const double mu = 0.0;
    const double sigma = 1.0;
    const double q = p - 0.5;
    if (std::fabs(q) <= 0.425) {
        const double r = 0.180625 - q * q;
        const double num = (((((((2.5090809287301226727e+3 * r +
                          3.3430575583588128105e+4) * r +
                          6.7265770927008700853e+4) * r +
                          4.5921953931549871457e+4) * r +
                          1.3731693765509461125e+4) * r +
                          1.9715909503065514427e+3) * r +
                          1.3314166789178437745e+2) * r +
                          3.3871328727963666080e+0) * q;
        const double den = (((((((5.2264952788528545610e+3 * r +
                          2.8729085735721942674e+4) * r +
                          3.9307895800092710610e+4) * r +
                          2.1213794301586595867e+4) * r +
                          5.3941960214247511077e+3) * r +
                          6.8718700749205790830e+2) * r +
                          4.2313330701600911252e+1) * r +
                          1.0);
        const double x = num / den;
        return mu + (x * sigma);
    }
    double r = q <= 0.0 ? p : 1.0 - p;
    r = std::sqrt(-std::log(r));
    double num = 0;
    double den = 0;
    if (r <= 5.0) {
        r = r - 1.6;
        num = (((((((7.74545014278341407640e-4 * r +
                          2.27238449892691845833e-2) * r +
                          2.41780725177450611770e-1) * r +
                          1.27045825245236838258e+0) * r +
                          3.64784832476320460504e+0) * r +
                          5.76949722146069140550e+0) * r +
                          4.63033784615654529590e+0) * r +
                          1.42343711074968357734e+0);
        den = (((((((1.05075007164441684324e-9 * r +
                          5.47593808499534494600e-4) * r +
                          1.51986665636164571966e-2) * r +
                          1.48103976427480074590e-1) * r +
                          6.89767334985100004550e-1) * r +
                          1.67638483018380384940e+0) * r +
                          2.05319162663775882187e+0) * r +
                          1.0);
    } else {
        r = r - 5.0;
        num = (((((((2.01033439929228813265e-7 * r +
                          2.71155556874348757815e-5) * r +
                          1.24266094738807843860e-3) * r +
                          2.65321895265761230930e-2) * r +
                          2.96560571828504891230e-1) * r +
                          1.78482653991729133580e+0) * r +
                          5.46378491116411436990e+0) * r +
                          6.65790464350110377720e+0);
        den = (((((((2.04426310338993978564e-15 * r +
                          1.42151175831644588870e-7) * r +
                          1.84631831751005468180e-5) * r +
                          7.86869131145613259100e-4) * r +
                          1.48753612908506148525e-2) * r +
                          1.36929880922735805310e-1) * r +
                          5.99832206555887937690e-1) * r +
                          1.0);
    }
    double x = num / den;
    if (q < 0.0) x = -x;
    return mu + (x * sigma);
}

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:49-95
kernel::EvidenceRules make_evidence_rules(const EvidencePolicy& p) {
    constexpr const char* name = "evidence_policy";
    if (!(std::isfinite(p.chance_rate) && p.chance_rate >= 0 && p.chance_rate < 1))
        invalid(name, "chance_rate");
    if (!(std::isfinite(p.accept_margin) && p.accept_margin >= 0 &&
          p.accept_margin <= 1 - p.chance_rate))
        invalid(name, "accept_margin");
    if (!(std::isfinite(p.confidence_level) && p.confidence_level > 0 &&
          p.confidence_level < 1))
        invalid(name, "confidence_level");
    if (!(std::isfinite(p.prior_alpha) && p.prior_alpha > 0)) invalid(name, "prior_alpha");
    if (!(std::isfinite(p.prior_beta) && p.prior_beta > 0)) invalid(name, "prior_beta");
    if (p.minimum_effective_samples_per_axis == 0 || p.minimum_source_diversity == 0 ||
        p.minimum_axis_source_diversity == 0 || p.minimum_context_diversity == 0 ||
        p.recent_window == 0 || p.minimum_recent_samples == 0)
        invalid(name, "minimum_limits");
    if (p.minimum_recent_samples > p.recent_window) invalid(name, "minimum_recent_samples");
    if (!unit(p.regime_change_threshold)) invalid(name, "regime_change_threshold");
    if (p.axis_count == 0 || p.axis_count > kernel::max_axes) invalid(name, "axis_count");

    kernel::EvidenceRules rules;
    rules.z_ = standard_normal_quantile(0.5 + p.confidence_level / 2);
    rules.z_squared_ = rules.z_ * rules.z_;
    rules.threshold_ = p.chance_rate + p.accept_margin;
    rules.prior_alpha_ = p.prior_alpha;
    rules.prior_beta_ = p.prior_beta;
    rules.minimum_effective_samples_per_axis_ = p.minimum_effective_samples_per_axis;
    rules.minimum_source_diversity_ = p.minimum_source_diversity;
    rules.minimum_axis_source_diversity_ = p.minimum_axis_source_diversity;
    rules.minimum_context_diversity_ = p.minimum_context_diversity;
    rules.minimum_recent_samples_ = p.minimum_recent_samples;
    rules.recent_window_ = p.recent_window;
    rules.regime_change_threshold_ = p.regime_change_threshold;
    rules.axis_count_ = p.axis_count;
    if (!kernel::rules_valid(rules)) invalid(name, "derived_rules");
    return rules;
}

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:49-67
Digest256 evidence_policy_digest(const EvidencePolicy& p) {
    Sha256 hash;
    hash.update(std::string_view("swegca.evidence_policy.v1"));
    const auto put = [&hash](std::uint64_t value, std::size_t width) {
        std::array<std::byte, 8> bytes{};
        for (std::size_t at = 0; at < width; ++at)
            bytes[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
        hash.update(std::span<const std::byte>(bytes.data(), width));
    };
    for (const double value : {p.chance_rate, p.accept_margin, p.confidence_level,
                               p.prior_alpha, p.prior_beta})
        put(std::bit_cast<std::uint64_t>(value), 8);
    for (const std::uint32_t value :
         {p.minimum_effective_samples_per_axis, p.minimum_source_diversity,
          p.minimum_axis_source_diversity, p.minimum_context_diversity, p.recent_window,
          p.minimum_recent_samples})
        put(value, 4);
    put(std::bit_cast<std::uint64_t>(p.regime_change_threshold), 8);
    put(p.axis_count, 4);
    return Digest256(hash.finish());
}

}  // namespace swegca::architecture

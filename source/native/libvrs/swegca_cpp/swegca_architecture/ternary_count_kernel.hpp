#pragma once
#include "swegca_architecture/evidence_kernel.hpp"
#include <limits>
namespace swegca::architecture::kernel {
// Persistent order is (accept, reject, abstain). Counts are never ratios or
// a reconstructed history from an old floating-point strength.
struct TernaryCount {
    std::uint64_t accept=0,reject=0,abstain=0;
    constexpr bool operator==(const TernaryCount&) const noexcept = default;
};
// Neutral support balance corresponds to the old neutral strength of 1.
// Preserve the user's inclusive activation boundary without float conversion.
[[nodiscard]] constexpr bool count_evidence_eligible(const TernaryCount& value) noexcept {
    return value.accept >= value.reject;
}
static_assert(sizeof(TernaryCount)==3*sizeof(std::uint64_t));
// Call only with a verdict issued by the core. Overflow fails without changing
// any component; no saturation, wrapping, or fabricated abstention.
[[nodiscard]] inline bool accumulate_verdict(TernaryCount& value,EvidenceStatus verdict) noexcept {
    std::uint64_t* slot=nullptr;
    switch(verdict){
    case EvidenceStatus::accept:slot=&value.accept;break;
    case EvidenceStatus::reject:slot=&value.reject;break;
    case EvidenceStatus::abstain:slot=&value.abstain;break;
    default:return false;
    }
    if(*slot==std::numeric_limits<std::uint64_t>::max())return false;
    ++*slot;return true;
}
}

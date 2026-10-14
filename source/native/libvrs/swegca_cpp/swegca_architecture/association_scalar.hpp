#pragma once
#include "swegca_architecture/evidence_scalar.hpp"
namespace swegca::architecture::kernel {
struct AssociationEvidence {
    platform::uint64_t support=0;
    // A valid measured false predicate is refutation of THIS relation.
    // An invalid input/read is insufficient, never a negative observation.
    platform::uint64_t refute=0;
};
enum class AssociationReason : platform::uint8_t {
    unresolved=1, observed_association=2, explicit_refutation=3, conflicting_evidence=4,
};
// This distinct result cannot be passed to World/semantic promotion gates that
// require EvidenceJudgment. The general four-axis decision remains unchanged.
class AssociationJudgment final {
public:
    constexpr AssociationJudgment() noexcept = default;
    [[nodiscard]] constexpr EvidenceStatus status() const noexcept{return status_;}
    [[nodiscard]] constexpr AssociationReason reason() const noexcept{return reason_;}
private:
    friend AssociationJudgment judge_association(const AssociationEvidence&) noexcept;
    EvidenceStatus status_=EvidenceStatus::abstain;
    AssociationReason reason_=AssociationReason::unresolved;
};
// Existing local VRS rule: support strengthens, explicit refutation weakens,
// unresolved/conflicting evidence preserves. No truth probability is claimed.
[[nodiscard]] inline AssociationJudgment judge_association(const AssociationEvidence& e) noexcept {
    AssociationJudgment result;
    if(e.support&&e.refute)result.reason_=AssociationReason::conflicting_evidence;
    else if(e.support){result.status_=EvidenceStatus::accept;result.reason_=AssociationReason::observed_association;}
    else if(e.refute){result.status_=EvidenceStatus::reject;result.reason_=AssociationReason::explicit_refutation;}
    return result;
}
}

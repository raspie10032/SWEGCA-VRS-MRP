#pragma once

#include "swegca_architecture/evidence_observation_kernel.hpp"
#include "swegca_architecture/connection_strength_kernel.hpp"
#include "swegca_architecture/association_scalar.hpp"
#include <span>
#include <algorithm>

namespace swegca::architecture::kernel {

// A verified SOURCE BINDING, not image identity, tag truth, or World authority.
// Host supplies digests from authenticated original records and the existing
// paired processing record. The user's verified image/tag/DINO bundle remains
// one observation; its modalities are not independent producers.
class BoundExperience final {
public:
    constexpr BoundExperience() noexcept = default;
    [[nodiscard]] constexpr bool valid() const noexcept { return valid_; }
private:
    friend BoundExperience bind_experience(const Digest&,const Digest&,const Digest&,const Digest&) noexcept;
    bool valid_=false;
};
[[nodiscard]] inline BoundExperience bind_experience(const Digest& actual_input,
    const Digest& declared_input,const Digest& observation_record,const Digest& processing_record) noexcept {
    BoundExperience result;
    result.valid_=named_digest(actual_input)&&actual_input==declared_input&&
        named_digest(observation_record)&&named_digest(processing_record);
    return result;
}

// These predicates describe recorded membership, not semantic truth.
// A valid observed nonmatch is false; invalid input is not a negative sample.
[[nodiscard]] inline PredicateObservation observe_member_association(const BoundExperience& binding,
    std::span<const std::uint32_t> actual_tags,std::uint32_t left,std::uint32_t right) noexcept {
    bool seen_left=false,seen_right=false;
    for(auto tag:actual_tags){seen_left|=tag==left;seen_right|=tag==right;}
    return {binding.valid() && left!=right, seen_left && seen_right};
}
[[nodiscard]] inline PredicateObservation observe_recorded_member(
    const BoundExperience& binding, std::span<const std::uint32_t> members,
    std::uint32_t member) noexcept {
    return {binding.valid(), std::find(members.begin(),members.end(),member)!=members.end()};
}
[[nodiscard]] inline PredicateObservation observe_tag_image_match(
    const BoundExperience& binding, std::span<const std::uint32_t> members,
    std::uint32_t member) noexcept {
    return observe_recorded_member(binding,members,member);
}
[[nodiscard]] inline PredicateObservation observe_common_member(
    const BoundExperience& left, std::span<const std::uint32_t> left_members,
    const BoundExperience& right, std::span<const std::uint32_t> right_members,
    std::uint32_t member) noexcept {
    const auto a=observe_recorded_member(left,left_members,member);
    const auto b=observe_recorded_member(right,right_members,member);
    return {a.valid && b.valid, a.holds && b.holds};
}

class AssociationStrength final {
public:
    [[nodiscard]] constexpr bool valid() const noexcept{return valid_;}
    [[nodiscard]] constexpr double current() const noexcept{return current_;}
    [[nodiscard]] constexpr bool evidence_eligible() const noexcept {
        return valid_ && connection_evidence_eligible(current_);
    }
private:
    friend AssociationStrength revise_association_strength(double,const AssociationJudgment&) noexcept;
    bool valid_=false;double current_=0;
};
[[nodiscard]] inline AssociationStrength revise_association_strength(
    double previous,const AssociationJudgment& judgment) noexcept {
    AssociationStrength result;
    if(!finite_count(previous))return result;
    const auto status=judgment.status();
    const auto current=status==EvidenceStatus::accept?previous*1.01:
        status==EvidenceStatus::reject?previous*.995:previous;
    if(!finite_count(current))return result;
    result.valid_=true;result.current_=current;return result;
}
} // namespace swegca::architecture::kernel

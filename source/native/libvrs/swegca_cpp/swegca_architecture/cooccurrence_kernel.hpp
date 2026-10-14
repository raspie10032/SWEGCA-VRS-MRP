#pragma once
#include "swegca_architecture/evidence_observation_kernel.hpp"
#include <bit>
#include <span>
namespace swegca::architecture::kernel {
// Bit positions identify the same canonical image experience in both inputs.
// known excludes invalid observations; present is a subset of known.
struct MembershipBits { std::span<const std::uint64_t> known, present; };
struct CooccurrenceObservation { PredicateObservation predicate; std::uint64_t witnesses=0; };
[[nodiscard]] inline CooccurrenceObservation observe_cooccurrence(
    MembershipBits a,MembershipBits b) noexcept {
    if(a.known.empty()||a.known.size()!=a.present.size()||a.known.size()!=b.known.size()||a.known.size()!=b.present.size())return {};
    std::uint64_t witnesses=0;bool unresolved=false;
    for(std::size_t i=0;i<a.known.size();++i){
        if((a.present[i]&~a.known[i])||(b.present[i]&~b.known[i]))return {};
        witnesses+=std::popcount(a.present[i]&b.present[i]);
        // Unknown can matter only where neither operand is known false.
        const auto false_a=a.known[i]&~a.present[i],false_b=b.known[i]&~b.present[i];
        unresolved|=(~(a.known[i]&b.known[i])&~false_a&~false_b)!=0;
    }
    return {{witnesses!=0||!unresolved,witnesses!=0},witnesses};
}
}

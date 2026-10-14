#pragma once
#include "swegca_architecture/ternary_count_kernel.hpp"

namespace swegca::architecture::kernel {
// User-defined three-state growth observations. These are descriptive inputs,
// not a truth probability, a promotion decision or a traversal permission.
// Experience validity is independent. invalid_counts names a malformed count
// window only; it must never be interpreted as an invalid experience.
enum class GrowthState : std::uint8_t { invalid_counts, no_observation, observed };
struct ConnectionGrowth {
 GrowthState state=GrowthState::invalid_counts;
 TernaryCount delta{};
 std::uint64_t observations=0;
 double accept_fraction=0,refute_fraction=0,abstain_fraction=0;
};
[[nodiscard]] constexpr ConnectionGrowth connection_growth(
 const TernaryCount& before,const TernaryCount& after) noexcept {
 if(after.accept<before.accept||after.reject<before.reject||after.abstain<before.abstain)return {};
 const TernaryCount delta{after.accept-before.accept,after.reject-before.reject,after.abstain-before.abstain};
 constexpr auto max=std::numeric_limits<std::uint64_t>::max();
 if(delta.reject>max-delta.accept)return {};
 const auto decided=delta.accept+delta.reject;
 if(delta.abstain>max-decided)return {};
 const auto total=decided+delta.abstain;
 if(!total)return {GrowthState::no_observation,delta,0,0,0,0};
 const auto n=static_cast<double>(total);
 return {GrowthState::observed,delta,total,double(delta.accept)/n,double(delta.reject)/n,double(delta.abstain)/n};
}
// Merge actual counts from separate blocks before dividing. Do not average
// block fractions: different blocks can contain different observation counts.
[[nodiscard]] constexpr bool merge_growth_counts(TernaryCount& total,const TernaryCount& add)noexcept{
 constexpr auto max=std::numeric_limits<std::uint64_t>::max();
 if(add.accept>max-total.accept||add.reject>max-total.reject||add.abstain>max-total.abstain)return false;
 total.accept+=add.accept;total.reject+=add.reject;total.abstain+=add.abstain;return true;
}
struct ExperienceSpread {
 bool valid=false,observed=false;
 std::uint64_t population=0,tested=0,supported=0;
 double tested_fraction=0,supported_fraction=0;
};
// The VRS owner supplies exact deduplicated experience cardinalities. Count
// repeated verification in growth, but never as a new distinct experience.
[[nodiscard]] constexpr ExperienceSpread experience_spread(
 std::uint64_t population,std::uint64_t tested,std::uint64_t supported) noexcept {
 if(supported>tested||tested>population)return {};
 if(!population)return {true,false,0,0,0,0,0};
 return {true,tested!=0,population,tested,supported,double(tested)/double(population),double(supported)/double(population)};
}
}

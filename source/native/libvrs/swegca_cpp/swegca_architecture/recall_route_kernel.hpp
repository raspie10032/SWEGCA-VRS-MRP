#pragma once

#include "swegca_architecture/session_kernel.hpp"
#include "swegca_architecture/head_publication_kernel.hpp"
#include <tuple>
#include <cstddef>

namespace swegca::architecture::kernel {

// User's temporary-first rule. An unavailable tier is an error, never a miss.
enum class RecallScope { unavailable, temporary, main };
// A current exact cue precedes the continued memory key within one tier.
// Both are familiarity cues only, not evidence of current truth.
enum class FamiliarityKey { missing, exact, continuation, context };
// Scheduling one original for Replay, not judging its truth. Every outcome
// remains accessible. Strength is the stored connection confidence; recency
// breaks equal-confidence ties. Exact ties use canonical identity/address so
// map insertion order and recovery do not change the choice.
struct ReplayCandidate {
    double strength = 0;
    std::uint64_t observed_at = 0;
    Digest connection{};
    RecordAddress original;
};
enum class ReplayPreference { invalid, keep, replace };
[[nodiscard]] inline ReplayPreference prefer_replay(const ReplayCandidate* current,
    const ReplayCandidate& candidate) noexcept {
    const auto valid=[](const ReplayCandidate& value) {
        return finite_count(value.strength) && named_digest(value.connection) && head_address_valid(value.original);
    };
    if(!valid(candidate) || (current && !valid(*current))) return ReplayPreference::invalid;
    if(!current) return ReplayPreference::replace;
    if(candidate.strength!=current->strength)
        return candidate.strength>current->strength ? ReplayPreference::replace : ReplayPreference::keep;
    if(candidate.observed_at!=current->observed_at)
        return candidate.observed_at>current->observed_at ? ReplayPreference::replace : ReplayPreference::keep;
    const auto key=[](const ReplayCandidate& value) {
        return std::tie(value.connection,value.original.block,value.original.offset,value.original.bytes,value.original.digest);
    };
    return key(candidate)<key(*current) ? ReplayPreference::replace : ReplayPreference::keep;
}
// A dialogue seed starts from an explicitly recorded input key, not a
// lifecycle notification. Established context traversal keeps every outcome.
[[nodiscard]] constexpr bool context_reference_eligible(bool has_input_key,bool seed_only) noexcept {
    return !seed_only || has_input_key;
}
// Authenticate membership of one already selected reference in the recorded
// lookup. This grants no truth verdict and does not run candidate selection.
[[nodiscard]] inline bool restored_reference_matches(FamiliarityKey kind,
    const Digest& input,const Digest& lookup,const Digest& connection,
    const Digest& reference_cue,const Digest& context,bool has_input_key,bool seed_only) noexcept {
    if(!named_digest(input)||!named_digest(lookup)||!named_digest(connection))return false;
    switch(kind){
    case FamiliarityKey::exact:
        return !seed_only&&input==lookup&&reference_cue==lookup;
    case FamiliarityKey::continuation:
        return !seed_only&&connection==lookup;
    case FamiliarityKey::context:
        return context==lookup&&context_reference_eligible(has_input_key,seed_only);
    default:return false;
    }
}
// Receipt representation only. No candidate or evidence is discarded. Compare
// bounded snapshot metadata with individual address pins without multiplication
// overflow; the caller accounts for its own VRS storage representation.
[[nodiscard]] constexpr bool recall_range_receipt(std::size_t count,std::size_t range_bytes,
    std::size_t pin_bytes,bool pins_resident=true) noexcept {
    // Cold metadata stays as a bounded address range until selected Replay;
    // constructing individual pins would perform speculative disk reads.
    return count && (!pins_resident || (pin_bytes &&
        count >= range_bytes/pin_bytes + (range_bytes%pin_bytes!=0)));
}
// A Main-owned context linking distinct connections may extend a continued
// memory. The caller supplies recorded index membership, never a truth score.
[[nodiscard]] constexpr FamiliarityKey familiarity_key(bool exact, bool continued,bool shared_context=false,
    bool linked_context=false) noexcept {
    return exact ? FamiliarityKey::exact : shared_context&&linked_context ? FamiliarityKey::context : continued ? FamiliarityKey::continuation :
        shared_context ? FamiliarityKey::context : FamiliarityKey::missing;
}
[[nodiscard]] constexpr RecallScope recall_scope(bool temporary_usable, bool temporary_found) noexcept {
    if (!temporary_usable) return RecallScope::unavailable;
    return temporary_found ? RecallScope::temporary : RecallScope::main;
}
[[nodiscard]] constexpr bool main_session_readable(SessionPhase phase, bool usable) noexcept {
    return usable && phase == SessionPhase::published;
}

}  // namespace swegca::architecture::kernel

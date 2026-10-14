#pragma once
#include "swegca_architecture/evidence_observation_kernel.hpp"
namespace swegca::architecture::kernel {
// Address/index integrity only. A portal never grants an evidence verdict.
struct PortalRange {
    Digest connection{};
    std::uint64_t begin=0,end=0;
    bool operator==(const PortalRange&) const = default;
};
[[nodiscard]] inline bool portal_range_valid(const PortalRange& value) noexcept {
    return named_digest(value.connection)&&value.begin<value.end;
}
[[nodiscard]] inline bool portal_range_follows(const PortalRange& previous,const PortalRange& next) noexcept {
    return portal_range_valid(previous)&&portal_range_valid(next)&&
        (previous.connection<next.connection||(previous.connection==next.connection&&previous.end<=next.begin));
}
} // namespace swegca::architecture::kernel

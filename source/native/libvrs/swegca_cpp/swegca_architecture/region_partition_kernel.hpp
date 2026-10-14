#pragma once
#include <cstddef>
#include <optional>

namespace swegca::architecture::kernel {
// Address-region partitioning, not evidence approval or semantic similarity.
// Main supplies the distinct ordered connection count and its resource bound.
[[nodiscard]] constexpr std::optional<std::size_t> region_partition_end(
    std::size_t begin,std::size_t total,std::size_t capacity) noexcept {
    if(!capacity||begin>=total)return std::nullopt;
    return begin+(total-begin<capacity?total-begin:capacity);
}
} // namespace swegca::architecture::kernel

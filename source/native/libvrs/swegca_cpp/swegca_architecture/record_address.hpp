#pragma once

#include "swegca_architecture/digest_bytes.hpp"
#include <cstdint>

namespace swegca::architecture {

// Fixed-width address bytes shared by storage and the core's lineage checks.
// No I/O, allocation, memory budget, or storage policy belongs to this type.
struct RecordAddress {
    DigestBytes block{};
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
    DigestBytes digest{};
    bool operator==(const RecordAddress&) const = default;
};

}  // namespace swegca::architecture

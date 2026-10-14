#pragma once
#include "swegca_architecture/sha256.hpp"
namespace swegca::architecture {
inline Sha256 agent_delivery_prefix(std::uint64_t sequence,std::uint64_t observed) {
    Sha256 hash;hash.update("SWEGCA native delivery v1");
    std::array<std::byte,16> prefix{};
    for(unsigned n=0;n<8;++n){prefix[n]=std::byte((sequence>>(n*8))&255);prefix[8+n]=std::byte((observed>>(n*8))&255);}
    hash.update(prefix);return hash;
}
inline DigestBytes agent_delivery_identity(std::uint64_t sequence,std::uint64_t observed,std::string_view native) {
    auto hash=agent_delivery_prefix(sequence,observed);hash.update(native);return hash.finish();
}
} // namespace swegca::architecture

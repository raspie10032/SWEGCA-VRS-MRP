#pragma once
#include <cstddef>
namespace swegca::architecture::kernel {
// Resource placement only; no experience, strength or truth is discarded.
// A partial writer tail or a shared/pinned segment must remain resident.
enum class MetadataRelease { retain, persist_then_release, release };
[[nodiscard]] constexpr bool metadata_page_beneficial(bool backed,std::size_t resident,std::size_t backing) noexcept {
    return backed || resident>backing;
}
[[nodiscard]] constexpr bool metadata_pressure(std::size_t used,std::size_t target,bool preparing) noexcept {
    return !preparing && used>target;
}
// Called only when the last derived-page handle is being destroyed. Original
// experience storage is never eligible. Unknown or shared inodes are retained.
[[nodiscard]] constexpr bool discard_metadata_page(bool same_owned_inode,
    bool sole_link) noexcept { return same_owned_inode && sole_link; }
[[nodiscard]] constexpr MetadataRelease metadata_release(bool complete,
    std::size_t owners,bool backed) noexcept {
    if(!complete||owners!=1)return MetadataRelease::retain;
    return backed?MetadataRelease::release:MetadataRelease::persist_then_release;
}
}

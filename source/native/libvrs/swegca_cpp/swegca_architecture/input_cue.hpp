#pragma once

#include "swegca_architecture/sha256.hpp"

namespace swegca::architecture {

// The recorded dialogue context key. Computed at session setup, never an
// extra name hash before each Recall. This is identity, not semantic evidence.
[[nodiscard]] inline DigestBytes input_session_context(const DigestBytes& owner,std::string_view session) {
    // The fixed-width owner binds provider/instance/session provenance. Equal
    // display names from independent stores do not imply shared dialogue.
    Sha256 digest;digest.update("SWEGCA input session v2");digest.update(owner);
    digest.update(session);return digest.finish();
}

// Content addressing only: an exact cue carries no support/refutation verdict.
// Framing prevents media/content boundary ambiguity; all bytes are retained.
[[nodiscard]] inline Sha256 input_cue_prefix(std::uint64_t media_bytes) {
    Sha256 digest;
    digest.update("SWEGCA original input cue v1");
    std::array<std::byte, 8> length{};
    for (unsigned i = 0; i < 8; ++i) length[i] = std::byte((media_bytes >> (8 * i)) & 255);
    digest.update(length); return digest;
}
[[nodiscard]] inline DigestBytes input_cue(std::string_view media, std::span<const std::byte> content) {
    auto digest=input_cue_prefix(media.size());
    digest.update(media); digest.update(content);
    return digest.finish();
}

// Producer-declared subclaim identity, not proof that the subclaim is relevant
// to, equivalent to, or sufficient for its parent input. Keep its evidence in
// a separate connection even if the scope text repeats the entire input.
[[nodiscard]] inline DigestBytes input_observation_scope(const DigestBytes& parent,
    std::string_view scope) {
    Sha256 digest; digest.update("SWEGCA input observation scope v1"); digest.update(parent);
    std::array<std::byte, 8> length{};
    for (unsigned i=0;i<8;++i) length[i]=std::byte((std::uint64_t(scope.size())>>(8*i))&255);
    digest.update(length);digest.update(scope);return digest.finish();
}

// Separate provenance domain from producer-declared scopes and the parent
// cognition journal. A producer cannot name a scope that aliases this channel.
[[nodiscard]] inline DigestBytes related_cognition_channel(const DigestBytes& input) {
    Sha256 digest;digest.update("SWEGCA related cognition v1");digest.update(input);return digest.finish();
}

// A connection's comparison cannot overwrite another related requirement.
[[nodiscard]] inline DigestBytes related_connection_cognition_channel(const DigestBytes& input,
    const DigestBytes& connection) {
    Sha256 digest;digest.update("SWEGCA related connection cognition v1");
    digest.update(input);digest.update(connection);return digest.finish();
}

}  // namespace swegca::architecture

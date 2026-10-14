#pragma once

#include <cstdint>

namespace swegca::architecture::kernel {

// User's session rule: live experience stays temporary; Main publication is
// permitted only after explicit session end. An idle timer is not an end event.
// Physical storage success supplies the next state; this kernel performs no I/O.
enum class SessionPhase : std::uint8_t { invalid = 0, active = 1, ended = 2, published = 3 };
enum class SessionOperation : std::uint8_t { append = 1, end = 2, publish = 3 };

[[nodiscard]] constexpr bool next_session_phase(SessionPhase from, SessionOperation operation,
    SessionPhase& next) noexcept {
    next = SessionPhase::invalid;
    switch (operation) {
    case SessionOperation::append:
        if (from != SessionPhase::active) return false;
        next = from;
        return true;
    case SessionOperation::end:
        if (from != SessionPhase::active) return false;
        next = SessionPhase::ended;
        return true;
    case SessionOperation::publish:
        if (from != SessionPhase::ended) return false;
        next = SessionPhase::published;
        return true;
    }
    return false;
}

}  // namespace swegca::architecture::kernel

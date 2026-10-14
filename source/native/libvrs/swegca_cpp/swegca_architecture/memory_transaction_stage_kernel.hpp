#pragma once

#include <cstdint>

// SWEGCA nano-core: the stages of one World-linked memory transaction and
// the stage changes the author makes. Kernel rules as in evidence_kernel.hpp:
// no allocation, lock, exception, I/O or string; the kernel checks its own
// inputs and fails closed.
//
// Main-owned, not here: the journal and its compare-and-swap of a stored
// stage, the precondition for starting a transaction (no incomplete
// transaction exists and the id is new, checked by the author's
// _prepare_journal :72-93 before it writes `prepared` at :94-111),
// recovery ordering and compensation records, and the memory and state
// work each stage records. A stage is a recorded name. `state_committed`
// in particular proves nothing about Main's current publication: the
// author sets it right after `memory_committed` (:265-271), with the World
// write committed before the promotion began (:224).
//
// Rules: tinylm-slicer-sanabi-bazzite
// src/tinylm_slicer/mosaic_world_memory_transaction.py@3bddcb7:67-183,212-361.
namespace swegca::architecture::kernel {

// The six stages the architecture keeps (inventory 3.G), each a stage
// literal in the author's journal.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:177-180
enum class MemoryTransactionStage : std::uint8_t {
    invalid = 0,           // failed-call sentinel, never a journal stage
    prepared = 1,          // :99
    memory_committed = 2,  // :265
    state_committed = 3,   // :270
    completed = 4,         // :272
    rollback_pending = 5,  // :299, :347
    rolled_back = 6,       // :163, :311, :359
};

// The author path that changes a stored stage. The same pair of stages can be
// reached by one path and not by another, so an edge names its path.
enum class MemoryTransactionEdge : std::uint8_t {
    commit_memory = 1,    // promote_world_linked_semantic :265
    commit_state = 2,     // :266-271
    complete = 3,         // :272
    begin_rollback = 4,   // rollback :295-300, retract :343-348
    finish_rollback = 5,  // rollback :307-312, retract :355-360
    recover = 6,          // recover_incomplete_world_memory_transactions :160-167
};

// Lineage: native mechanism — the author's stage is one of six text literals;
// a C++ enum can hold any value, so the kernel admits only those six.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:177-180
[[nodiscard]] constexpr bool memory_transaction_stage_valid(MemoryTransactionStage stage) noexcept {
    const auto value = static_cast<std::uint8_t>(stage);
    return value >= static_cast<std::uint8_t>(MemoryTransactionStage::prepared) &&
           value <= static_cast<std::uint8_t>(MemoryTransactionStage::rolled_back);
}

// Every stage except completed and rolled_back; the author blocks a new
// transaction on one and recovers each.
// Lineage: direct — "stage NOT IN ('completed', 'rolled_back')".
// SWEGCA: src/tinylm_slicer/mosaic_world_memory_transaction.py@3bddcb7:75-79
[[nodiscard]] constexpr bool memory_transaction_incomplete(MemoryTransactionStage stage) noexcept {
    return memory_transaction_stage_valid(stage) && stage != MemoryTransactionStage::completed &&
           stage != MemoryTransactionStage::rolled_back;
}

// The stage `edge` leads to from `from`. The forward path goes prepared,
// memory_committed, state_committed, completed; a rollback or retraction
// goes from completed to rollback_pending and then rolled_back; recovery
// moves any incomplete stage straight to rolled_back, never through
// rollback_pending. The author changes a stored stage only when it still
// equals the expected one (:122-129, :163-165), which is `from` here. Any
// other pair, or a value outside the enumerations, has no edge: the function
// returns false and resets `out` to invalid; a failed call never preserves a
// previous completed stage. Main must fail closed on false. rolled_back has
// no edge out. The invalid sentinel is not a persistent transaction stage.
// Lineage: direct — every stage change the author makes, no other.
// SWEGCA: src/tinylm_slicer/mosaic_world_memory_transaction.py@3bddcb7:114-183
// SWEGCA: src/tinylm_slicer/mosaic_world_memory_transaction.py@3bddcb7:212-361
[[nodiscard]] constexpr bool next_memory_transaction_stage(MemoryTransactionStage from,
                                                           MemoryTransactionEdge edge,
                                                           MemoryTransactionStage& out) noexcept {
    out = MemoryTransactionStage::invalid;
    if (!memory_transaction_stage_valid(from)) return false;
    MemoryTransactionStage next = from;
    switch (edge) {
    case MemoryTransactionEdge::commit_memory:
        if (from != MemoryTransactionStage::prepared) return false;
        next = MemoryTransactionStage::memory_committed;
        break;
    case MemoryTransactionEdge::commit_state:
        if (from != MemoryTransactionStage::memory_committed) return false;
        next = MemoryTransactionStage::state_committed;
        break;
    case MemoryTransactionEdge::complete:
        if (from != MemoryTransactionStage::state_committed) return false;
        next = MemoryTransactionStage::completed;
        break;
    case MemoryTransactionEdge::begin_rollback:
        if (from != MemoryTransactionStage::completed) return false;
        next = MemoryTransactionStage::rollback_pending;
        break;
    case MemoryTransactionEdge::finish_rollback:
        if (from != MemoryTransactionStage::rollback_pending) return false;
        next = MemoryTransactionStage::rolled_back;
        break;
    case MemoryTransactionEdge::recover:
        if (!memory_transaction_incomplete(from)) return false;
        next = MemoryTransactionStage::rolled_back;
        break;
    default:
        return false;
    }
    out = next;
    return true;
}

// During recovery, a memory rollback that fails is passed over only while the
// transaction is still `prepared`, when no memory mutation was recorded; at
// any other stage the author raises it. Whether a rollback is attempted at
// all (the author skips it without an update id, :150) is the caller's.
// Lineage: direct — `except (KeyError, ValueError): if stage != "prepared": raise`.
// SWEGCA: src/tinylm_slicer/mosaic_world_memory_transaction.py@3bddcb7:150-159
[[nodiscard]] constexpr bool recovery_tolerates_failed_memory_rollback(MemoryTransactionStage stage) noexcept {
    return stage == MemoryTransactionStage::prepared;
}

}  // namespace swegca::architecture::kernel

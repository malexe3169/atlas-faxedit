#ifndef FH_ATLAS_DEV_MUSIC_INTENT_H
#define FH_ATLAS_DEV_MUSIC_INTENT_H

#include <cstdint>

using byte = std::uint8_t;
using word = std::uint16_t;

// Public ABI shared by the living-score conductor, scheduler role, installer,
// script event producer, and runtime probes. This is the only supported ABI.
namespace fh::ami {
	constexpr byte KIND{ 0x06 };
	constexpr byte PUBLISH_READY{ 0x80 };
	constexpr byte TXN_LOCK{ 0x40 };
	constexpr byte RESERVED_MASK{ 0x3c };
	constexpr byte STATE_MASK{ 0x03 };
	constexpr byte FAMILY_MASK{ 0x3c };
	constexpr byte PACKED_FAMILY_STATE_MASK{ 0x3f };
	constexpr byte COMMAND_LIFECYCLE{ 0x40 };
	constexpr byte COMMAND_EVENT{ 0x80 };
	constexpr byte COMMAND_KIND_MASK{ 0xc0 };
	constexpr byte LANDING_STAGED_EVENT{ 0x40 };
	constexpr byte LANDING_OBLIGATION{ 0x80 };
	constexpr byte LANDING_INTERLOCK_MASK{ 0xc0 };

	constexpr byte STATE_CALM{ 0 };
	constexpr byte STATE_EXPLORE{ 1 };
	constexpr byte STATE_DANGER{ 2 };
	constexpr byte MANTRA_FAMILY_INDEX{ 7 };
	constexpr byte MANTRA_FAMILY_BITS{ MANTRA_FAMILY_INDEX << 2 };

	constexpr word RAM_REQUEST{ 0x04ef };
	constexpr word RAM_RNG_LO{ 0x04f0 };
	constexpr word RAM_RNG_HI{ 0x04f1 };
	constexpr word RAM_ACTIVE_TOKEN{ 0x04f2 };
	constexpr word RAM_PUBLISH{ 0x04f3 };
	constexpr word RAM_DWELL_LO{ 0x04f4 };
	constexpr word RAM_DWELL_HI{ 0x04f5 };
	constexpr word RAM_STAGED_TOKEN{ 0x04f6 };
	constexpr word RAM_LANDING{ 0x04f7 };
	constexpr word RAM_MUSIC_OWNER{ 0x00fa };

	// Script-side first-writer-wins event mailbox. Zero is empty; values 1..32
	// encode sparse event indexes 0..31.
	constexpr word RAM_EVENT_MAILBOX{ 0x04df };
	constexpr byte EVENT_MAILBOX_EMPTY{ 0x00 };
	constexpr byte MAX_EVENT_INDEX{ 0x1f };

	// Exhaustive emitted-byte timing certificate. Counts use Ricoh 2A03 / NMOS
	// 6502 timing and conservatively charge the page-cross cycle on every taken
	// branch, so the envelope is independent of publisher placement.
	namespace timing {
		constexpr byte CERTIFICATE_VERSION{ 1 };
		constexpr word PUBLISHER_MAX_CYCLES{ 348 };
		constexpr word PUBLISHER_MAX_INSTRUCTIONS{ 114 };
		constexpr word SCHEDULER_CORE_OVERHEAD_MAX_CYCLES{ 138 };
		constexpr word SCHEDULER_CORE_OVERHEAD_MAX_INSTRUCTIONS{ 37 };
		constexpr word SCHEDULER_HOOK_CALL_AND_PADDING_CYCLES{ 10 };
		constexpr word SCHEDULER_HOOK_CALL_AND_PADDING_INSTRUCTIONS{ 3 };
		constexpr word DISPLACED_VANILLA_CYCLES{ 6 };
		constexpr word DISPLACED_VANILLA_INSTRUCTIONS{ 2 };
		constexpr word SCHEDULER_PRE_DISPATCH_MAX_CYCLES{
			SCHEDULER_CORE_OVERHEAD_MAX_CYCLES
			+ SCHEDULER_HOOK_CALL_AND_PADDING_CYCLES };
		constexpr word SCHEDULER_PRE_DISPATCH_MAX_INSTRUCTIONS{
			SCHEDULER_CORE_OVERHEAD_MAX_INSTRUCTIONS
			+ SCHEDULER_HOOK_CALL_AND_PADDING_INSTRUCTIONS };
		constexpr word SCHEDULER_INCREMENTAL_CPU_MAX_CYCLES{
			SCHEDULER_PRE_DISPATCH_MAX_CYCLES - DISPLACED_VANILLA_CYCLES };
		constexpr word SCHEDULER_INCREMENTAL_MAX_INSTRUCTIONS{
			SCHEDULER_PRE_DISPATCH_MAX_INSTRUCTIONS
			- DISPLACED_VANILLA_INSTRUCTIONS };
		constexpr word COMBINED_HOOK_CPU_MAX_CYCLES{
			PUBLISHER_MAX_CYCLES + SCHEDULER_PRE_DISPATCH_MAX_CYCLES };
		constexpr word COMBINED_HOOK_MAX_INSTRUCTIONS{
			PUBLISHER_MAX_INSTRUCTIONS + SCHEDULER_PRE_DISPATCH_MAX_INSTRUCTIONS };
		constexpr word COMBINED_INCREMENTAL_CPU_MAX_CYCLES{
			PUBLISHER_MAX_CYCLES + SCHEDULER_INCREMENTAL_CPU_MAX_CYCLES };
		constexpr word COMBINED_INCREMENTAL_MAX_INSTRUCTIONS{
			PUBLISHER_MAX_INSTRUCTIONS + SCHEDULER_INCREMENTAL_MAX_INSTRUCTIONS };
		constexpr word OAM_DMA_STALL_MIN_CYCLES{ 513 };
		constexpr word OAM_DMA_STALL_MAX_CYCLES{ 514 };
		constexpr word OAM_DMA_ALIGNMENT_DELTA_MAX_CYCLES{ 1 };
		constexpr word COMBINED_INCREMENTAL_BUDGET_MAX_CYCLES{
			COMBINED_INCREMENTAL_CPU_MAX_CYCLES
			+ OAM_DMA_ALIGNMENT_DELTA_MAX_CYCLES };
		constexpr word COMBINED_INCREMENTAL_BUDGET_MAX_INSTRUCTIONS{
			COMBINED_INCREMENTAL_MAX_INSTRUCTIONS };
		constexpr word COMBINED_HOOK_WITH_DMA_MAX_CYCLES{
			COMBINED_HOOK_CPU_MAX_CYCLES + OAM_DMA_STALL_MAX_CYCLES };
	}
}

#endif

#include "AtlasDevFrameScheduler.h"
#include "AtlasDevMusicIntent.h"
#include "HackManager.h"
#include "fe/Config.h"
#include "common/klib/Asm6502.h"
#include "fh_constants.h"
#include <format>
#include <stdexcept>
#include <string>
#include <utility>

// Procedural music intent publisher. This is a permanently boot-armed PRE
// role on AtlasDevFrameScheduler. It publishes one cue index at $04ef for the
// procedural music conductor, but only while that conductor marks its binding
// active in $04f2. The fixed game policy is:
//
//   danger  when an active entity slot has nonzero live HP
//   calm    inside a building ($0499 == 0)
//   explore everywhere else
//
// calm/explore/danger map those semantic states to the artist kit's cue
// indexes (0..15). hysteresis_frames is the number of consecutive eligible PRE
// samples required before a different state is published. Zero and one both
// publish on the first sample. The candidate byte uses bit 7 as its initialized
// marker; the remaining two bytes are a 16-bit sample counter.

namespace {
	void emit_restore_x_and_return(klib::Asm6502& code) {
		code.pla();
		code.tax();
		code.rts();
	}

	void emit_candidate_state(klib::Asm6502& code) {
		using namespace fh::ami;
		// X is the desired state.  The first store removes every publisher bit
		// while preserving the conductor lock; the second adds only low state
		// bits.  An observer can therefore never mistake a partial candidate
		// update for a ready publication.
		code.lda_abs(RAM_PUBLISH);
		code.and_imm(TXN_LOCK);
		code.sta_abs(RAM_PUBLISH);
		code.txa();
		code.ora_abs(RAM_PUBLISH);
		code.sta_abs(RAM_PUBLISH);
	}

	void emit_eor_abs(klib::Asm6502& code, word address) {
		code.db(0x4d); // EOR absolute
		code.dw(address);
	}

	void emit_publisher(klib::Asm6502& code, word hysteresis) {
		using namespace fh::ami;

		// A is the scheduler slot kind. Preserve the permanent-lane rule.
		code.cmp_imm(KIND);
		code.beq("@v2_kind_ok");
		code.rts();
		code.label("@v2_kind_ok");
		code.txa();
		code.pha();

		// The high bit of the active-node token is the sole lifecycle gate.
		// Inactive cleanup owns only publisher bits and never clears TXN_LOCK.
		code.lda_abs(RAM_ACTIVE_TOKEN);
		code.bmi("@v2_active");
		code.lda_abs(RAM_PUBLISH);
		code.and_imm(TXN_LOCK);
		code.sta_abs(RAM_PUBLISH);
		code.lda_imm(0x00);
		code.sta_abs(RAM_DWELL_LO);
		code.sta_abs(RAM_DWELL_HI);
		code.jmp("@v2_done");

		code.label("@v2_active");
		// A ready transaction is immutable until the conductor consumes it.
		code.lda_abs(RAM_PUBLISH);
		code.bpl("@v2_not_ready");
		emit_restore_x_and_return(code);
		code.label("@v2_not_ready");
		// Both conductor transactions and event publication freeze publisher
		// state byte-for-byte.  The event producer stores its F7 snapshot before
		// its EF command, so checking both F7 high bits closes that NMI window.
		code.lda_abs(RAM_PUBLISH);
		code.and_imm(TXN_LOCK);
		code.beq("@v2_no_lock");
		emit_restore_x_and_return(code);
		code.label("@v2_no_lock");
		code.lda_abs(RAM_LANDING);
		code.and_imm(LANDING_INTERLOCK_MASK);
		code.beq("@v2_no_landing_interlock");
		emit_restore_x_and_return(code);
		code.label("@v2_no_landing_interlock");
		// Canonicalize an unlocked, non-ready state before classification.  The
		// publisher never emits reserved bits even if F3 was externally damaged.
		code.lda_abs(RAM_PUBLISH);
		code.and_imm(static_cast<byte>(TXN_LOCK | STATE_MASK));
		code.sta_abs(RAM_PUBLISH);

		// Scan all eight entity slots using the living-score state policy.
		code.ldx_imm(0x07);
		code.label("@v2_entity");
		code.lda_abs_x(fh::RAM::EntitySlotActive);
		code.bmi("@v2_next_entity");
		code.lda_abs_x(fh::RAM::EntityHealth);
		code.bne("@v2_danger");
		code.label("@v2_next_entity");
		code.dex();
		code.bpl("@v2_entity");

		code.lda_abs(fh::RAM::AreaMode);
		code.beq("@v2_calm");
		code.lda_imm(STATE_EXPLORE);
		code.jmp("@v2_desired");
		code.label("@v2_calm");
		code.lda_imm(STATE_CALM);
		code.jmp("@v2_desired");
		code.label("@v2_danger");
		// Mantra/Death has no game-requested crisis state.  Its live danger fact
		// is deterministically clamped to explore; death itself is a conductor
		// event and never comes through this ordinary publisher.  The structural
		// crisis fallback nodes remain available to the validated score format.
		code.lda_abs(RAM_LANDING);
		code.and_imm(FAMILY_MASK);
		code.cmp_imm(MANTRA_FAMILY_BITS);
		code.bne("@v2_danger_allowed");
		code.lda_imm(STATE_EXPLORE);
		code.jmp("@v2_desired");
		code.label("@v2_danger_allowed");
		code.lda_imm(STATE_DANGER);

		code.label("@v2_desired");
		code.tax();
		// If the semantic state already matches the committed landing, clear
		// publisher history while preserving the transaction lock.
		code.txa();
		emit_eor_abs(code, RAM_LANDING);
		code.and_imm(STATE_MASK);
		code.bne("@v2_transition");
		emit_candidate_state(code);
		code.lda_imm(0x00);
		code.sta_abs(RAM_DWELL_LO);
		code.sta_abs(RAM_DWELL_HI);
		code.jmp("@v2_done");

		code.label("@v2_transition");
		// A changed candidate begins a new consecutive-sample run.  A zero
		// dwell also means no prior candidate, including state zero.
		code.txa();
		emit_eor_abs(code, RAM_PUBLISH);
		code.and_imm(STATE_MASK);
		code.bne("@v2_new_candidate");
		code.lda_abs(RAM_DWELL_LO);
		code.ora_abs(RAM_DWELL_HI);
		code.beq("@v2_new_candidate");

		if (hysteresis > 1) {
			// A thresholded candidate is saturated: retry publication without
			// incrementing until every conductor/loader gate becomes safe.
			code.lda_abs(RAM_DWELL_HI);
			code.cmp_imm(static_cast<byte>(hysteresis >> 8));
			code.bcc("@v2_increment");
			code.bne("@v2_publish_attempt");
			code.lda_abs(RAM_DWELL_LO);
			code.cmp_imm(static_cast<byte>(hysteresis));
			code.bcc("@v2_increment");
			code.jmp("@v2_publish_attempt");

			code.label("@v2_increment");
			code.inc_abs(RAM_DWELL_LO);
			code.bne("@v2_compare_incremented");
			code.inc_abs(RAM_DWELL_HI);
			code.label("@v2_compare_incremented");
			code.lda_abs(RAM_DWELL_HI);
			code.cmp_imm(static_cast<byte>(hysteresis >> 8));
			code.bcc("@v2_done");
			code.bne("@v2_publish_attempt");
			code.lda_abs(RAM_DWELL_LO);
			code.cmp_imm(static_cast<byte>(hysteresis));
			code.bcc("@v2_done");
			code.jmp("@v2_publish_attempt");
		}
		else {
			code.jmp("@v2_publish_attempt");
		}

		code.label("@v2_new_candidate");
		emit_candidate_state(code);
		code.lda_imm(0x01);
		code.sta_abs(RAM_DWELL_LO);
		code.lda_imm(0x00);
		code.sta_abs(RAM_DWELL_HI);
		if (hysteresis > 1)
			code.jmp("@v2_done");

		code.label("@v2_publish_attempt");
		// Fail closed against every producer/consumer interleaving.  A blocked
		// candidate and its saturated dwell remain latched for a later sample.
		code.lda_abs(RAM_REQUEST);
		code.and_imm(COMMAND_KIND_MASK);
		code.bne("@v2_done");
		code.lda_abs(RAM_STAGED_TOKEN);
		code.cmp_abs(RAM_ACTIVE_TOKEN);
		code.bne("@v2_done");
		code.lda_zp(static_cast<byte>(RAM_MUSIC_OWNER));
		code.beq("@v2_publish");
		code.bmi("@v2_publish");
		code.jmp("@v2_done");

		// READY is the only publication store.  The conductor later merges F7's
		// family with these low state bits and owns every write to EF/F7/F6.
		code.label("@v2_publish");
		code.lda_abs(RAM_PUBLISH);
		code.and_imm(static_cast<byte>(TXN_LOCK | STATE_MASK));
		code.ora_imm(PUBLISH_READY);
		code.sta_abs(RAM_PUBLISH);

		code.label("@v2_done");
		emit_restore_x_and_return(code);
	}
}

word fh::HackManager::install_AtlasDevMusicIntent(const fe::Config& p_config,
	std::vector<byte>& p_rom, word cpu_addr, const fh::GeneralHack& p_hack) const {
	using namespace fh::afs;
	using namespace fh::ami;
	const std::string region{ p_config.get_region() };
	if (!region.empty() && region != "us")
		throw std::runtime_error(
			"AtlasDevMusicIntent currently supports USA rev0 only");

	// owns the exact nine-byte block $04EF-$04F7.  Reject selected optional
	// records intersecting its last byte before any ROM write.
	bool on_screen_enter_selected{ false };
	bool set_entity_frame_selected{ false };
	for (const auto& [opcode, signature] : p_config.bmap("iscript_opcodes")) {
		(void)opcode;
		if (signature.find("Impl=AtlasDevOnScreenEnter") != std::string::npos)
			on_screen_enter_selected = true;
		if (signature.find("Impl=AtlasDevSetEntityFrame") != std::string::npos)
			set_entity_frame_selected = true;
	}
	if (on_screen_enter_selected) {
		constexpr char RAM_ID[]{ "hack_script_on_screen_enter_ram_addr" };
		const std::size_t other_begin{ p_config.constant_or(RAM_ID, RAM_REQUEST) };
		if (other_begin <= RAM_LANDING && other_begin + 6 >= RAM_REQUEST)
			throw std::runtime_error(
				"AtlasDevMusicIntent: RAM $04ef-$04f7 conflicts with AtlasDevOnScreenEnter");
	}
	if (set_entity_frame_selected) {
		constexpr char RAM_ID[]{ "hack_script_set_entity_frame_ram_addr" };
		const std::size_t other_begin{ p_config.constant_or(RAM_ID, RAM_STAGED_TOKEN) };
		if (other_begin <= RAM_LANDING && other_begin + 7 >= RAM_REQUEST)
			throw std::runtime_error(
				"AtlasDevMusicIntent: RAM $04ef-$04f7 conflicts with AtlasDevSetEntityFrame");
	}

	const word base{ find_base(p_rom) };
	if (base == 0)
		throw std::runtime_error(
			"AtlasDevMusicIntent requires the AtlasDevFrameScheduler hack installed first");

	const word hysteresis{ p_hack.word_or("hysteresis_frames", 30) };
	klib::Asm6502 code;
	emit_publisher(code, hysteresis);
	const std::size_t code_end{ static_cast<std::size_t>(cpu_addr) + code.size() };
	if (cpu_addr < 0xc000 || code_end > 0xffff)
		throw std::runtime_error(
			"AtlasDevMusicIntent: publisher must fit in the fixed bank");

	const auto code_offset{ klib::Asm6502::get_file_offset(15, cpu_addr) };
	if (code_offset > p_rom.size() || code.size() > p_rom.size() - code_offset)
		throw std::runtime_error("AtlasDevMusicIntent: ROM is too small for the publisher");
	for (std::size_t i{ 0 }; i < code.size(); ++i)
		if (p_rom[code_offset + i] != 0xff)
			throw std::runtime_error(std::format(
				"AtlasDevMusicIntent: bank 15 space at ${:04x} is not free", cpu_addr + i));

	const auto scheduler{ klib::Asm6502::get_file_offset(15, base) };
	const auto read_operand{ [&p_rom, scheduler](std::size_t site) {
		return static_cast<word>(p_rom[scheduler + site]
			| (p_rom[scheduler + site + 1] << 8));
	} };
	const word stub_target{ static_cast<word>(base + OFF_STUB) };
	constexpr std::size_t pre_sites[3]{ OFF_PRE0, OFF_PRE1, OFF_PRE2 };
	std::size_t slot{ 3 };
	for (std::size_t i{ 0 }; i < 3; ++i) {
		const byte owner{ p_rom[scheduler + OFF_ARM0 + i] };
		const word vector{ read_operand(pre_sites[i]) };
		if (owner == KIND)
			throw std::runtime_error(
				"AtlasDevMusicIntent: scheduler kind 6 is already claimed");
		if (slot == 3 && owner == 0x00 && vector == stub_target)
			slot = i;
	}
	if (slot == 3)
		throw std::runtime_error(
			"AtlasDevMusicIntent: scheduler PRE table has no unclaimed slot");

	code.apply_hack_and_clear(p_rom, 15, cpu_addr);
	p_rom[scheduler + pre_sites[slot]] = cpu_addr & 0xff;
	p_rom[scheduler + pre_sites[slot] + 1] = cpu_addr >> 8;
	p_rom[scheduler + OFF_ARM0 + slot] = KIND;
	return static_cast<word>(code_end);
}

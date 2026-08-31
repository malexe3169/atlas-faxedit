#include "common/klib/Asm6502.h"
#include "fe/Config.h"
#include "fh/AtlasDevFrameScheduler.h"
#include "fh/AtlasDevMusicIntent.h"
#include "fh/GeneralHack.h"
#include "fh/HackManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
	using namespace fh::ami;

	constexpr word SCHEDULER_ORG{ 0xfcce };
	constexpr word ROLE_ORG{ SCHEDULER_ORG + fh::afs::CORE_SIZE };
	constexpr std::size_t ROM_SIZE{ 0x40010 };
	constexpr std::array<byte, 5> HOOK1_ORIG{ 0xa9, 0x07, 0x8d, 0x14, 0x40 };
	constexpr std::array<byte, 5> HOOK2_ORIG{ 0x8d, 0x01, 0x20, 0xa5, 0x5a };
	constexpr word ENTITY_ACTIVE{ 0x02cc };
	constexpr word ENTITY_HEALTH{ 0x0344 };
	constexpr word AREA_MODE{ 0x0499 };
	constexpr byte OUTRO_FAMILY_INDEX{ 11 };

	static_assert(RAM_LANDING - RAM_REQUEST + 1 == 9,
		"procedural-music ABI must remain a nine-byte record");
	static_assert((PUBLISH_READY | TXN_LOCK | RESERVED_MASK | STATE_MASK) == 0xff,
		"publisher flag masks must partition $04F3");

	void require(bool condition, const std::string& message) {
		if (!condition)
			throw std::runtime_error(message);
	}

	std::vector<byte> vanilla_rom() {
		std::vector<byte> rom(ROM_SIZE, 0xff);
		rom[0] = 'N'; rom[1] = 'E'; rom[2] = 'S'; rom[3] = 0x1a;
		rom[4] = 0x10; rom[5] = 0x00; rom[6] = 0x10; rom[7] = 0x00;
		const auto hook1{ klib::Asm6502::get_file_offset(15, 0xc9af) };
		const auto hook2{ klib::Asm6502::get_file_offset(15, 0xc9de) };
		for (std::size_t i{ 0 }; i < HOOK1_ORIG.size(); ++i) {
			rom[hook1 + i] = HOOK1_ORIG[i];
			rom[hook2 + i] = HOOK2_ORIG[i];
		}
		return rom;
	}

	std::vector<fh::GeneralHack> hacks(const std::string& text) {
		return fh::filter_general_hacks(15, fh::parse_general_hacks(text));
	}

	void install_scheduler(std::vector<byte>& rom) {
		const auto used{ fh::HackManager{}.install_general_hacks(fe::Config{}, rom, 15,
			SCHEDULER_ORG, 0xfff0, hacks("AtlasDevFrameScheduler"), nullptr) };
		require(used == fh::afs::CORE_SIZE, "scheduler allocation size changed");
	}

	std::size_t install_role(std::vector<byte>& rom, const std::string& params = {},
		std::size_t end = 0xfff0, const fe::Config& config = fe::Config{}) {
		const auto spec{ params.empty()
			? std::string("AtlasDevMusicIntent")
			: std::string("AtlasDevMusicIntent ") + params };
		return fh::HackManager{}.install_general_hacks(config, rom, 15,
			ROLE_ORG, end, hacks(spec), nullptr);
	}

	std::size_t scheduler_offset(const std::vector<byte>& rom) {
		const word base{ fh::afs::find_base(rom) };
		require(base == SCHEDULER_ORG, "scheduler discovery or base changed");
		return klib::Asm6502::get_file_offset(15, base);
	}

	word read_word(const std::vector<byte>& bytes, std::size_t offset) {
		return static_cast<word>(bytes[offset] | (bytes[offset + 1] << 8));
	}

	word installed_handler(const std::vector<byte>& rom, std::size_t* slot_out = nullptr) {
		const auto scheduler{ scheduler_offset(rom) };
		constexpr std::array<std::size_t, 3> sites{
			fh::afs::OFF_PRE0, fh::afs::OFF_PRE1, fh::afs::OFF_PRE2
		};
		for (std::size_t i{ 0 }; i < sites.size(); ++i)
			if (rom[scheduler + fh::afs::OFF_ARM0 + i] == KIND) {
				if (slot_out)
					*slot_out = i;
				return read_word(rom, scheduler + sites[i]);
			}
		throw std::runtime_error("music intent role did not own an arm slot");
	}

	struct Tiny6502 {
		struct Cost {
			std::size_t cycles{};
			std::size_t instructions{};
			friend bool operator==(const Cost&, const Cost&) = default;
		};
		struct Write {
			std::size_t instruction{};
			word address{};
			byte before{};
			byte after{};
		};

		std::array<byte, 0x10000> mem{};
		word pc{};
		byte a{};
		byte x{};
		byte sp{ 0xfd };
		bool n{};
		bool z{};
		bool c{};
		bool conservative_branch_pages{};
		Cost cost{};
		std::vector<Write> writes;

		explicit Tiny6502(const std::vector<byte>& rom, word entry,
			bool worst_case_branch_pages = false)
			: pc{ entry }, conservative_branch_pages{ worst_case_branch_pages } {
			const auto bank15{ klib::Asm6502::get_file_offset(15, 0xc000) };
			for (std::size_t i{ 0 }; i < 0x4000; ++i)
				mem[0xc000 + i] = rom.at(bank15 + i);
			for (std::size_t i{ 0 }; i < 8; ++i)
				mem[ENTITY_ACTIVE + i] = 0xff;
		}

		word operand() {
			const word value{ static_cast<word>(mem[pc] | (mem[pc + 1] << 8)) };
			pc = static_cast<word>(pc + 2);
			return value;
		}

		void set_nz(byte value) {
			z = value == 0;
			n = (value & 0x80) != 0;
		}

		void compare(byte value) {
			const byte result{ static_cast<byte>(a - value) };
			c = a >= value;
			set_nz(result);
		}

		void branch(bool take) {
			const auto delta{ static_cast<std::int8_t>(mem[pc++]) };
			if (!take) {
				cost.cycles += 2;
				return;
			}
			const word from{ pc };
			pc = static_cast<word>(pc + delta);
			cost.cycles += 3 + (conservative_branch_pages
				|| (from & 0xff00) != (pc & 0xff00) ? 1 : 0);
		}

		void push(byte value) { mem[0x0100 + sp--] = value; }
		byte pull() { return mem[0x0100 + ++sp]; }

		void write(word address, byte value) {
			writes.push_back(Write{ cost.instructions, address, mem[address], value });
			mem[address] = value;
		}

		Cost run(byte kind = KIND, byte initial_x = 0x5a) {
			a = kind;
			x = initial_x;
			cost = {};
			writes.clear();
			const byte initial_sp{ sp };
			std::size_t call_depth{ 1 };
			for (std::size_t steps{ 0 }; steps < 3000; ++steps) {
				const byte op{ mem[pc++] };
				++cost.instructions;
				switch (op) {
				case 0x05: a = static_cast<byte>(a | mem[mem[pc++]]); set_nz(a); cost.cycles += 3; break;
				case 0x09: a = static_cast<byte>(a | mem[pc++]); set_nz(a); cost.cycles += 2; break;
				case 0x0d: { const word at{ operand() }; a = static_cast<byte>(a | mem[at]); set_nz(a); cost.cycles += 4; break; }
				case 0x10: branch(!n); break;
				case 0x20: {
					const word target{ operand() };
					const word ret{ static_cast<word>(pc - 1) };
					push(static_cast<byte>(ret >> 8)); push(static_cast<byte>(ret));
					pc = target; ++call_depth; cost.cycles += 6; break;
				}
				case 0x29: a = static_cast<byte>(a & mem[pc++]); set_nz(a); cost.cycles += 2; break;
				case 0x30: branch(n); break;
				case 0x45: a = static_cast<byte>(a ^ mem[mem[pc++]]); set_nz(a); cost.cycles += 3; break;
				case 0x48: push(a); cost.cycles += 3; break;
				case 0x4c: pc = operand(); cost.cycles += 3; break;
				case 0x4d: { const word at{ operand() }; a = static_cast<byte>(a ^ mem[at]); set_nz(a); cost.cycles += 4; break; }
				case 0x60: {
					cost.cycles += 6;
					if (--call_depth == 0) {
						require(sp == initial_sp, "publisher returned with unbalanced stack");
						return cost;
					}
					const word lo{ pull() }; const word hi{ pull() };
					pc = static_cast<word>(((hi << 8) | lo) + 1); break;
				}
				case 0x68: a = pull(); set_nz(a); cost.cycles += 4; break;
				case 0x8a: a = x; set_nz(a); cost.cycles += 2; break;
				case 0x8d: { const word at{ operand() }; write(at, a); cost.cycles += 4; break; }
				case 0x90: branch(!c); break;
				case 0xa2: x = mem[pc++]; set_nz(x); cost.cycles += 2; break;
				case 0xa5: a = mem[mem[pc++]]; set_nz(a); cost.cycles += 3; break;
				case 0xa9: a = mem[pc++]; set_nz(a); cost.cycles += 2; break;
				case 0xaa: x = a; set_nz(x); cost.cycles += 2; break;
				case 0xad: { const word at{ operand() }; a = mem[at]; set_nz(a); cost.cycles += 4; break; }
				case 0xbd: {
					const word at{ operand() };
					const word indexed{ static_cast<word>(at + x) };
					a = mem[indexed]; set_nz(a);
					cost.cycles += 4 + ((at & 0xff00) != (indexed & 0xff00) ? 1 : 0);
					break;
				}
				case 0xc9: compare(mem[pc++]); cost.cycles += 2; break;
				case 0xca: x = static_cast<byte>(x - 1); set_nz(x); cost.cycles += 2; break;
				case 0xcd: { const word at{ operand() }; compare(mem[at]); cost.cycles += 4; break; }
				case 0xd0: branch(!z); break;
				case 0xea: cost.cycles += 2; break;
				case 0xee: {
					const word at{ operand() };
					write(at, static_cast<byte>(mem[at] + 1)); set_nz(mem[at]);
					cost.cycles += 6; break;
				}
				case 0xf0: branch(z); break;
				default:
					throw std::runtime_error("publisher emitted unsupported test opcode "
						+ std::to_string(op));
				}
			}
			throw std::runtime_error("publisher did not return within 3000 instructions");
		}
	};

	byte packed(byte family, byte state) {
		return static_cast<byte>((family << 2) | state);
	}

	void require_no_request_store_opcode(const std::vector<byte>& rom,
		word handler, std::size_t size) {
		const auto base{ klib::Asm6502::get_file_offset(15, handler) };
		for (std::size_t i{ 0 }; i < size;) {
			const byte op{ rom.at(base + i) };
			std::size_t length{};
			switch (op) {
			case 0x48: case 0x60: case 0x68: case 0x8a:
			case 0xaa: case 0xca: case 0xea:
				length = 1; break;
			case 0x05: case 0x09: case 0x10: case 0x29: case 0x30:
			case 0x45: case 0x90: case 0xa2: case 0xa5: case 0xa9:
			case 0xc9: case 0xd0: case 0xf0:
				length = 2; break;
			case 0x0d: case 0x20: case 0x4c: case 0x4d: case 0x8d:
			case 0xad: case 0xbd: case 0xcd: case 0xee:
				length = 3; break;
			default:
				throw std::runtime_error(
					"publisher contains undecodable opcode in ownership proof");
			}
			if (op == 0x8d || op == 0xee) {
				const word target{ static_cast<word>(rom.at(base + i + 1)
					| (rom.at(base + i + 2) << 8)) };
				require(target != RAM_REQUEST,
					"emitted publisher contains a write opcode targeting $04EF");
			}
			i += length;
		}
	}

	void classify(Tiny6502& cpu, byte state, int danger_slot = 0) {
		for (std::size_t i{ 0 }; i < 8; ++i) {
			cpu.mem[ENTITY_ACTIVE + i] = 0xff;
			cpu.mem[ENTITY_HEALTH + i] = 0;
		}
		cpu.mem[AREA_MODE] = state == STATE_CALM ? 0 : 4;
		if (state == STATE_DANGER) {
			cpu.mem[ENTITY_ACTIVE + static_cast<std::size_t>(danger_slot)] = 0;
			cpu.mem[ENTITY_HEALTH + static_cast<std::size_t>(danger_slot)] = 1;
		}
	}

	void configure(Tiny6502& cpu, byte family, byte current_state,
		byte desired_state, int danger_slot = 0) {
		const byte token{ static_cast<byte>(0x80 | family) };
		cpu.mem[RAM_ACTIVE_TOKEN] = token;
		cpu.mem[RAM_STAGED_TOKEN] = token;
		cpu.mem[RAM_LANDING] = packed(family, current_state);
		cpu.mem[RAM_REQUEST] = packed(family, current_state);
		cpu.mem[RAM_PUBLISH] = current_state;
		cpu.mem[RAM_DWELL_LO] = 0;
		cpu.mem[RAM_DWELL_HI] = 0;
		cpu.mem[RAM_MUSIC_OWNER] = 0;
		classify(cpu, desired_state, danger_slot);
	}

	void rerun(Tiny6502& cpu, word handler) {
		cpu.pc = handler;
		cpu.run();
	}

	void test_install_and_hysteresis() {
		auto rom{ vanilla_rom() };
		install_scheduler(rom);
		const auto size{ install_role(rom, "hysteresis_frames=3") };
		std::size_t slot{};
		const word handler{ installed_handler(rom, &slot) };
		require(size > 0 && ROLE_ORG + size < 0xfff0 && handler == ROLE_ORG && slot == 0,
			"publisher did not cursor-allocate and claim the first PRE slot");
		require_no_request_store_opcode(rom, handler, size);

		Tiny6502 cpu(rom, handler);
		configure(cpu, 6, STATE_CALM, STATE_EXPLORE);
		cpu.run();
		require(cpu.x == 0x5a && cpu.mem[RAM_REQUEST] == packed(6, STATE_CALM)
			&& cpu.mem[RAM_PUBLISH] == STATE_EXPLORE
			&& cpu.mem[RAM_DWELL_LO] == 1 && cpu.mem[RAM_DWELL_HI] == 0,
			"first explore sample did not start hysteresis");
		rerun(cpu, handler);
		require(cpu.mem[RAM_REQUEST] == packed(6, STATE_CALM)
			&& cpu.mem[RAM_DWELL_LO] == 2,
			"second explore sample published early");
		rerun(cpu, handler);
		require(cpu.mem[RAM_REQUEST] == packed(6, STATE_CALM)
			&& cpu.mem[RAM_PUBLISH] == (PUBLISH_READY | STATE_EXPLORE)
			&& cpu.mem[RAM_DWELL_LO] == 3,
			"third explore sample did not publish a saturated transaction");
		const auto ready_store{ std::ranges::find_if(cpu.writes,
			[](const auto& write) {
				return write.address == RAM_PUBLISH
					&& (write.after & PUBLISH_READY) != 0;
			}) };
		require(ready_store != cpu.writes.end()
			&& ready_store == std::prev(cpu.writes.end()),
			"READY publication was not the final publisher store");
		for (const auto& write : cpu.writes)
			require(write.address != RAM_REQUEST,
				"executed publication wrote conductor-owned $04EF");

		std::size_t reserved_cases{};
		for (byte family{ 0 }; family < 16; ++family)
			for (byte pattern{ 0 }; pattern < 16; ++pattern) {
				Tiny6502 reserved(rom, handler);
				configure(reserved, family, STATE_CALM, STATE_EXPLORE);
				reserved.mem[RAM_PUBLISH] = static_cast<byte>(
					(pattern << 2) | STATE_EXPLORE);
				reserved.mem[RAM_DWELL_LO] = 3;
				const byte request_before{ reserved.mem[RAM_REQUEST] };
				reserved.run();
				require(reserved.mem[RAM_REQUEST] == request_before
					&& reserved.mem[RAM_LANDING] == packed(family, STATE_CALM)
					&& reserved.mem[RAM_PUBLISH]
					== (PUBLISH_READY | STATE_EXPLORE),
					"reserved candidate bits changed conductor family/request state");
				for (const auto& write : reserved.writes)
					require(write.address != RAM_REQUEST,
						"reserved-bit mutation path wrote conductor-owned $04EF");
				++reserved_cases;
			}
		require(reserved_cases == 16 * 16,
			"reserved candidate mutation case count drifted");
		std::cout << "reserved-bit ownership witness: cases="
			<< reserved_cases << '\n';

		// A ready candidate is immutable even when the game facts and conductor
		// obligation change underneath it.
		cpu.mem[RAM_LANDING] |= LANDING_OBLIGATION;
		classify(cpu, STATE_DANGER);
		const auto before_publish{ cpu.mem[RAM_PUBLISH] };
		const auto before_lo{ cpu.mem[RAM_DWELL_LO] };
		const auto before_hi{ cpu.mem[RAM_DWELL_HI] };
		const auto before_request{ cpu.mem[RAM_REQUEST] };
		rerun(cpu, handler);
		require(cpu.mem[RAM_PUBLISH] == before_publish
			&& cpu.mem[RAM_DWELL_LO] == before_lo
			&& cpu.mem[RAM_DWELL_HI] == before_hi
			&& cpu.mem[RAM_REQUEST] == before_request,
			"ready transaction did not persist through an obligation");

		std::cout << "publisher artifact: bytes=" << size
			<< ", cpu=$" << std::hex << ROLE_ORG << "-$"
			<< (ROLE_ORG + size - 1) << std::dec << '\n';
	}

	void test_mantra_never_publishes_danger() {
		auto rom{ vanilla_rom() };
		install_scheduler(rom);
		install_role(rom, "hysteresis_frames=1");
		const word handler{ installed_handler(rom) };
		std::size_t cases{};
		for (int slot{ 0 }; slot < 8; ++slot)
			for (byte current{ 0 }; current < 4; ++current)
				for (const byte landing_interlock : { byte{ 0 }, LANDING_STAGED_EVENT,
					LANDING_OBLIGATION,
					static_cast<byte>(LANDING_STAGED_EVENT | LANDING_OBLIGATION) })
					for (const byte lock : { byte{ 0 }, TXN_LOCK }) {
						Tiny6502 cpu(rom, handler);
						configure(cpu, MANTRA_FAMILY_INDEX, current, STATE_DANGER, slot);
						cpu.mem[RAM_LANDING] |= landing_interlock;
						cpu.mem[RAM_PUBLISH] = lock | current;
						cpu.run();
						if (landing_interlock == 0 && lock == 0)
							require((cpu.mem[RAM_PUBLISH] & STATE_MASK) != STATE_DANGER,
								"Mantra danger leaked into publisher candidate state");
						for (const auto& write : cpu.writes)
							require(write.address != RAM_REQUEST,
								"Mantra publisher path wrote conductor-owned EF");
						++cases;
					}
		require(cases == 8 * 4 * 4 * 2,
			"Mantra clamp matrix case count drifted");
		std::cout << "Mantra danger clamp witness: cases=" << cases << '\n';
	}

	void test_inactive_and_ready_ownership() {
		auto rom{ vanilla_rom() };
		install_scheduler(rom);
		install_role(rom);
		const word handler{ installed_handler(rom) };
		for (unsigned int token{ 0 }; token < 0x80; ++token)
			for (const byte lock : { byte{ 0 }, TXN_LOCK })
				for (const byte ready : { byte{ 0 }, PUBLISH_READY })
					for (byte state{ 0 }; state < 4; ++state) {
						Tiny6502 cpu(rom, handler);
						configure(cpu, 3, STATE_CALM, STATE_DANGER);
						cpu.mem[RAM_ACTIVE_TOKEN] = static_cast<byte>(token);
						cpu.mem[RAM_PUBLISH] = lock | ready | state;
						cpu.mem[RAM_DWELL_LO] = 0x34;
						cpu.mem[RAM_DWELL_HI] = 0x12;
						const byte request_before{ cpu.mem[RAM_REQUEST] };
						const byte staged_before{ cpu.mem[RAM_STAGED_TOKEN] };
						const byte landing_before{ cpu.mem[RAM_LANDING] };
						cpu.run();
						require(cpu.mem[RAM_PUBLISH] == lock
							&& cpu.mem[RAM_DWELL_LO] == 0
							&& cpu.mem[RAM_DWELL_HI] == 0
							&& cpu.mem[RAM_REQUEST] == request_before
							&& cpu.mem[RAM_STAGED_TOKEN] == staged_before
							&& cpu.mem[RAM_LANDING] == landing_before,
							"inactive cleanup escaped publisher ownership");
					}

		Tiny6502 ready(rom, handler);
		configure(ready, 4, STATE_CALM, STATE_DANGER);
		ready.mem[RAM_PUBLISH] = PUBLISH_READY | TXN_LOCK | STATE_EXPLORE;
		ready.mem[RAM_DWELL_LO] = 0x34;
		ready.mem[RAM_DWELL_HI] = 0x12;
		const auto before{ ready.mem };
		ready.run();
		require(ready.mem[RAM_PUBLISH] == before[RAM_PUBLISH]
			&& ready.mem[RAM_DWELL_LO] == before[RAM_DWELL_LO]
			&& ready.mem[RAM_DWELL_HI] == before[RAM_DWELL_HI]
			&& ready.mem[RAM_REQUEST] == before[RAM_REQUEST],
			"active ready publication was not immutable");
	}

	fe::Config test_config(const std::string& region, word on_screen_ram,
		bool select_on_screen, word set_entity_frame_ram,
		bool select_set_entity_frame) {
		static std::uint64_t sequence{};
		const auto nonce{ static_cast<std::uint64_t>(
			std::chrono::steady_clock::now().time_since_epoch().count()) ^ ++sequence };
		const auto path{ std::filesystem::temp_directory_path()
			/ ("atlas-music-intent-config-" + std::to_string(nonce) + ".xml") };
		{
			std::ofstream out(path);
			require(static_cast<bool>(out), "could not create test config");
			out << "<eoe_config>\n"
				"  <regions><region name=\"us\"/><region name=\"eu\"/></regions>\n"
				"  <consts>\n"
				"    <const name=\"hack_script_on_screen_enter_ram_addr\" value=\"$"
				<< std::hex << on_screen_ram << "\"/>\n"
				"    <const name=\"hack_script_set_entity_frame_ram_addr\" value=\"$"
				<< set_entity_frame_ram << "\"/>\n"
				"  </consts>\n"
				"  <byte_to_string_maps><byte_to_string_map name=\"iscript_opcodes\">\n";
			if (select_on_screen)
				out << "    <entry byte=\"$18\" str=\"Impl=AtlasDevOnScreenEnter\"/>\n";
			if (select_set_entity_frame)
				out << "    <entry byte=\"$19\" str=\"Impl=AtlasDevSetEntityFrame\"/>\n";
			out << "  </byte_to_string_map></byte_to_string_maps>\n</eoe_config>\n";
		}
		try {
			fe::Config result(path.string(), {}, std::vector<byte>(ROM_SIZE, 0xff), region);
			std::filesystem::remove(path);
			return result;
		}
		catch (...) {
			std::filesystem::remove(path);
			throw;
		}
	}


	void test_fail_closed_install() {
		require(hacks("AtlasDevMusicIntent hysteresis_frames=30").size() == 1,
			"general-hack name or parameter was not accepted");
		for (const std::string invalid : {
			"AtlasDevMusicIntent calm=0",
			"AtlasDevMusicIntent explore=1",
			"AtlasDevMusicIntent danger=2",
			"AtlasDevMusicIntent hysteresis=30" }) {
			bool threw{};
			try { (void)hacks(invalid); }
			catch (const std::runtime_error&) { threw = true; }
			require(threw, "accepted a removed/unknown parameter: " + invalid);
		}

		const auto require_refusal{ [](const std::string& expected,
			auto prepare, std::size_t end = 0xfff0) {
			auto rom{ vanilla_rom() };
			install_scheduler(rom);
			prepare(rom);
			const auto before{ rom };
			bool threw{};
			std::string message;
			try { install_role(rom, {}, end); }
			catch (const std::runtime_error& error) {
				threw = true; message = error.what();
			}
			require(threw && message.find(expected) != std::string::npos,
				"refusal reported wrong result: " + message);
			require(rom == before, "rejected install mutated ROM");
		} };
		require_refusal("kind 6 is already claimed", [](auto& rom) {
			const auto off{ scheduler_offset(rom) };
			rom[off + fh::afs::OFF_ARM0] = KIND;
		});
		require_refusal("is not free", [](auto& rom) {
			rom[klib::Asm6502::get_file_offset(15, ROLE_ORG)] = 0;
		});
		require_refusal("overflow", [](auto&) {}, ROLE_ORG + 1);

		for (const auto& config : {
			test_config("us", RAM_LANDING, true, 0x0600, false),
			test_config("us", 0x0600, false, RAM_LANDING, true),
			test_config("eu", 0x0600, false, 0x0600, false) }) {
			auto rom{ vanilla_rom() };
			install_scheduler(rom);
			const auto before{ rom };
			bool threw{};
			try { install_role(rom, {}, 0xfff0, config); }
			catch (const std::runtime_error&) { threw = true; }
			require(threw && rom == before,
				"region/RAM-through-F7 refusal was not atomic");
		}

		for (const auto& config : {
			test_config("us", 0x04f8, true, 0x0600, false),
			test_config("us", 0x0600, false, 0x04f8, true) }) {
			auto rom{ vanilla_rom() };
			install_scheduler(rom);
			require(install_role(rom, {}, 0xfff0, config) > 0,
				"rejected an adjacent non-overlapping RAM record");
		}
	}

	void write_file(const std::filesystem::path& path, const std::vector<byte>& bytes) {
		std::ofstream output(path, std::ios::binary);
		require(static_cast<bool>(output), "could not create CLI fixture");
		output.write(reinterpret_cast<const char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
		require(static_cast<bool>(output), "could not write CLI fixture");
	}

	std::vector<byte> read_file(const std::filesystem::path& path) {
		std::ifstream input(path, std::ios::binary);
		require(static_cast<bool>(input), "could not read CLI output");
		return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
	}

	std::string shell_quote(const std::filesystem::path& path) {
		const std::string value{ path.string() };
#ifdef _WIN32
		return '"' + value + '"';
#else
		std::string result{ "'" };
		for (const char c : value)
			result += c == '\'' ? "'\\''" : std::string(1, c);
		return result + "'";
#endif
	}

	int run_cli(const std::filesystem::path& executable,
		const std::vector<std::string>& arguments) {
		std::string command{ shell_quote(executable) };
		for (const auto& argument : arguments)
			command += " " + shell_quote(argument);
		return std::system(command.c_str());
	}

	struct TemporaryDirectory {
		std::filesystem::path path;
		~TemporaryDirectory() {
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}
	};

	void verify_cli(const std::filesystem::path& executable) {
		require(std::filesystem::is_regular_file(executable), "CLI executable missing");
		const auto nonce{ static_cast<std::uint64_t>(
			std::chrono::steady_clock::now().time_since_epoch().count()) };
		TemporaryDirectory temporary{ std::filesystem::temp_directory_path()
			/ ("faxedit-pmusic-cli-" + std::to_string(nonce)) };
		std::filesystem::create_directories(temporary.path);
		const auto input{ temporary.path / "conductor.nes" };
		const auto output{ temporary.path / "publisher.nes" };
		const auto report{ temporary.path / "publisher.json" };
		const auto rejected{ temporary.path / "rejected.nes" };
		auto original{ vanilla_rom() };
		original[0x14010 + 0x210] = 0x20;
		original[0x14010 + 0x211] = 0xde;
		original[0x14010 + 0xf00] = 0xa5;
		write_file(input, original);
		const std::vector<byte> sentinel{ 0xde, 0xad, 0xbe, 0xef };
		write_file(rejected, sentinel);

		require(run_cli(executable, {
			"pmi", input.string(), output.string(), "--region", "us",
			"--ram-base", "0x04ef", "--hysteresis", "257",
			"--json", report.string()
		}) == 0, "dedicated CLI returned failure");
		const auto installed{ read_file(output) };
		require(installed.size() == original.size()
			&& std::equal(original.begin(), original.begin() + 0x3c010, installed.begin()),
			"CLI changed ROM size or conductor-owned bank 5");
		const auto report_bytes{ read_file(report) };
		const std::string report_text(report_bytes.begin(), report_bytes.end());
		require(report_text.find("faxedit-music-intent-install") != std::string::npos
			&& report_text.find("\"end_exclusive\": \"0x04f8\"") != std::string::npos
			&& report_text.find("\"hysteresis_frames\": 257") != std::string::npos
			&& report_text.find("\"publisher_mask\": \"0x83\"") != std::string::npos
			&& report_text.find("\"conductor_mask\": \"0x40\"") != std::string::npos
			&& report_text.find("\"publisher_writes_request_0x04ef\": false")
				!= std::string::npos
			&& report_text.find("publish_ready_0x04f3_last") != std::string::npos
			&& report_text.find("atlasdev-music-intent-nmi-timing") != std::string::npos,
			"CLI report omitted ABI, config, or timing contract");

		require(run_cli(executable, {
			"pmi", input.string(), rejected.string(), "--region", "us", "--calm", "0"
		}) != 0 && read_file(rejected) == sentinel,
			"CLI accepted a removed cue parameter or replaced rejected output");
		require(run_cli(executable, {
			"pmi", input.string(), rejected.string(), "--region", "us", "--ram-base", "0x04f0"
		}) != 0 && read_file(rejected) == sentinel,
			"CLI accepted a mismatched RAM base or replaced rejected output");
	}
}

int main(int argc, char** argv) {
	try {
		test_install_and_hysteresis();
		test_mantra_never_publishes_danger();
		test_inactive_and_ready_ownership();
		test_fail_closed_install();
		if (argc == 3 && std::string(argv[1]) == "--cli")
			verify_cli(argv[2]);
		else if (argc != 1)
			throw std::runtime_error(
				"usage: atlas_music_intent_regression [--cli eoe-cli]");
		std::cout << "atlas music intent regressions: ok\n";
		return 0;
	}
	catch (const std::exception& error) {
		std::cerr << "atlas music intent regressions: " << error.what() << '\n';
		return 1;
	}
}

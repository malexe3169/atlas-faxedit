#include "common/klib/Asm6502.h"
#include "fe/Config.h"
#include "fh/AtlasDevMusicIntent.h"
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

	constexpr std::size_t ROM_SIZE{ 0x40010 };
	constexpr byte BASE_OPCODE_COUNT{ 0x18 };
	constexpr word HANDLER_ORG{ 0xa000 };
	constexpr word OLD_TABLE_LO{ 0x9000 };
	constexpr word OLD_TABLE_HI{ OLD_TABLE_LO + BASE_OPCODE_COUNT };
	constexpr word JUMP_REF_HI{ 0x8273 };
	constexpr word JUMP_REF_LO{ 0x8277 };
	constexpr std::array<byte, 27> EXPECTED_HANDLER{
		0x20, 0xa4, 0x87,       // JSR $87a4 (LoadByte)
		0xc9, 0x20,             // CMP #$20
		0xb0, 0x11,             // BCS done
		0x18,                   // CLC
		0x69, 0x01,             // ADC #1
		0x48,                   // PHA
		0xad, 0xdf, 0x04,       // LDA $04df
		0xd0, 0x07,             // BNE busy
		0x68,                   // PLA
		0x8d, 0xdf, 0x04,       // STA $04df -- only publication
		0x4c, 0x6e, 0x82,       // JMP $826e
		0x68,                   // busy: PLA
		0x4c, 0x6e, 0x82,       // done: JMP $826e
	};

	void require(bool condition, const std::string& message) {
		if (!condition)
			throw std::runtime_error(message);
	}

	void write_word(std::vector<byte>& rom, word cpu, word value) {
		const auto offset{ klib::Asm6502::get_file_offset(12, cpu) };
		rom.at(offset) = static_cast<byte>(value & 0xff);
		rom.at(offset + 1) = static_cast<byte>(value >> 8);
	}

	std::vector<byte> synthetic_rom() {
		std::vector<byte> rom(ROM_SIZE, 0xff);
		rom[0] = 'N'; rom[1] = 'E'; rom[2] = 'S'; rom[3] = 0x1a;
		rom[4] = 0x10; rom[5] = 0x00; rom[6] = 0x10; rom[7] = 0x00;
		write_word(rom, JUMP_REF_HI, OLD_TABLE_HI);
		write_word(rom, JUMP_REF_LO, OLD_TABLE_LO);
		for (byte opcode{ 0 }; opcode < BASE_OPCODE_COUNT; ++opcode) {
			const word target{ static_cast<word>(0x8300 + opcode * 4) };
			rom.at(klib::Asm6502::get_file_offset(12,
				static_cast<word>(OLD_TABLE_LO + opcode))) = static_cast<byte>(target);
			rom.at(klib::Asm6502::get_file_offset(12,
				static_cast<word>(OLD_TABLE_HI + opcode))) = static_cast<byte>(target >> 8);
		}
		return rom;
	}

	void test_native_emission() {
		auto rom{ synthetic_rom() };
		const fe::Config config(EOE_CONFIG_SOURCE, "", rom, "us");
		const auto signature{ config.str_map("iscript_opcode_impls").at(
			"AtlasDevTriggerMusicEvent") };
		require(signature == "Args=Byte", "typed opcode signature changed");

		const auto start{ klib::Asm6502::get_file_offset(12, HANDLER_ORG) };
		fh::HackManager{}.apply_script_library(config, rom, start,
			{ fh::HackLib::AtlasDevTriggerMusicEvent }, BASE_OPCODE_COUNT);
		const word table{ klib::Asm6502::read_word(rom, 12, JUMP_REF_LO) };
		const auto end{ klib::Asm6502::get_file_offset(12, table) };
		const std::vector<byte> actual(rom.begin() + start, rom.begin() + end);
		require(actual.size() == EXPECTED_HANDLER.size()
			&& std::equal(actual.begin(), actual.end(), EXPECTED_HANDLER.begin()),
			"native event handler bytes changed");

		const std::array<byte, 3> mailbox_store{ 0x8d, 0xdf, 0x04 };
		const std::array<byte, 3> request_store{ 0x8d, 0xef, 0x04 };
		const std::array<byte, 3> landing_store{ 0x8d, 0xf7, 0x04 };
		require(std::search(actual.begin(), actual.end(), mailbox_store.begin(),
			mailbox_store.end()) != actual.end(), "handler omitted mailbox store");
		require(std::search(actual.begin(), actual.end(), request_store.begin(),
			request_store.end()) == actual.end(), "handler wrote conductor request");
		require(std::search(actual.begin(), actual.end(), landing_store.begin(),
			landing_store.end()) == actual.end(), "handler wrote conductor landing");
	}

	byte enqueue(byte event_index, byte mailbox) {
		if (event_index > MAX_EVENT_INDEX || mailbox != EVENT_MAILBOX_EMPTY)
			return mailbox;
		return static_cast<byte>(event_index + 1);
	}

	void test_exhaustive_contract() {
		static_assert(RAM_EVENT_MAILBOX == 0x04df);
		static_assert(EVENT_MAILBOX_EMPTY == 0);
		static_assert(MAX_EVENT_INDEX == 31);
		for (unsigned event{ 0 }; event <= 0xff; ++event)
			for (unsigned mailbox{ 0 }; mailbox <= 0xff; ++mailbox) {
				const byte result{ enqueue(static_cast<byte>(event),
					static_cast<byte>(mailbox)) };
				const byte expected{ event < 32 && mailbox == 0
					? static_cast<byte>(event + 1)
					: static_cast<byte>(mailbox) };
				require(result == expected, "queue model diverged");
			}

		// Exhaust every NMI boundary around the producer's read/branch/store.
		// The consumer first copies then clears.  A consumed old value may win,
		// or the new value may remain queued; no torn/out-of-range value appears.
		for (byte event{ 0 }; event < 32; ++event)
			for (byte initial{ 0 }; initial <= 32; ++initial)
				for (unsigned boundary{ 0 }; boundary < 4; ++boundary) {
					byte mailbox{ initial };
					byte consumed{};
					auto nmi = [&] {
						consumed = mailbox;
						mailbox = EVENT_MAILBOX_EMPTY;
					};
					if (boundary == 0) nmi();
					const bool saw_empty{ mailbox == EVENT_MAILBOX_EMPTY };
					if (boundary == 1) nmi();
					if (saw_empty) {
						if (boundary == 2) nmi();
						mailbox = static_cast<byte>(event + 1);
					}
					if (boundary == 3) nmi();
					require(mailbox <= 32 && consumed <= 32,
						"NMI interleaving created an invalid queue value");
					if (initial == 0)
						require(mailbox == event + 1 || consumed == event + 1,
							"empty queue lost the new event");
				}
	}

	void write_file(const std::filesystem::path& path,
		const std::vector<byte>& bytes) {
		std::ofstream output(path, std::ios::binary);
		require(static_cast<bool>(output), "could not create event CLI ROM fixture");
		output.write(reinterpret_cast<const char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
		require(static_cast<bool>(output), "could not write event CLI ROM fixture");
	}

	void write_text_file(const std::filesystem::path& path,
		const std::string& text) {
		std::ofstream output(path);
		require(static_cast<bool>(output), "could not create event CLI text fixture");
		output << text;
		require(static_cast<bool>(output), "could not write event CLI text fixture");
	}

	std::vector<byte> read_file(const std::filesystem::path& path) {
		std::ifstream input(path, std::ios::binary);
		require(static_cast<bool>(input), "could not read event CLI output");
		return { std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>() };
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
		const std::vector<std::string>& arguments,
		const std::filesystem::path& working_directory) {
		std::string command;
#ifdef _WIN32
		command = "cd /d " + shell_quote(working_directory) + " && ";
#else
		command = "cd " + shell_quote(working_directory) + " && ";
#endif
		command += shell_quote(executable);
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

	std::string generated_script_fragment() {
		std::string result{
			"[defines]\n"
			"define GENERIC 0\n"
			"define MUSIC_EVENT_DEATH 0\n"
			"define MUSIC_EVENT_BOSS_EXIT 2\n"
			"[reserved_strings]\n"
			"[shops]\n"
			"[iscript]\n"
		};
		for (unsigned entrypoint{ 0 }; entrypoint < 152; ++entrypoint) {
			result += ".entrypoint " + std::to_string(entrypoint) + "\n";
			result += ".textbox GENERIC\n";
			if (entrypoint == 0)
				result += "TriggerMusicEvent MUSIC_EVENT_DEATH\n";
			result += "End\n";
		}
		return result;
	}

	void verify_cli(const std::filesystem::path& executable) {
		require(std::filesystem::is_regular_file(executable),
			"event CLI executable missing");
		const auto nonce{ static_cast<std::uint64_t>(
			std::chrono::steady_clock::now().time_since_epoch().count()) };
		TemporaryDirectory temporary{ std::filesystem::temp_directory_path()
			/ ("faxedit-pmusic-event-cli-" + std::to_string(nonce)) };
		std::filesystem::create_directories(temporary.path);

		const auto source{ temporary.path / "source.nes" };
		const auto script{ temporary.path / "events.asm" };
		const auto output{ temporary.path / "output.nes" };
		write_file(source, synthetic_rom());
		write_text_file(script, generated_script_fragment());
		write_text_file(temporary.path / "eoe_config_override.xml",
			"<eoe_config>\n"
			"  <byte_to_string_maps>\n"
			"    <byte_to_string_map name=\"iscript_opcodes\">\n"
			"      <entry byte=\"$18\" str=\"Mnemonic=TriggerMusicEvent,Impl=AtlasDevTriggerMusicEvent\" />\n"
			"    </byte_to_string_map>\n"
			"  </byte_to_string_maps>\n"
			"</eoe_config>\n");

		require(run_cli(executable, {
			"b", script.string(), output.string(),
			"-s", source.string(), "-r", "us"
		}, temporary.path) == 0,
			"eoe-cli rejected the generated event defines/config fragment");
		const auto installed{ read_file(output) };
		require(installed.size() == ROM_SIZE,
			"event CLI changed the synthetic ROM size");
		require(std::equal(EXPECTED_HANDLER.begin(), EXPECTED_HANDLER.end(),
			installed.begin() + 0x32d9b),
			"event CLI did not emit the expected native handler");

		constexpr std::size_t PTR_LO{ 0x31f7b };
		constexpr std::size_t ENTRYPOINTS{ 152 };
		const word entrypoint{ static_cast<word>(installed.at(PTR_LO)
			| (static_cast<word>(installed.at(PTR_LO + ENTRYPOINTS)) << 8)) };
		const auto call{ klib::Asm6502::get_file_offset(12, entrypoint) + 1 };
		require(installed.at(call) == BASE_OPCODE_COUNT
			&& installed.at(call + 1) == 0,
			"generated symbolic event call did not compile to opcode $18/index 0");
	}
}

int main(int argc, char** argv) {
	try {
		test_native_emission();
		test_exhaustive_contract();
		if (argc == 3 && std::string(argv[1]) == "--cli")
			verify_cli(argv[2]);
		else if (argc != 1)
			throw std::runtime_error(
				"usage: atlas_music_event_regression [--cli eoe-cli]");
		std::cout << "atlas music event regressions: ok\n";
		return 0;
	}
	catch (const std::exception& error) {
		std::cerr << "atlas music event regressions: " << error.what() << '\n';
		return 1;
	}
}

#include "Cli.h"
#include "ConfigPaths.h"
#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <format>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include "application_constants.h"
#include "fe/fe_app_constants.h"
#include "fe/Message.h"
#include "fe/game/game_gfx.h"
#include "fe/nes_constants.h"
#include "fi/fi_constants.h"
#include "common/klib/Asm6502.h"
#include "common/klib/Kfile.h"
#include "common/klib/Kstring.h"
#include "fe/ROM_Manager.h"
#include "fe/script/ScriptManager.h"
#include "fm/ProceduralMusic.h"
#include "fh/AtlasDevFrameScheduler.h"
#include "fh/AtlasDevMusicIntent.h"
#include "fh/GeneralHack.h"
#include "fh/HackManager.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
	struct OwnedRange {
		std::string_view name;
		std::size_t cpu_start;
		std::size_t cpu_end;
		std::size_t file_start;
		std::size_t file_end;
	};

	std::filesystem::path normalized_absolute(const std::filesystem::path& path) {
		std::error_code ec;
		auto result{ std::filesystem::absolute(path, ec) };
		if (ec)
			result = path;
		auto canonical{ std::filesystem::weakly_canonical(result, ec) };
		return ec ? result.lexically_normal() : canonical;
	}

	std::string sha256(std::span<const byte> input) {
		static constexpr std::array<std::uint32_t, 64> ROUND{
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
			0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
			0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
			0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
			0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
			0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
			0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
			0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
			0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
		};
		std::array<std::uint32_t, 8> hash{
			0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
		};
		std::vector<byte> padded(input.begin(), input.end());
		const std::uint64_t bit_count{ static_cast<std::uint64_t>(input.size()) * 8 };
		padded.push_back(0x80);
		while (padded.size() % 64 != 56)
			padded.push_back(0x00);
		for (int shift{ 56 }; shift >= 0; shift -= 8)
			padded.push_back(static_cast<byte>(bit_count >> shift));

		for (std::size_t block{ 0 }; block < padded.size(); block += 64) {
			std::array<std::uint32_t, 64> words{};
			for (std::size_t i{ 0 }; i < 16; ++i) {
				const auto offset{ block + i * 4 };
				words[i] = (static_cast<std::uint32_t>(padded[offset]) << 24)
					| (static_cast<std::uint32_t>(padded[offset + 1]) << 16)
					| (static_cast<std::uint32_t>(padded[offset + 2]) << 8)
					| padded[offset + 3];
			}
			for (std::size_t i{ 16 }; i < words.size(); ++i) {
				const auto s0{ std::rotr(words[i - 15], 7)
					^ std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3) };
				const auto s1{ std::rotr(words[i - 2], 17)
					^ std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10) };
				words[i] = words[i - 16] + s0 + words[i - 7] + s1;
			}

			auto [a, b, c, d, e, f, g, h] = hash;
			for (std::size_t i{ 0 }; i < words.size(); ++i) {
				const auto sigma1{ std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25) };
				const auto choice{ (e & f) ^ (~e & g) };
				const auto temp1{ h + sigma1 + choice + ROUND[i] + words[i] };
				const auto sigma0{ std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22) };
				const auto majority{ (a & b) ^ (a & c) ^ (b & c) };
				const auto temp2{ sigma0 + majority };
				h = g; g = f; f = e; e = d + temp1;
				d = c; c = b; b = a; a = temp1 + temp2;
			}
			hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d;
			hash[4] += e; hash[5] += f; hash[6] += g; hash[7] += h;
		}

		std::string result;
		for (const auto value : hash)
			result += std::format("{:08x}", value);
		return result;
	}

	std::string sha256(const std::vector<byte>& bytes, const OwnedRange& range) {
		return sha256(std::span<const byte>{
			bytes.data() + range.file_start, range.file_end - range.file_start });
	}

	std::uint64_t owned_fnv1a64(const std::vector<byte>& bytes,
		std::span<const OwnedRange> ranges) {
		std::uint64_t hash{ 0xcbf29ce484222325ULL };
		for (const auto& range : ranges)
			for (std::size_t i{ range.file_start }; i < range.file_end; ++i) {
				hash ^= bytes[i];
				hash *= 0x100000001b3ULL;
			}
		return hash;
	}
}

static void print_message(const fe::Message& p_message) {
	std::cout << p_message.text << '\n';
}

void fi::Cli::print_header(void) const {
	std::cout << fe::c::APP_NAME << " " << fe::c::APP_VERSION << " - Command-line interface\n";
	std::cout << "Author: Kai E. Fr";
	output_oe_on_windows();
	std::cout << "land (" << fe::c::APP_URL << ")\n";
	std::cout << "Build date: " << __DATE__ << " " << __TIME__ << " CET\n\n";
}

void fi::Cli::print_help(void) const {
	std::cout <<
		"Usage:\n"
		"  eoe-cli <command> <input> <output> [options]\n\n"
		"Commands:\n"
		"  Project XML:\n"
		"    xproj,   extract-project - Extract project XML from ROM\n"
		"    bproj,   build-project   - Build ROM from project XML\n"
		"\n"
		"  IScripts (interaction scripts):\n"
		"    x,   extract             - Disassemble IScripts from ROM\n"
		"    b,   build               - Assemble IScripts into ROM\n"
		"\n"
		"  BScripts (behavior scripts):\n"
		"    xb,  extract-bscript     - Disassemble BScripts from ROM\n"
		"    bb,  build-bscript       - Assemble BScripts and patch ROM\n"
		"\n"
		"  MScripts (low level music format):\n"
		"    xm,  extract-music       - Disassemble MScripts from ROM\n"
		"    bm,  build-music         - Assemble MScripts and patch ROM\n"
		"\n"
		"  MML (high level music format):\n"
		"    xmml, extract-mml        - Extract music as MML from ROM\n"
		"    bmml, build-mml          - Compile MML and patch ROM\n"
		"\n"
		"  Miscellaneous strings and constants:\n"
		"    xmisc, extract-misc      - Extract miscellaneous data from ROM\n"
		"    bmisc, build-misc        - Patch ROM with miscellaneous data\n"
		"\n"
		"  MIDI:\n"
		"    m2m, mml-to-midi         - Convert MML to MIDI files\n"
		"    r2m, rom-to-midi         - Extract music from ROM as MIDI files\n"
		"\n"
		"  LilyPond:\n"
		"    m2l, mml-to-ly           - Convert MML to LilyPond files\n"
		"    r2l, rom-to-ly           - Extract music from ROM as LilyPond files\n"
		"\n"
		"  Procedural music:\n"
		"    pmc, pmusic-compile      - Validate annotated MML and emit a JSON kit\n"
		"    pmi, install-music-intent\n"
		"                             - Install the transaction-safe state publisher\n\n";

	std::cout << "Options:\n";
	std::cout << "  Common options:\n";
	std::cout << "    -r, --region                 ROM region which must be defined in the configuration xml (auto-detected by default)\n";
	std::cout << "    -f, --force                  Force file overwrite when extracting data (disabled by default)\n";
	std::cout << "    -s, --source-rom             Source ROM when assembling (by default the output file itself)\n";
	std::cout << "    -o, --original-size          Only patch original ROM location (disabled by default)\n";
	std::cout << "  IScript options:\n";
	std::cout << "    -p, --no-shop-comments       Disable shop comment extraction (enabled by default)\n";
	std::cout << "  MScript options:\n";
	std::cout << "    -n, --no-notes               Do not emit notes in music disassembly (notes enabled by default)\n";
	std::cout << "  MML options:\n";
	std::cout << "    -lp, --lilypond-percussion   Add percussion staff to the LilyPond output (disabled by default)\n";
	std::cout << "  Project build options:\n";
	std::cout << "    -skip, --skip <list>         Comma-separated list of subsystems to omit from patching (see the docs)\n";
	std::cout << "    -aco, --allow-cin-overflow   Allow cinematic data to grow into iScript region 2\n";
	std::cout << "  Procedural-music provider options:\n";
	std::cout << "    -pr, --ram-base <address>     ABI base; must be $04ef (default $04ef)\n";
	std::cout << "    -ph, --hysteresis <n>         Consecutive eligible samples, 0..65535 (default 30)\n";
	std::cout << "    -pj, --json <file>            Write deterministic installation report JSON\n";
}

fi::Cli::Cli(int argc, char** argv) :
	m_strict{ false },
	m_shop_comments{ true },
	m_overwrite{ false },
	m_notes{ true },
	m_lilypond_percussion{ false },
	m_allow_cinematic_overflow{ false },
	m_pmusic_hysteresis_frames{ 30 },
	m_pmusic_ram_base{ fh::ami::RAM_REQUEST }
{
	const auto config_paths{ paths::resolve_config_paths(argc > 0 ? argv[0] : nullptr) };
	m_config_xml = config_paths.base.string();
	m_config_override_xml = config_paths.override.string();

	print_header();

	if (argc <= 1) {
		print_help();
		return;
	}

	set_mode(argv[1]);
	if (argc < 4)
		throw std::runtime_error(std::format(
			"Command '{}' requires both input and output arguments", argv[1]));

	m_in_file = argv[2];
	m_out_file = argv[3];
	parse_arguments(4, argc, argv);

	// we have the info we need to execute
	// IScript dispatch
	if (m_script_mode == fi::ScriptMode::IScriptBuild) {
		asm_to_nes(m_in_file, m_out_file,
			m_source_rom.empty() ? m_out_file : m_source_rom,
			m_strict);
	}
	else if (m_script_mode == fi::ScriptMode::IScriptExtract)
		nes_to_asm(m_in_file, m_out_file, m_shop_comments, m_overwrite);
	// BScript dispatch
	else if (m_script_mode == fi::ScriptMode::BScriptBuild) {
		basm_to_nes(m_in_file, m_out_file,
			m_source_rom.empty() ? m_out_file : m_source_rom,
			m_strict);
	}
	else if (m_script_mode == fi::ScriptMode::BScriptExtract)
		nes_to_basm(m_in_file, m_out_file, m_overwrite);
	// MScript dispatch
	else if (m_script_mode == fi::ScriptMode::MScriptBuild)
		masm_to_nes(m_in_file, m_out_file, m_source_rom.empty() ? m_out_file : m_source_rom);
	else if (m_script_mode == fi::ScriptMode::MScriptExtract)
		nes_to_masm(m_in_file, m_out_file, m_overwrite);
	// MML dispatch
	else if (m_script_mode == fi::ScriptMode::MmlBuild)
		mml_to_nes(m_in_file, m_out_file, m_source_rom.empty() ? m_out_file : m_source_rom);
	else if (m_script_mode == fi::ScriptMode::MmlExtract)
		nes_to_mml(m_in_file, m_out_file, m_overwrite);
	// midi dispatch
	else if (m_script_mode == fi::ScriptMode::MmlToMidi)
		mml_to_midi(m_in_file, m_out_file);
	else if (m_script_mode == fi::ScriptMode::RomToMidi)
		rom_to_midi(m_in_file, m_out_file);
	// Lilypond dispatch
	else if (m_script_mode == fi::ScriptMode::MmlToLilyPond)
		mml_to_lilypond(m_in_file, m_out_file);
	else if (m_script_mode == fi::ScriptMode::RomToLilyPond)
		rom_to_lilypond(m_in_file, m_out_file);
	else if (m_script_mode == fi::ScriptMode::ProceduralMusicCompile)
		compile_procedural_music(m_in_file, m_out_file);
	else if (m_script_mode == fi::ScriptMode::ProceduralMusicProviderInstall)
		install_procedural_music_provider(m_in_file, m_out_file);
	// miscellaneous data dispatch
	else if (m_script_mode == fi::ScriptMode::MiscBuild)
		misc_to_nes(m_in_file, m_out_file, m_source_rom.empty() ? m_out_file : m_source_rom);
	else if (m_script_mode == fi::ScriptMode::MiscExtract)
		nes_to_misc(m_in_file, m_out_file, m_overwrite);
	// debug
	else if (m_script_mode == fi::ScriptMode::DumpConfig)
		dump_config(m_in_file, m_out_file);
	// project commands
	else if (m_script_mode == fi::ScriptMode::ProjectBuild)
		project_to_nes(m_in_file, m_out_file, m_source_rom.empty() ? m_out_file : m_source_rom);
	else if (m_script_mode == fi::ScriptMode::ProjectExtract)
		nes_to_project(m_in_file, m_out_file, m_overwrite);
	// ROM expansion
	else if (m_script_mode == fi::ScriptMode::ExpandROM)
		expand_rom(m_in_file, m_out_file);
	else if (m_script_mode == fi::ScriptMode::RemapFog)
		remap_fog(m_in_file, m_source_rom.empty() ? m_out_file : m_source_rom,
			m_out_file, m_tileset_no, m_tiles);
	// can't really happen
	else
		throw(std::runtime_error("Invalid script mode"));
}

void fi::Cli::project_to_nes(const std::string& p_xml_filename, const std::string& p_out_filename,
	const std::string& p_source_rom_filename) {
	const auto rom{ load_rom_and_config(p_source_rom_filename) };
	auto game{ fe::game::load_game_xml_from_file(m_config, p_xml_filename, rom, print_message) };
	fe::game::patch_rom_to_file(m_config, game, p_out_filename,
		get_rom_patch_options(), print_message);
}

void fi::Cli::nes_to_project(const std::string& p_nes_filename, const std::string& p_xml_filename,
	bool p_overwrite) {
	if (!p_overwrite && klib::file::file_exists(p_xml_filename))
		throw std::runtime_error(std::format("File {} already exists, and overwrite flag (-f) not set",
			p_xml_filename));
	fe::Game game{ load_game(p_nes_filename) };
	fe::game::save_game_xml_to_file(m_config, game, p_xml_filename, print_message);
}

void fi::Cli::asm_to_nes(const std::string& p_asm_filename,
	const std::string& p_out_filename,
	const std::string& p_source_rom_filename,
	bool p_strict) {

	const auto rom{ load_rom_and_config(p_source_rom_filename) };
	const auto opcode_defs{ fe::script::get_iscript_opcode_info(m_config) };

	fe::script::asm_iscripts_to_file(m_config, rom, p_asm_filename, p_out_filename, opcode_defs, p_strict, print_message);
}

void fi::Cli::basm_to_nes(const std::string& p_basm_filename,
	const std::string& p_nes_filename,
	const std::string& p_source_rom_filename,
	bool p_strict) {

	const auto rom{ load_rom_and_config(p_source_rom_filename) };
	fe::script::asm_bscripts_to_file(m_config, rom, p_basm_filename, p_nes_filename, p_strict, print_message);

}

void fi::Cli::masm_to_nes(const std::string& p_mml_filename,
	const std::string& p_nes_filename,
	const std::string& p_source_rom_filename) {

	const auto rom{ load_rom_and_config(p_source_rom_filename) };
	fe::script::asm_mscripts_to_file(m_config, rom, p_mml_filename, p_nes_filename, print_message);
}

void fi::Cli::misc_to_nes(const std::string& p_txt_filename,
	const std::string& p_nes_filename,
	const std::string& p_source_rom_filename) {

	const auto rom{ load_rom_and_config(p_source_rom_filename) };

	fe::script::build_misc_to_file(m_config, rom, p_txt_filename, p_nes_filename, print_message);
}

void fi::Cli::nes_to_asm(const std::string& p_nes_filename,
	const std::string& p_asm_filename, bool p_shop_comments, bool p_overwrite) {

	// show params
	if (p_shop_comments)
		std::cout << "Will show shop data as comments where they are referenced\n";
	if (p_overwrite)
		std::cout << "Will overwrite output assembly file if it already exists\n";

	const auto rom_data{ load_rom_and_config(p_nes_filename) };
	const auto opcode_info{ fe::script::get_iscript_opcode_info(m_config) };

	fe::script::disasm_iscripts_to_file(m_config, rom_data, opcode_info.opcodes,
		p_asm_filename, p_shop_comments, p_overwrite, print_message);
}

void fi::Cli::nes_to_basm(const std::string& p_nes_filename,
	const std::string& p_basm_filename, bool p_overwrite) {

	// show params
	if (p_overwrite)
		std::cout << "Will overwrite output assembly file if it already exists\n";

	const auto rom_data{ load_rom_and_config(p_nes_filename) };
	fe::script::disasm_bscripts_to_file(m_config, rom_data, p_basm_filename, p_overwrite, print_message);
}

void fi::Cli::nes_to_masm(const std::string& p_nes_filename,
	const std::string& p_mml_filename, bool p_overwrite) {

	const auto rom_data{ load_rom_and_config(p_nes_filename) };

	fe::script::disasm_mscripts_to_file(m_config, rom_data, p_mml_filename, m_notes, p_overwrite, print_message);
}

void fi::Cli::nes_to_misc(const std::string& p_nes_filename,
	const std::string& p_txt_filename,
	bool p_overwrite) {
	const auto rom_data{ load_rom_and_config(p_nes_filename) };

	fe::script::extract_misc_to_file(m_config, rom_data, p_txt_filename, m_strict, p_overwrite, print_message);
}

void fi::Cli::nes_to_mml(const std::string& p_nes_filename,
	const std::string& p_mml_filename,
	bool p_overwrite) {

	const auto rom_data{ load_rom_and_config(p_nes_filename) };

	fe::script::decompile_mml_to_file(m_config, rom_data, p_mml_filename, p_overwrite, print_message);
}

void fi::Cli::mml_to_nes(const std::string& p_mml_filename,
	const std::string& p_nes_filename,
	const std::string& p_source_rom_filename) {

	const auto rom{ load_rom_and_config(p_source_rom_filename) };

	fe::script::compile_mml_to_file(m_config, rom, p_mml_filename, p_nes_filename, print_message);
}

void fi::Cli::rom_to_midi(const std::string& p_nes_filename,
	const std::string& p_out_file_prefix) {

	const auto rom_data{ load_rom_and_config(p_nes_filename) };

	fe::script::rom_to_midi_files(m_config, rom_data, p_out_file_prefix, print_message);
}

void fi::Cli::mml_to_midi(const std::string& p_mml_filename,
	const std::string& p_out_file_prefix) {
	fe::script::mml_to_midi_files(
		p_mml_filename,
		p_out_file_prefix,
		print_message);
}

void fi::Cli::rom_to_lilypond(const std::string& p_nes_filename,
	const std::string& p_out_file_prefix) {
	const auto rom_data{ load_rom_and_config(p_nes_filename) };

	fe::script::rom_to_lilypond_files(m_config, rom_data, p_out_file_prefix, m_lilypond_percussion,
		print_message);
}

void fi::Cli::mml_to_lilypond(const std::string& p_mml_filename,
	const std::string& p_out_file_prefix) {
	fe::script::mml_to_lilypond_files(p_mml_filename, p_out_file_prefix, m_lilypond_percussion,
		print_message);
}

void fi::Cli::compile_procedural_music(const std::string& p_mml_filename,
	const std::string& p_json_filename) {
	const auto source{ klib::file::read_file_as_strings(p_mml_filename) };
	const auto json{ fm::pmusic::compile_json(source, p_mml_filename) };
	klib::file::write_bytes_to_file(
		std::vector<byte>(json.begin(), json.end()), p_json_filename);
	std::cout << "Procedural music kit written to " << p_json_filename << "!\n";
}

void fi::Cli::install_procedural_music_provider(
	const std::string& p_base_rom_filename,
	const std::string& p_output_rom_filename) {
	using namespace fh::ami;
	if (m_pmusic_ram_base != RAM_REQUEST)
		throw std::runtime_error(
			"Procedural-music ABI is fixed at RAM base $04ef");
	static constexpr std::array<byte, 3> SHA256_ORACLE{ 'a', 'b', 'c' };
	if (sha256(SHA256_ORACLE)
		!= "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
		throw std::runtime_error(
			"Procedural-music report SHA-256 self-test failed");

	const auto input_path{ normalized_absolute(p_base_rom_filename) };
	const auto output_path{ normalized_absolute(p_output_rom_filename) };
	if (input_path == output_path)
		throw std::runtime_error(
			"Procedural-music provider input and output ROM paths must differ");
	if (!m_pmusic_report_json.empty()) {
		const auto report_path{ normalized_absolute(m_pmusic_report_json) };
		if (report_path == input_path || report_path == output_path)
			throw std::runtime_error(
				"Procedural-music report path must differ from both ROM paths");
	}

	auto rom{ load_rom_and_config(p_base_rom_filename) };
	const auto before{ rom };
	if (rom.size() != fe::nc::VANILLA_ROM_SIZE
		|| rom[4] != fe::nc::VANILLA_BANK_COUNT)
		throw std::runtime_error(
			"Procedural-music provider requires a 16-bank Faxanadu ROM");
	const auto free_ranges{ fe::ROM_Manager::parse_bank_15_free_ranges(m_config) };
	if (free_ranges.empty())
		throw std::runtime_error(
			"No bank 15 free space is configured for the procedural-music provider");
	const auto free_file_range{ fe::ROM_Manager::find_trailing_free_range(
		rom, free_ranges.back()) };
	const auto [cpu_start, cpu_end]{
		fe::ROM_Manager::file_range_to_cpu_range(free_file_range) };
	const auto spec{ std::format(
		"AtlasDevFrameScheduler\n"
		"AtlasDevMusicIntent hysteresis_frames={}",
		m_pmusic_hysteresis_frames) };
	const auto hacks{ fh::filter_general_hacks(15, fh::parse_general_hacks(spec)) };
	const auto used{ fh::HackManager{}.install_general_hacks(
		m_config, rom, 15, cpu_start, cpu_end, hacks, nullptr) };

	if (rom.size() != before.size())
		throw std::runtime_error(
			"Procedural-music provider changed the ROM size");
	const word scheduler_base{ fh::afs::find_base(rom) };
	if (scheduler_base == 0)
		throw std::runtime_error(
			"Procedural-music provider failed its scheduler read-back check");
	const auto scheduler_offset{
		klib::Asm6502::get_file_offset(15, scheduler_base) };
	constexpr std::array<std::size_t, 3> PRE_SITES{
		fh::afs::OFF_PRE0, fh::afs::OFF_PRE1, fh::afs::OFF_PRE2
	};
	word publisher_base{ 0 };
	for (std::size_t i{ 0 }; i < PRE_SITES.size(); ++i)
		if (rom[scheduler_offset + fh::afs::OFF_ARM0 + i] == KIND) {
			publisher_base = static_cast<word>(
				rom[scheduler_offset + PRE_SITES[i]]
				| (rom[scheduler_offset + PRE_SITES[i] + 1] << 8));
			break;
		}
	if (publisher_base == 0)
		throw std::runtime_error(
			"Procedural-music provider failed its publisher read-back check");

	const std::size_t allocation_cpu_end{ cpu_start + used };
	if (publisher_base < cpu_start || publisher_base >= allocation_cpu_end)
		throw std::runtime_error(
			"Procedural-music provider reported an invalid publisher allocation");
	const auto hook1_file{ klib::Asm6502::get_file_offset(15, 0xc9af) };
	const auto hook2_file{ klib::Asm6502::get_file_offset(15, 0xc9de) };
	const auto allocation_file{ klib::Asm6502::get_file_offset(
		15, static_cast<word>(cpu_start)) };
	const auto publisher_file{ klib::Asm6502::get_file_offset(15, publisher_base) };
	const std::array<OwnedRange, 3> owned_ranges{
		OwnedRange{ "nmi_dma_hook", 0xc9af, 0xc9b4, hook1_file, hook1_file + 5 },
		OwnedRange{ "post_deadline_hook", 0xc9de, 0xc9e3, hook2_file, hook2_file + 5 },
		OwnedRange{ "scheduler_and_publisher", cpu_start, allocation_cpu_end,
			allocation_file, allocation_file + used }
	};
	const OwnedRange publisher_range{
		"publisher", publisher_base, allocation_cpu_end,
		publisher_file, allocation_file + used
	};
	std::size_t changed_bytes{ 0 };
	for (std::size_t i{ 0 }; i < rom.size(); ++i) {
		if (rom[i] == before[i])
			continue;
		++changed_bytes;
		const bool owned{ std::ranges::any_of(owned_ranges,
			[i](const OwnedRange& range) {
				return i >= range.file_start && i < range.file_end;
			}) };
		if (!owned)
			throw std::runtime_error(
				"Procedural-music provider escaped its declared bank-15 ownership");
	}

	std::string report{ std::format(
		"{{\n"
		"  \"format\": \"faxedit-music-intent-install\",\n"
		"  \"region\": \"us\",\n"
		"  \"input_sha256\": \"{}\",\n"
		"  \"output_sha256\": \"{}\",\n"
		"  \"ram_abi\": {{\n"
		"    \"start\": \"0x04ef\",\n"
		"    \"end_exclusive\": \"0x04f8\",\n"
		"    \"size\": 9,\n"
		"    \"conductor_owned\": [\n"
		"      {{\"start\": \"0x04ef\", \"end_exclusive\": \"0x04f3\"}},\n"
		"      {{\"start\": \"0x04f6\", \"end_exclusive\": \"0x04f8\"}}\n"
		"    ],\n"
		"    \"publisher_owned\": [\n"
		"      {{\"start\": \"0x04f3\", \"end_exclusive\": \"0x04f6\"}}\n"
		"    ],\n"
		"    \"bit_shared_0x04f3\": {{\n"
		"      \"publisher_mask\": \"0x83\",\n"
		"      \"conductor_mask\": \"0x40\",\n"
		"      \"reserved_mask\": \"0x3c\",\n"
		"      \"publication_store\": \"publish_ready_0x04f3_last\"\n"
		"    }}\n"
		"  }},\n"
		"  \"config\": {{\n"
		"    \"hysteresis_frames\": {},\n"
		"    \"state_codes\": {{\"calm\": 0, \"explore\": 1, \"danger\": 2}},\n"
		"    \"mantra_danger_policy\": \"clamp_to_explore\"\n"
		"  }},\n"
		"  \"producer_contract\": {{\n"
		"    \"active_gate\": \"0x04f2 bit7\",\n"
		"    \"family_source_for_mantra_clamp\": \"0x04f7 bits5..2\",\n"
		"    \"publisher_writes_request_0x04ef\": false,\n"
		"    \"request_family_merge_owner\": \"conductor\",\n"
		"    \"landing_interlock_mask\": \"0xc0\",\n"
		"    \"request_command_interlock_mask\": \"0xc0\",\n"
		"    \"music_owner_allowed\": [\"0x00\", \"0x80..0xff\"],\n"
		"    \"staged_token_must_equal_active_token\": true,\n"
		"    \"ready_and_dwell_latch_until_consumed\": true\n"
		"  }},\n"
		"  \"scheduler_base\": \"0x{:04x}\",\n"
		"  \"publisher_base\": \"0x{:04x}\",\n"
		"  \"publisher_artifact\": {{\n"
		"    \"cpu_start\": \"0x{:04x}\",\n"
		"    \"cpu_end_exclusive\": \"0x{:04x}\",\n"
		"    \"size\": {},\n"
		"    \"sha256\": \"{}\"\n"
		"  }},\n"
		"  \"nmi_timing_certificate\": {{\n"
		"    \"format\": \"atlasdev-music-intent-nmi-timing\",\n"
		"    \"version\": {},\n"
		"    \"cpu\": \"Ricoh 2A03 (NMOS 6502 timing)\",\n"
		"    \"cycle_bound\": \"placement-and-configuration-independent-conservative\",\n"
		"    \"instruction_bound\": \"maximum-executed-path\",\n"
		"    \"publisher\": {{\n"
		"      \"max_cycles\": {},\n"
		"      \"max_instructions\": {}\n"
		"    }},\n"
		"    \"scheduler_pre_dispatch\": {{\n"
		"      \"core_overhead_max\": {{\"cycles\": {}, \"instructions\": {}}},\n"
		"      \"hook_call_and_padding\": {{\"cycles\": {}, \"instructions\": {}}},\n"
		"      \"hook_cpu_instruction_max\": {{\"cycles\": {}, \"instructions\": {}}},\n"
		"      \"displaced_vanilla\": {{\"cycles\": {}, \"instructions\": {}}},\n"
		"      \"incremental_cpu_instruction_max\": {{\"cycles\": {}, \"instructions\": {}}}\n"
		"    }},\n"
		"    \"oam_dma\": {{\n"
		"      \"stall_min_cycles\": {},\n"
		"      \"stall_max_cycles\": {},\n"
		"      \"alignment_delta_over_vanilla_max_cycles\": {}\n"
		"    }},\n"
		"    \"combined\": {{\n"
		"      \"hook_cpu_instruction_max\": {{\"cycles\": {}, \"instructions\": {}}},\n"
		"      \"hook_with_dma_max_cycles\": {},\n"
		"      \"incremental_cpu_instruction_max\": {{\"cycles\": {}, \"instructions\": {}}},\n"
		"      \"incremental_over_vanilla_max\": {{\"cycles\": {}, \"instructions\": {}}}\n"
		"    }},\n"
		"    \"scope\": {{\n"
		"      \"publisher_entry_through_rts\": true,\n"
		"      \"scheduler_boot_arm_included\": true,\n"
		"      \"scheduler_counter_rollover_included\": true,\n"
		"      \"displaced_oam_register_store_included\": true,\n"
		"      \"oam_dma_alignment_delta_in_incremental_bound\": true,\n"
		"      \"scheduler_quiet_eligible_path\": true,\n"
		"      \"one_active_pre_role\": true,\n"
		"      \"every_taken_branch_charged_page_cross\": true,\n"
		"      \"nmi_entry_exit_excluded\": true,\n"
		"      \"other_scheduler_roles_excluded\": true\n"
		"    }}\n"
		"  }},\n"
		"  \"owned_ranges\": [\n",
		sha256(std::span<const byte>{ before.data(), before.size() }),
		sha256(std::span<const byte>{ rom.data(), rom.size() }),
		m_pmusic_hysteresis_frames, scheduler_base, publisher_base,
		publisher_range.cpu_start, publisher_range.cpu_end,
		publisher_range.file_end - publisher_range.file_start,
		sha256(rom, publisher_range),
		fh::ami::timing::CERTIFICATE_VERSION,
		fh::ami::timing::PUBLISHER_MAX_CYCLES,
		fh::ami::timing::PUBLISHER_MAX_INSTRUCTIONS,
		fh::ami::timing::SCHEDULER_CORE_OVERHEAD_MAX_CYCLES,
		fh::ami::timing::SCHEDULER_CORE_OVERHEAD_MAX_INSTRUCTIONS,
		fh::ami::timing::SCHEDULER_HOOK_CALL_AND_PADDING_CYCLES,
		fh::ami::timing::SCHEDULER_HOOK_CALL_AND_PADDING_INSTRUCTIONS,
		fh::ami::timing::SCHEDULER_PRE_DISPATCH_MAX_CYCLES,
		fh::ami::timing::SCHEDULER_PRE_DISPATCH_MAX_INSTRUCTIONS,
		fh::ami::timing::DISPLACED_VANILLA_CYCLES,
		fh::ami::timing::DISPLACED_VANILLA_INSTRUCTIONS,
		fh::ami::timing::SCHEDULER_INCREMENTAL_CPU_MAX_CYCLES,
		fh::ami::timing::SCHEDULER_INCREMENTAL_MAX_INSTRUCTIONS,
		fh::ami::timing::OAM_DMA_STALL_MIN_CYCLES,
		fh::ami::timing::OAM_DMA_STALL_MAX_CYCLES,
		fh::ami::timing::OAM_DMA_ALIGNMENT_DELTA_MAX_CYCLES,
		fh::ami::timing::COMBINED_HOOK_CPU_MAX_CYCLES,
		fh::ami::timing::COMBINED_HOOK_MAX_INSTRUCTIONS,
		fh::ami::timing::COMBINED_HOOK_WITH_DMA_MAX_CYCLES,
		fh::ami::timing::COMBINED_INCREMENTAL_CPU_MAX_CYCLES,
		fh::ami::timing::COMBINED_INCREMENTAL_MAX_INSTRUCTIONS,
		fh::ami::timing::COMBINED_INCREMENTAL_BUDGET_MAX_CYCLES,
		fh::ami::timing::COMBINED_INCREMENTAL_BUDGET_MAX_INSTRUCTIONS) };
	for (std::size_t i{ 0 }; i < owned_ranges.size(); ++i) {
		const auto& range{ owned_ranges[i] };
		report += std::format(
			"    {{\n"
			"      \"name\": \"{}\",\n"
			"      \"bank\": 15,\n"
			"      \"cpu_start\": \"0x{:04x}\",\n"
			"      \"cpu_end_exclusive\": \"0x{:04x}\",\n"
			"      \"file_start\": \"0x{:05x}\",\n"
			"      \"file_end_exclusive\": \"0x{:05x}\",\n"
			"      \"size\": {},\n"
			"      \"input_sha256\": \"{}\",\n"
			"      \"output_sha256\": \"{}\"\n"
			"    }}{}\n",
			range.name, range.cpu_start, range.cpu_end,
			range.file_start, range.file_end, range.file_end - range.file_start,
			sha256(before, range), sha256(rom, range),
			i + 1 == owned_ranges.size() ? "" : ",");
	}
	std::size_t owned_bytes{ 0 };
	for (const auto& range : owned_ranges)
		owned_bytes += range.file_end - range.file_start;
	report += std::format(
		"  ],\n"
		"  \"owned_bytes\": {},\n"
		"  \"changed_bytes\": {},\n"
		"  \"owned_input_fnv1a64\": \"{:016x}\",\n"
		"  \"owned_output_fnv1a64\": \"{:016x}\",\n"
		"  \"bank5_preserved\": true\n"
		"}}\n",
		owned_bytes, changed_bytes,
		owned_fnv1a64(before, owned_ranges), owned_fnv1a64(rom, owned_ranges));

	klib::file::write_bytes_to_file(rom, p_output_rom_filename);
	if (!m_pmusic_report_json.empty())
		klib::file::write_string_to_file(report, m_pmusic_report_json);
	std::cout << std::format(
		"Procedural-music provider installed: scheduler=${:04X}, publisher=${:04X}, "
		"owned={}; changed={}; bank 5 preserved byte-for-byte.\n",
		scheduler_base, publisher_base, owned_bytes, changed_bytes);
}

void fi::Cli::dump_config(const std::string& p_nes_filename,
	const std::string& p_dump_filename) {
	load_rom_and_config(p_nes_filename);

	klib::file::write_string_to_file(m_config.to_string(), p_dump_filename);
	std::cout << "Wrote resolved configuration dump to " << p_dump_filename << "!\n";
}

void fi::Cli::expand_rom(const std::string& p_nes_filename,
	const std::string& p_out_nes_filename) {
	auto rom{ load_rom_and_config(p_nes_filename) };
	fh::HackManager::install_hack_surom_expansion(m_config, rom);
	klib::file::write_bytes_to_file(rom, p_out_nes_filename);
	std::cout << "ROM expanded!\n";
}

void fi::Cli::remap_fog(const std::string& p_in_nes_filename,
	const std::string& p_in_xml_filename,
	const std::string& p_out_xml_filename,
	byte p_tileset_no, const std::vector<byte>& p_tiles) {
	const auto rom{ load_rom_and_config(p_in_nes_filename) };
	auto game{ fe::game::load_game_xml_from_file(m_config, p_in_xml_filename, rom, print_message) };

	std::cout << "Attempting to remap fog chr-tile indexes\n";
	fe::game::gfx::remap_fog_chr_tiles(m_config, game, p_tileset_no, p_tiles);
	fe::game::save_game_xml_to_file(m_config, game, p_out_xml_filename, print_message);
}

void fi::Cli::parse_arguments(int arg_start, int argc, char** argv) {
	for (int i{ arg_start }; i < argc; ++i) {
		std::string argvi{ argv[i] };
		const auto require_value = [&i, argc, argv](const char* error) {
			if (i + 1 >= argc)
				throw std::runtime_error(error);
			return argv[++i];
		};
		if (argvi == appc::CLI_SOURCE_ROM.first ||
			argvi == appc::CLI_SOURCE_ROM.second) {
			m_source_rom = require_value(
				"Source ROM option was set, but no source ROM file was specified");
		}
		else if (argvi == appc::CLI_REGION.first ||
			argvi == appc::CLI_REGION.second) {
			m_region = require_value(
				"Region option was used, but no ROM region was specified");
		}
		else if (argvi == appc::CLI_TILESET.first ||
			argvi == appc::CLI_TILESET.second) {
			if (i + 1 >= argc)
				throw std::runtime_error("Tileset index not specified");
			else
				m_tileset_no = static_cast<byte>(klib::str::parse_numeric(argv[++i]));
		}
		else if (argvi == appc::CLI_TILES.first ||
			argvi == appc::CLI_TILES.second) {
			if (i + 1 >= argc)
				throw std::runtime_error("chr-tiles list not given");
			else {
				m_tiles = klib::str::parse_byte_list(argv[++i]);
			}
		}
		else if (argvi == appc::CLI_SKIP_PATCHING.first ||
			argvi == appc::CLI_SKIP_PATCHING.second) {
			const auto skip_list{ klib::str::split_string(require_value(
				"Skip ROM patching option was used, but no options list was specified"), ',') };
			for (const auto& list_elem : skip_list) {
				const auto option{ klib::str::to_lower(klib::str::trim(list_elem)) };
				if (option.empty())
					throw std::runtime_error("Empty ROM patch option in skip list");
				m_patch_skips.push_back(option);
			}
		}
		else if (argvi == appc::CLI_PMUSIC_HYSTERESIS_FRAMES.first
			|| argvi == appc::CLI_PMUSIC_HYSTERESIS_FRAMES.second) {
			const int value{ klib::str::parse_numeric(require_value(
				"Hysteresis option is missing its value")) };
			if (value < 0 || value > 0xffff)
				throw std::runtime_error("Hysteresis frames must be in 0..65535");
			m_pmusic_hysteresis_frames = static_cast<std::uint16_t>(value);
		}
		else if (argvi == appc::CLI_PMUSIC_RAM_BASE.first
			|| argvi == appc::CLI_PMUSIC_RAM_BASE.second) {
			const int value{ klib::str::parse_numeric(require_value(
				"RAM-base option is missing its value")) };
			if (value < 0 || value > 0xffff)
				throw std::runtime_error("RAM base must be a 16-bit address");
			m_pmusic_ram_base = static_cast<std::uint16_t>(value);
		}
		else if (argvi == appc::CLI_PMUSIC_REPORT_JSON.first
			|| argvi == appc::CLI_PMUSIC_REPORT_JSON.second) {
			m_pmusic_report_json = require_value(
				"JSON-report option is missing its value");
		}
		else
			set_flag(argvi);
	}
}

std::vector<byte> fi::Cli::load_rom_and_config(
	const std::string& p_nes_filename) {

	std::cout << "Attempting to read " << p_nes_filename << "\n";
	const auto rom_data{ klib::file::read_file_as_bytes(p_nes_filename) };

	m_config = fe::Config(
		m_config_xml,
		m_config_override_xml,
		rom_data,
		m_region
	);

	if (m_region.empty())
		std::cout << "ROM region resolved to '" << m_config.get_region() << "'\n";
	else
		std::cout << "ROM region specified as '" << m_region << "'\n";

	return rom_data;
}

fe::Game fi::Cli::load_game(const std::string& p_nes_filename) {
	const auto rom{ load_rom_and_config(p_nes_filename) };
	return fe::game::load_rom(rom, m_config, print_message).game;
}

fe::game::RomPatchOptions fi::Cli::get_rom_patch_options(void) const {
	fe::game::RomPatchOptions options{};

	for (const auto& list_elem : m_patch_skips) {
		if (list_elem == "bank15_data")
			options.bank15_data = false;
		else if (list_elem == "bg_gfx")
			options.bg_gfx = false;
		else if (list_elem == "cinematics")
			options.cinematics = false;
		else if (list_elem == "fog")
			options.fog = false;
		else if (list_elem == "jump_on_tiles")
			options.jump_on_tiles = false;
		else if (list_elem == "mattock_animations")
			options.mattock_animations = false;
		else if (list_elem == "metadata")
			options.metadata = false;
		else if (list_elem == "palettes")
			options.palettes = false;
		else if (list_elem == "push_blocks")
			options.push_blocks = false;
		else if (list_elem == "scenes")
			options.scenes = false;
		else if (list_elem == "sprite_data")
			options.sprite_data = false;
		else if (list_elem == "sprite_gfx")
			options.sprite_gfx = false;
		else if (list_elem == "stages")
			options.stages = false;
		else if (list_elem == "tilemaps")
			options.tilemaps = false;
		else if (list_elem == "world_chr_data")
			options.world_chr_data = false;
		else
			throw std::runtime_error(std::format("Unknown patching skip code: {}", list_elem));
	}

	options.throw_on_cinematic_overflow = !m_allow_cinematic_overflow;

	return options;
}

void fi::Cli::set_mode(const std::string& p_mode) {
	if (check_mode(p_mode, appc::CMD_BUILD)) {
		m_script_mode = fi::ScriptMode::IScriptBuild;
	}
	else if (check_mode(p_mode, appc::CMD_EXTRACT)) {
		m_script_mode = fi::ScriptMode::IScriptExtract;
	}
	else if (check_mode(p_mode, appc::CMD_BUILD_BSCRIPTS)) {
		m_script_mode = fi::ScriptMode::BScriptBuild;
	}
	else if (check_mode(p_mode, appc::CMD_EXTRACT_BSCRIPTS)) {
		m_script_mode = fi::ScriptMode::BScriptExtract;
	}
	else if (check_mode(p_mode, appc::CMD_BUILD_MUSIC)) {
		m_script_mode = fi::ScriptMode::MScriptBuild;
	}
	else if (check_mode(p_mode, appc::CMD_EXTRACT_MUSIC)) {
		m_script_mode = fi::ScriptMode::MScriptExtract;
	}
	else if (check_mode(p_mode, appc::CMD_BUILD_MML)) {
		m_script_mode = fi::ScriptMode::MmlBuild;
	}
	else if (check_mode(p_mode, appc::CMD_EXTRACT_MML)) {
		m_script_mode = fi::ScriptMode::MmlExtract;
	}
	else if (check_mode(p_mode, appc::CMD_MML_TO_MIDI)) {
		m_script_mode = fi::ScriptMode::MmlToMidi;
	}
	else if (check_mode(p_mode, appc::CMD_ROM_TO_MIDI)) {
		m_script_mode = fi::ScriptMode::RomToMidi;
	}
	else if (check_mode(p_mode, appc::CMD_MML_TO_LILYPOND)) {
		m_script_mode = fi::ScriptMode::MmlToLilyPond;
	}
	else if (check_mode(p_mode, appc::CMD_ROM_TO_LILYPOND)) {
		m_script_mode = fi::ScriptMode::RomToLilyPond;
	}
	else if (check_mode(p_mode, appc::CMD_PMUSIC_COMPILE)) {
		m_script_mode = fi::ScriptMode::ProceduralMusicCompile;
	}
	else if (check_mode(p_mode, appc::CMD_PMUSIC_PROVIDER_INSTALL)) {
		m_script_mode = fi::ScriptMode::ProceduralMusicProviderInstall;
	}
	else if (check_mode(p_mode, appc::CMD_BUILD_MISC)) {
		m_script_mode = fi::ScriptMode::MiscBuild;
	}
	else if (check_mode(p_mode, appc::CMD_EXTRACT_MISC)) {
		m_script_mode = fi::ScriptMode::MiscExtract;
	}
	else if (check_mode(p_mode, appc::CMD_DUMP_CONFIG)) {
		m_script_mode = fi::ScriptMode::DumpConfig;
	}
	else if (check_mode(p_mode, appc::CMD_BUILD_PROJECT)) {
		m_script_mode = fi::ScriptMode::ProjectBuild;
	}
	else if (check_mode(p_mode, appc::CMD_EXTRACT_PROJECT)) {
		m_script_mode = fi::ScriptMode::ProjectExtract;
	}
	else if (check_mode(p_mode, appc::CMD_EXPAND_ROM)) {
		m_script_mode = fi::ScriptMode::ExpandROM;
	}
	else if (check_mode(p_mode, appc::CMD_REMAP_FOG)) {
		m_script_mode = fi::ScriptMode::RemapFog;
	}
	else throw std::runtime_error("Unknown command " + p_mode);
}

bool fi::Cli::check_mode(const std::string& p_mode,
	const std::pair<std::string, std::string>& p_cmds) {
	return (p_mode == p_cmds.first || p_mode == p_cmds.second);
}

// TODO: Streamline flag lookup with const map
void fi::Cli::set_flag(const std::string& p_flag) {
	for (std::size_t i{ 0 }; i < appc::CLI_FLAGS.size(); ++i) {
		if (p_flag == appc::CLI_FLAGS[i].first || p_flag == appc::CLI_FLAGS[i].second) {
			toggle_flag(i);
			return;
		}
	}

	throw std::runtime_error("Unknown option " + p_flag);
}

void fi::Cli::toggle_flag(std::size_t p_flag_idx) {
	if (p_flag_idx == 0)
		m_shop_comments = !m_shop_comments;
	else if (p_flag_idx == 1)
		m_strict = !m_strict;
	else if (p_flag_idx == 2)
		m_overwrite = !m_overwrite;
	else if (p_flag_idx == 3)
		m_notes = !m_notes;
	else if (p_flag_idx == 4)
		m_lilypond_percussion = !m_lilypond_percussion;
	else if (p_flag_idx == 5)
		m_allow_cinematic_overflow = !m_allow_cinematic_overflow;
}

// sad that this is needed in 2026
void fi::Cli::output_oe_on_windows(void) const {

#ifdef _WIN32
	UINT old_cp = GetConsoleOutputCP();
	SetConsoleOutputCP(CP_UTF8);
#endif

	std::cout << "ø";

#ifdef _WIN32
	SetConsoleOutputCP(old_cp);
#endif
}

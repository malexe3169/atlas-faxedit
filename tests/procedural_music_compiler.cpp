#include "fm/ProceduralMusic.h"
#include "fm/fm_constants.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

	void require(bool condition, const std::string& message) {
		if (!condition)
			throw std::runtime_error(message);
	}

	struct FamilyFixture {
		const char* id;
		int tempo;
		const char* meter;
		const char* default_state;
		bool terminal;
	};

	std::vector<std::string> family_kit() {
		constexpr std::array<FamilyFixture, 16> families{
			FamilyFixture{ "intro", 90, "4/4", "establish", false },
			{ "dartmoor", 105, "4/4", "establish", false },
			{ "trunk", 115, "3/4", "establish", false },
			{ "branches", 125, "6/8", "establish", false },
			{ "mist", 80, "4/4", "establish", false },
			{ "towers", 130, "4/4", "establish", false },
			{ "eolis", 100, "4/4", "establish", false },
			{ "mantra", 70, "4/4", "establish", false },
			{ "towns", 110, "2/4", "establish", false },
			{ "boss", 150, "5/4", "crisis", false },
			{ "hourglass", 140, "4/4", "drive", false },
			{ "outro", 85, "4/4", "establish", true },
			{ "king", 75, "4/4", "establish", false },
			{ "guru", 60, "4/4", "establish", false },
			{ "shop", 120, "3/4", "drive", false },
			{ "zenis", 90, "5/4", "establish", false }
		};
		constexpr std::array<const char*, 3> states{
			"establish", "drive", "crisis"
		};
		std::vector<std::string> source{
			"; @pmusic-kit version=2 id=family-test title=\"Family test\"",
			"; @pmusic-style id=family-bible sha256=\"621d8fd742eda6b70317a4390cd9d8cb217a30487634e614c8ed261cbfa238da\"",
			"; @pmusic-policy default_family=eolis default_state=establish repeat=compact-no-repeat boundary=phrase clock=ntsc quantum=1"
		};
		for (std::size_t i{ 0 }; i < families.size(); ++i) {
			const auto& family{ families[i] };
			source.push_back(std::format(
				"; @pmusic-family id={} stock_song={} default_state={} terminal={}",
				family.id, i + 1, family.default_state,
				family.terminal ? "true" : "false"));
			for (std::size_t state{ 0 }; state < states.size(); ++state)
				source.push_back(std::format(
					"; @pmusic-state family={} id={} section={}-{} intensity={}",
					family.id, states[state], family.id, states[state],
					std::array<int, 3>{ 48, 128, 224 }[state]));
		}
		source.push_back("; @pmusic-event-route event=death timing=immediate source_family=any from_join=load kind=cut landing=fixed landing_family=mantra landing_state=establish");
		source.push_back("; @pmusic-event-route event=boss_entry timing=immediate source_family=any from_join=load kind=stinger landing=fixed landing_family=boss landing_state=crisis");
		source.push_back("; @pmusic-event-route event=boss_exit timing=boundary source_family=boss from_join=* kind=bridge landing=requested");

		int song{ 1 };
		for (const auto& family : families)
			for (const auto* state : states)
				for (int variation{ 0 }; variation < 2; ++variation) {
					const char suffix{ static_cast<char>('a' + variation) };
					source.push_back(std::format(
						"; @pmusic-phrase id={0}-{1}-{2} role=loop family={0} states=\"{1}\" weight=2 entry_join={0}-loop exit_join={0}-loop tempo={3}/1 meter={4}",
						family.id, state, suffix, family.tempo, family.meter));
					source.push_back(std::format("#song {}", song++));
					source.push_back(std::format("t{}", family.tempo));
					source.push_back(std::format("#time \"{}\"", family.meter));
					const bool sparse{ std::string(family.id) == "mist"
						&& std::string(state) == "establish" };
					source.push_back(sparse
						? "#sq1 { l4 r !end }" : "#sq1 { l4 c !end }");
					source.push_back(sparse
						? "#sq2 { l4 r !end }" : "#sq2 { l4 e !end }");
					source.push_back(sparse
						? "#tri { l4 r !end }" : "#tri { l4 c !end }");
					source.push_back(sparse
						? "#noise { l4 r !end }" : "#noise { l4 p1 !end }");
				}
		source.push_back("; @pmusic-phrase id=boss-attack role=stinger family=boss event=boss_entry weight=1 entry_join=load exit_join=boss-loop tempo=160/1 meter=7/8");
		source.push_back(std::format("#song {}", song++));
		source.push_back("t160");
		source.push_back("#time \"7/8\"");
		source.push_back("#sq1 { l4 c !end }");
		source.push_back("#sq2 { l4 e !end }");
		source.push_back("#tri { l4 c !end }");
		source.push_back("#noise { l4 p1 !end }");
		source.push_back("; @pmusic-phrase id=boss-recovery role=bridge family=boss event=boss_exit weight=1 entry_join=boss-loop exit_join=boss-loop tempo=150/1 meter=5/4");
		source.push_back(std::format("#song {}", song));
		source.push_back("t150");
		source.push_back("#time \"5/4\"");
		source.push_back("#sq1 { l4 d !end }");
		source.push_back("#sq2 { l4 f !end }");
		source.push_back("#tri { l4 d !end }");
		source.push_back("#noise { l4 p1 !end }");
		return source;
	}

	void replace_once(std::vector<std::string>& source, const std::string& from,
		const std::string& to) {
		for (auto& line : source) {
			const auto position{ line.find(from) };
			if (position != std::string::npos) {
				line.replace(position, from.size(), to);
				return;
			}
		}
		throw std::runtime_error("test fixture replacement target not found");
	}

	void test_compiles_authored_worldscore() {
		const auto kit{ fm::pmusic::compile(family_kit()) };
		require(kit.families.size() == 16 && kit.routes.size() == 96
			&& kit.event_routes.size() == 3 && kit.phrases.size() == 98,
			"family, route, event, or node count changed");
		require(kit.phrases.front().index == 16
			&& kit.phrases[95].index == 111
			&& kit.phrases[96].index == 112
			&& kit.phrases[97].index == 113,
			"authored node planes changed");
		require(std::none_of(kit.phrases.begin(), kit.phrases.end(),
			[](const auto& phrase) { return phrase.kind == "stock"; }),
			"compiler emitted a stock node");

		const auto& pool{ kit.families[0].states[0].pool };
		require(pool.size() == 2
			&& pool[0].phrase == "intro-establish-a"
			&& pool[0].begin == 0 && pool[0].end == 128
			&& pool[1].phrase == "intro-establish-b"
			&& pool[1].begin == 128 && pool[1].end == 256,
			"authored A/B pool lowering changed");

		const auto authored{ [&](const std::string& id) {
			const auto phrase{ std::find_if(kit.phrases.begin(), kit.phrases.end(),
				[&](const auto& candidate) { return candidate.id == id; }) };
			return phrase != kit.phrases.end() && phrase->kind == "authored";
		} };
		for (const auto& family : kit.families)
			for (const auto& state : family.states)
				for (const auto& entry : state.pool)
					require(authored(entry.phrase),
						"replacement state pool references a non-authored node");
		for (const auto& route : kit.routes)
			for (const auto& entry : route.pool)
				require(authored(entry.phrase),
					"replacement family route references a non-authored node");
		for (const auto& route : kit.event_routes)
			for (const auto& entry : route.pool)
				require(authored(entry.phrase),
					"replacement event route references a non-authored node");

		const auto json{ fm::pmusic::to_json(kit) };
		require(json == fm::pmusic::compile_json(family_kit())
			&& json.find("\"kind\": \"stock\"") == std::string::npos
			&& json.find("\"phrase_count\": 98") != std::string::npos,
			"authored-only JSON contradicted its policy");
	}

	template<typename Mutator>
	void require_rejection(const std::string& expected, Mutator mutate) {
		auto source{ family_kit() };
		mutate(source);
		bool rejected{ false };
		try {
			(void)fm::pmusic::compile(source,
				"fixtures/worldscore-invalid.mml");
		}
		catch (const std::runtime_error& error) {
			rejected = true;
			require(std::string(error.what()).find(expected) != std::string::npos,
				"wrong rejection: " + std::string(error.what()));
		}
		require(rejected, "compiler accepted invalid source: " + expected);
	}

	void test_rejections() {
		{
			auto source{ family_kit() };
			replace_once(source, "t90", "T90");
			const auto kit{ fm::pmusic::compile(source) };
			require(kit.phrases.size() == 98,
				"uppercase song-level tempo was not accepted by the parser");
		}
		{
			auto source{ family_kit() };
			replace_once(source, "meter=4/4",
				"meter=4/4 timing=tracker");
			replace_once(source, "#sq1 { l4 c !end }",
				"#sq1 { l~6 c !end }");
			replace_once(source, "#sq2 { l4 e !end }",
				"#sq2 { l~6 e !end }");
			replace_once(source, "#tri { l4 c !end }",
				"#tri { l~6 c !end }");
			replace_once(source, "#noise { l4 p1 !end }",
				"#noise { l~6 p1 !end }");
			const auto kit{ fm::pmusic::compile(source) };
			require(kit.phrases.front().ticks == 6,
				"tracker timing did not preserve exact raw ticks");
		}
		require_rejection("phrase timing must be tracker", [](auto& source) {
			replace_once(source, "meter=4/4", "meter=4/4 timing=approximate");
		});
		require_rejection("exactly one explicit song-level tempo", [](auto& source) {
			replace_once(source, "t90", "");
		});
		require_rejection("exactly one explicit song-level tempo", [](auto& source) {
			const auto tempo{ std::find(source.begin(), source.end(), "t100") };
			if (tempo == source.end())
				throw std::runtime_error("test fixture t100 not found");
			*tempo = "";
			const auto channel{ std::find_if(tempo, source.end(), [](const auto& line) {
				return line == "#sq1 { l4 c !end }";
			}) };
			if (channel == source.end())
				throw std::runtime_error("test fixture SQ1 not found");
			const auto position{ static_cast<std::size_t>(
				std::distance(source.begin(), channel)) };
			source[position] = "#SQ1 {";
			source.insert(source.begin() + static_cast<std::ptrdiff_t>(position + 1),
				"t100 l4 c !end }");
		});
		require_rejection("before its channels", [](auto& source) {
			const auto tempo{ std::find(source.begin(), source.end(), "t100") };
			if (tempo == source.end())
				throw std::runtime_error("test fixture t100 not found");
			*tempo = "";
			const auto noise{ std::find(tempo, source.end(),
				"#noise { l4 p1 !end }") };
			if (noise == source.end())
				throw std::runtime_error("test fixture Noise channel not found");
			const auto position{ static_cast<std::size_t>(
				std::distance(source.begin(), noise)) };
			source.insert(source.begin() + static_cast<std::ptrdiff_t>(position + 1),
				"t100");
		});
		require_rejection("exactly one explicit song-level tempo", [](auto& source) {
			const auto tempo{ std::find(source.begin(), source.end(), "t100") };
			if (tempo == source.end())
				throw std::runtime_error("test fixture t100 not found");
			source.insert(tempo, "t100");
		});
		require_rejection("exactly one explicit song-level tempo", [](auto& source) {
			const auto tempo{ std::find(source.begin(), source.end(), "t100") };
			if (tempo == source.end())
				throw std::runtime_error("test fixture t100 not found");
			const auto noise{ std::find(tempo, source.end(),
				"#noise { l4 p1 !end }") };
			if (noise == source.end())
				throw std::runtime_error("test fixture Noise channel not found");
			const auto position{ static_cast<std::size_t>(
				std::distance(source.begin(), noise)) };
			source.insert(source.begin() + static_cast<std::ptrdiff_t>(position + 1),
				"t100");
		});
		require_rejection("channel-local tempo changes", [](auto& source) {
			replace_once(source, "#sq1 { l4 c !end }",
				"#sq1 { t180 l4 c !end }");
			replace_once(source, "#sq2 { l4 e !end }",
				"#sq2 { t180 l4 e !end }");
			replace_once(source, "#tri { l4 c !end }",
				"#tri { t180 l4 c !end }");
			replace_once(source, "#noise { l4 p1 !end }",
				"#noise { t180 l4 p1 !end }");
		});
		require_rejection("raw tick durations", [](auto& source) {
			replace_once(source, "#sq1 { l4 c !end }",
				"#sq1 { l~15 c !end }");
		});
		require_rejection("raw tick durations", [](auto& source) {
			replace_once(source, "#sq1 { l4 c !end }",
				"#sq1 { c~15 !end }");
		});
		require_rejection("raw tick durations", [](auto& source) {
			replace_once(source, "#sq1 { l4 r !end }",
				"#sq1 { r~15 !end }");
		});
		require_rejection("meter metadata does not match", [](auto& source) {
			replace_once(source, "#time \"4/4\"", "#time \"3/4\"");
		});
		require_rejection("lower-case hex", [](auto& source) {
			replace_once(source,
				"621d8fd742eda6b70317a4390cd9d8cb217a30487634e614c8ed261cbfa238da",
				"621D8FD742EDA6B70317A4390CD9D8CB217A30487634E614C8ED261CBFA238DA");
		});
		require_rejection("unknown key 'stock_playback'",
			[](auto& source) {
				replace_once(source, "quantum=1",
					"quantum=1 stock_playback=random");
			});
		require_rejection("@pmusic-stock is not supported",
			[](auto& source) {
				source.insert(source.begin() + 3,
					"; @pmusic-stock id=legacy family=intro states=\"establish\" weight=1 entry_join=intro-loop exit_join=intro-loop song=1");
			});
		require_rejection("weight sum 257 exceeds 256", [](auto& source) {
			replace_once(source,
				"id=intro-establish-a role=loop family=intro states=\"establish\" weight=2",
				"id=intro-establish-a role=loop family=intro states=\"establish\" weight=255");
		});
	}

}

int main(int argc, char** argv) {
	try {
		if (argc == 3 && std::string(argv[1]) == "--write-fixture") {
			std::ofstream output{ argv[2] };
			if (!output)
				throw std::runtime_error("could not create procedural music fixture");
			for (const auto& line : family_kit())
				output << line << '\n';
			if (!output)
				throw std::runtime_error("could not write procedural music fixture");
			return 0;
		}
		if (argc != 1)
			throw std::runtime_error(
				"usage: procedural_music_compiler [--write-fixture PATH]");
		test_compiles_authored_worldscore();
		test_rejections();
		std::cout << "procedural music compiler: ok\n";
		return 0;
	}
	catch (const std::exception& error) {
		std::cerr << "procedural music compiler: " << error.what() << '\n';
		return 1;
	}
}

#ifndef FM_PROCEDURAL_MUSIC_H
#define FM_PROCEDURAL_MUSIC_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using byte = unsigned char;

namespace fm::pmusic {

	constexpr std::size_t MAX_TITLE_CODEPOINTS{ 96 };
	constexpr std::uint64_t MAX_PHRASE_TICKS{ 65535 };
	constexpr std::size_t FAMILY_COUNT{ 16 };
	constexpr std::size_t FAMILY_STATE_COUNT{ 3 };
	constexpr std::size_t MAX_FAMILY_PHRASES{ 128 };
	constexpr std::size_t MAX_FAMILY_MUSIC_BYTES{ 65535 };
	constexpr std::size_t MAX_FAMILY_EVENT_ROUTES{ 32 };
	constexpr std::size_t MAX_FAMILY_CORE_NODES{ 96 };
	constexpr std::size_t MAX_FAMILY_EVENT_NODES{ 16 };
	static_assert(FAMILY_COUNT + MAX_FAMILY_CORE_NODES
		+ MAX_FAMILY_EVENT_NODES == MAX_FAMILY_PHRASES);
	constexpr std::size_t MAX_CORE_NODES_PER_FAMILY{ 8 };
	constexpr std::size_t MAX_NAMED_JOINS_PER_FAMILY{ 16 };
	constexpr std::size_t MAX_LOCAL_BRIDGE_CANDIDATES{ 64 };

	struct PoolBucket {
		std::string phrase;
		int begin;
		int end;
	};

	struct TempoGrid {
		int bpm_numerator;
		int bpm_denominator;
		int beats_per_bar;
		int beat_unit;
		int quantum_ticks;
	};

	struct FamilyState {
		std::size_t index;
		std::string id;
		std::string section;
		int intensity;
		std::vector<PoolBucket> pool;
	};

	struct MusicFamily {
		std::size_t index;
		std::string id;
		int stock_song;
		std::string default_state;
		bool terminal;
		std::vector<FamilyState> states;
	};

	struct FamilyRoute {
		std::string family;
		std::string from_join;
		std::string to_state;
		std::string kind;
		std::vector<PoolBucket> pool;
	};

	struct EventLanding {
		std::string mode;
		std::optional<std::string> family;
		std::optional<std::string> state;
	};

	struct FamilyEventRoute {
		std::size_t event_index;
		std::string event;
		std::string timing;
		std::string source_family;
		std::string from_join;
		std::string kind;
		EventLanding landing;
		std::vector<PoolBucket> pool;
	};

	struct FamilyPhrase {
		std::size_t index;
		std::string id;
		std::string kind;
		std::string role;
		std::string family;
		std::vector<std::string> states;
		std::optional<std::string> from_join;
		std::optional<std::string> to_state;
		std::optional<std::string> event;
		int weight;
		std::string entry_join;
		std::string exit_join;
		int song;
		std::optional<int> stock_song;
		std::optional<TempoGrid> timing;
		std::uint64_t ticks;
		std::array<std::vector<byte>, 4> channels;
	};

	struct Policy {
		std::string default_family;
		std::string default_state;
		std::string repeat;
		std::string boundary;
		std::string clock;
		int quantum_ticks;
	};

	struct Kit {
		std::string id;
		std::string title;
		std::string style_profile_id;
		std::string style_profile_sha256;
		Policy policy;
		std::vector<MusicFamily> families;
		std::vector<FamilyRoute> routes;
		std::vector<FamilyEventRoute> event_routes;
		std::vector<FamilyPhrase> phrases;
	};

	struct Selection {
		std::string phrase;
		std::uint16_t next_seed;
		byte bucket;
	};

	Kit compile(const std::vector<std::string>& p_mml,
		const std::string& p_source_name = "<memory>");
	std::string to_json(const Kit& p_kit);
	std::string compile_json(const std::vector<std::string>& p_mml,
		const std::string& p_source_name = "<memory>");

	// Runtime preview/reference helper. The authored kit deliberately carries no
	// seed; callers choose a fixed, password-derived, or game-derived nonzero
	// seed and retain the returned state between phrase boundaries.
}

#endif

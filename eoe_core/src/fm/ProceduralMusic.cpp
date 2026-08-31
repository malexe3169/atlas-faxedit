#include "ProceduralMusic.h"
#include "ProceduralMusicDirectiveParser.h"
#include "ProceduralMusicPhraseLowering.h"

#include "common/klib/Kstring.h"
#include "fm/fm_constants.h"
#include "fm/song/MMLEvent.h"
#include "fm/song/MMLSongCollection.h"
#include "fm/song/Parser.h"
#include "fm/song/Tokenizer.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>

namespace {

	using fm::pmusic::detail::Directive;
	using fm::pmusic::detail::ParsedFamilySource;
	using fm::pmusic::detail::SourceError;
	using fm::pmusic::detail::SourceSpan;
	using fm::pmusic::detail::channel_name;
	using fm::pmusic::detail::channel_ticks;
	using fm::pmusic::detail::duration_mismatch_span;
	using fm::pmusic::detail::instruction_bytes;
	using fm::pmusic::detail::parse_songs;
	using fm::pmusic::detail::prepend_canonical_prologue;
	using fm::pmusic::detail::validate_first_timed_event;
	using fm::pmusic::detail::validate_linear_channel;

	struct ExpandedRoute {
		std::string kind;
		std::size_t line;
	};

	constexpr std::array<const char*, fm::pmusic::FAMILY_COUNT> FAMILY_IDS{
		"intro", "dartmoor", "trunk", "branches", "mist", "towers",
		"eolis", "mantra", "towns", "boss", "hourglass", "outro",
		"king", "guru", "shop", "zenis"
	};
	constexpr std::array<const char*, fm::pmusic::FAMILY_STATE_COUNT> STATE_IDS{
		"establish", "drive", "crisis"
	};
	constexpr const char* LOAD_JOIN{ "load" };
	constexpr const char* ANY_FAMILY{ "any" };
	constexpr const char* FAMILY_REPEAT_POLICY{ "compact-no-repeat" };

	[[noreturn]] void fail(SourceSpan span, const std::string& message) {
		throw SourceError(span, message);
	}

	[[noreturn]] void fail(std::size_t line, const std::string& message) {
		fail(SourceSpan{ line, 1 }, message);
	}

	SourceSpan source_span(const fm::MMLSong& song) {
		return {
			static_cast<std::size_t>(std::max(song.source_line, 1)),
			static_cast<std::size_t>(std::max(song.source_column, 1))
		};
	}

	SourceSpan source_span(const fm::MMLChannel& channel) {
		return {
			static_cast<std::size_t>(std::max(channel.source_line, 1)),
			static_cast<std::size_t>(std::max(channel.source_column, 1))
		};
	}

	std::optional<SourceSpan> embedded_source_span(const std::string& message) {
		static const std::regex location{
			R"(line ([0-9]+)\s*,?\s*col(?:umn)?\s*([0-9]+))",
			std::regex::icase
		};
		std::smatch match;
		if (!std::regex_search(message, match, location))
			return std::nullopt;
		try {
			return SourceSpan{
				static_cast<std::size_t>(std::stoull(match[1].str())),
				static_cast<std::size_t>(std::stoull(match[2].str()))
			};
		}
		catch (...) {
			return std::nullopt;
		}
	}

	std::string format_source_error(const std::string& source_name,
		SourceSpan span, const std::string& message) {
		const std::string label{ message.starts_with("PMusic")
			? message : "PMusic: " + message };
		return std::format("{}:{}:{}: {}",
			source_name.empty() ? "<memory>" : source_name,
			span.line, span.column, label);
	}

	bool is_key_start(char value) {
		return value >= 'a' && value <= 'z';
	}

	bool is_key_char(char value) {
		return (value >= 'a' && value <= 'z')
			|| (value >= '0' && value <= '9')
			|| value == '_' || value == '-';
	}

	bool is_id(const std::string& value) {
		if (value.empty() || value.size() > 32 || !is_key_start(value.front()))
			return false;
		return std::all_of(value.begin() + 1, value.end(), is_key_char);
	}

	std::size_t utf8_codepoint_count(const std::string& value, std::size_t line) {
		std::size_t count{ 0 };
		for (std::size_t cursor{ 0 }; cursor < value.size(); ++count) {
			const auto lead{ static_cast<unsigned char>(value[cursor++]) };
			if (lead <= 0x7f)
				continue;

			std::size_t continuation_count{ 0 };
			std::uint32_t codepoint{ 0 };
			std::uint32_t minimum{ 0 };
			if (lead >= 0xc2 && lead <= 0xdf) {
				continuation_count = 1;
				codepoint = lead & 0x1f;
				minimum = 0x80;
			}
			else if (lead >= 0xe0 && lead <= 0xef) {
				continuation_count = 2;
				codepoint = lead & 0x0f;
				minimum = 0x800;
			}
			else if (lead >= 0xf0 && lead <= 0xf4) {
				continuation_count = 3;
				codepoint = lead & 0x07;
				minimum = 0x10000;
			}
			else
				fail(line, "title must contain valid UTF-8");

			if (cursor + continuation_count > value.size())
				fail(line, "title must contain valid UTF-8");
			for (std::size_t i{ 0 }; i < continuation_count; ++i) {
				const auto next{ static_cast<unsigned char>(value[cursor++]) };
				if ((next & 0xc0) != 0x80)
					fail(line, "title must contain valid UTF-8");
				codepoint = (codepoint << 6) | (next & 0x3f);
			}
			if (codepoint < minimum || codepoint > 0x10ffff
				|| (codepoint >= 0xd800 && codepoint <= 0xdfff))
				fail(line, "title must contain valid UTF-8");
		}
		return count;
	}

	void require_quoted_keys(const Directive& directive,
		const std::set<std::string>& quoted_keys) {
		for (const auto& entry : directive.values) {
			const auto& key{ entry.first };
			const bool quoted{ directive.quoted_keys.contains(key) };
			if (quoted_keys.contains(key) && !quoted)
				fail(directive.line, std::format(
					"{} key '{}' must use a quoted JSON string", directive.name, key));
			if (!quoted_keys.contains(key) && quoted)
				fail(directive.line, std::format(
					"{} key '{}' must be unquoted", directive.name, key));
		}
	}

	void require_keys(const Directive& directive,
		const std::set<std::string>& required,
		const std::set<std::string>& optional = {}) {
		for (const auto& key : required)
			if (!directive.values.contains(key))
				fail(directive.line, std::format("{} requires key '{}'",
					directive.name, key));

		for (const auto& [key, value] : directive.values)
			if (!required.contains(key) && !optional.contains(key))
				fail(directive.line, std::format("unknown key '{}' for {}",
					key, directive.name));
	}

	int parse_integer(const Directive& directive, const std::string& key,
		int minimum, int maximum) {
		const auto& value{ directive.values.at(key) };
		if (value.empty() || !std::all_of(value.begin(), value.end(), [](char ch) {
			return ch >= '0' && ch <= '9';
			}))
			fail(directive.line, std::format("'{}' must be a decimal integer", key));

		long long parsed{ 0 };
		try {
			parsed = std::stoll(value);
		}
		catch (...) {
			fail(directive.line, std::format("'{}' is outside the supported range", key));
		}
		if (parsed < minimum || parsed > maximum)
			fail(directive.line, std::format("'{}' must be between {} and {}",
				key, minimum, maximum));
		return static_cast<int>(parsed);
	}

	std::pair<int, int> parse_fraction(const Directive& directive,
		const std::string& key, int numerator_maximum, int denominator_maximum) {
		const auto& value{ directive.values.at(key) };
		const auto slash{ value.find('/') };
		if (slash == std::string::npos || slash != value.rfind('/')
			|| slash == 0 || slash + 1 == value.size())
			fail(directive.line, std::format(
				"'{}' must be two decimal integers separated by '/'", key));

		Directive numerator{ directive.name,
			{ { key, value.substr(0, slash) } }, {}, directive.line };
		Directive denominator{ directive.name,
			{ { key, value.substr(slash + 1) } }, {}, directive.line };
		return {
			parse_integer(numerator, key, 1, numerator_maximum),
			parse_integer(denominator, key, 1, denominator_maximum)
		};
	}

	void require_id(const Directive& directive, const std::string& key,
		bool allow_wildcard = false) {
		const auto& value{ directive.values.at(key) };
		if ((!allow_wildcard || value != "*") && !is_id(value))
			fail(directive.line, std::format(
				"'{}' must match [a-z][a-z0-9_-]{{0,31}}{}",
				key, allow_wildcard ? " or be '*'" : ""));
	}

	std::size_t family_index(const std::string& value, std::size_t line,
		const std::string& label = "family") {
		const auto found{ std::find_if(FAMILY_IDS.begin(), FAMILY_IDS.end(),
			[&](const char* candidate) { return value == candidate; }) };
		if (found == FAMILY_IDS.end())
			fail(line, std::format("unknown {} '{}'", label, value));
		return static_cast<std::size_t>(found - FAMILY_IDS.begin());
	}

	std::size_t state_index(const std::string& value, std::size_t line,
		const std::string& label = "state") {
		const auto found{ std::find_if(STATE_IDS.begin(), STATE_IDS.end(),
			[&](const char* candidate) { return value == candidate; }) };
		if (found == STATE_IDS.end())
			fail(line, std::format("unknown {} '{}'", label, value));
		return static_cast<std::size_t>(found - STATE_IDS.begin());
	}

	bool parse_boolean(const Directive& directive, const std::string& key) {
		const auto& value{ directive.values.at(key) };
		if (value == "true")
			return true;
		if (value == "false")
			return false;
		fail(directive.line, std::format("'{}' must be true or false", key));
	}

	std::vector<std::string> parse_id_list(const Directive& directive,
		const std::string& key) {
		const auto& value{ directive.values.at(key) };
		std::vector<std::string> result;
		std::size_t begin{ 0 };
		while (begin <= value.size()) {
			const auto comma{ value.find(',', begin) };
			const auto token{ klib::str::trim(value.substr(begin,
				comma == std::string::npos ? std::string::npos : comma - begin)) };
			if (token.empty() || !is_id(token))
				fail(directive.line, std::format(
					"'{}' must be a comma-separated list of identifiers", key));
			result.push_back(token);
			if (comma == std::string::npos)
				break;
			begin = comma + 1;
		}
		return result;
	}

	bool is_lower_sha256(const std::string& value) {
		return value.size() == 64 && std::all_of(value.begin(), value.end(),
			[](char ch) { return (ch >= '0' && ch <= '9')
				|| (ch >= 'a' && ch <= 'f'); });
	}

	void reject_title_controls(const Directive& directive) {
		for (const unsigned char ch : directive.values.at("title"))
			if (ch < 0x20 || ch == 0x7f)
				fail(directive.line,
					"v2 title must not contain terminal control characters");
	}

	std::vector<fm::pmusic::PoolBucket> make_family_pool(
		std::vector<const fm::pmusic::FamilyPhrase*> entries) {
		std::sort(entries.begin(), entries.end(), [](const auto* lhs, const auto* rhs) {
			return lhs->index < rhs->index;
		});
		entries.erase(std::unique(entries.begin(), entries.end(),
			[](const auto* lhs, const auto* rhs) { return lhs->id == rhs->id; }),
			entries.end());
		if (entries.empty())
			throw std::runtime_error("PMusic: family pool has no selectable phrases");
		int total_weight{ 0 };
		for (const auto* entry : entries)
			total_weight += entry->weight;
		if (total_weight > 256)
			throw std::runtime_error(std::format(
				"PMusic: family pool weight sum {} exceeds 256", total_weight));

		std::vector<fm::pmusic::PoolBucket> result;
		int cursor{ 0 };
		int cumulative{ 0 };
		for (const auto* entry : entries) {
			cumulative += entry->weight;
			const int end{ (256 * cumulative + total_weight - 1) / total_weight };
			if (end <= cursor)
				throw std::runtime_error(
					"PMusic: family pool compiled a zero-width candidate");
			result.push_back({ entry->id, cursor, end });
			cursor = end;
		}
		if (cursor != 256)
			throw std::runtime_error(
				"PMusic: internal family weighted-pool error");
		return result;
	}


}

fm::pmusic::Kit fm::pmusic::compile(
	const std::vector<std::string>& p_mml,
	const std::string& p_source_name) try {
	const auto source{ fm::pmusic::detail::parse_directives(p_mml) };

	require_keys(source.kit, { "version", "id", "title" });
	require_quoted_keys(source.kit, { "title" });
	if (parse_integer(source.kit, "version", 2, 2) != 2)
		fail(source.kit.line, "family compiler requires kit version 2");
	require_id(source.kit, "id");
	const auto title_length{ utf8_codepoint_count(
		source.kit.values.at("title"), source.kit.line) };
	if (title_length == 0 || title_length > MAX_TITLE_CODEPOINTS)
		fail(source.kit.line, std::format(
			"title must contain between 1 and {} Unicode code points",
			MAX_TITLE_CODEPOINTS));
	reject_title_controls(source.kit);

	require_keys(source.style, { "id", "sha256" });
	require_quoted_keys(source.style, { "sha256" });
	require_id(source.style, "id");
	if (!is_lower_sha256(source.style.values.at("sha256")))
		fail(source.style.line,
			"style sha256 must contain exactly 64 lower-case hex digits");

	require_keys(source.policy,
		{ "default_family", "default_state", "repeat", "boundary",
			"clock", "quantum" });
	require_quoted_keys(source.policy, {});
	require_id(source.policy, "default_family");
	require_id(source.policy, "default_state");
	family_index(source.policy.values.at("default_family"),
		source.policy.line, "default family");
	state_index(source.policy.values.at("default_state"),
		source.policy.line, "default state");
	if (source.policy.values.at("repeat") != FAMILY_REPEAT_POLICY)
		fail(source.policy.line, std::format(
			"v2 policy repeat must be '{}'", FAMILY_REPEAT_POLICY));
	if (source.policy.values.at("boundary") != "phrase")
		fail(source.policy.line, "v2 policy boundary must be 'phrase'");
	if (source.policy.values.at("clock") != "ntsc")
		fail(source.policy.line, "v2 policy clock must be 'ntsc'");
	const int quantum_ticks{ parse_integer(source.policy, "quantum", 1, 255) };

	Kit result{
		source.kit.values.at("id"),
		source.kit.values.at("title"),
		source.style.values.at("id"),
		source.style.values.at("sha256"),
		{
			source.policy.values.at("default_family"),
			source.policy.values.at("default_state"),
			source.policy.values.at("repeat"),
			source.policy.values.at("boundary"),
			source.policy.values.at("clock"),
			quantum_ticks
		},
		{}, {}, {}, {}
	};

	if (source.families.size() != FAMILY_COUNT)
		fail(source.kit.line, "v2 must declare all sixteen families");
	for (std::size_t i{ 0 }; i < source.families.size(); ++i) {
		const auto& directive{ source.families[i] };
		require_keys(directive,
			{ "id", "stock_song", "default_state", "terminal" });
		require_quoted_keys(directive, {});
		require_id(directive, "id");
		require_id(directive, "default_state");
		const auto& id{ directive.values.at("id") };
		if (id != FAMILY_IDS[i])
			fail(directive.line, std::format(
				"family {} must be declared as '{}'", i, FAMILY_IDS[i]));
		const int stock_song{ parse_integer(directive, "stock_song", 1, 16) };
		if (stock_song != static_cast<int>(i + 1))
			fail(directive.line, std::format(
				"family '{}' must map to stock song {}", id, i + 1));
		state_index(directive.values.at("default_state"), directive.line,
			"family default state");
		const bool terminal{ parse_boolean(directive, "terminal") };
		if (terminal != (id == "outro"))
			fail(directive.line, "only the Outro family may be terminal");
		result.families.push_back({ i, id, stock_song,
			directive.values.at("default_state"), terminal, {} });
	}
	if (result.policy.default_family == "outro")
		fail(source.policy.line, "policy default family cannot be terminal Outro");
	const auto policy_family_index{ family_index(result.policy.default_family,
		source.policy.line) };
	if (result.families[policy_family_index].default_state
		!= result.policy.default_state)
		fail(source.policy.line,
			"policy default state must equal the default family's default state");

	if (source.states.size() != FAMILY_COUNT * FAMILY_STATE_COUNT)
		fail(source.kit.line,
			"every v2 family must declare establish, drive, and crisis states");
	for (std::size_t family_no{ 0 }; family_no < FAMILY_COUNT; ++family_no) {
		int prior_intensity{ -1 };
		for (std::size_t state_no{ 0 }; state_no < FAMILY_STATE_COUNT; ++state_no) {
			const auto& directive{
				source.states[family_no * FAMILY_STATE_COUNT + state_no] };
			require_keys(directive,
				{ "family", "id", "section", "intensity" });
			require_quoted_keys(directive, {});
			require_id(directive, "family");
			require_id(directive, "id");
			require_id(directive, "section");
			if (directive.values.at("family") != FAMILY_IDS[family_no]
				|| directive.values.at("id") != STATE_IDS[state_no])
				fail(directive.line, std::format(
					"state declarations must follow canonical family/state order; expected {}:{}",
					FAMILY_IDS[family_no], STATE_IDS[state_no]));
			const int intensity{ parse_integer(directive, "intensity", 0, 255) };
			if (intensity <= prior_intensity)
				fail(directive.line,
					"family state intensities must strictly increase");
			prior_intensity = intensity;
			result.families[family_no].states.push_back({ state_no,
				directive.values.at("id"), directive.values.at("section"),
				intensity, {} });
		}
	}

	if (!source.stocks.empty())
		fail(source.stocks.front().line,
			"@pmusic-stock is not supported; game song numbers are context keys only");
	std::set<std::string> phrase_ids;
	auto collection{ parse_songs(p_mml) };
	if (collection.songs.size() != source.phrases.size())
		fail(source.kit.line, std::format(
			"MML parser found {} songs but metadata mapped {}",
			collection.songs.size(), source.phrases.size()));

	std::size_t total_music_bytes{ 0 };
	std::size_t core_nodes{ 0 };
	std::size_t event_nodes{ 0 };
	bool saw_event_phrase{ false };
	std::map<std::string, std::size_t> phrase_lines;
	for (std::size_t i{ 0 }; i < source.phrases.size(); ++i) {
		const auto& annotation{ source.phrases[i] };
		const auto& directive{ annotation.directive };
		auto& song{ collection.songs[i] };
		if (song.index != annotation.song)
			fail(source_span(song), std::format(
				"MML parser song order differs at #song {}", annotation.song));

		const bool has_event{ directive.values.contains("event") };
		const auto role_it{ directive.values.find("role") };
		if (role_it == directive.values.end())
			fail(directive.line, "@pmusic-phrase requires key 'role'");
		const auto& role{ role_it->second };
		std::set<std::string> required{
			"id", "role", "family", "weight", "entry_join", "exit_join",
			"tempo", "meter"
		};
		if (role == "loop") {
			if (has_event)
				fail(directive.line, "loop phrase cannot name an event");
			required.insert("states");
		}
		else if (role == "bridge" && !has_event) {
			required.insert("from_join");
			required.insert("to_state");
		}
		else if ((role == "bridge" || role == "stinger") && has_event)
			required.insert("event");
		else
			fail(directive.line,
				"phrase role must be loop, local bridge, or event bridge/stinger");
		require_keys(directive, required, { "timing" });
		require_quoted_keys(directive,
			role == "loop" ? std::set<std::string>{ "states" }
				: std::set<std::string>{});
		for (const auto& key : { "id", "family", "entry_join", "exit_join" })
			require_id(directive, key);
		if (has_event)
			require_id(directive, "event");
		const auto& phrase_id{ directive.values.at("id") };
		const bool tracker_timing{ directive.values.contains("timing") };
		if (tracker_timing && directive.values.at("timing") != "tracker")
			fail(directive.line, "phrase timing must be tracker when present");
		if (!phrase_ids.insert(phrase_id).second)
			fail(directive.line, std::format(
				"duplicate phrase id '{}'", phrase_id));
		phrase_lines[phrase_id] = directive.line;
		const auto family_no{ family_index(
			directive.values.at("family"), directive.line) };
		const int weight{ parse_integer(directive, "weight", 1, 255) };
		if (directive.values.at("exit_join") == LOAD_JOIN)
			fail(directive.line, "phrase exit_join cannot use reserved load");
		if (!has_event && directive.values.at("entry_join") == LOAD_JOIN)
			fail(directive.line,
				"entry_join=load is reserved for immediate event phrases");

		std::vector<std::string> states;
		std::optional<std::string> from_join;
		std::optional<std::string> to_state;
		std::optional<std::string> event;
		if (role == "loop") {
			states = parse_id_list(directive, "states");
			if (states.empty() || states.size() > FAMILY_STATE_COUNT)
				fail(directive.line, "loop states must contain 1..3 values");
			std::size_t previous_state{ 0 };
			bool first_state{ true };
			for (const auto& state : states) {
				const auto current{ state_index(state, directive.line) };
				if (!first_state && current <= previous_state)
					fail(directive.line,
						"states must follow establish, drive, crisis order without duplicates");
				first_state = false;
				previous_state = current;
			}
		}
		else if (has_event)
			event = directive.values.at("event");
		else {
			require_id(directive, "from_join");
			require_id(directive, "to_state");
			from_join = directive.values.at("from_join");
			to_state = directive.values.at("to_state");
			state_index(to_state.value(), directive.line, "target state");
			if (directive.values.at("entry_join") != from_join.value())
				fail(directive.line,
					"local bridge entry_join must equal from_join");
		}

		const auto [bpm_numerator, bpm_denominator]{
			parse_fraction(directive, "tempo", 65535, 255) };
		const auto [beats_per_bar, beat_unit]{
			parse_fraction(directive, "meter", 32, 32) };
		if ((beat_unit & (beat_unit - 1)) != 0)
			fail(directive.line, "meter beat unit must be a power of two");
		if (song.song_level_tempo_count != 1)
			fail(source_span(song), std::format(
				"v2 song {} needs exactly one explicit song-level tempo declaration before its channels",
				song.index));
		if (!song.has_explicit_tempo || song.has_post_channel_tempo)
			fail(source_span(song), std::format(
				"v2 song {} needs its explicit song-level tempo declaration before its channels",
				song.index));
		const fm::Fraction expected_tempo{ bpm_numerator, bpm_denominator };
		if (!(song.tempo == expected_tempo))
			fail(source_span(song), std::format(
				"phrase '{}' tempo metadata does not match song tempo",
				phrase_id));
		const auto expected_meter{ std::format("{}/{}", beats_per_bar, beat_unit) };
		if (song.m_time_sig.empty())
			fail(source_span(song), std::format(
				"v2 song {} needs an explicit #time directive", song.index));
		if (song.get_time_sig() != expected_meter)
			fail(source_span(song), std::format(
				"phrase '{}' meter metadata does not match #time",
				phrase_id));

		if (song.channels.size() != 4)
			fail(source_span(song), std::format(
				"PMusic song {}: exactly four channels are required", song.index));
		std::set<fm::ChannelType> channel_types;
		for (const auto& channel : song.channels) {
			if (!(channel.song_tempo == expected_tempo))
				fail(source_span(channel), std::format(
					"PMusic song {} channel {} starts before its declared tempo",
					song.index, channel_name(channel.channel_type)));
			if (!channel_types.insert(channel.channel_type).second)
				fail(source_span(channel), std::format(
					"PMusic song {}: duplicate {} channel", song.index,
					channel_name(channel.channel_type)));
		}
		if (channel_types.size() != 4)
			fail(source_span(song), std::format(
				"PMusic song {}: sq1, sq2, tri, and noise are all required",
				song.index));
		song.sort();

		std::size_t runtime_index{ 0 };
		if (has_event) {
			saw_event_phrase = true;
			if (event_nodes >= MAX_FAMILY_EVENT_NODES)
				fail(directive.line, std::format(
					"v2 permits at most {} event nodes", MAX_FAMILY_EVENT_NODES));
			runtime_index = FAMILY_COUNT + MAX_FAMILY_CORE_NODES + event_nodes++;
		}
		else {
			if (saw_event_phrase)
				fail(directive.line,
					"all non-event core songs must precede event songs");
			if (core_nodes >= MAX_FAMILY_CORE_NODES)
				fail(directive.line, std::format(
					"v2 permits at most {} authored core nodes",
					MAX_FAMILY_CORE_NODES));
			runtime_index = FAMILY_COUNT + core_nodes++;
		}

		FamilyPhrase phrase{
			runtime_index, phrase_id, "authored", role,
			directive.values.at("family"), std::move(states),
			std::move(from_join), std::move(to_state), std::move(event),
			weight, directive.values.at("entry_join"),
			directive.values.at("exit_join"), song.index, std::nullopt,
			TempoGrid{ bpm_numerator, bpm_denominator, beats_per_bar,
				beat_unit, quantum_ticks },
			0, {}
		};
		std::array<std::uint64_t, 4> tick_counts{};
		for (std::size_t channel_index{ 0 }; channel_index < 4; ++channel_index) {
			auto& channel{ song.channels[channel_index] };
			try {
				validate_linear_channel(channel, song.index, true, false,
					tracker_timing);
				auto compiled{ channel.to_bytecode() };
				if (compiled.entrypt_idx != 0 || compiled.instrs.empty()
					|| compiled.instrs.back().opcode_byte
						!= fm::c::MSCRIPT_OPCODE_END)
					throw std::runtime_error(std::format(
						"PMusic song {} channel {}: invalid linear channel output",
						song.index, channel_name(channel.channel_type)));
				for (const auto& instruction : compiled.instrs)
					if (instruction.jump_target.has_value())
						throw std::runtime_error(std::format(
							"PMusic song {} channel {}: jump target is not allowed",
							song.index, channel_name(channel.channel_type)));
				const bool needs_duration{ !validate_first_timed_event(
					compiled.instrs, channel_index == 3, song.index,
					channel_name(channel.channel_type), true) };
				prepend_canonical_prologue(compiled.instrs,
					channel_index, needs_duration);
				tick_counts[channel_index] = channel_ticks(compiled.instrs,
					channel_index == 3, song.index,
					channel_name(channel.channel_type));
				if (channel_index == 3)
					compiled.instrs.back().opcode_byte
						= fm::c::MSCRIPT_OPCODE_RESTART;
				phrase.channels[channel_index]
					= instruction_bytes(compiled.instrs);
				if (channel_index == 3)
					phrase.channels[channel_index].push_back(
						fm::c::MSCRIPT_OPCODE_END);
			}
			catch (const SourceError&) {
				throw;
			}
			catch (const std::runtime_error& error) {
				fail(source_span(channel), error.what());
			}
		}
		if (tick_counts[0] == 0)
			fail(source_span(song.channels[0]), std::format(
				"PMusic song {}: phrase duration must be nonzero", song.index));
		if (!std::all_of(tick_counts.begin() + 1, tick_counts.end(),
			[&](std::uint64_t value) { return value == tick_counts[0]; }))
			fail(duration_mismatch_span(song, tick_counts), std::format(
				"PMusic song {}: channel tick totals differ ({}, {}, {}, {})",
				song.index, tick_counts[0], tick_counts[1],
				tick_counts[2], tick_counts[3]));
		if (tick_counts[0] > MAX_PHRASE_TICKS)
			fail(source_span(song.channels[0]), std::format(
				"PMusic song {}: phrase duration exceeds {} ticks",
				song.index, MAX_PHRASE_TICKS));
		if (tick_counts[0] % static_cast<std::uint64_t>(quantum_ticks) != 0)
			fail(directive.line, std::format(
				"phrase '{}' is off-grid: {} ticks is not divisible by quantum {}",
				phrase_id, tick_counts[0], quantum_ticks));
		for (std::size_t channel_index{ 0 };
			channel_index < phrase.channels.size(); ++channel_index) {
			const auto& bytes{ phrase.channels[channel_index] };
			if (bytes.size() > MAX_FAMILY_MUSIC_BYTES - total_music_bytes)
				fail(source_span(song.channels[channel_index]), std::format(
					"PMusic: compiled family streams exceed {} bytes",
					MAX_FAMILY_MUSIC_BYTES));
			total_music_bytes += bytes.size();
		}
		phrase.ticks = tick_counts[0];
		result.phrases.push_back(std::move(phrase));
		(void)family_no;
	}

	for (std::size_t family_no{ 0 }; family_no < FAMILY_COUNT; ++family_no) {
		const auto& family_id{ result.families[family_no].id };
		const auto core_count{ std::count_if(result.phrases.begin(),
			result.phrases.end(), [&](const FamilyPhrase& phrase) {
				return phrase.family == family_id && !phrase.event.has_value();
			}) };
		if (core_count > static_cast<std::ptrdiff_t>(MAX_CORE_NODES_PER_FAMILY))
			fail(source.families[family_no].line, std::format(
				"family '{}' has {} core nodes; runtime permits at most {}",
				family_id, core_count, MAX_CORE_NODES_PER_FAMILY));
		for (std::size_t state_no{ 0 }; state_no < FAMILY_STATE_COUNT; ++state_no) {
			const std::string state_id{ STATE_IDS[state_no] };
			std::vector<const FamilyPhrase*> candidates;
			bool authored{ false };
			for (const auto& phrase : result.phrases)
				if (phrase.family == family_id && phrase.role == "loop"
					&& std::find(phrase.states.begin(), phrase.states.end(), state_id)
						!= phrase.states.end()) {
					candidates.push_back(&phrase);
					authored = authored || phrase.kind == "authored";
				}
			if (!authored)
				fail(source.states[family_no * FAMILY_STATE_COUNT + state_no].line,
					std::format("family '{}' state '{}' needs authored variation",
						family_id, state_id));
			if (candidates.size() < 2)
				fail(source.states[family_no * FAMILY_STATE_COUNT + state_no].line,
					"compact-no-repeat state pools need at least two phrases");
			try {
				result.families[family_no].states[state_no].pool
					= make_family_pool(std::move(candidates));
			}
			catch (const std::runtime_error& error) {
				fail(source.states[family_no * FAMILY_STATE_COUNT + state_no].line,
					error.what());
			}
		}
	}

	std::array<std::set<std::string>, FAMILY_COUNT> source_joins;
	for (auto& joins : source_joins)
		joins.insert(LOAD_JOIN);
	for (const auto& phrase : result.phrases)
		source_joins[family_index(phrase.family, source.kit.line)].insert(
			phrase.exit_join);
	using RouteKey = std::tuple<std::string, std::string, std::string>;
	std::map<RouteKey, ExpandedRoute> route_overrides;
	for (const auto& directive : source.routes) {
		require_keys(directive,
			{ "family", "from_join", "to_state", "kind" });
		require_quoted_keys(directive, {});
		require_id(directive, "family");
		require_id(directive, "from_join", true);
		require_id(directive, "to_state", true);
		const auto family_no{ family_index(
			directive.values.at("family"), directive.line) };
		const auto& kind{ directive.values.at("kind") };
		if (kind != "direct" && kind != "cut" && kind != "bridge")
			fail(directive.line,
				"v2 route kind must be direct, cut, or bridge");
		std::vector<std::string> joins;
		if (directive.values.at("from_join") == "*") {
			for (const auto& join : source_joins[family_no])
				if (join != LOAD_JOIN)
					joins.push_back(join);
		}
		else {
			const auto& join{ directive.values.at("from_join") };
			if (!source_joins[family_no].contains(join))
				fail(directive.line, std::format(
					"route source join '{}' is not a family exit join", join));
			joins.push_back(join);
		}
		std::vector<std::string> states;
		if (directive.values.at("to_state") == "*")
			for (const auto* state : STATE_IDS)
				states.emplace_back(state);
		else {
			state_index(directive.values.at("to_state"), directive.line,
				"route target state");
			states.push_back(directive.values.at("to_state"));
		}
		for (const auto& join : joins)
			for (const auto& state : states) {
				const RouteKey key{ directive.values.at("family"), join, state };
				const auto existing{ route_overrides.find(key) };
				if (existing != route_overrides.end()
					&& existing->second.kind != kind)
					fail(directive.line, std::format(
						"route {}:{}->{} has conflicting kinds",
						directive.values.at("family"), join, state));
				route_overrides[key] = { kind, directive.line };
			}
	}

	std::map<RouteKey, std::size_t> route_index_by_key;
	std::map<std::string, int> local_owner_counts;
	std::size_t local_candidate_count{ 0 };
	for (std::size_t family_no{ 0 }; family_no < FAMILY_COUNT; ++family_no) {
		const std::string family_id{ FAMILY_IDS[family_no] };
		for (const auto& join : source_joins[family_no])
			for (const auto* state_value : STATE_IDS) {
				const std::string state{ state_value };
				const RouteKey key{ family_id, join, state };
				const auto definition{ route_overrides.find(key) };
				const std::string kind{ definition == route_overrides.end()
					? (join == LOAD_JOIN ? "cut" : "direct")
					: definition->second.kind };
				const std::size_t line{ definition == route_overrides.end()
					? source.families[family_no].line
					: definition->second.line };
				if (join == LOAD_JOIN && kind != "cut")
					fail(line, "load routes must be loop-only cuts");
				if (join != LOAD_JOIN && kind == "cut")
					fail(line, "cut routes are valid only for load");
				std::vector<const FamilyPhrase*> candidates;
				for (const auto& phrase : result.phrases) {
					if (kind == "direct" && phrase.family == family_id
						&& phrase.role == "loop" && phrase.entry_join == join
						&& std::find(phrase.states.begin(), phrase.states.end(), state)
							!= phrase.states.end())
						candidates.push_back(&phrase);
					else if (kind == "cut" && phrase.family == family_id
						&& phrase.role == "loop"
						&& std::find(phrase.states.begin(), phrase.states.end(), state)
							!= phrase.states.end())
						candidates.push_back(&phrase);
					else if (kind == "bridge" && phrase.family == family_id
						&& phrase.role == "bridge" && !phrase.event.has_value()
						&& phrase.from_join == join && phrase.to_state == state)
						candidates.push_back(&phrase);
				}
				if (candidates.empty())
					fail(line, std::format(
						"route {}:{}->{} has no {} candidates{}",
						family_id, join, state, kind,
						kind == "direct" ? "; declare a bridge route" : ""));
				std::vector<PoolBucket> pool;
				try {
					pool = make_family_pool(candidates);
				}
				catch (const std::runtime_error& error) {
					fail(line, error.what());
				}
				if (kind == "direct" && pool.size() == 1
					&& candidates.front()->exit_join == join)
					fail(line, std::format(
						"route {}:{}->{} can immediately repeat its only source phrase",
						family_id, join, state));
				if (kind == "bridge") {
					local_candidate_count += pool.size();
					for (const auto& entry : pool)
						++local_owner_counts[entry.phrase];
				}
				route_index_by_key[key] = result.routes.size();
				result.routes.push_back({ family_id, join, state,
					kind, std::move(pool) });
			}
	}
	if (result.routes.size() > 432)
		fail(source.kit.line, "v2 route table exceeds 432 rows");
	if (local_candidate_count > MAX_LOCAL_BRIDGE_CANDIDATES)
		fail(source.kit.line, std::format(
			"local bridge routes contain {} candidates; runtime permits {}",
			local_candidate_count, MAX_LOCAL_BRIDGE_CANDIDATES));

	for (const auto& phrase : result.phrases) {
		if (phrase.role != "bridge" || phrase.event.has_value())
			continue;
		if (local_owner_counts[phrase.id] != 1)
			fail(phrase_lines.at(phrase.id), std::format(
				"local bridge '{}' must belong to exactly one route", phrase.id));
		const RouteKey landing_key{ phrase.family, phrase.exit_join,
			phrase.to_state.value() };
		const auto landing{ route_index_by_key.find(landing_key) };
		if (landing == route_index_by_key.end()
			|| result.routes[landing->second].kind == "bridge")
			fail(phrase_lines.at(phrase.id), std::format(
				"local bridge '{}' does not land on a direct/cut route",
				phrase.id));
	}

	struct EventSignature {
		std::size_t index;
		std::string timing;
		std::string kind;
		std::string landing;
		std::string landing_family;
		std::string landing_state;
	};
	std::map<std::string, EventSignature> event_signatures;
	std::set<std::tuple<std::string, std::string, std::string>> event_keys;
	std::map<std::string, int> event_owner_counts;
	for (const auto& directive : source.event_routes) {
		const auto landing_it{ directive.values.find("landing") };
		if (landing_it == directive.values.end())
			fail(directive.line, "@pmusic-event-route requires key 'landing'");
		const bool fixed{ landing_it->second == "fixed" };
		require_keys(directive,
			{ "event", "timing", "source_family", "from_join", "kind",
				"landing" },
			fixed ? std::set<std::string>{ "landing_family", "landing_state" }
				: std::set<std::string>{});
		require_quoted_keys(directive, {});
		for (const auto& key : { "event", "source_family" })
			require_id(directive, key);
		require_id(directive, "from_join", true);
		const auto& event{ directive.values.at("event") };
		const auto& timing{ directive.values.at("timing") };
		const auto& source_family{ directive.values.at("source_family") };
		const auto& kind{ directive.values.at("kind") };
		const auto& landing_mode{ directive.values.at("landing") };
		if (timing != "immediate" && timing != "boundary")
			fail(directive.line,
				"event timing must be immediate or boundary");
		if (source_family != ANY_FAMILY)
			family_index(source_family, directive.line, "event source family");
		if (source_family == "outro")
			fail(directive.line,
				"event source family cannot be terminal Outro");
		if (kind != "cut" && kind != "bridge" && kind != "stinger")
			fail(directive.line,
				"event kind must be cut, bridge, or stinger");
		if (landing_mode != "fixed" && landing_mode != "requested"
			&& landing_mode != "return")
			fail(directive.line,
				"event landing must be fixed, requested, or return");
		std::string landing_family;
		std::string landing_state;
		if (fixed) {
			if (!directive.values.contains("landing_family")
				|| !directive.values.contains("landing_state"))
				fail(directive.line,
					"fixed event landing requires landing_family and landing_state");
			require_id(directive, "landing_family");
			require_id(directive, "landing_state");
			landing_family = directive.values.at("landing_family");
			landing_state = directive.values.at("landing_state");
			family_index(landing_family, directive.line, "landing family");
			state_index(landing_state, directive.line, "landing state");
		}
		if (timing == "immediate") {
			if (directive.values.at("from_join") != LOAD_JOIN)
				fail(directive.line,
					"immediate events must use from_join=load");
			if (!fixed)
				fail(directive.line,
					"immediate events require a fixed landing");
		}
		else {
			if (source_family == ANY_FAMILY)
				fail(directive.line,
					"boundary events require one exact source family");
			if (directive.values.at("from_join") == LOAD_JOIN)
				fail(directive.line,
					"boundary events cannot use from_join=load");
		}
		if (kind == "cut" && (timing != "immediate" || !fixed))
			fail(directive.line,
				"cut events must be immediate fixed landings");
		if ((landing_mode == "requested" || landing_mode == "return")
			&& timing != "boundary")
			fail(directive.line,
				"requested/return landings require boundary timing");

		auto signature_it{ event_signatures.find(event) };
		if (signature_it == event_signatures.end()) {
			signature_it = event_signatures.emplace(event, EventSignature{
				event_signatures.size(), timing, kind, landing_mode,
				landing_family, landing_state }).first;
		}
		else {
			const auto& signature{ signature_it->second };
			if (signature.timing != timing || signature.kind != kind
				|| signature.landing != landing_mode
				|| signature.landing_family != landing_family
				|| signature.landing_state != landing_state)
				fail(directive.line, std::format(
					"all routes for event '{}' must share timing, kind, and landing",
					event));
		}

		std::vector<std::string> joins;
		if (directive.values.at("from_join") == "*") {
			if (timing != "boundary")
				fail(directive.line,
					"from_join=* is valid only for boundary events");
			const auto source_no{ family_index(source_family, directive.line) };
			for (const auto& join : source_joins[source_no])
				if (join != LOAD_JOIN)
					joins.push_back(join);
		}
		else
			joins.push_back(directive.values.at("from_join"));
		if (joins.empty())
			fail(directive.line,
				"event wildcard does not match a source join");

		for (const auto& join : joins) {
			if (timing == "boundary") {
				const auto source_no{ family_index(source_family, directive.line) };
				if (!source_joins[source_no].contains(join))
					fail(directive.line, std::format(
						"event source join '{}' is not a family exit join", join));
			}
			const auto key{ std::make_tuple(event, source_family, join) };
			if (!event_keys.insert(key).second)
				fail(directive.line, std::format(
					"duplicate event route {}:{}:{}",
					event, source_family, join));
			std::vector<const FamilyPhrase*> candidates;
			for (const auto& phrase : result.phrases) {
				if (kind == "cut") {
					if (phrase.family == landing_family && phrase.role == "loop"
						&& std::find(phrase.states.begin(), phrase.states.end(),
							landing_state) != phrase.states.end())
						candidates.push_back(&phrase);
				}
				else if (phrase.event == event && phrase.role == kind
					&& phrase.entry_join == join
					&& ((timing == "boundary" && phrase.family == source_family)
						|| (timing == "immediate"
							&& phrase.family == landing_family)))
					candidates.push_back(&phrase);
			}
			if (candidates.empty())
				fail(directive.line, std::format(
					"event route {}:{}:{} has no {} candidates",
					event, source_family, join, kind));
			std::vector<PoolBucket> pool;
			try {
				pool = make_family_pool(candidates);
			}
			catch (const std::runtime_error& error) {
				fail(directive.line, error.what());
			}
			if (kind != "cut")
				for (const auto& entry : pool)
					++event_owner_counts[entry.phrase];
			EventLanding landing{ landing_mode, std::nullopt, std::nullopt };
			if (fixed) {
				landing.family = landing_family;
				landing.state = landing_state;
			}
			result.event_routes.push_back({ signature_it->second.index,
				event, timing, source_family, join, kind,
				std::move(landing), std::move(pool) });
		}
	}
	if (result.event_routes.size() > MAX_FAMILY_EVENT_ROUTES)
		fail(source.kit.line, std::format(
			"v2 permits at most {} expanded event routes",
			MAX_FAMILY_EVENT_ROUTES));
	std::sort(result.event_routes.begin(), result.event_routes.end(),
		[](const FamilyEventRoute& lhs, const FamilyEventRoute& rhs) {
			const auto source_order{ [](const std::string& family) {
				if (family == ANY_FAMILY)
					return -1;
				const auto found{ std::find_if(FAMILY_IDS.begin(), FAMILY_IDS.end(),
					[&](const char* value) { return family == value; }) };
				return static_cast<int>(found - FAMILY_IDS.begin());
			} };
			return std::make_tuple(lhs.event_index, source_order(lhs.source_family),
				lhs.from_join) < std::make_tuple(rhs.event_index,
					source_order(rhs.source_family), rhs.from_join);
		});

	for (const auto& [event, signature] : event_signatures)
		for (const auto* family : FAMILY_IDS) {
			const auto matches{ std::count_if(result.event_routes.begin(),
				result.event_routes.end(), [&](const FamilyEventRoute& route) {
					return route.event == event && route.timing == "immediate"
						&& (route.source_family == ANY_FAMILY
							|| route.source_family == family);
				}) };
			if (matches > 1)
				fail(source.kit.line, std::format(
					"event '{}' has ambiguous immediate routes for family '{}'",
					event, family));
		}

	for (const auto& phrase : result.phrases) {
		if (phrase.event.has_value()) {
			if (event_owner_counts[phrase.id] != 1)
				fail(phrase_lines.at(phrase.id), std::format(
					"event phrase '{}' must belong to exactly one route", phrase.id));
			const auto owner{ std::find_if(result.event_routes.begin(),
				result.event_routes.end(), [&](const FamilyEventRoute& route) {
					return std::any_of(route.pool.begin(), route.pool.end(),
						[&](const PoolBucket& entry) {
							return entry.phrase == phrase.id;
						});
				}) };
			if (owner == result.event_routes.end())
				fail(phrase_lines.at(phrase.id), "event phrase has no owner");
			if ((owner->timing == "immediate")
				!= (phrase.entry_join == LOAD_JOIN))
				fail(phrase_lines.at(phrase.id), std::format(
					"event phrase '{}' must use entry_join=load exactly for immediate timing",
					phrase.id));
		}
		else if ((phrase.role == "bridge" || phrase.role == "stinger")
			&& local_owner_counts[phrase.id] != 1)
			fail(phrase_lines.at(phrase.id), std::format(
				"transition phrase '{}' has no unique route owner", phrase.id));
	}

	std::array<std::set<std::string>, FAMILY_COUNT> named_joins;
	for (const auto& phrase : result.phrases) {
		const auto family_no{ family_index(phrase.family, source.kit.line) };
		if (phrase.entry_join != LOAD_JOIN)
			named_joins[family_no].insert(phrase.entry_join);
		named_joins[family_no].insert(phrase.exit_join);
	}
	for (const auto& route : result.event_routes)
		if (route.timing == "boundary")
			named_joins[family_index(route.source_family, source.kit.line)].insert(
				route.from_join);
	for (std::size_t i{ 0 }; i < named_joins.size(); ++i)
		if (named_joins[i].size() > MAX_NAMED_JOINS_PER_FAMILY)
			fail(source.families[i].line, std::format(
				"family '{}' uses {} joins; runtime permits at most {}",
				FAMILY_IDS[i], named_joins[i].size(),
				MAX_NAMED_JOINS_PER_FAMILY));

	std::set<std::string> reachable;
	for (const auto& route : result.routes)
		if (route.from_join == LOAD_JOIN)
			for (const auto& entry : route.pool)
				reachable.insert(entry.phrase);
	for (const auto& route : result.event_routes)
		if (route.timing == "immediate")
			for (const auto& entry : route.pool)
				reachable.insert(entry.phrase);
	auto phrase_by_id{ [&](const std::string& id) -> const FamilyPhrase& {
		const auto found{ std::find_if(result.phrases.begin(), result.phrases.end(),
			[&](const FamilyPhrase& phrase) { return phrase.id == id; }) };
		if (found == result.phrases.end())
			throw std::runtime_error("PMusic: internal phrase lookup failed");
		return *found;
	} };
	bool changed{ true };
	while (changed) {
		changed = false;
		const auto snapshot{ reachable };
		for (const auto& phrase_id : snapshot) {
			const auto& phrase{ phrase_by_id(phrase_id) };
			std::set<std::pair<std::string, std::string>> targets;
			if (!phrase.event.has_value() && phrase.role == "loop")
				for (const auto* state : STATE_IDS)
					targets.emplace(phrase.family, state);
			else if (!phrase.event.has_value())
				targets.emplace(phrase.family, phrase.to_state.value());
			else {
				for (const auto& route : result.event_routes) {
					const bool owns{ std::any_of(route.pool.begin(), route.pool.end(),
						[&](const PoolBucket& entry) {
							return entry.phrase == phrase.id;
						}) };
					if (!owns)
						continue;
					if (route.landing.mode == "fixed")
						targets.emplace(route.landing.family.value(),
							route.landing.state.value());
					else if (route.landing.mode == "requested")
						for (const auto* family : FAMILY_IDS)
							for (const auto* state : STATE_IDS)
								targets.emplace(family, state);
					else
						for (const auto& source_phrase : result.phrases)
							if (source_phrase.family == route.source_family
								&& source_phrase.role == "loop"
								&& source_phrase.exit_join == route.from_join)
								for (const auto& state : source_phrase.states)
									targets.emplace(route.source_family, state);
				}
			}
			for (const auto& [target_family, target_state] : targets) {
				const std::string join{ target_family == phrase.family
					? phrase.exit_join : LOAD_JOIN };
				const auto route_it{ route_index_by_key.find(
					RouteKey{ target_family, join, target_state }) };
				if (route_it == route_index_by_key.end())
					throw std::runtime_error(
						"PMusic: internal family continuation lookup failed");
				for (const auto& entry : result.routes[route_it->second].pool)
					if (reachable.insert(entry.phrase).second)
						changed = true;
			}
			if (!phrase.event.has_value() && phrase.role == "loop")
				for (const auto& route : result.event_routes)
					if (route.timing == "boundary"
						&& route.source_family == phrase.family
						&& route.from_join == phrase.exit_join)
						for (const auto& entry : route.pool)
							if (reachable.insert(entry.phrase).second)
								changed = true;
		}
	}
	if (reachable.size() != result.phrases.size()) {
		std::vector<std::string> missing;
		for (const auto& phrase : result.phrases)
			if (!reachable.contains(phrase.id))
				missing.push_back(phrase.id);
		fail(source.kit.line, "family selector graph contains unreachable phrases: "
			+ (missing.empty() ? std::string{} : missing.front()));
	}

	return result;
}
catch (const SourceError& error) {
	throw std::runtime_error(format_source_error(
		p_source_name, error.span, error.what()));
}
catch (const std::runtime_error& error) {
	throw std::runtime_error(format_source_error(p_source_name,
		embedded_source_span(error.what()).value_or(SourceSpan{}), error.what()));
}
catch (const std::exception& error) {
	throw std::runtime_error(format_source_error(p_source_name,
		embedded_source_span(error.what()).value_or(SourceSpan{}), error.what()));
}

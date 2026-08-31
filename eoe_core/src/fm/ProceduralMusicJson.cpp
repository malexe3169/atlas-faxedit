#include "ProceduralMusic.h"

#include "fm/fm_constants.h"

#include <format>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

	constexpr const char* REST_MASK_FEATURE{ "latched-channel-rest-mask-v1" };

	std::string json_escape(const std::string& input) {
		std::ostringstream result;
		for (const unsigned char ch : input) {
			switch (ch) {
			case '"': result << "\\\""; break;
			case '\\': result << "\\\\"; break;
			case '\b': result << "\\b"; break;
			case '\f': result << "\\f"; break;
			case '\n': result << "\\n"; break;
			case '\r': result << "\\r"; break;
			case '\t': result << "\\t"; break;
			default:
				if (ch < 0x20)
					result << "\\u00" << std::hex << std::setw(2)
						<< std::setfill('0') << static_cast<int>(ch) << std::dec;
				else
					result << static_cast<char>(ch);
			}
		}
		return result.str();
	}

	std::string quote(const std::string& input) {
		return "\"" + json_escape(input) + "\"";
	}

	std::string bytes_to_hex(const std::vector<byte>& bytes) {
		std::ostringstream result;
		result << std::hex << std::setfill('0');
		for (const byte value : bytes)
			result << std::setw(2) << static_cast<int>(value);
		return result.str();
	}

	std::string fnv1a64(const std::vector<byte>& bytes) {
		std::uint64_t hash{ 14695981039346656037ull };
		for (const byte value : bytes) {
			hash ^= value;
			hash *= 1099511628211ull;
		}
		return std::format("fnv1a64:{:016x}", hash);
	}

	std::vector<byte> phrase_bytes(const fm::pmusic::FamilyPhrase& phrase) {
		std::vector<byte> result;
		for (const auto& channel : phrase.channels)
			result.insert(result.end(), channel.begin(), channel.end());
		return result;
	}

	void emit_pool(std::ostringstream& output,
		const std::vector<fm::pmusic::PoolBucket>& pool, int indent) {
		output << "[";
		if (!pool.empty())
			output << "\n";
		for (std::size_t i{ 0 }; i < pool.size(); ++i) {
			output << std::string(static_cast<std::size_t>(indent + 2), ' ')
				<< "{\"phrase\": " << quote(pool[i].phrase)
				<< ", \"begin\": " << pool[i].begin
				<< ", \"end\": " << pool[i].end << "}";
			if (i + 1 != pool.size())
				output << ',';
			output << '\n';
		}
		if (!pool.empty())
			output << std::string(static_cast<std::size_t>(indent), ' ');
		output << ']';
	}

}

std::string fm::pmusic::to_json(const Kit& p_kit) {
	std::ostringstream output;
	output << "{\n"
		<< "  \"format\": \"faxanadu-procedural-music-kit-v2\",\n"
		<< "  \"version\": 2,\n"
		<< "  \"kit\": {\n"
		<< "    \"id\": " << quote(p_kit.id) << ",\n"
		<< "    \"title\": " << quote(p_kit.title) << ",\n"
		<< "    \"style_profile\": {\"id\": "
		<< quote(p_kit.style_profile_id) << ", \"sha256\": "
		<< quote(p_kit.style_profile_sha256) << "}\n"
		<< "  },\n"
		<< "  \"policy\": {\n"
		<< "    \"default_family\": " << quote(p_kit.policy.default_family) << ",\n"
		<< "    \"default_state\": " << quote(p_kit.policy.default_state) << ",\n"
		<< "    \"repeat\": " << quote(p_kit.policy.repeat) << ",\n"
		<< "    \"boundary\": " << quote(p_kit.policy.boundary) << ",\n"
		<< "    \"clock\": " << quote(p_kit.policy.clock) << ",\n"
		<< "    \"quantum_ticks\": " << p_kit.policy.quantum_ticks << "\n"
		<< "  },\n"
		<< "  \"families\": [\n";
	for (std::size_t i{ 0 }; i < p_kit.families.size(); ++i) {
		const auto& family{ p_kit.families[i] };
		output << "    {\n"
			<< "      \"index\": " << family.index << ",\n"
			<< "      \"id\": " << quote(family.id) << ",\n"
			<< "      \"stock_song\": " << family.stock_song << ",\n"
			<< "      \"default_state\": " << quote(family.default_state) << ",\n"
			<< "      \"terminal\": " << (family.terminal ? "true" : "false") << ",\n"
			<< "      \"states\": [\n";
		for (std::size_t j{ 0 }; j < family.states.size(); ++j) {
			const auto& state{ family.states[j] };
			output << "        {\n"
				<< "          \"index\": " << state.index << ",\n"
				<< "          \"id\": " << quote(state.id) << ",\n"
				<< "          \"section\": " << quote(state.section) << ",\n"
				<< "          \"intensity\": " << state.intensity << ",\n"
				<< "          \"pool\": ";
			emit_pool(output, state.pool, 10);
			output << "\n        }";
			if (j + 1 != family.states.size())
				output << ',';
			output << '\n';
		}
		output << "      ]\n"
			<< "    }";
		if (i + 1 != p_kit.families.size())
			output << ',';
		output << '\n';
	}
	output << "  ],\n"
		<< "  \"routes\": [\n";
	for (std::size_t i{ 0 }; i < p_kit.routes.size(); ++i) {
		const auto& route{ p_kit.routes[i] };
		output << "    {\n"
			<< "      \"family\": " << quote(route.family) << ",\n"
			<< "      \"from_join\": " << quote(route.from_join) << ",\n"
			<< "      \"to_state\": " << quote(route.to_state) << ",\n"
			<< "      \"kind\": " << quote(route.kind) << ",\n"
			<< "      \"pool\": ";
		emit_pool(output, route.pool, 6);
		output << "\n    }";
		if (i + 1 != p_kit.routes.size())
			output << ',';
		output << '\n';
	}
	output << "  ],\n"
		<< "  \"event_routes\": [";
	if (!p_kit.event_routes.empty())
		output << '\n';
	for (std::size_t i{ 0 }; i < p_kit.event_routes.size(); ++i) {
		const auto& route{ p_kit.event_routes[i] };
		output << "    {\n"
			<< "      \"event_index\": " << route.event_index << ",\n"
			<< "      \"event\": " << quote(route.event) << ",\n"
			<< "      \"timing\": " << quote(route.timing) << ",\n"
			<< "      \"source_family\": " << quote(route.source_family) << ",\n"
			<< "      \"from_join\": " << quote(route.from_join) << ",\n"
			<< "      \"kind\": " << quote(route.kind) << ",\n"
			<< "      \"landing\": {\"mode\": " << quote(route.landing.mode);
		if (route.landing.mode == "fixed")
			output << ", \"family\": " << quote(route.landing.family.value())
				<< ", \"state\": " << quote(route.landing.state.value());
		output << "},\n"
			<< "      \"pool\": ";
		emit_pool(output, route.pool, 6);
		output << "\n    }";
		if (i + 1 != p_kit.event_routes.size())
			output << ',';
		output << '\n';
	}
	if (!p_kit.event_routes.empty())
		output << "  ";
	output << "],\n"
		<< "  \"phrases\": [\n";

	std::vector<byte> all_bytes;
	std::size_t music_bytes{ 0 };
	for (std::size_t i{ 0 }; i < p_kit.phrases.size(); ++i) {
		const auto& phrase{ p_kit.phrases[i] };
		const bool stock{ phrase.kind == "stock" };
		output << "    {\n"
			<< "      \"index\": " << phrase.index << ",\n"
			<< "      \"id\": " << quote(phrase.id) << ",\n"
			<< "      \"kind\": " << quote(phrase.kind) << ",\n"
			<< "      \"role\": " << quote(phrase.role) << ",\n"
			<< "      \"family\": " << quote(phrase.family) << ",\n";
		if (phrase.role == "loop") {
			output << "      \"states\": [";
			for (std::size_t j{ 0 }; j < phrase.states.size(); ++j) {
				if (j != 0)
					output << ", ";
				output << quote(phrase.states[j]);
			}
			output << "],\n";
		}
		else if (phrase.event.has_value())
			output << "      \"event\": " << quote(phrase.event.value()) << ",\n";
		else
			output << "      \"from_join\": " << quote(phrase.from_join.value()) << ",\n"
				<< "      \"to_state\": " << quote(phrase.to_state.value()) << ",\n";
		output << "      \"weight\": " << phrase.weight << ",\n"
			<< "      \"entry_join\": " << quote(phrase.entry_join) << ",\n"
			<< "      \"exit_join\": " << quote(phrase.exit_join);
		if (stock) {
			if (!phrase.stock_song.has_value())
				throw std::runtime_error(std::format(
					"PMusic JSON stock phrase '{}' needs a stock song", phrase.id));
			output << ",\n"
				<< "      \"stock_song\": " << phrase.stock_song.value() << "\n";
		}
		else {
			if (!phrase.timing.has_value())
				throw std::runtime_error(std::format(
					"PMusic JSON authored phrase '{}' needs timing", phrase.id));
			const auto bytes{ phrase_bytes(phrase) };
			all_bytes.insert(all_bytes.end(), bytes.begin(), bytes.end());
			music_bytes += bytes.size();
			const auto& timing{ phrase.timing.value() };
			output << ",\n"
				<< "      \"song\": " << phrase.song << ",\n"
				<< "      \"timing\": {\"bpm_numerator\": "
				<< timing.bpm_numerator << ", \"bpm_denominator\": "
				<< timing.bpm_denominator << ", \"beats_per_bar\": "
				<< timing.beats_per_bar << ", \"beat_unit\": "
				<< timing.beat_unit << "},\n"
				<< "      \"ticks\": " << phrase.ticks << ",\n"
				<< "      \"channels\": [";
			for (std::size_t channel{ 0 }; channel < phrase.channels.size(); ++channel) {
				if (channel != 0)
					output << ", ";
				output << quote(bytes_to_hex(phrase.channels[channel]));
			}
			output << "],\n"
				<< "      \"channel_bytes\": [";
			for (std::size_t channel{ 0 }; channel < phrase.channels.size(); ++channel) {
				if (channel != 0)
					output << ", ";
				output << phrase.channels[channel].size();
			}
			output << "],\n"
				<< "      \"byte_count\": " << bytes.size() << ",\n"
				<< "      \"byte_hash\": " << quote(fnv1a64(bytes)) << "\n";
		}
		output << "    }";
		if (i + 1 != p_kit.phrases.size())
			output << ',';
		output << '\n';
	}
	output << "  ],\n"
		<< "  \"compiled\": {\n"
		<< "    \"phrase_count\": " << p_kit.phrases.size() << ",\n"
		<< "    \"channel_count\": 4,\n"
		<< "    \"music_bytes\": " << music_bytes << ",\n"
		<< "    \"byte_hash\": " << quote(fnv1a64(all_bytes)) << ",\n"
		<< "    \"required_runtime_features\": ["
		<< quote(REST_MASK_FEATURE) << "]\n"
		<< "  }\n"
		<< "}\n";
	return output.str();
}

std::string fm::pmusic::compile_json(const std::vector<std::string>& p_mml,
	const std::string& p_source_name) {
	return to_json(compile(p_mml, p_source_name));
}

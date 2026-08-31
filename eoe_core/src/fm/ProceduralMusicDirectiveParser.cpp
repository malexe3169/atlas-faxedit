#include "ProceduralMusicDirectiveParser.h"
#include "ProceduralMusic.h"

#include "common/klib/Kstring.h"
#include "fm/fm_constants.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <optional>
#include <sstream>
#include <utility>

namespace {

	using fm::pmusic::detail::Directive;
	using fm::pmusic::detail::ParsedFamilySource;
	using fm::pmusic::detail::SourceError;
	using fm::pmusic::detail::SourceSpan;

	[[noreturn]] void fail(SourceSpan span, const std::string& message) {
		throw SourceError(span, message);
	}

	[[noreturn]] void fail(std::size_t line, const std::string& message) {
		fail(SourceSpan{ line, 1 }, message);
	}

	bool is_key_start(char value) {
		return value >= 'a' && value <= 'z';
	}

	bool is_key_char(char value) {
		return (value >= 'a' && value <= 'z')
			|| (value >= '0' && value <= '9')
			|| value == '_' || value == '-';
	}

	bool is_bare_value(const std::string& value) {
		if (value == "*")
			return true;
		const auto decimal{ [](const auto begin, const auto end) {
			return begin != end && std::all_of(begin, end, [](char ch) {
				return ch >= '0' && ch <= '9';
			});
		} };
		if (decimal(value.begin(), value.end()))
			return true;
		const auto slash{ value.find('/') };
		if (slash != std::string::npos && slash == value.rfind('/')
			&& decimal(value.begin(), value.begin() + slash)
			&& decimal(value.begin() + slash + 1, value.end()))
			return true;
		return !value.empty() && is_key_start(value.front())
			&& std::all_of(value.begin() + 1, value.end(), is_key_char);
	}

	void append_utf8(std::string& output, std::uint32_t codepoint,
		std::size_t line) {
		if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
			fail(line, "invalid Unicode code point in quoted value");

		if (codepoint <= 0x7f)
			output.push_back(static_cast<char>(codepoint));
		else if (codepoint <= 0x7ff) {
			output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
			output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
		}
		else if (codepoint <= 0xffff) {
			output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
			output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
			output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
		}
		else {
			output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
			output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
			output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
			output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
		}
	}

	int hex_digit(char value, std::size_t line) {
		if (value >= '0' && value <= '9')
			return value - '0';
		if (value >= 'a' && value <= 'f')
			return value - 'a' + 10;
		if (value >= 'A' && value <= 'F')
			return value - 'A' + 10;
		fail(line, "invalid Unicode escape in quoted value");
	}

	std::uint32_t parse_hex_quad(const std::string& text, std::size_t& cursor,
		std::size_t line) {
		if (cursor + 4 > text.size())
			fail(line, "truncated Unicode escape in quoted value");
		std::uint32_t result{ 0 };
		for (int i{ 0 }; i < 4; ++i)
			result = (result << 4) | static_cast<std::uint32_t>(hex_digit(text[cursor++], line));
		return result;
	}

	std::string parse_quoted_value(const std::string& text, std::size_t& cursor,
		std::size_t line) {
		std::string result;
		++cursor; // opening quote

		while (cursor < text.size()) {
			const unsigned char raw{ static_cast<unsigned char>(text[cursor++]) };
			if (raw == '"')
				return result;
			if (raw < 0x20)
				fail(line, "control character in quoted value");
			if (raw != '\\') {
				result.push_back(static_cast<char>(raw));
				continue;
			}

			if (cursor >= text.size())
				fail(line, "truncated escape in quoted value");
			const char escaped{ text[cursor++] };
			switch (escaped) {
			case '"': result.push_back('"'); break;
			case '\\': result.push_back('\\'); break;
			case '/': result.push_back('/'); break;
			case 'b': result.push_back('\b'); break;
			case 'f': result.push_back('\f'); break;
			case 'n': result.push_back('\n'); break;
			case 'r': result.push_back('\r'); break;
			case 't': result.push_back('\t'); break;
			case 'u': {
				std::uint32_t codepoint{ parse_hex_quad(text, cursor, line) };
				if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
					if (cursor + 2 > text.size() || text[cursor] != '\\'
						|| text[cursor + 1] != 'u')
						fail(line, "high surrogate is not followed by a low surrogate");
					cursor += 2;
					const std::uint32_t low{ parse_hex_quad(text, cursor, line) };
					if (low < 0xdc00 || low > 0xdfff)
						fail(line, "invalid low surrogate in quoted value");
					codepoint = 0x10000 + ((codepoint - 0xd800) << 10)
						+ (low - 0xdc00);
				}
				append_utf8(result, codepoint, line);
				break;
			}
			default:
				fail(line, std::format("invalid escape \\{} in quoted value", escaped));
			}
		}

		fail(line, "unterminated quoted value");
	}


	Directive parse_directive(const std::string& content, std::size_t line) {
		std::size_t cursor{ 0 };
		while (cursor < content.size()
			&& !std::isspace(static_cast<unsigned char>(content[cursor])))
			++cursor;

		Directive result{ content.substr(0, cursor), {}, {}, line };
		if (!result.name.starts_with("@pmusic-"))
			fail(line, "invalid procedural-music directive");

		while (cursor < content.size()) {
			while (cursor < content.size()
				&& std::isspace(static_cast<unsigned char>(content[cursor])))
				++cursor;
			if (cursor == content.size())
				break;
			if (!is_key_start(content[cursor]))
				fail(line, "metadata keys must start with a lower-case letter");

			const std::size_t key_start{ cursor++ };
			while (cursor < content.size() && is_key_char(content[cursor]))
				++cursor;
			const std::string key{ content.substr(key_start, cursor - key_start) };
			if (cursor >= content.size() || content[cursor] != '=')
				fail(line, std::format("metadata key '{}' is missing '='", key));
			++cursor;
			if (cursor >= content.size())
				fail(line, std::format("metadata key '{}' has no value", key));

			std::string value;
			const bool quoted{ content[cursor] == '"' };
			if (quoted)
				value = parse_quoted_value(content, cursor, line);
			else {
				const std::size_t value_start{ cursor };
				while (cursor < content.size()
					&& !std::isspace(static_cast<unsigned char>(content[cursor])))
					++cursor;
				value = content.substr(value_start, cursor - value_start);
				if (value.empty())
					fail(line, std::format("metadata key '{}' has no value", key));
				if (!is_bare_value(value))
					fail(line, std::format(
						"metadata key '{}' has an invalid bare value", key));
			}

			if (cursor < content.size()
				&& !std::isspace(static_cast<unsigned char>(content[cursor])))
				fail(line, std::format("unexpected text after value for '{}'", key));
			if (!result.values.emplace(key, value).second)
				fail(line, std::format("duplicate metadata key '{}'", key));
			if (quoted)
				result.quoted_keys.insert(key);
		}

		return result;
	}


	bool song_header(const std::string& line, int& song,
		std::size_t line_number) {
		const auto code{ klib::str::trim(klib::str::strip_comment(line)) };
		if (!code.starts_with("#song"))
			return false;
		if (code.size() > 5
			&& !std::isspace(static_cast<unsigned char>(code[5])))
			return false;

		std::istringstream input(code.substr(5));
		std::string value;
		input >> value;
		if (value.empty() || !std::all_of(value.begin(), value.end(), [](char ch) {
			return ch >= '0' && ch <= '9';
			}))
			fail(line_number, "every #song requires a decimal song number");
		try {
			song = std::stoi(value);
		}
		catch (...) {
			fail(line_number, "song number is outside the supported range");
		}
		return true;
	}


}

fm::pmusic::detail::ParsedFamilySource fm::pmusic::detail::parse_directives(
	const std::vector<std::string>& lines) {
	ParsedFamilySource result;
	bool have_kit{ false };
	bool have_style{ false };
	bool have_policy{ false };
	bool saw_song{ false };
	std::optional<Directive> pending_phrase;
	int expected_song{ 1 };

	for (std::size_t i{ 0 }; i < lines.size(); ++i) {
		const std::size_t line_number{ i + 1 };
		const auto trimmed{ klib::str::trim(lines[i]) };
		if (!trimmed.empty() && trimmed.front() == ';') {
			const auto content{ klib::str::trim(trimmed.substr(1)) };
			if (content.starts_with("@pmusic-")) {
				auto directive{ parse_directive(content, line_number) };
				if (directive.name == "@pmusic-phrase") {
					if (pending_phrase.has_value())
						fail(line_number,
							"two phrase directives target the same next #song");
					pending_phrase = std::move(directive);
				}
				else {
					if (saw_song)
						fail(line_number, std::format(
							"{} metadata must precede all songs", directive.name));
					if (directive.name == "@pmusic-kit") {
						if (have_kit)
							fail(line_number, "duplicate @pmusic-kit directive");
						have_kit = true;
						result.kit = std::move(directive);
					}
					else if (directive.name == "@pmusic-style") {
						if (have_style)
							fail(line_number, "duplicate @pmusic-style directive");
						have_style = true;
						result.style = std::move(directive);
					}
					else if (directive.name == "@pmusic-policy") {
						if (have_policy)
							fail(line_number, "duplicate @pmusic-policy directive");
						have_policy = true;
						result.policy = std::move(directive);
					}
					else if (directive.name == "@pmusic-family")
						result.families.push_back(std::move(directive));
					else if (directive.name == "@pmusic-state")
						result.states.push_back(std::move(directive));
					else if (directive.name == "@pmusic-route")
						result.routes.push_back(std::move(directive));
					else if (directive.name == "@pmusic-event-route")
						result.event_routes.push_back(std::move(directive));
					else if (directive.name == "@pmusic-stock")
						result.stocks.push_back(std::move(directive));
					else
						fail(line_number, std::format(
							"unknown v2 directive '{}'", directive.name));
				}
			}
		}

		int song{ 0 };
		if (song_header(lines[i], song, line_number)) {
			saw_song = true;
			if (song != expected_song)
				fail(line_number, std::format(
					"songs must be consecutive #song 1..N; expected {}, found {}",
					expected_song, song));
			if (!pending_phrase.has_value())
				fail(line_number, std::format(
					"#song {} is missing a preceding @pmusic-phrase directive", song));
			result.phrases.push_back({ std::move(pending_phrase.value()), song });
			pending_phrase.reset();
			++expected_song;
		}
	}

	if (!have_kit)
		fail(1, "missing @pmusic-kit directive");
	if (!have_style)
		fail(result.kit.line, "missing @pmusic-style directive");
	if (!have_policy)
		fail(result.kit.line, "missing @pmusic-policy directive");
	if (pending_phrase.has_value())
		fail(pending_phrase->line,
			"phrase directive is not followed by a #song");
	if (result.phrases.empty())
		fail(result.kit.line, "v2 needs authored family phrases");
	if (result.phrases.size() + result.stocks.size()
		> fm::pmusic::MAX_FAMILY_PHRASES)
		fail(result.phrases.back().directive.line, std::format(
			"at most {} family phrases are supported",
			fm::pmusic::MAX_FAMILY_PHRASES));
	return result;
}

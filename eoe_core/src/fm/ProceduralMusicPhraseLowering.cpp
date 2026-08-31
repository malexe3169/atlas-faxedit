#include "ProceduralMusicPhraseLowering.h"

#include "common/klib/Kstring.h"
#include "fm/fm_constants.h"
#include "fm/song/MMLEvent.h"
#include "fm/song/Parser.h"
#include "fm/song/Tokenizer.h"

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace fm::pmusic::detail {

	SourceSpan source_span(const MMLSong& song) {
		return {
			static_cast<std::size_t>(std::max(song.source_line, 1)),
			static_cast<std::size_t>(std::max(song.source_column, 1))
		};
	}

	SourceSpan source_span(const MMLChannel& channel) {
		return {
			static_cast<std::size_t>(std::max(channel.source_line, 1)),
			static_cast<std::size_t>(std::max(channel.source_column, 1))
		};
	}

	fm::MMLSongCollection parse_songs(const std::vector<std::string>& lines) {
		std::string text;
		for (const auto& line : lines)
			text += klib::str::strip_comment(line) + "\n";

		fm::Tokenizer tokenizer(text);
		const auto tokens{ tokenizer.tokenize() };
		fm::Parser parser(tokens);
		return parser.parse();
	}

	std::string channel_name(fm::ChannelType type) {
		switch (type) {
		case fm::ChannelType::sq1: return "sq1";
		case fm::ChannelType::sq2: return "sq2";
		case fm::ChannelType::tri: return "tri";
		case fm::ChannelType::noise: return "noise";
		}
		return "unknown";
	}

	SourceSpan duration_mismatch_span(const fm::MMLSong& song,
		const std::array<std::uint64_t, 4>& ticks) {
		std::map<std::uint64_t, std::size_t> frequencies;
		for (const auto value : ticks)
			++frequencies[value];

		std::uint64_t majority_ticks{ 0 };
		std::size_t majority_count{ 0 };
		std::size_t majority_ties{ 0 };
		for (const auto& [value, count] : frequencies) {
			if (count > majority_count) {
				majority_ticks = value;
				majority_count = count;
				majority_ties = 1;
			}
			else if (count == majority_count)
				++majority_ties;
		}
		if (majority_count > 1 && majority_ties == 1)
			for (std::size_t i{ 0 }; i < ticks.size(); ++i)
				if (ticks[i] != majority_ticks)
					return source_span(song.channels.at(i));

		return source_span(song);
	}

	void validate_linear_channel(const fm::MMLChannel& channel, int song,
		bool allow_leading_rest,
		bool allow_channel_tempo,
		bool allow_raw_duration) {
		int end_count{ 0 };
		bool saw_timed_event{ false };
		bool saw_sounded_event{ false };
		const bool square{ channel.channel_type == fm::ChannelType::sq1
			|| channel.channel_type == fm::ChannelType::sq2 };
		for (std::size_t i{ 0 }; i < channel.events.size(); ++i) {
			const auto& event{ channel.events[i] };
			if (std::holds_alternative<fm::NoteEvent>(event)) {
				if (!allow_raw_duration
					&& std::get<fm::NoteEvent>(event).raw.has_value())
					throw std::runtime_error(std::format(
						"PMusic song {} channel {}: raw tick durations are not allowed in v2; use musical lengths",
						song, channel_name(channel.channel_type)));
				if (channel.channel_type == fm::ChannelType::noise)
					throw std::runtime_error(std::format(
						"PMusic song {} channel noise: use percussion events, not pitched notes",
						song));
				saw_timed_event = true;
				saw_sounded_event = true;
			}
			else if (std::holds_alternative<fm::PercussionEvent>(event)) {
				if (channel.channel_type != fm::ChannelType::noise)
					throw std::runtime_error(std::format(
						"PMusic song {} channel {}: percussion is only valid on noise",
						song, channel_name(channel.channel_type)));
				const auto& percussion{ std::get<fm::PercussionEvent>(event) };
				if (percussion.perc_no < 0 || percussion.perc_no > 7
					|| (percussion.repeat > 15 && percussion.repeat != 256))
					throw std::runtime_error(std::format(
						"PMusic song {} channel noise: percussion type/repeat is not encodable",
						song));
				if (!saw_sounded_event && percussion.repeat != 1)
					throw std::runtime_error(std::format(
						"PMusic song {} channel noise: first percussion event must repeat once",
						song));
				saw_timed_event = true;
				saw_sounded_event = true;
			}
			else if (std::holds_alternative<fm::RestEvent>(event)) {
				if (!allow_raw_duration
					&& std::get<fm::RestEvent>(event).raw.has_value())
					throw std::runtime_error(std::format(
						"PMusic song {} channel {}: raw tick durations are not allowed in v2; use musical lengths",
						song, channel_name(channel.channel_type)));
				if (!allow_leading_rest && !saw_timed_event)
					throw std::runtime_error(std::format(
						"PMusic song {} channel {}: first timed event must be non-rest",
						song, channel_name(channel.channel_type)));
				saw_timed_event = true;
			}
			else if (std::holds_alternative<fm::LengthEvent>(event)
				&& !allow_raw_duration
				&& std::get<fm::LengthEvent>(event).raw.has_value())
				throw std::runtime_error(std::format(
					"PMusic song {} channel {}: raw tick durations are not allowed in v2; use musical lengths",
					song, channel_name(channel.channel_type)));
			else if (std::holds_alternative<fm::TempoSetEvent>(event)
				&& !allow_channel_tempo)
				throw std::runtime_error(std::format(
					"PMusic song {} channel {}: channel-local tempo changes are not allowed in v2; use written durations",
					song, channel_name(channel.channel_type)));
			else if (std::holds_alternative<fm::EndEvent>(event)) {
				++end_count;
				if (i + 1 != channel.events.size())
					throw std::runtime_error(std::format(
						"PMusic song {} channel {}: !end must be the final event",
						song, channel_name(channel.channel_type)));
			}
			else if (std::holds_alternative<fm::NOPEvent>(event))
				throw std::runtime_error(std::format(
					"PMusic song {} channel {}: authored !nop is not allowed",
					song, channel_name(channel.channel_type)));
			else if (std::holds_alternative<fm::RestartEvent>(event)
				|| std::holds_alternative<fm::StartEvent>(event)
				|| std::holds_alternative<fm::LabelEvent>(event)
				|| std::holds_alternative<fm::JSREvent>(event)
				|| std::holds_alternative<fm::ReturnEvent>(event)
				|| std::holds_alternative<fm::PushAddrEvent>(event)
				|| std::holds_alternative<fm::PopAddrEvent>(event)
				|| std::holds_alternative<fm::BeginLoopEvent>(event)
				|| std::holds_alternative<fm::LoopIfEvent>(event)
				|| std::holds_alternative<fm::EndLoopEvent>(event))
				throw std::runtime_error(std::format(
					"PMusic song {} channel {}: only linear finite channel flow is allowed",
					song, channel_name(channel.channel_type)));
			else if (std::holds_alternative<fm::SongTransposeEvent>(event)
				|| std::holds_alternative<fm::ChannelTransposeEvent>(event))
				throw std::runtime_error(std::format(
					"PMusic song {} channel {}: authored runtime transpose is not allowed in v1",
					song, channel_name(channel.channel_type)));
			else if ((std::holds_alternative<fm::EffectEvent>(event)
				|| std::holds_alternative<fm::EnvelopeEvent>(event)
				|| std::holds_alternative<fm::VolumeSetEvent>(event)
				|| std::holds_alternative<fm::PulseEvent>(event))) {
				if (!square)
					throw std::runtime_error(std::format(
						"PMusic song {} channel {}: square-channel control is not allowed here",
						song, channel_name(channel.channel_type)));
				if (saw_timed_event)
					throw std::runtime_error(std::format(
						"PMusic song {} channel {}: timbre controls must precede the first timed event",
						song, channel_name(channel.channel_type)));
			}
			else if (std::holds_alternative<fm::DetuneEvent>(event)) {
				if (channel.channel_type != fm::ChannelType::sq2)
					throw std::runtime_error(std::format(
						"PMusic song {} channel {}: !detune is only allowed on sq2",
						song, channel_name(channel.channel_type)));
				if (saw_timed_event)
					throw std::runtime_error(std::format(
						"PMusic song {} channel sq2: timbre controls must precede the first timed event",
						song));
			}
		}

		if (end_count != 1)
			throw std::runtime_error(std::format(
				"PMusic song {} channel {}: exactly one terminal !end is required",
				song, channel_name(channel.channel_type)));
	}

	bool validate_first_timed_event(
		const std::vector<fm::MusicInstruction>& instructions,
		bool noise, int song, const std::string& name,
		bool allow_leading_rest) {
		bool duration_seen{ false };
		for (const auto& instruction : instructions) {
			const byte opcode{ instruction.opcode_byte };
			if ((opcode >= fm::c::HEX_NOTELENGTH_MIN
				&& opcode < fm::c::HEX_NOTELENGTH_END)
				|| opcode == fm::c::MSCRIPT_OPCODE_SET_LENGTH)
				duration_seen = true;
			else if (opcode < fm::c::HEX_NOTELENGTH_MIN) {
				if (opcode == fm::c::HEX_REST && !allow_leading_rest)
					throw std::runtime_error(std::format(
						"PMusic song {} channel {}: first timed event must be non-rest",
						song, name));
				if (noise && opcode != fm::c::HEX_REST
					&& (opcode & 0x0f) != 1)
					throw std::runtime_error(std::format(
						"PMusic song {} channel noise: first percussion event must repeat once",
						song));
				return duration_seen;
			}
		}
		throw std::runtime_error(std::format(
			"PMusic song {} channel {}: no timed event precedes !end", song, name));
	}

	void prepend_canonical_prologue(std::vector<fm::MusicInstruction>& instructions,
		std::size_t channel, bool needs_duration) {
		std::vector<fm::MusicInstruction> prefix;
		auto add = [&](byte opcode, byte value) {
			prefix.push_back(fm::MusicInstruction{
				opcode, value, std::nullopt, std::nullopt
			});
		};

		if (channel == 0) {
			add(fm::c::MSCRIPT_OPCODE_GLOBAL_TRANSPOSE, 0);
			add(fm::c::MSCRIPT_OPCODE_CHANNEL_TRANSPOSE, 0);
			add(fm::c::MSCRIPT_OPCODE_SQ_PITCH_EFFECT, 0);
			add(fm::c::MSCRIPT_OPCODE_SQ_ENVELOPE, 0);
			add(fm::c::MSCRIPT_OPCODE_VOLUME, 0);
			add(fm::c::MSCRIPT_OPCODE_SQ_CONTROL, 0x90);
		}
		else if (channel == 1) {
			add(fm::c::MSCRIPT_OPCODE_CHANNEL_TRANSPOSE, 0);
			add(fm::c::MSCRIPT_OPCODE_SQ2_DETUNE, 0);
			add(fm::c::MSCRIPT_OPCODE_SQ_PITCH_EFFECT, 0);
			add(fm::c::MSCRIPT_OPCODE_SQ_ENVELOPE, 0);
			add(fm::c::MSCRIPT_OPCODE_VOLUME, 0);
			add(fm::c::MSCRIPT_OPCODE_SQ_CONTROL, 0x90);
		}
		else if (channel == 2)
			add(fm::c::MSCRIPT_OPCODE_CHANNEL_TRANSPOSE, 0);

		if (needs_duration)
			add(fm::c::MSCRIPT_OPCODE_SET_LENGTH, 1);
		prefix.insert(prefix.end(), instructions.begin(), instructions.end());
		instructions = std::move(prefix);
	}

	std::uint64_t channel_ticks(const std::vector<fm::MusicInstruction>& instructions,
		bool noise, int song, const std::string& name) {
		std::uint64_t ticks{ 0 };
		std::uint64_t length{ 1 };

		for (const auto& instruction : instructions) {
			const byte opcode{ instruction.opcode_byte };
			if (opcode >= fm::c::HEX_NOTELENGTH_MIN
				&& opcode < fm::c::HEX_NOTELENGTH_END) {
				length = static_cast<std::uint64_t>(opcode - fm::c::HEX_NOTELENGTH_MIN);
				if (length == 0)
					throw std::runtime_error(std::format(
						"PMusic song {} channel {}: zero tick length compiled",
						song, name));
			}
			else if (opcode == fm::c::MSCRIPT_OPCODE_SET_LENGTH) {
				if (!instruction.operand.has_value() || instruction.operand.value() == 0)
					throw std::runtime_error(std::format(
						"PMusic song {} channel {}: invalid set-length opcode",
						song, name));
				length = instruction.operand.value();
			}
			else if (opcode < fm::c::HEX_NOTELENGTH_MIN) {
				std::uint64_t repeats{ 1 };
				if (noise && opcode != fm::c::HEX_REST) {
					repeats = opcode & 0x0f;
					if (repeats == 0)
						repeats = 256;
				}
				if (ticks > std::numeric_limits<std::uint64_t>::max() - length * repeats)
					throw std::runtime_error("PMusic phrase tick count overflow");
				ticks += length * repeats;
			}
		}

		return ticks;
	}

	std::vector<byte> instruction_bytes(
		const std::vector<fm::MusicInstruction>& instructions) {
		std::vector<byte> result;
		for (const auto& instruction : instructions) {
			const auto bytes{ instruction.get_bytes() };
			result.insert(result.end(), bytes.begin(), bytes.end());
		}
		return result;
	}


}

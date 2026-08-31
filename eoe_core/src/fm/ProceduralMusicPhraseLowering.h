#pragma once

#include "ProceduralMusicDirectiveParser.h"
#include "fm/MusicOpcode.h"
#include "fm/song/MMLChannel.h"
#include "fm/song/MMLSongCollection.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fm::pmusic::detail {

	MMLSongCollection parse_songs(const std::vector<std::string>& lines);
	std::string channel_name(ChannelType type);
	SourceSpan duration_mismatch_span(const MMLSong& song,
		const std::array<std::uint64_t, 4>& ticks);
	void validate_linear_channel(const MMLChannel& channel, int song,
		bool allow_leading_rest = false,
		bool allow_channel_tempo = true,
		bool allow_raw_duration = true);
	bool validate_first_timed_event(
		const std::vector<MusicInstruction>& instructions,
		bool noise, int song, const std::string& name,
		bool allow_leading_rest = false);
	void prepend_canonical_prologue(std::vector<MusicInstruction>& instructions,
		std::size_t channel, bool needs_duration);
	std::uint64_t channel_ticks(const std::vector<MusicInstruction>& instructions,
		bool noise, int song, const std::string& name);
	std::vector<byte> instruction_bytes(
		const std::vector<MusicInstruction>& instructions);

}

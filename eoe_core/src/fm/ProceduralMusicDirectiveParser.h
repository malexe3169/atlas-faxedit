#pragma once

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fm::pmusic::detail {

	using Metadata = std::map<std::string, std::string>;

	struct SourceSpan {
		std::size_t line{ 1 };
		std::size_t column{ 1 };
	};

	class SourceError : public std::runtime_error {
	public:
		SourceError(SourceSpan p_span, const std::string& p_message) :
			std::runtime_error(p_message), span(p_span) {}

		SourceSpan span;
	};

	struct Directive {
		std::string name;
		Metadata values;
		std::set<std::string> quoted_keys;
		std::size_t line;
	};

	struct PhraseAnnotation {
		Directive directive;
		int song;
	};

	struct ParsedFamilySource {
		Directive kit;
		Directive style;
		Directive policy;
		std::vector<Directive> families;
		std::vector<Directive> states;
		std::vector<Directive> routes;
		std::vector<Directive> event_routes;
		std::vector<Directive> stocks;
		std::vector<PhraseAnnotation> phrases;
	};

	ParsedFamilySource parse_directives(const std::vector<std::string>& lines);

}

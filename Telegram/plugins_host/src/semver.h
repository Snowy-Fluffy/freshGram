#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fgpl {

struct Version {
	uint32_t major = 0;
	uint32_t minor = 0;
	uint32_t patch = 0;
	std::string pre;

	[[nodiscard]] std::string text() const;
};

[[nodiscard]] std::optional<Version> ParseVersion(std::string_view text);

[[nodiscard]] int Compare(const Version &a, const Version &b);

class VersionRange {
public:
	[[nodiscard]] static std::optional<VersionRange> Parse(
		std::string_view text);

	enum class Fit : uint8_t {
		Match,
		TooOld,
		TooNew,
	};

	[[nodiscard]] bool matches(const Version &version) const;
	[[nodiscard]] Fit fit(const Version &version) const;
	[[nodiscard]] const std::string &text() const {
		return _text;
	}

private:
	struct Comparator {
		enum class Op : uint8_t {
			Any,
			Equal,
			Greater,
			GreaterEqual,
			Less,
			LessEqual,
		};

		Op op = Op::Any;
		Version version;
	};

	std::vector<Comparator> _comparators;
	std::string _text;

};

} // namespace fgpl

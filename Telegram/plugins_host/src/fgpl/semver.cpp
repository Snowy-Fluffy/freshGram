#include "fgpl/semver.h"

#include <cctype>

namespace fgpl {
namespace {

[[nodiscard]] bool ParseNumber(
		std::string_view text,
		uint32_t &out) {
	if (text.empty() || text.size() > 9) {
		return false;
	} else if (text.size() > 1 && text[0] == '0') {
		return false;
	}
	auto value = uint32_t(0);
	for (const auto ch : text) {
		if (ch < '0' || ch > '9') {
			return false;
		}
		value = value * 10 + uint32_t(ch - '0');
	}
	out = value;
	return true;
}

[[nodiscard]] bool ValidPre(std::string_view pre) {
	if (pre.empty() || pre.size() > 32) {
		return false;
	}
	for (const auto ch : pre) {
		if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '.' && ch != '-') {
			return false;
		}
	}
	return pre.front() != '.'
		&& pre.back() != '.'
		&& pre.find("..") == std::string_view::npos;
}

[[nodiscard]] int ComparePre(const std::string &a, const std::string &b) {
	if (a.empty() && b.empty()) {
		return 0;
	} else if (a.empty()) {
		return 1;
	} else if (b.empty()) {
		return -1;
	}
	auto ia = size_t(0);
	auto ib = size_t(0);
	while (ia < a.size() || ib < b.size()) {
		if (ia >= a.size()) {
			return -1;
		} else if (ib >= b.size()) {
			return 1;
		}
		const auto ea = a.find('.', ia);
		const auto eb = b.find('.', ib);
		const auto pa = a.substr(ia, ea == std::string::npos ? ea : ea - ia);
		const auto pb = b.substr(ib, eb == std::string::npos ? eb : eb - ib);
		const auto numeric = [](const std::string &part) {
			for (const auto ch : part) {
				if (ch < '0' || ch > '9') {
					return false;
				}
			}
			return !part.empty();
		};
		if (numeric(pa) && numeric(pb)) {
			const auto na = std::stoull(pa);
			const auto nb = std::stoull(pb);
			if (na != nb) {
				return na < nb ? -1 : 1;
			}
		} else if (numeric(pa)) {
			return -1;
		} else if (numeric(pb)) {
			return 1;
		} else if (pa != pb) {
			return pa < pb ? -1 : 1;
		}
		ia = (ea == std::string::npos) ? a.size() : ea + 1;
		ib = (eb == std::string::npos) ? b.size() : eb + 1;
	}
	return 0;
}

} // namespace

std::string Version::text() const {
	auto result = std::to_string(major) + "." + std::to_string(minor)
		+ "." + std::to_string(patch);
	if (!pre.empty()) {
		result += "-" + pre;
	}
	return result;
}

std::optional<Version> ParseVersion(std::string_view text) {
	if (text.size() > 64) {
		return std::nullopt;
	}
	auto version = Version();
	const auto dash = text.find('-');
	auto core = text.substr(0, dash);
	if (dash != std::string_view::npos) {
		const auto pre = text.substr(dash + 1);
		if (!ValidPre(pre)) {
			return std::nullopt;
		}
		version.pre = std::string(pre);
	}
	const auto first = core.find('.');
	if (first == std::string_view::npos) {
		return std::nullopt;
	}
	const auto second = core.find('.', first + 1);
	if (second == std::string_view::npos
		|| core.find('.', second + 1) != std::string_view::npos) {
		return std::nullopt;
	}
	if (!ParseNumber(core.substr(0, first), version.major)
		|| !ParseNumber(core.substr(first + 1, second - first - 1), version.minor)
		|| !ParseNumber(core.substr(second + 1), version.patch)) {
		return std::nullopt;
	}
	return version;
}

int Compare(const Version &a, const Version &b) {
	if (a.major != b.major) {
		return a.major < b.major ? -1 : 1;
	} else if (a.minor != b.minor) {
		return a.minor < b.minor ? -1 : 1;
	} else if (a.patch != b.patch) {
		return a.patch < b.patch ? -1 : 1;
	}
	return ComparePre(a.pre, b.pre);
}

std::optional<VersionRange> VersionRange::Parse(std::string_view text) {
	if (text.empty() || text.size() > 128) {
		return std::nullopt;
	}
	auto range = VersionRange();
	range._text = std::string(text);
	auto position = size_t(0);
	while (position < text.size()) {
		while (position < text.size() && text[position] == ' ') {
			++position;
		}
		if (position >= text.size()) {
			break;
		}
		auto end = text.find(' ', position);
		if (end == std::string_view::npos) {
			end = text.size();
		}
		auto token = text.substr(position, end - position);
		position = end;
		if (range._comparators.size() >= 8) {
			return std::nullopt;
		}
		auto comparator = Comparator();
		if (token == "*") {
			comparator.op = Comparator::Op::Any;
			range._comparators.push_back(std::move(comparator));
			continue;
		}
		if (token.rfind(">=", 0) == 0) {
			comparator.op = Comparator::Op::GreaterEqual;
			token.remove_prefix(2);
		} else if (token.rfind("<=", 0) == 0) {
			comparator.op = Comparator::Op::LessEqual;
			token.remove_prefix(2);
		} else if (token.rfind(">", 0) == 0) {
			comparator.op = Comparator::Op::Greater;
			token.remove_prefix(1);
		} else if (token.rfind("<", 0) == 0) {
			comparator.op = Comparator::Op::Less;
			token.remove_prefix(1);
		} else if (token.rfind("=", 0) == 0) {
			comparator.op = Comparator::Op::Equal;
			token.remove_prefix(1);
		} else {
			comparator.op = Comparator::Op::Equal;
		}
		const auto version = ParseVersion(token);
		if (!version) {
			return std::nullopt;
		}
		comparator.version = *version;
		range._comparators.push_back(std::move(comparator));
	}
	if (range._comparators.empty()) {
		return std::nullopt;
	}
	return range;
}

VersionRange::Fit VersionRange::fit(const Version &version) const {
	auto result = Fit::Match;
	for (const auto &comparator : _comparators) {
		const auto order = Compare(version, comparator.version);
		auto failure = Fit::Match;
		switch (comparator.op) {
		case Comparator::Op::Any: break;
		case Comparator::Op::Equal:
			failure = (order < 0) ? Fit::TooOld : (order > 0) ? Fit::TooNew : Fit::Match;
			break;
		case Comparator::Op::Greater:
			failure = (order <= 0) ? Fit::TooOld : Fit::Match;
			break;
		case Comparator::Op::GreaterEqual:
			failure = (order < 0) ? Fit::TooOld : Fit::Match;
			break;
		case Comparator::Op::Less:
			failure = (order >= 0) ? Fit::TooNew : Fit::Match;
			break;
		case Comparator::Op::LessEqual:
			failure = (order > 0) ? Fit::TooNew : Fit::Match;
			break;
		}
		if (failure == Fit::TooOld) {
			return Fit::TooOld;
		} else if (failure == Fit::TooNew) {
			result = Fit::TooNew;
		}
	}
	return result;
}

bool VersionRange::matches(const Version &version) const {
	return fit(version) == Fit::Match;
}

} // namespace fgpl

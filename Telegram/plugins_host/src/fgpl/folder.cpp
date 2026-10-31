#include "fgpl/folder.h"

namespace fgpl::folder {
namespace {

[[nodiscard]] bool ValidUtf8(std::string_view text) {
	auto i = size_t(0);
	while (i < text.size()) {
		const auto lead = uint8_t(text[i]);
		auto extra = size_t(0);
		auto minimum = uint32_t(0);
		auto code = uint32_t(0);
		if (lead < 0x80) {
			++i;
			continue;
		} else if (lead >= 0xC2 && lead <= 0xDF) {
			extra = 1;
			minimum = 0x80;
			code = lead & 0x1F;
		} else if (lead >= 0xE0 && lead <= 0xEF) {
			extra = 2;
			minimum = 0x800;
			code = lead & 0x0F;
		} else if (lead >= 0xF0 && lead <= 0xF4) {
			extra = 3;
			minimum = 0x10000;
			code = lead & 0x07;
		} else {
			return false;
		}
		if (i + extra >= text.size()) {
			return false;
		}
		for (auto k = size_t(1); k <= extra; ++k) {
			const auto next = uint8_t(text[i + k]);
			if ((next & 0xC0) != 0x80) {
				return false;
			}
			code = (code << 6) | (next & 0x3F);
		}
		if (code < minimum
			|| code > 0x10FFFF
			|| (code >= 0xD800 && code <= 0xDFFF)) {
			return false;
		}
		i += extra + 1;
	}
	return true;
}

[[nodiscard]] bool ValidName(std::string_view name) {
	if (name.empty()
		|| name.size() > kMaxNameBytes
		|| name == "."
		|| name == ".."
		|| !ValidUtf8(name)) {
		return false;
	}
	for (const auto ch : name) {
		const auto code = uint8_t(ch);
		if (code < 0x20 || code == 0x7F || ch == '\\') {
			return false;
		}
#ifdef _WIN32
		if (std::string_view("<>:\"|?*").find(ch) != std::string_view::npos) {
			return false;
		}
#endif // _WIN32
	}
#ifdef _WIN32
	if (name.back() == '.' || name.back() == ' ') {
		return false;
	}
#endif // _WIN32
	return true;
}

} // namespace

Result<std::vector<std::string>, Fail> SplitPath(
		std::string_view path,
		bool allowEmpty) {
	if (path.size() > kMaxPathBytes
		|| path.find('\0') != std::string_view::npos
		|| (!path.empty() && path.front() == '/')) {
		return Fail::Invalid;
	}
	auto parts = std::vector<std::string>();
	while (!path.empty()) {
		const auto slash = path.find('/');
		const auto part = path.substr(0, slash);
		path = (slash == std::string_view::npos)
			? std::string_view()
			: path.substr(slash + 1);
		if (part.empty() || part == ".") {
			continue;
		} else if (!ValidName(part) || parts.size() >= kMaxDepth) {
			return Fail::Invalid;
		}
		parts.emplace_back(part);
	}
	if (parts.empty() && !allowEmpty) {
		return Fail::Invalid;
	}
	return parts;
}

} // namespace fgpl::folder

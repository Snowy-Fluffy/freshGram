#include "net_policy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>

namespace fgpl::net {
namespace {

[[nodiscard]] std::string Lower(std::string_view text) {
	auto result = std::string(text);
	for (auto &ch : result) {
		ch = char(std::tolower(static_cast<unsigned char>(ch)));
	}
	return result;
}

[[nodiscard]] std::optional<std::array<uint8_t, 4>> ParseV4(
		std::string_view text) {
	auto result = std::array<uint8_t, 4>();
	auto index = 0;
	auto value = 0;
	auto digits = 0;
	for (const auto ch : text) {
		if (ch >= '0' && ch <= '9') {
			value = value * 10 + (ch - '0');
			if (++digits > 3 || value > 255) {
				return std::nullopt;
			}
		} else if (ch == '.' && digits > 0 && index < 3) {
			result[size_t(index++)] = uint8_t(value);
			value = 0;
			digits = 0;
		} else {
			return std::nullopt;
		}
	}
	if (digits == 0 || index != 3) {
		return std::nullopt;
	}
	result[3] = uint8_t(value);
	return result;
}

[[nodiscard]] std::optional<std::array<uint8_t, 16>> ParseV6(
		std::string_view text) {
	if (text.find(':') == std::string_view::npos) {
		return std::nullopt;
	}
	auto head = std::vector<uint16_t>();
	auto tail = std::vector<uint16_t>();
	auto *current = &head;
	auto seenGap = false;
	auto position = size_t(0);
	if (text.rfind("::", 0) == 0) {
		seenGap = true;
		current = &tail;
		position = 2;
	}
	while (position < text.size()) {
		auto end = text.find(':', position);
		const auto piece = text.substr(
			position,
			end == std::string_view::npos ? std::string_view::npos : end - position);
		if (piece.empty()) {
			return std::nullopt;
		}
		if (piece.find('.') != std::string_view::npos) {
			if (end != std::string_view::npos) {
				return std::nullopt;
			}
			const auto v4 = ParseV4(piece);
			if (!v4) {
				return std::nullopt;
			}
			current->push_back(uint16_t((*v4)[0] << 8 | (*v4)[1]));
			current->push_back(uint16_t((*v4)[2] << 8 | (*v4)[3]));
			position = text.size();
			break;
		}
		if (piece.size() > 4) {
			return std::nullopt;
		}
		auto value = 0;
		for (const auto ch : piece) {
			const auto digit = std::isdigit(static_cast<unsigned char>(ch))
				? ch - '0'
				: (std::tolower(static_cast<unsigned char>(ch)) >= 'a'
					&& std::tolower(static_cast<unsigned char>(ch)) <= 'f')
				? std::tolower(static_cast<unsigned char>(ch)) - 'a' + 10
				: -1;
			if (digit < 0) {
				return std::nullopt;
			}
			value = value * 16 + digit;
		}
		current->push_back(uint16_t(value));
		if (end == std::string_view::npos) {
			position = text.size();
			break;
		}
		position = end + 1;
		if (position < text.size() && text[position] == ':') {
			if (seenGap) {
				return std::nullopt;
			}
			seenGap = true;
			current = &tail;
			++position;
		} else if (position == text.size()) {
			return std::nullopt;
		}
	}
	const auto total = head.size() + tail.size();
	if (total > 8 || (!seenGap && total != 8) || (seenGap && total == 8)) {
		return std::nullopt;
	}
	auto words = std::array<uint16_t, 8>();
	for (auto i = size_t(0); i != head.size(); ++i) {
		words[i] = head[i];
	}
	for (auto i = size_t(0); i != tail.size(); ++i) {
		words[8 - tail.size() + i] = tail[i];
	}
	auto result = std::array<uint8_t, 16>();
	for (auto i = size_t(0); i != 8; ++i) {
		result[i * 2] = uint8_t(words[i] >> 8);
		result[i * 2 + 1] = uint8_t(words[i]);
	}
	return result;
}

[[nodiscard]] bool PublicV4(const std::array<uint8_t, 4> &a) {
	const auto first = a[0];
	const auto second = a[1];
	if (first == 0 || first == 10 || first == 127 || first >= 224) {
		return false;
	} else if (first == 100 && second >= 64 && second <= 127) {
		return false;
	} else if (first == 169 && second == 254) {
		return false;
	} else if (first == 172 && second >= 16 && second <= 31) {
		return false;
	} else if (first == 192 && second == 168) {
		return false;
	} else if (first == 192 && second == 0 && a[2] == 0) {
		return false;
	} else if (first == 192 && second == 0 && a[2] == 2) {
		return false;
	} else if (first == 198 && (second == 18 || second == 19)) {
		return false;
	} else if (first == 198 && second == 51 && a[2] == 100) {
		return false;
	} else if (first == 203 && second == 0 && a[2] == 113) {
		return false;
	}
	return true;
}

[[nodiscard]] bool ValidHostname(std::string_view host) {
	if (host.empty() || host.size() > 253) {
		return false;
	}
	auto label = size_t(0);
	auto previous = '.';
	for (const auto ch : host) {
		if (ch == '.') {
			if (label == 0 || previous == '-') {
				return false;
			}
			label = 0;
			previous = ch;
			continue;
		}
		const auto good = std::isalnum(static_cast<unsigned char>(ch))
			|| ch == '-'
			|| ch == '_';
		if (!good || ++label > 63 || (label == 1 && ch == '-')) {
			return false;
		}
		previous = ch;
	}
	return label != 0 && previous != '-';
}

} // namespace

Result<Url, std::string> ParseUrl(std::string_view text) {
	if (text.size() > 2048) {
		return std::string("the URL is too long");
	}
	for (const auto ch : text) {
		if (static_cast<unsigned char>(ch) < 0x21
			|| static_cast<unsigned char>(ch) > 0x7E) {
			return std::string("the URL has forbidden characters");
		}
	}
	constexpr auto kScheme = std::string_view("https://");
	if (Lower(text.substr(0, kScheme.size())) != kScheme) {
		return std::string("only https URLs are allowed");
	}
	auto rest = text.substr(kScheme.size());
	const auto slash = rest.find_first_of("/?#");
	const auto authority = rest.substr(0, slash);
	auto target = (slash == std::string_view::npos)
		? std::string("/")
		: std::string(rest.substr(slash));
	if (target.front() != '/') {
		target.insert(target.begin(), '/');
	}
	const auto fragment = target.find('#');
	if (fragment != std::string::npos) {
		target.resize(fragment);
	}
	if (authority.empty()
		|| authority.find('@') != std::string_view::npos
		|| authority.front() == '[') {
		return std::string("the URL has no usable host");
	}
	auto url = Url();
	const auto colon = authority.rfind(':');
	auto host = authority;
	if (colon != std::string_view::npos) {
		host = authority.substr(0, colon);
		const auto port = authority.substr(colon + 1);
		if (port.empty() || port.size() > 5) {
			return std::string("a bad port");
		}
		auto number = 0;
		for (const auto ch : port) {
			if (ch < '0' || ch > '9') {
				return std::string("a bad port");
			}
			number = number * 10 + (ch - '0');
		}
		if (number < 1 || number > 65535) {
			return std::string("a bad port");
		}
		url.port = number;
	}
	url.host = Lower(host);
	while (!url.host.empty() && url.host.back() == '.') {
		url.host.pop_back();
	}
	if (!ValidHostname(url.host)) {
		return std::string("a bad host name");
	} else if (ParseV4(url.host)) {
		return std::string("IP addresses are not allowed");
	} else if (url.host == "localhost"
		|| url.host.ends_with(".localhost")
		|| url.host.ends_with(".local")
		|| url.host.ends_with(".internal")) {
		return std::string("local hosts are not allowed");
	}
	url.target = std::move(target);
	url.text = "https://" + url.host
		+ (url.port == 443 ? std::string() : ":" + std::to_string(url.port))
		+ url.target;
	return url;
}

bool HostAllowed(const Url &url, const std::vector<std::string> &patterns) {
	for (const auto &raw : patterns) {
		auto pattern = Lower(raw);
		auto port = 443;
		const auto colon = pattern.rfind(':');
		if (colon != std::string::npos) {
			port = std::atoi(pattern.c_str() + colon + 1);
			pattern.resize(colon);
		}
		if (port != url.port) {
			continue;
		} else if (pattern.rfind("*.", 0) == 0) {
			const auto suffix = pattern.substr(1);
			if (url.host.size() > suffix.size()
				&& url.host.ends_with(suffix)) {
				return true;
			}
		} else if (pattern == url.host) {
			return true;
		}
	}
	return false;
}

bool IsPublicAddress(std::string_view address) {
	if (const auto v4 = ParseV4(address)) {
		return PublicV4(*v4);
	}
	const auto v6 = ParseV6(address);
	if (!v6) {
		return false;
	}
	const auto &b = *v6;
	auto zeros = true;
	for (auto i = 0; i != 10; ++i) {
		zeros = zeros && b[size_t(i)] == 0;
	}
	if (zeros && b[10] == 0xFF && b[11] == 0xFF) {
		return PublicV4({ b[12], b[13], b[14], b[15] });
	}
	auto allZero = true;
	for (const auto byte : b) {
		allZero = allZero && byte == 0;
	}
	auto loopback = true;
	for (auto i = 0; i != 15; ++i) {
		loopback = loopback && b[size_t(i)] == 0;
	}
	if (allZero || (loopback && b[15] == 1)) {
		return false;
	} else if ((b[0] & 0xFE) == 0xFC || b[0] == 0xFF) {
		return false;
	} else if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) {
		return false;
	} else if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0D && b[3] == 0xB8) {
		return false;
	} else if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xFF && b[3] == 0x9B) {
		return PublicV4({ b[12], b[13], b[14], b[15] });
	}
	return true;
}

bool IsValidHeaderName(std::string_view name) {
	if (name.empty() || name.size() > 64) {
		return false;
	}
	for (const auto ch : name) {
		if (!std::isalnum(static_cast<unsigned char>(ch))
			&& std::string_view("!#$%&'*+-.^_`|~").find(ch)
				== std::string_view::npos) {
			return false;
		}
	}
	return true;
}

bool IsForbiddenRequestHeader(std::string_view name) {
	const auto lower = Lower(name);
	static const auto forbidden = std::vector<std::string>{
		"host", "content-length", "transfer-encoding", "connection", "upgrade",
		"te", "trailer", "expect", "cookie", "cookie2", "proxy-authorization",
		"proxy-connection", "keep-alive", "via", "forwarded", "x-forwarded-for",
		"x-forwarded-host", "x-forwarded-proto", "x-real-ip",
	};
	return std::find(forbidden.begin(), forbidden.end(), lower) != forbidden.end()
		|| lower.rfind("proxy-", 0) == 0
		|| lower.rfind("sec-", 0) == 0;
}

bool IsAllowedMethod(std::string_view method) {
	return method == "GET"
		|| method == "POST"
		|| method == "PUT"
		|| method == "PATCH"
		|| method == "DELETE"
		|| method == "HEAD";
}

} // namespace fgpl::net

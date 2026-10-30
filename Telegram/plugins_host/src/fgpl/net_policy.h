#pragma once

#include "fgpl/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fgpl::net {

struct Url {
	std::string host;
	int port = 443;
	std::string target;
	std::string text;
	bool ip = false;
	std::string scheme = "https";
	bool tls = true;
};

struct Endpoint {
	std::string host;
	int port = 0;
	bool ip = false;
};

[[nodiscard]] Result<Url, std::string> ParseUrl(std::string_view text);

[[nodiscard]] Result<Endpoint, std::string> ParseEndpoint(
	std::string_view host,
	int64_t port);

// The host is "localhost", an address of 127.0.0.0/8 or ::1.
[[nodiscard]] bool IsLoopbackHost(std::string_view host);
// http, https, ws or wss URLs of a loopback host; the error is "not local"
// for any other host, so that the caller may try ParseUrl.
[[nodiscard]] Result<Url, std::string> ParseLocalUrl(std::string_view text);
[[nodiscard]] Result<Endpoint, std::string> ParseLocalEndpoint(
	std::string_view host,
	int64_t port);

[[nodiscard]] bool IsPublicAddress(std::string_view address);

[[nodiscard]] bool IsForbiddenRequestHeader(std::string_view name);

[[nodiscard]] bool IsValidHeaderName(std::string_view name);

[[nodiscard]] bool IsAllowedMethod(std::string_view method);

// Continues decoding a chunked body that starts at raw[at]; false if malformed or over maxBytes.
[[nodiscard]] bool DecodeChunked(
	const std::string &raw,
	size_t &at,
	size_t maxBytes,
	std::string &out,
	bool &complete);

} // namespace fgpl::net

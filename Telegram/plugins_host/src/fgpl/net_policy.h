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

[[nodiscard]] bool IsPublicAddress(std::string_view address);

[[nodiscard]] bool IsForbiddenRequestHeader(std::string_view name);

[[nodiscard]] bool IsValidHeaderName(std::string_view name);

[[nodiscard]] bool IsAllowedMethod(std::string_view method);

} // namespace fgpl::net

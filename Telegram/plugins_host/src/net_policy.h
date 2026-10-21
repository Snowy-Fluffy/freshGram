#pragma once

#include "result.h"

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
};

[[nodiscard]] Result<Url, std::string> ParseUrl(std::string_view text);

[[nodiscard]] bool HostAllowed(
	const Url &url,
	const std::vector<std::string> &patterns);

[[nodiscard]] bool IsPublicAddress(std::string_view address);

[[nodiscard]] bool IsForbiddenRequestHeader(std::string_view name);

[[nodiscard]] bool IsValidHeaderName(std::string_view name);

[[nodiscard]] bool IsAllowedMethod(std::string_view method);

} // namespace fgpl::net

#pragma once

#include "fgpl/cbor.h"
#include "fgpl/result.h"

#include <string>
#include <string_view>

namespace fgpl::json {

struct Limits {
	size_t maxBytes = size_t(256) << 10;
	size_t maxDepth = 16;
	size_t maxItems = 20000;
};

constexpr auto kMaxSafeInteger = int64_t(9007199254740991);

[[nodiscard]] Result<cbor::Value, std::string> Parse(
	std::string_view text,
	const Limits &limits = {});

[[nodiscard]] std::string Canonical(const cbor::Value &value);

} // namespace fgpl::json

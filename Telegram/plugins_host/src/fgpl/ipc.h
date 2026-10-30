#pragma once

#include "fgpl/cbor.h"
#include "fgpl/os.h"
#include "fgpl/result.h"

#include <cstddef>
#include <variant>

namespace fgpl::ipc {

enum class IoError : uint8_t {
	Closed,
	Failed,
	TooLarge,
	Timeout,
	Malformed,
};

constexpr auto kMaxFrameBytes = size_t(256) << 20;

[[nodiscard]] Result<std::monostate, IoError> WriteFrame(
	os::Handle fd,
	const cbor::Value &message,
	size_t maxBytes = kMaxFrameBytes);

[[nodiscard]] Result<cbor::Value, IoError> ReadFrame(
	os::Handle fd,
	const cbor::Limits &limits,
	size_t maxBytes = kMaxFrameBytes,
	int timeoutMs = -1);

[[nodiscard]] cbor::Limits TrustedLimits();
[[nodiscard]] cbor::Limits StrictLimits();

} // namespace fgpl::ipc

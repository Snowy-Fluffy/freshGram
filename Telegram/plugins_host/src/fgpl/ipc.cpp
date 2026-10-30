#include "fgpl/ipc.h"

#include "fgpl/ipc_io.h"

#include <algorithm>
#include <chrono>

namespace fgpl::ipc {
namespace {

constexpr auto kReadStep = size_t(1) << 20;

} // namespace

cbor::Limits TrustedLimits() {
	auto result = cbor::Limits();
	result.maxDepth = 16;
	result.maxItems = size_t(1) << 20;
	result.maxContainer = size_t(1) << 18;
	result.maxString = kMaxFrameBytes;
	result.maxBytes = kMaxFrameBytes;
	return result;
}

cbor::Limits StrictLimits() {
	auto result = cbor::Limits();
	result.maxDepth = 24;
	result.maxItems = 1 << 16;
	result.maxContainer = 1 << 14;
	result.maxString = size_t(2) << 20;
	result.maxBytes = size_t(128) << 20;
	return result;
}

Result<std::monostate, IoError> WriteFrame(
		os::Handle fd,
		const cbor::Value &message,
		size_t maxBytes) {
	auto body = cbor::Bytes();
	if (!cbor::Encode(message, body, 24)) {
		return IoError::Malformed;
	} else if (body.size() > maxBytes) {
		return IoError::TooLarge;
	}
	const auto size = uint32_t(body.size());
	auto frame = cbor::Bytes();
	frame.reserve(4 + body.size());
	frame.insert(frame.end(), {
		uint8_t(size >> 24),
		uint8_t(size >> 16),
		uint8_t(size >> 8),
		uint8_t(size),
	});
	frame.insert(frame.end(), body.begin(), body.end());
	if (!detail::WriteAll(fd, frame.data(), frame.size())) {
		return IoError::Failed;
	}
	return std::monostate();
}

Result<cbor::Value, IoError> ReadFrame(
		os::Handle fd,
		const cbor::Limits &limits,
		size_t maxBytes,
		int timeoutMs) {
	const auto timed = (timeoutMs >= 0);
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timed ? timeoutMs : 0);
	uint8_t header[4];
	const auto head = detail::ReadExact(fd, header, 4, deadline, timed);
	if (!head.ok()) {
		return head.error();
	}
	const auto size = (size_t(header[0]) << 24)
		| (size_t(header[1]) << 16)
		| (size_t(header[2]) << 8)
		| size_t(header[3]);
	if (size > maxBytes) {
		return IoError::TooLarge;
	}
	auto body = cbor::Bytes();
	while (body.size() != size) {
		const auto at = body.size();
		const auto step = std::min(size - at, kReadStep);
		body.resize(at + step);
		const auto rest = detail::ReadExact(fd, body.data() + at, step, deadline, timed);
		if (!rest.ok()) {
			return (rest.error() == IoError::Closed)
				? IoError::Failed
				: rest.error();
		}
	}
	auto decoded = cbor::Decode(body, limits);
	if (!decoded.ok()) {
		return IoError::Malformed;
	}
	return std::move(decoded.value());
}

} // namespace fgpl::ipc

#include "fgpl/ipc.h"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>

namespace fgpl::ipc {
namespace {

[[nodiscard]] bool WriteAll(int fd, const uint8_t *data, size_t size) {
	while (size != 0) {
		const auto written = write(fd, data, size);
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		data += written;
		size -= size_t(written);
	}
	return true;
}

[[nodiscard]] Result<std::monostate, IoError> ReadExact(
		int fd,
		uint8_t *data,
		size_t size,
		std::chrono::steady_clock::time_point deadline,
		bool timed) {
	auto first = true;
	while (size != 0) {
		if (timed) {
			const auto left = std::chrono::duration_cast<
				std::chrono::milliseconds>(
					deadline - std::chrono::steady_clock::now()).count();
			if (left <= 0) {
				return IoError::Timeout;
			}
			auto descriptor = pollfd{ .fd = fd, .events = POLLIN, .revents = 0 };
			const auto ready = poll(&descriptor, 1, int(left));
			if (ready < 0) {
				if (errno == EINTR) {
					continue;
				}
				return IoError::Failed;
			} else if (ready == 0) {
				return IoError::Timeout;
			}
		}
		const auto count = read(fd, data, size);
		if (count < 0) {
			if (errno == EINTR) {
				continue;
			}
			return IoError::Failed;
		} else if (count == 0) {
			return first ? IoError::Closed : IoError::Failed;
		}
		first = false;
		data += count;
		size -= size_t(count);
	}
	return std::monostate();
}

} // namespace

cbor::Limits TrustedLimits() {
	auto result = cbor::Limits();
	result.maxDepth = 16;
	result.maxItems = size_t(1) << 20;
	result.maxContainer = size_t(1) << 18;
	result.maxString = kMaxFrameBytes;
	return result;
}

cbor::Limits StrictLimits() {
	auto result = cbor::Limits();
	result.maxDepth = 24;
	result.maxItems = 1 << 16;
	result.maxContainer = 1 << 14;
	result.maxString = size_t(2) << 20;
	return result;
}

Result<std::monostate, IoError> WriteFrame(
		int fd,
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
	if (!WriteAll(fd, frame.data(), frame.size())) {
		return IoError::Failed;
	}
	return std::monostate();
}

Result<cbor::Value, IoError> ReadFrame(
		int fd,
		const cbor::Limits &limits,
		size_t maxBytes,
		int timeoutMs) {
	const auto timed = (timeoutMs >= 0);
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timed ? timeoutMs : 0);
	uint8_t header[4];
	const auto head = ReadExact(fd, header, 4, deadline, timed);
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
	auto body = cbor::Bytes(size);
	const auto rest = ReadExact(fd, body.data(), size, deadline, timed);
	if (!rest.ok()) {
		return (rest.error() == IoError::Closed)
			? IoError::Failed
			: rest.error();
	}
	auto decoded = cbor::Decode(body, limits);
	if (!decoded.ok()) {
		return IoError::Malformed;
	}
	return std::move(decoded.value());
}

} // namespace fgpl::ipc

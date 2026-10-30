#include "fgpl/ipc_io.h"

#include <poll.h>
#include <unistd.h>

#include <cerrno>

namespace fgpl::ipc::detail {

bool WriteAll(os::Handle handle, const uint8_t *data, size_t size) {
	const auto fd = int(handle);
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

Result<std::monostate, IoError> ReadExact(
		os::Handle handle,
		uint8_t *data,
		size_t size,
		std::chrono::steady_clock::time_point deadline,
		bool timed) {
	const auto fd = int(handle);
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

} // namespace fgpl::ipc::detail

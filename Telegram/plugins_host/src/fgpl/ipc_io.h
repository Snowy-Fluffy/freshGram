#pragma once

#include "fgpl/ipc.h"

#include <chrono>

namespace fgpl::ipc::detail {

[[nodiscard]] bool WriteAll(os::Handle fd, const uint8_t *data, size_t size);

// Reads exactly size bytes, a clean end before the first byte is Closed.
[[nodiscard]] Result<std::monostate, IoError> ReadExact(
	os::Handle fd,
	uint8_t *data,
	size_t size,
	std::chrono::steady_clock::time_point deadline,
	bool timed);

} // namespace fgpl::ipc::detail
